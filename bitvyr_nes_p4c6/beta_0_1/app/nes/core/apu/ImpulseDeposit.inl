/**
 * @file ImpulseDeposit.inl
 * @brief Повторно включаемое точное FIR-тело, работающее с локальными delta и phase.
 * @ingroup nes_apu
 */
// Единственное точное тело зеркального FIR. Требует локальных delta и phase.
    const auto* coefficient = tables_.impulse[phase].data();
    auto* pending = pendingImpulse_.data() + impulsePosition_;
    auto* const end = pending + impulseSize;
    // Большой переход расширенного MMC5 DAC умножается в64bit до операции.
    // Обычные NES-переходы сохраняют прежний32bit быстрый FIR и точные PCM.
    if(delta>65535 || delta< -65535){
        depositWideDelta(delta,phase);
        return;
    }
    // Каждый tap адресует абсолютное назначение в удвоенном буфере.
    // Точные произведения DAC/int16 помещаются в signed32; накопление остаётся int64.
    // Четыре соседних tap не требуют modulo/повторного построения адреса на каждом tap.
    // Сохранить ядро четырёх tap, не позволяя O3 развернуть все 32.
#if defined(__clang__)
#pragma clang loop unroll(disable)
#elif defined(__GNUC__)
#pragma GCC unroll 1
#endif
    do {
        pending[0] += static_cast<std::int64_t>(delta * static_cast<std::int32_t>(coefficient[0]));
        pending[1] += static_cast<std::int64_t>(delta * static_cast<std::int32_t>(coefficient[1]));
        pending[2] += static_cast<std::int64_t>(delta * static_cast<std::int32_t>(coefficient[2]));
        pending[3] += static_cast<std::int64_t>(delta * static_cast<std::int32_t>(coefficient[3]));
        pending += 4;
        coefficient += 4;
    } while (pending != end);
