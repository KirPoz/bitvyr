#include "BatchAudioControl.h"
#include "IdleUnbindWire.h"
#include "RawIdleGate.h"
#include "C6SelectRetained.h"
#include "BatchColour.h"
#include "C6BootMeta.h"
#include "RawWire.h"
#include "RawPort.h"
#include "ResolvedColourRun.h"
#include "ResolvedColourCodec.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
/** @file C6Endpoint.cpp @brief Два статических входных слота и отдельный bounded worker C6. */
#include "C6Workload.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_hosted_peer_data.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <array>
#include <atomic>
#include <cstring>

namespace {
/** @brief Слот заимствуется Rx→worker через Ready release/acquire; wire копируется один раз. */
struct Slot final {
    std::atomic<unsigned> state{0U}; // Free0/Writing1/Ready2/Executing3.
    std::size_t bytes{};
    std::array<std::uint8_t, nes::c6_workload::requestHeaderBytes + nes::c6_workload::maxPayloadBytes> wire{};
};
std::array<Slot, 2> slots;
std::array<std::uint8_t, nes::c6_workload::replyHeaderBytes + nes::c6_workload::maxPayloadBytes> reply;
StaticTask_t controlBlock;
static_assert(sizeof(StackType_t) == 1U, "ESP-IDF stack depth must be byte-sized in this adapter");
std::array<StackType_t, 4096U / sizeof(StackType_t)> stack;
TaskHandle_t taskHandle{};
std::atomic<bool> initialized{false};
std::atomic<std::uint32_t> dropped{0U};

/** @brief Читает32-битные host cycles; частота отдельно возвращается в reply. */
std::uint32_t readClock() noexcept { return esp_cpu_get_cycle_count(); }

void rawDrain() noexcept;

/** @brief Обрабатывает только собственные слоты, затем возвращает их Rx без освобождения памяти. */
void worker(void*) noexcept {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        for (auto& slot : slots) {
            unsigned ready = 2U;
            if (!slot.state.compare_exchange_strong(ready, 3U, std::memory_order_acquire)) continue;
            const auto bytes = nes::c6_workload::execute(slot.wire.data(), slot.bytes,
                reply.data(), reply.size(), std::uint32_t(esp_clk_cpu_freq()), readClock);
            if (bytes) {
                // SDK 2.13.1 копирует весь payload в свой buf до возврата send_custom_data.
                // Здесь не делается вывод о доставке: host обязан проверить keyed reply.
                (void)esp_hosted_send_custom_data(nes::c6_workload::responseId, reply.data(), bytes);
            }
            slot.state.store(0U, std::memory_order_release);
        }
        rawDrain();
    }
}

/** @brief Копирует допустимый wire в свободный слот и немедленно отдаёт Rx поток SDK. */
void receive(std::uint32_t id, const std::uint8_t* data, std::size_t bytes, void*) noexcept {
    if (id != nes::c6_workload::requestId || !data || bytes > slots[0].wire.size()) {
        dropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    for (auto& slot : slots) {
        unsigned free = 0U;
        if (!slot.state.compare_exchange_strong(free, 1U, std::memory_order_acquire)) continue;
        std::memcpy(slot.wire.data(), data, bytes); slot.bytes = bytes;
        slot.state.store(2U, std::memory_order_release);
        xTaskNotifyGive(taskHandle); return;
    }
    dropped.fetch_add(1U, std::memory_order_relaxed);
}
#include "RawWorker.inc"
} // namespace

/** @brief Передаёт bounded Hosted raw RX прежнему receiver; не создаёт новое право reuse. */
extern "C" void nes_hosted_raw_rx(const std::uint8_t* data,std::size_t bytes) { receiveRaw(data,bytes); }

/**
 * @brief Однократно создаёт задачу C6 и callback после esp_hosted_coprocessor_init().
 * @return ESP_OK либо ошибка SDK; повторный вызов возвращает ESP_ERR_INVALID_STATE.
 * @details Флаг initialized ставится до создания задачи: даже после ошибки повтор
 * не создаёт вторую задачу или callback. Автоматического отката нет; нужен reboot.
 * @details Один priority1 worker и stack4096Б. Старый probe хранит два input4144Б,
 * reply4160Б и TCB; сверх них существуют четыре RawSlot, FullAudioControl и
 * цветной consumer. Полный SRAM/стек бюджет проверяется по actual C6 ELF и
 * watermark, а не по прежней оценке17КБ только probe. Нет ROM/guest-state/NVS writes.
 * При ошибке регистрации static storage и ожидающий worker остаются до reboot;
 * тайм-аут не выдаётся за completion. SDK allocator использует свои обычные buf.
 */
extern "C" esp_err_t c6_probe_endpoint_init() noexcept {
    bool expected = false;
    if (!initialized.compare_exchange_strong(expected, true)) return ESP_ERR_INVALID_STATE;
    taskHandle = xTaskCreateStatic(worker, "nes_c6_probe", stack.size(), nullptr, 1U,
                                  stack.data(), &controlBlock);
    if (!taskHandle) return ESP_ERR_NO_MEM;
    return esp_hosted_register_custom_callback(nes::c6_workload::requestId, receive, nullptr);
}
