/**
 * @file Cpu6502.cpp
 * @brief Собственный шинно-тактовый интерпретатор Ricoh 2A03/2A07.
 * @ingroup nes_cpu
 */
#include "Cpu6502.h"

#include "Machine.h"

namespace nes::core {

// Матрица NMOS-опкодов — аппаратное кодирование, а не исполняемый код
// другого эмулятора. Имена/режимы следуют справочнику инструкций NESdev.
#define INS(code, op, mode) {Op::op, Mode::mode},
const Cpu6502::Instruction Cpu6502::instructions_[256] = {
#include "OpcodeTable.inl"
};
#undef INS

/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param addressValue Фактически выставляемый адрес PPU; аппаратный диапазон нормализуется исходным шинным путём.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Cpu6502::read(std::uint16_t addressValue) noexcept {
    irqAtLastCycleStart_ = irqLine_;
    nmiAtLastCycleStart_ = nmiPending_;
    ++state_.cycles;
    return bus_.read(addressValue);
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param addressValue Фактически выставляемый адрес PPU; аппаратный диапазон нормализуется исходным шинным путём.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Cpu6502::write(std::uint16_t addressValue, std::uint8_t value) noexcept {
    irqAtLastCycleStart_ = irqLine_;
    nmiAtLastCycleStart_ = nmiPending_;
    ++state_.cycles;
    bus_.write(addressValue, value);
}

/**
 * @brief Читает следующий байт кода и продвигает PC.
 * @details Использует тот же порядок доступа и обновления состояния шины, что и обычное исполнение.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Cpu6502::fetch() noexcept {
    const auto value = read(state_.pc);
    ++state_.pc;
    return value;
}

/**
 * @brief Считывает младший и старший байты операнда в их шинном порядке.
 * @details Переполнение 16-битного PC и порядок чтений сохраняются; это не безэффектная загрузка машинного uint16.
 * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
 */
std::uint16_t Cpu6502::fetchWord() noexcept {
    const std::uint16_t low = fetch();
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(fetch()) << 8));
}

/**
 * @brief Записывает байт на страницу стека CPU и уменьшает SP.
 * @details Такт записи и открытое состояние шины учитываются исходным CPU-протоколом.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Cpu6502::push(std::uint8_t value) noexcept {
    write(static_cast<std::uint16_t>(0x100U | state_.sp), value);
    --state_.sp;
}

/**
 * @brief Увеличивает SP и читает байт страницы стека CPU.
 * @details Возвращаемое значение и фактические шинные такты требуются инструкциям возврата и восстановления флагов.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Cpu6502::pull() noexcept {
    ++state_.sp;
    return read(static_cast<std::uint16_t>(0x100U | state_.sp));
}

#define NES_CPU_ALU_STATE state_
#define NES_CPU_ALU_SIGNATURE_setFlag void Cpu6502::setFlag(Flag bit, bool set) noexcept
#define NES_CPU_ALU_CALL_setFlag(...) setFlag(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_flag bool Cpu6502::flag(Flag bit) const noexcept
#define NES_CPU_ALU_CALL_flag(...) flag(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_setNz void Cpu6502::setNz(std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_setNz(...) setNz(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_adc void Cpu6502::adc(std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_adc(...) adc(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_compare void Cpu6502::compare(std::uint8_t left, std::uint8_t right) noexcept
#define NES_CPU_ALU_CALL_compare(...) compare(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_modify std::uint8_t Cpu6502::modify(Op op, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_modify(...) modify(__VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_applyRead void Cpu6502::applyRead(Op op, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_applyRead(...) applyRead(__VA_ARGS__)
#define NES_CPU_ALU_EMIT_setFlag
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_setFlag

#define NES_CPU_ALU_EMIT_flag
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_flag

#define NES_CPU_ALU_EMIT_setNz
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_setNz

/**
 * @brief Восстанавливает указанные регистры для тестового или отладочного среза.
 * @details Не является полным save-state машины и не восстанавливает автоматически картридж, PPU, APU и внешние буферы.
 * @param state Состояние регистров, которое этот ALU/отладочный помощник читает или изменяет.
 */
