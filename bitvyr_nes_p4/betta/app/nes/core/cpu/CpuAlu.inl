/**
 * @file CpuAlu.inl
 * @brief Единые параметризованные тела ALU для member-регистров и локального quiet-состояния.
 * @ingroup nes_cpu
 */
// Единый источник ALU для исходного member-состояния и локальных регистров блока.
#ifdef NES_CPU_ALU_EMIT_setFlag
/**
 * @brief Меняет только указанный флаг локального или member-состояния CPU.
 * @param bit Маска бита P; остальные биты сохраняются.
 * @param set Требуемое логическое состояние бита.
 * @details Сигнатуру и выбранное состояние предоставляет включающий файл.
 * Операция не читает шину, не добавляет такт и не опрашивает прерывание.
 */
NES_CPU_ALU_SIGNATURE_setFlag {
    if (set) NES_CPU_ALU_STATE.p |= bit;
    else NES_CPU_ALU_STATE.p &= static_cast<std::uint8_t>(~bit);
}
#endif
#ifdef NES_CPU_ALU_EMIT_flag
/**
 * @brief Читает флаг выбранного состояния CPU без изменения регистра P.
 * @param bit Проверяемая маска P.
 * @return true, если хотя бы один бит маски установлен.
 */
NES_CPU_ALU_SIGNATURE_flag { return (NES_CPU_ALU_STATE.p & bit) != 0; }
#endif
#ifdef NES_CPU_ALU_EMIT_setNz
/**
 * @brief Вычисляет только флаги Z/N восьмибитного результата.
 * @param value Уже полученный результат; само значение и остальные флаги не меняются.
 */
NES_CPU_ALU_SIGNATURE_setNz {
#if defined(NES_CORE_CPU_CANONICAL_FLAGS) && NES_CORE_CPU_CANONICAL_FLAGS
    // P остаётся каноническим сразу: сохраняются все биты кроме N/Z.
    NES_CPU_ALU_STATE.p = static_cast<std::uint8_t>(
        (NES_CPU_ALU_STATE.p & static_cast<std::uint8_t>(~(N | Z))) |
        (value & N) | (static_cast<unsigned>(value == 0) << 1U));
#else
    NES_CPU_ALU_CALL_setFlag(Z, value == 0);
    NES_CPU_ALU_CALL_setFlag(N, (value & 0x80U) != 0);
#endif
}
#endif
#ifdef NES_CPU_ALU_EMIT_adc
/**
 * @brief Выполняет двоичное сложение A+value+C с флагами C/V/N/Z.
 * @param value Уже прочитанный восьмибитный операнд.
 * @details NES не выполняет BCD даже при D=1. SBC использует это же тело
 * с инверсией операнда. История шинных чтений остаётся у вызывающего.
 */
NES_CPU_ALU_SIGNATURE_adc {
    const auto old = NES_CPU_ALU_STATE.a;
    const auto sum = static_cast<std::uint16_t>(old + value + (NES_CPU_ALU_CALL_flag(C) ? 1U : 0U));
    NES_CPU_ALU_STATE.a = static_cast<std::uint8_t>(sum);
#if defined(NES_CORE_CPU_CANONICAL_FLAGS) && NES_CORE_CPU_CANONICAL_FLAGS
    // Старый C уже вошёл в sum; остальные четыре флага публикуются одним P-store.
    const auto result = NES_CPU_ALU_STATE.a;
    NES_CPU_ALU_STATE.p = static_cast<std::uint8_t>(
        (NES_CPU_ALU_STATE.p & static_cast<std::uint8_t>(~(C | V | N | Z))) |
        ((sum >> 8U) & C) | ((~(old ^ value) & (old ^ result) & N) >> 1U) |
        (result & N) | (static_cast<unsigned>(result == 0) << 1U));
#else
    NES_CPU_ALU_CALL_setFlag(C, sum > 0xFFU);
    NES_CPU_ALU_CALL_setFlag(V, ((~(old ^ value) & (old ^ NES_CPU_ALU_STATE.a)) & 0x80U) != 0);
    NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a);
