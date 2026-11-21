#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::audio_probe {
using ReadCycles = std::uint32_t (*)() noexcept;
/**
 * @brief Единолично исполняет bounded filter probe над встроенным реальным корпусом.
 * @param request Восемь LE байт: version1, fixture0..1, repeats1..16, flags0, reserved32=0.
 * @param requestBytes Должно быть8; malformed запрос не запускает kernel.
 * @param output Исключительный reply payload не менее144Б; ссылки наружу не сохраняются.
 * @param capacity Размер output; результат публикуется RawWorker только после полного возврата.
 * @param readCycles Счётчик C6 cycles; не wall-time хоста. Обёртка обязана передать реальную частоту.
 * @return Число LE байт ответа либо0 при отказе. Рабочие arrays статические: только existing C6 worker.
 */
std::size_t filterProbe(const std::uint8_t* request, std::size_t requestBytes,
    std::uint8_t* output, std::size_t capacity, ReadCycles readCycles) noexcept;
} // namespace nes::audio_probe