void Cpu6502::setState(State state) noexcept {
    state_ = state;
    state_.p = static_cast<std::uint8_t>((state_.p | U) & ~B);
    nmiPending_ = false;
    nmiReady_ = false;
    nmiAtLastCycleStart_ = false;
    irqPending_ = irqLine_ && !flag(I);
    irqAtLastCycleStart_ = irqLine_;
    inStep_ = false;
}

/**
 * @brief Обновляет электрический уровень IRQ без его немедленного обслуживания.
 * @details Решение CPU об обслуживании учитывает маску I и положение последнего цикла.
 * @param asserted Текущий логический уровень IRQ; квалификация момента опроса сохраняется у CPU.
 */
void Cpu6502::setIrqLine(bool asserted) noexcept {
    irqLine_ = asserted;
    // Изменение линии между инструкциями, например при DMA, уже присутствует
    // при возобновлении. Незавершённая инструкция опрашивает предыдущий такт.
    if (!inStep_) irqPending_ = asserted && !flag(I);
}

/**
 * @brief Защёлкивает уже квалифицированный фронт NMI для CPU.
 * @details Это событие синхронизатора, а не произвольное чтение электрического уровня.
 */
void Cpu6502::requestNmi() noexcept {
    nmiPending_ = true;
    if (!inStep_) nmiReady_ = true;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void Cpu6502::reset() noexcept {
    inStep_ = true;
    state_.jammed = false;
    read(state_.pc);
    read(state_.pc);
    for (unsigned i = 0; i < 3; ++i) {
        read(static_cast<std::uint16_t>(0x100U | state_.sp));
        --state_.sp;
    }
    const std::uint16_t low = read(0xFFFC);
    state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(read(0xFFFD)) << 8));
    state_.p = static_cast<std::uint8_t>((state_.p | I | U) & ~B);
    nmiPending_ = false;
    nmiReady_ = false;
    nmiAtLastCycleStart_ = false;
    irqPending_ = false;
    inStep_ = false;
}

/**
 * @brief Публикует текущий адрес на шине PPU.
 * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
 * @param mode Декодированный режим адресации 6502, определяющий настоящие operand/dummy-чтения.
 * @param access Класс шинного доступа Read/Write/Modify; определяет фиктивные чтения и штраф пересечения страницы.
 * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
 */
std::uint16_t Cpu6502::address(Mode mode, Access access) noexcept {
    std::uint16_t result = 0;
    switch (mode) {
    case Mode::Imm: result = state_.pc++; break;
    case Mode::Zp: result = fetch(); break;
    case Mode::ZpX:
    case Mode::ZpY: {
        const auto base = fetch();
        read(base);
        result = static_cast<std::uint8_t>(base + (mode == Mode::ZpX ? state_.x : state_.y));
        break;
    }
    case Mode::Abs: result = fetchWord(); break;
    case Mode::AbsX:
    case Mode::AbsY: {
        const auto base = fetchWord();
        baseAddress_ = base;
        result = static_cast<std::uint16_t>(base + (mode == Mode::AbsX ? state_.x : state_.y));
        if (access != Access::Read || (base & 0xFF00U) != (result & 0xFF00U)) {
            read(static_cast<std::uint16_t>((base & 0xFF00U) | (result & 0xFFU)));
        }
        return result;
    }
    case Mode::XInd: {
        const auto base = fetch();
        read(base);
        const auto pointer = static_cast<std::uint8_t>(base + state_.x);
        const std::uint16_t low = read(pointer);
        result = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(read(static_cast<std::uint8_t>(pointer + 1U))) << 8));
        break;
    }
    case Mode::IndY: {
        const auto pointer = fetch();
        const std::uint16_t low = read(pointer);
        const std::uint16_t base = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(read(static_cast<std::uint8_t>(pointer + 1U))) << 8));
        baseAddress_ = base;
        result = static_cast<std::uint16_t>(base + state_.y);
        if (access != Access::Read || (base & 0xFF00U) != (result & 0xFF00U)) {
            read(static_cast<std::uint16_t>((base & 0xFF00U) | (result & 0xFFU)));
        }
        return result;
    }
    default: break; // Imp/Acc/Rel/JMP indirect имеют явные последовательности тактов.
    }
    baseAddress_ = result;
    return result;
}

