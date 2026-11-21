#pragma once
#include "../protocol/RawWire.h"
#include <array>
#include <atomic>

namespace nes::c6raw {
/** @brief Неблокирующее принятие драйвером: false не вызывает done, true вызывает ровно один done при освобождении источника. */
using TrySend = bool (*)(unsigned slot, const std::uint8_t*, std::size_t, void* context, void (*done)(void*));
/** @brief Два вечных слота между одним владельцем и RX/driver callbacks; timeout не освобождает слот. */
class Channel final {
public:
    /** @brief Устанавливает драйвер до запуска Hosted/RX; последующая замена запрещена. */
    explicit Channel(TrySend send) noexcept : send_(send) {}
    /** @brief Копирует один запрос; false означает отсутствие передачи и сохранённое владение вызывающего кода. */
    bool submit(Key key, const std::uint8_t* payload, std::size_t bytes, std::uint8_t flags=0) noexcept;
    /** @brief RX-поток копирует единственный подходящий ответ; чужие/поздние/дублирующие пакеты отвергаются. */
    bool receive(const std::uint8_t* packet, std::size_t bytes) noexcept;
    /** @brief Владелец получает borrowed reply только после ответа И освобождения TX источника. */
    bool poll(unsigned slot, View& out) noexcept;
    /** @brief Помечает дедлайн пропущенным без cancellation/reuse; позднее completion по-прежнему требуется. */
    void timeout(unsigned slot) noexcept;
    /** @brief Освобождает лишь оба реально завершённых направления; false сохраняет все буферы. */
    bool release(unsigned slot) noexcept;
    /** @brief Для телеметрии возвращает число занятых слотов, включая quarantined timeout. */
    unsigned occupied() const noexcept;
private:
    /** @brief Статическая память живёт вместе с Channel; объект нельзя разрушать при occupied()!=0. */
    struct Slot final {
        std::atomic<unsigned> state{0}, rx{0};
        std::atomic<bool> txOwned{false}, late{false};
        Key key{};
        std::size_t bytes{};
        std::array<std::uint8_t,requestPacketBytes> tx{};
        std::array<std::uint8_t,replyPacketBytes> reply{};
    };
    /** @brief SDK подтверждает прекращение чтения source; это не подтверждение доставки remote. */
    static void sourceDone(void* context) noexcept;
    std::array<Slot,slotCount> slots_{};
    TrySend send_{};
};
}
