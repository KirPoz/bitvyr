/**
 * @file AudioRenderer.h
 * @brief Renderer-owned PCM и проверка непрерывности звуковых пакетов.
 * @ingroup nes_apu
 */
#pragma once
#include "Apu.h"
#include "AudioPacket.h"
#include "AudioPacketView.h"

namespace nes::core {
enum class AudioRenderResult : std::uint8_t { Ok,InvalidPacket,StaleGeneration,SequenceMismatch,TimelineMismatch,StatusMismatch,DmcMismatch,NeedsReset,FixedFilterFault };

// Отдельный объект потока владельца. Не хранит ссылки на Machine/Cartridge и
// получает DMC-байты только из текущего неизменяемого retired-пакета.
/**
 * @brief Независимый потребитель аудиопакетов со своим полным APU и PCM. Не исполняет CPU производителя повторно.
 * @ingroup nes_apu
 */
class AudioRenderer final:private ApuBus {
public:
    /**
     * @brief Создаёт независимый DSP-потребитель аудиопакетов.
     * @details Он владеет своим полным APU/PCM, но не исполняет CPU производителя и не читает его текущую ROM вместо сохранённых DMC-ответов.
     */
    AudioRenderer() noexcept:apu_(*this){}
    /**
     * @brief PRIVATE cold query: counts/CRC32 pulse,TND,FIR в явном LE16 порядке.
     * @param out Шесть writable u32: count,CRC для каждой таблицы; не пересекает renderer.
     * @details После полного constructor; единственный worker, до первого render. Не сбрасывает
     * и не продвигает APU; CRC не подтверждает качество PCM или произвольный ROM.
     */
    void diagnosticTableCrcs(std::uint32_t(&out)[6])const noexcept;

    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    AudioRenderer(const AudioRenderer&)=delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    AudioRenderer& operator=(const AudioRenderer&)=delete;
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     */
    void reset() noexcept;
    /**
     * @brief Восстанавливает звук либо изображение из проверенного входного пакета.
     * @details Проверяет идентичность и временной порядок. Неполный или ошибочный пакет не является готовым результатом.
     * @return Вычисленное значение или состояние типа AudioRenderResult согласно описанному контракту.
     * @param packet Запечатанный неизменяемый AudioPacket с реальными DMC-байтами; сохраняется до завершения render.
     */
    [[nodiscard]] AudioRenderResult render(const AudioPacket& packet) noexcept;
    /**
     * @brief Рендерит caller-owned immutable used-record view без full packet allocation.
     * @param packet Header/записи удерживаются внешним single owner до полного возврата.
     * @return Старый AudioRenderResult; неверный extent/count отвергается до мутации APU.
     * @details Все прежние timestamp/sequence/status/DMC/merge проверки сохраняются.
     */
    [[nodiscard]] AudioRenderResult render(const AudioPacketView& packet) noexcept;
    /**
     * @brief Возвращает состояние или результат текущей привязки.
     * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool valid() const noexcept{return valid_;}
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const std::int16_t* samples() const noexcept{return valid_?apu_.samples():nullptr;}
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    [[nodiscard]] std::size_t sampleCount() const noexcept{return valid_?apu_.sampleCount():0;}
    /**
     * @brief Возвращает идентификатор или временную границу текущего интервала.
     * @details Поколение, последовательность и гостевые такты нельзя подменять номером физической LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t generation() const noexcept{return generation_;}
    /**
     * @brief Возвращает идентификатор или временную границу текущего интервала.
     * @details Поколение, последовательность и гостевые такты нельзя подменять номером физической LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t sequence() const noexcept{return lastSequence_;}
    /**
     * @brief Возвращает номер PPU-кадра, которому соответствует текущий результат.
     * @details Не является номером LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t ppuFrame() const noexcept{return ppuFrame_;}
    /**
     * @brief Возвращает уже учтённое гостевое время соответствующего устройства.
     * @details Временные владельцы могут включать накопленные, но ещё не материализованные такты; это не время хоста.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t cycles() const noexcept{return apu_.cycles_+apu_.pendingCycles_;}
private:
    /**
     * @brief Доводит повторный PPU до абсолютного гостевого времени.
     * @details Не исполняет CPU/APU теневой машины и сохраняет NTSC/PAL-фазу.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     */
    void advance(std::uint64_t cycle) noexcept;
    /**
     * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
     * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t dmcRead(std::uint16_t address) noexcept override;
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     */
    void dmcStall(unsigned count) noexcept override;
    Apu apu_;
    const AudioPacketView* packet_=nullptr;
    std::uint64_t generation_=0,nextSequence_=0,lastSequence_=0,ppuFrame_=0;
    unsigned dmcRead_=0,dmcStall_=0;
    Region region_=Region::Ntsc;
    bool valid_=false,poisoned_=false,dmcFault_=false;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    std::uint16_t capabilities_=0;
#endif
};
} // пространство имён nes::core