#define NES_CPU_ALU_EMIT_adc
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_adc

#define NES_CPU_ALU_EMIT_compare
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_compare

#define NES_CPU_ALU_EMIT_modify
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_modify

#define NES_CPU_ALU_EMIT_applyRead
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_applyRead
#undef NES_CPU_ALU_STATE
#undef NES_CPU_ALU_SIGNATURE_setFlag
#undef NES_CPU_ALU_CALL_setFlag
#undef NES_CPU_ALU_SIGNATURE_flag
#undef NES_CPU_ALU_CALL_flag
#undef NES_CPU_ALU_SIGNATURE_setNz
#undef NES_CPU_ALU_CALL_setNz
#undef NES_CPU_ALU_SIGNATURE_adc
#undef NES_CPU_ALU_CALL_adc
#undef NES_CPU_ALU_SIGNATURE_compare
#undef NES_CPU_ALU_CALL_compare
#undef NES_CPU_ALU_SIGNATURE_modify
#undef NES_CPU_ALU_CALL_modify
#undef NES_CPU_ALU_SIGNATURE_applyRead
#undef NES_CPU_ALU_CALL_applyRead


/**
 * @brief Выполняет шинную последовательность входа в прерывание CPU.
 * @details Сохраняет PC и статус в стек, использует нужный вектор и не подменяет уже начатую инструкцию поздним IRQ.
 * @param nmi Квалифицированный NMI-фронт, доставляемый в прежней точке CPU-опроса.
 * @param software true для программного BRK с B=1; аппаратное прерывание сохраняет исходный B/PC-протокол.
 */
void Cpu6502::interrupt(bool nmi, bool software) noexcept {
    if (software) fetch(); // Заполняющий байт BRK читается и пропускается.
    else { read(state_.pc); read(state_.pc); }
    push(static_cast<std::uint8_t>(state_.pc >> 8));
    push(static_cast<std::uint8_t>(state_.pc));
    push(static_cast<std::uint8_t>((state_.p | U | (software ? B : 0)) & (software ? 0xFFU : ~B)));
    // Machine квалифицирует NMI-фронт до передачи защёлки. Опросить её
    // до выборки вектора; фронт во время этой выборки сохраняется для
    // следующей инструкции. Сохранённые PC и B описывают исходное событие.
    if (nmiPending_) nmi = true;
    if (nmi) nmiPending_ = false;
    nmiReady_ = false;
    setFlag(I, true);
    const std::uint16_t vector = nmi ? 0xFFFA : 0xFFFE;
    const std::uint16_t low = read(vector);
    state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(read(static_cast<std::uint16_t>(vector + 1))) << 8));
}