#endif
}
#endif
#ifdef NES_CPU_ALU_EMIT_compare
/**
 * @brief Вычисляет C/N/Z для сравнения без изменения операндов.
 * @param left Левое восьмибитное значение регистра.
 * @param right Правое восьмибитное значение памяти или immediate.
 * @details C означает left>=right, N/Z берутся из восьмибитного вычитания.
 */
NES_CPU_ALU_SIGNATURE_compare {
#if defined(NES_CORE_CPU_CANONICAL_FLAGS) && NES_CORE_CPU_CANONICAL_FLAGS
    // CMP/CPX/CPY сохраняют V; восемь младших битов разности определяют N/Z.
    const auto result = static_cast<std::uint8_t>(left - right);
    NES_CPU_ALU_STATE.p = static_cast<std::uint8_t>(
        (NES_CPU_ALU_STATE.p & static_cast<std::uint8_t>(~(C | N | Z))) |
        static_cast<unsigned>(left >= right) | (result & N) |
        (static_cast<unsigned>(result == 0) << 1U));
#else
    NES_CPU_ALU_CALL_setFlag(C, left >= right);
    NES_CPU_ALU_CALL_setNz(static_cast<std::uint8_t>(left - right));
#endif
}
#endif
#ifdef NES_CPU_ALU_EMIT_modify
/**
 * @brief Вычисляет модифицированный байт RMW и эффекты составной ALU-инструкции.
 * @param op Декодированный ASL/LSR/ROL/ROR/INC/DEC либо составной неофициальный op.
 * @param value Настоящий ранее прочитанный байт; шинные записи выполняет caller.
 * @return Байт для финальной записи. Для составных op также обновлены A/флаги.
 * @details Этот helper не заменяет dummy-write и не меняет их хронологию.
 */
NES_CPU_ALU_SIGNATURE_modify {
    const auto carry = NES_CPU_ALU_CALL_flag(C) ? 1U : 0U;
    switch (op) {
    case Op::ASL: case Op::SLO:
        NES_CPU_ALU_CALL_setFlag(C, (value & 0x80U) != 0); value = static_cast<std::uint8_t>(value << 1); break;
    case Op::LSR: case Op::SRE:
        NES_CPU_ALU_CALL_setFlag(C, (value & 1U) != 0); value = static_cast<std::uint8_t>(value >> 1); break;
    case Op::ROL: case Op::RLA:
        NES_CPU_ALU_CALL_setFlag(C, (value & 0x80U) != 0); value = static_cast<std::uint8_t>((value << 1) | carry); break;
    case Op::ROR: case Op::RRA:
        NES_CPU_ALU_CALL_setFlag(C, (value & 1U) != 0); value = static_cast<std::uint8_t>((value >> 1) | (carry << 7)); break;
    case Op::INC: case Op::ISC: ++value; break;
    case Op::DEC: case Op::DCP: --value; break;
    default: break;
    }
    switch (op) {
    case Op::SLO: NES_CPU_ALU_STATE.a |= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::RLA: NES_CPU_ALU_STATE.a &= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::SRE: NES_CPU_ALU_STATE.a ^= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::RRA: NES_CPU_ALU_CALL_adc(value); break;
    case Op::DCP: NES_CPU_ALU_CALL_compare(NES_CPU_ALU_STATE.a, value); break;
    case Op::ISC: NES_CPU_ALU_CALL_adc(static_cast<std::uint8_t>(~value)); break;
    default: NES_CPU_ALU_CALL_setNz(value); break;
    }
    return value;
}
#endif
#ifdef NES_CPU_ALU_EMIT_applyRead
/**
 * @brief Применяет регистровый эффект инструкции после настоящего чтения операнда.
 * @param op Уже декодированная операция семейства read.
 * @param value Возвращённый шиной операнд; повторного чтения здесь нет.
 * @details Изменяет только указанные операцией регистры и флаги; тайминг,
 * открытая шина, IRQ/DMC и NOP-чтения обслуживаются внешним интерпретатором.
 */