/**
 * @brief Исполняет один шаг устройства с предусмотренными шинными эффектами.
 * @details Для Cpu6502 это одна инструкция или обслуживание прерывания; собственные такты CPU отличаются от времени машины с DMA.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::uint32_t Cpu6502::step() noexcept {
    const auto before = state_.cycles;
    inStep_ = true;
    if (state_.jammed) {
        read(state_.pc);
        inStep_ = false;
        return 1;
    }
    if (nmiReady_ || irqPending_) {
        interrupt(nmiReady_, false);
        irqPending_ = false;
        inStep_ = false;
        return static_cast<std::uint32_t>(state_.cycles - before);
    }
    const bool oldInterruptMask = flag(I);
    const auto instruction = instructions_[fetch()];
    bool earlyIrqPoll = false;
    bool earlyIrqLine = false;
    bool earlyNmi = false;
    switch (instruction.op) {
    case Op::BRK: interrupt(false, true); break;
    case Op::JAM: read(state_.pc); state_.jammed = true; break;
    case Op::JSR: {
        const std::uint16_t low = fetch();
        read(static_cast<std::uint16_t>(0x100U | state_.sp));
        push(static_cast<std::uint8_t>(state_.pc >> 8));
        push(static_cast<std::uint8_t>(state_.pc));
        state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(fetch()) << 8));
        break;
    }
    case Op::RTS: {
        read(state_.pc); read(static_cast<std::uint16_t>(0x100U | state_.sp));
        const std::uint16_t low = pull();
        state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(pull()) << 8));
        read(state_.pc); ++state_.pc; break;
    }
    case Op::RTI: {
        read(state_.pc); read(static_cast<std::uint16_t>(0x100U | state_.sp));
        state_.p = static_cast<std::uint8_t>((pull() | U) & ~B);
        const std::uint16_t low = pull();
        state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(pull()) << 8)); break;
    }
    case Op::JMP: {
        const auto pointer = fetchWord();
        if (instruction.mode == Mode::Abs) state_.pc = pointer;
        else {
            const std::uint16_t low = read(pointer);
            const auto highAddress = static_cast<std::uint16_t>((pointer & 0xFF00U) | ((pointer + 1U) & 0xFFU));
            state_.pc = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(read(highAddress)) << 8));
        }
        break;
    }
    case Op::PHA: case Op::PHP:
        read(state_.pc); push(instruction.op == Op::PHA ? state_.a : static_cast<std::uint8_t>(state_.p | U | B)); break;
    case Op::PLA: case Op::PLP:
        read(state_.pc); read(static_cast<std::uint16_t>(0x100U | state_.sp));
        if (instruction.op == Op::PLA) { state_.a = pull(); setNz(state_.a); }
        else state_.p = static_cast<std::uint8_t>((pull() | U) & ~B);
        break;
    case Op::BPL: case Op::BMI: case Op::BVC: case Op::BVS:
    case Op::BCC: case Op::BCS: case Op::BNE: case Op::BEQ: {
        const auto displacement = static_cast<std::int8_t>(fetch());
        bool take = false;
        switch (instruction.op) {
        case Op::BPL: take = !flag(N); break; case Op::BMI: take = flag(N); break;
        case Op::BVC: take = !flag(V); break; case Op::BVS: take = flag(V); break;
        case Op::BCC: take = !flag(C); break; case Op::BCS: take = flag(C); break;
        case Op::BNE: take = !flag(Z); break; case Op::BEQ: take = flag(Z); break;
        default: break;
        }
        // Переходы опрашивают до чтения операнда, а при пересечении страницы ещё
        // до коррекции PCH. Перед первым фиктивным тактом опроса нет.
        earlyIrqPoll = true; earlyIrqLine = irqAtLastCycleStart_;
        earlyNmi = nmiAtLastCycleStart_;
        if (take) {
            const auto next = static_cast<std::uint16_t>(state_.pc + displacement);
            read(state_.pc);
            if ((state_.pc & 0xFF00U) != (next & 0xFF00U)) {
                earlyIrqLine = earlyIrqLine || irqLine_;
                earlyNmi = earlyNmi || nmiPending_;
                read(static_cast<std::uint16_t>((state_.pc & 0xFF00U) | (next & 0xFFU)));
            }
            state_.pc = next;
        }
        break;
    }
    default:
        if (instruction.mode == Mode::Imp) {
            read(state_.pc);
            switch (instruction.op) {
            case Op::CLC: setFlag(C, false); break; case Op::SEC: setFlag(C, true); break;
            case Op::CLI: setFlag(I, false); break; case Op::SEI: setFlag(I, true); break;
            case Op::CLD: setFlag(D, false); break; case Op::SED: setFlag(D, true); break;
            case Op::CLV: setFlag(V, false); break;
            case Op::DEY: --state_.y; setNz(state_.y); break; case Op::INY: ++state_.y; setNz(state_.y); break;
            case Op::DEX: --state_.x; setNz(state_.x); break; case Op::INX: ++state_.x; setNz(state_.x); break;
            case Op::TXA: state_.a = state_.x; setNz(state_.a); break; case Op::TYA: state_.a = state_.y; setNz(state_.a); break;
            case Op::TAX: state_.x = state_.a; setNz(state_.x); break; case Op::TAY: state_.y = state_.a; setNz(state_.y); break;
            case Op::TSX: state_.x = state_.sp; setNz(state_.x); break; case Op::TXS: state_.sp = state_.x; break;
            default: break;
            }
        } else if (instruction.mode == Mode::Acc) {
            read(state_.pc); state_.a = modify(instruction.op, state_.a);
        } else {
            bool store = false;
            bool rmw = false;
            switch (instruction.op) {
            case Op::STA: case Op::STX: case Op::STY: case Op::SAX:
            case Op::SHX: case Op::SHY: case Op::AHX: case Op::TAS: store = true; break;
            case Op::ASL: case Op::LSR: case Op::ROL: case Op::ROR: case Op::INC: case Op::DEC:
            case Op::SLO: case Op::RLA: case Op::SRE: case Op::RRA: case Op::DCP: case Op::ISC: rmw = true; break;
            default: break;
            }
            auto location = address(instruction.mode, store ? Access::Write : rmw ? Access::Modify : Access::Read);
            if (store) {
                std::uint8_t value = state_.a;
                switch (instruction.op) {
                case Op::STX: value = state_.x; break; case Op::STY: value = state_.y; break;
                case Op::SAX: value = static_cast<std::uint8_t>(state_.a & state_.x); break;
                case Op::SHX: value = state_.x; break; case Op::SHY: value = state_.y; break;
                case Op::TAS: state_.sp = static_cast<std::uint8_t>(state_.a & state_.x); value = state_.sp; break;
                case Op::AHX: value = static_cast<std::uint8_t>(state_.a & state_.x); break;
                default: break;
                }
                if (instruction.op == Op::SHX || instruction.op == Op::SHY || instruction.op == Op::AHX || instruction.op == Op::TAS) {
                    // Обычное поведение masked-store без остановки RDY. DMC-
                    // остановка на фиктивном адресном такте может менять настоящее поведение кремния.
                    value &= static_cast<std::uint8_t>((baseAddress_ >> 8) + 1U);
                    if ((baseAddress_ & 0xFF00U) != (location & 0xFF00U)) location = static_cast<std::uint16_t>((static_cast<std::uint16_t>(value) << 8) | (location & 0xFFU));
                }
                write(location, value);
            } else {
                const auto value = read(location);
                if (rmw) { write(location, value); write(location, modify(instruction.op, value)); }
                else applyRead(instruction.op, value);
            }
        }
        break;
    }
    const bool delayedI = instruction.op == Op::CLI || instruction.op == Op::SEI || instruction.op == Op::PLP;
    const bool sampledMask = delayedI ? oldInterruptMask : flag(I);
    irqPending_ = (earlyIrqPoll ? earlyIrqLine : irqAtLastCycleStart_) && !sampledMask;
    // BRK/interrupt-последовательности не опрашивают повторно: минимум первая инструкция
    // обработчика исполняется до фронта, пришедшего после выбора вектора.
    nmiReady_ = instruction.op != Op::BRK && instruction.op != Op::JAM
        && (earlyIrqPoll ? earlyNmi : nmiPending_);
    inStep_ = false;
    return static_cast<std::uint32_t>(state_.cycles - before);
}

} // пространство имён nes::core