NES_CPU_ALU_SIGNATURE_applyRead {
    switch (op) {
    case Op::ORA: NES_CPU_ALU_STATE.a |= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::AND: NES_CPU_ALU_STATE.a &= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::EOR: NES_CPU_ALU_STATE.a ^= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::ADC: NES_CPU_ALU_CALL_adc(value); break;
    case Op::SBC: NES_CPU_ALU_CALL_adc(static_cast<std::uint8_t>(~value)); break;
    case Op::LDA: NES_CPU_ALU_STATE.a = value; NES_CPU_ALU_CALL_setNz(value); break;
    case Op::LDX: NES_CPU_ALU_STATE.x = value; NES_CPU_ALU_CALL_setNz(value); break;
    case Op::LDY: NES_CPU_ALU_STATE.y = value; NES_CPU_ALU_CALL_setNz(value); break;
    case Op::LAX: NES_CPU_ALU_STATE.a = NES_CPU_ALU_STATE.x = value; NES_CPU_ALU_CALL_setNz(value); break;
    case Op::LAS: NES_CPU_ALU_STATE.a = NES_CPU_ALU_STATE.x = NES_CPU_ALU_STATE.sp = static_cast<std::uint8_t>(NES_CPU_ALU_STATE.sp & value); NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    case Op::CMP: NES_CPU_ALU_CALL_compare(NES_CPU_ALU_STATE.a, value); break;
    case Op::CPX: NES_CPU_ALU_CALL_compare(NES_CPU_ALU_STATE.x, value); break;
    case Op::CPY: NES_CPU_ALU_CALL_compare(NES_CPU_ALU_STATE.y, value); break;
    case Op::BIT:
        NES_CPU_ALU_CALL_setFlag(Z, (NES_CPU_ALU_STATE.a & value) == 0); NES_CPU_ALU_CALL_setFlag(V, (value & V) != 0); NES_CPU_ALU_CALL_setFlag(N, (value & N) != 0); break;
    case Op::ANC: NES_CPU_ALU_STATE.a &= value; NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); NES_CPU_ALU_CALL_setFlag(C, NES_CPU_ALU_CALL_flag(N)); break;
    case Op::ALR: NES_CPU_ALU_STATE.a &= value; NES_CPU_ALU_STATE.a = NES_CPU_ALU_CALL_modify(Op::LSR, NES_CPU_ALU_STATE.a); break;
    case Op::ARR:
        value &= NES_CPU_ALU_STATE.a;
        NES_CPU_ALU_STATE.a = static_cast<std::uint8_t>((value >> 1) | (NES_CPU_ALU_CALL_flag(C) ? 0x80U : 0U));
        NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); NES_CPU_ALU_CALL_setFlag(C, (NES_CPU_ALU_STATE.a & 0x40U) != 0);
        NES_CPU_ALU_CALL_setFlag(V, ((NES_CPU_ALU_STATE.a ^ (NES_CPU_ALU_STATE.a << 1)) & 0x40U) != 0); break;
    case Op::AXS:
        value = static_cast<std::uint8_t>((NES_CPU_ALU_STATE.a & NES_CPU_ALU_STATE.x) - value);
        NES_CPU_ALU_CALL_setFlag(C, value <= (NES_CPU_ALU_STATE.a & NES_CPU_ALU_STATE.x)); NES_CPU_ALU_STATE.x = value; NES_CPU_ALU_CALL_setNz(value); break;
    case Op::XAA:
        // Аналогозависимые XAA/LAX-immediate не гарантированы
        // на всех физических микросхемах. Здесь выбран детерминированный X & operand,
        // без моделирования температуры кристалла и шинного шума.
        NES_CPU_ALU_STATE.a = static_cast<std::uint8_t>(NES_CPU_ALU_STATE.x & value); NES_CPU_ALU_CALL_setNz(NES_CPU_ALU_STATE.a); break;
    default: break; // NOP с обращением к памяти намеренно сохраняет чтения.
    }
}
#endif
