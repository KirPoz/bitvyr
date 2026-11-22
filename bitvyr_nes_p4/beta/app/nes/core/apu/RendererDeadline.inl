/**
 * @file RendererDeadline.inl
 * @brief PRIVATE: точные локальные deadlines полного renderer APU, без новых fields/API.
 * @details Включается в начале advanceRendererCpuCycles под OFF-экспериментом.
 * Внешние регистры/packet I/O и frame-события остаются границами. DMC active и
 * period0 сохраняют прежний путь. Источники ROM/плата/задачи здесь не читаются.
 * PRIVATE DMC-флаг снимает только два запрета ниже: asyncQuietCpuCycles
 * всё равно ограничивает каждый блок до DMC timer/refill; саму границу
 * пересекает исходное advanceCpuCycles. Новых полей/таймеров/API нет.
 */
    if (count >= 8 && (triangle_.period || triangleMean) && !frameResetDelay_
#if !defined(NES_CORE_APU_RENDERER_DMC_DEADLINES) || !NES_CORE_APU_RENDERER_DMC_DEADLINES
        && dmc_.silent && dmc_.bufferEmpty && !dmc_.remaining
#endif
    ) {
        // pending уже относится к выполненному guest-time; только материализовать
        // его перед локальными timestamp, не прибавляя к новому count повторно.
        synchronize();
        // write() требует первую исходную scalar-итерацию даже без timer edge.
        if (eventCountdown_ == 1) { advanceCpuCycles(1, triangleMean); --count; }
        while (count >= 8 && !frameResetDelay_
#if !defined(NES_CORE_APU_RENDERER_DMC_DEADLINES) || !NES_CORE_APU_RENDERER_DMC_DEADLINES
               && dmc_.silent && dmc_.bufferEmpty && !dmc_.remaining
#endif
        ) {
            const unsigned quiet = asyncQuietCpuCycles();
            const unsigned block = std::min(count, quiet);
            if (block < 8) {
                // Ровно пересечь ближайший frame/DMC edge прежним телом.
                eventCountdown_ = static_cast<std::uint8_t>(nextEventDistance(triangleMean));
                const unsigned literal = std::min(count, quiet + 1U);
                advanceCpuCycles(literal, triangleMean); count -= literal;
                continue;
            }
            const auto baseCycle = cycles_;
            const bool triangleActive = triangle_.length && triangle_.linear;
            const bool noiseActive = noise_.length && noise_.envelope.output();
            bool pulseActive[2];
            unsigned nextPulse[2], pulseStamp[2] = {0, 0};
            constexpr unsigned never = 0x3fffffffU;
            const unsigned firstEven = (baseCycle & 1U) ? 1U : 2U;
            for (unsigned i = 0; i < 2; ++i) {
                const auto& pulse = pulses_[i];
                pulseActive[i] = pulse.length && pulse.period >= 8 &&
                    pulse.envelope.output() && sweepTarget(i) <= 0x7ff;
                nextPulse[i] = pulseActive[i] ? firstEven + 2U * pulse.timer : never;
            }
            bool triangleMeanEntry = triangleMean && triangleActive && triangle_.period <= 1U && triangle_.timer > triangle_.period;
            unsigned nextTriangle = triangleActive && !(triangleMean && triangleMeanEligible())
                ? (triangleMeanEntry ? unsigned(triangle_.timer - triangle_.period) : unsigned(triangle_.timer) + 1U) : never;
            unsigned nextNoise = noiseActive ? unsigned(noise_.timer) + 1U : never;
            unsigned nextSample = (cpuRate_ - samplePhase_ + sampleRate - 1U) / sampleRate;
            unsigned elapsed = 0, triangleStamp = 0, noiseStamp = 0;
            const unsigned noisePeriod = noisePeriods[pal_ ? 1 : 0][noise_.control & 15];
            while (true) {
                const unsigned event = std::min(std::min(nextPulse[0], nextPulse[1]),
                    std::min(std::min(nextTriangle, nextNoise), nextSample));
                if (event > block) break;
                const unsigned delta = event - elapsed;
                cycles_ += delta; frameCycle_ += delta; samplePhase_ += delta * sampleRate;
                elapsed = event;
                bool dacEdge = false;
                for (unsigned i = 0; i < 2; ++i) if (nextPulse[i] == event) {
                    auto& pulse = pulses_[i];
                    pulse.timer = pulse.period;
                    pulse.sequence = static_cast<std::uint8_t>((pulse.sequence + 1U) & 7U);
                    pulseStamp[i] = event;
                    nextPulse[i] += 2U * (pulse.period + 1U);
                    dacEdge = true;
                }
                if (nextTriangle == event) {
                    triangle_.timer = triangle_.period;
                    if (triangleMeanEntry) {
                        // Это decrement до eligibility, не waveform edge; sequence прежний.
                        triangleMeanEntry = false; nextTriangle = never;
                    } else {
                        triangle_.sequence = static_cast<std::uint8_t>((triangle_.sequence + 1U) & 31U);
                        nextTriangle += triangle_.period + 1U;
                    }
                    triangleStamp = event;
                    dacEdge = true;
                }
                if (nextNoise == event) {
                    noise_.timer = static_cast<std::uint16_t>(noisePeriod - 1U);
                    const unsigned tap = (noise_.control & 0x80) ? 6U : 1U;
                    const unsigned feedback = (noise_.shift ^ (noise_.shift >> tap)) & 1U;
                    noise_.shift = static_cast<std::uint16_t>((noise_.shift >> 1) | (feedback << 14));
                    noiseStamp = event; nextNoise += noisePeriod;
                    dacEdge = true;
                }
                // Каналы уже достигли текущего CPU-такта, но этот DAC-переход
                // вносится ПОСЛЕ PCM, как clockTimers → emitSample → depositDelta.
                if (nextSample == event) {
                    samplePhase_ -= cpuRate_; emitSample();
                    nextSample += (cpuRate_ - samplePhase_ + sampleRate - 1U) / sampleRate;
                }
                if (dacEdge) {
                    const auto mixed = mixerOutput(triangleMean);
                    if (mixed != lastMix_) {
                        depositDelta(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(lastMix_), triangleMean);
                        lastMix_ = mixed;
                    }
                }
            }
            const unsigned tail = block - elapsed;
            cycles_ += tail; frameCycle_ += tail; samplePhase_ += tail * sampleRate;
            // Материализовать скрытые фазы к тому же точному возвратному cut.
            for (unsigned i = 0; i < 2; ++i) {
                auto& pulse = pulses_[i];
                const unsigned clocks = block - pulseStamp[i];
                const unsigned ticks = (clocks + unsigned((baseCycle + pulseStamp[i]) & 1U)) / 2U;
                if (ticks <= pulse.timer) pulse.timer = static_cast<std::uint16_t>(pulse.timer - ticks);
                else {
                    const unsigned after = ticks - pulse.timer - 1U;
                    const unsigned period = pulse.period + 1U;
                    const unsigned edges = 1U + after / period;
                    pulse.timer = static_cast<std::uint16_t>(pulse.period - after % period);
                    pulse.sequence = static_cast<std::uint8_t>((pulse.sequence + edges) & 7U);
                }
            }
            const unsigned triangleClocks = block - triangleStamp;
            if (triangleClocks <= triangle_.timer)
                triangle_.timer = static_cast<std::uint16_t>(triangle_.timer - triangleClocks);
            else {
                const unsigned after = triangleClocks - triangle_.timer - 1U;
                const unsigned period = triangle_.period + 1U;
                const unsigned edges = 1U + after / period;
                triangle_.timer = static_cast<std::uint16_t>(triangle_.period - after % period);
                if (triangleActive) triangle_.sequence = static_cast<std::uint8_t>((triangle_.sequence + edges) & 31U);
            }
            advanceNoise(noise_, block - noiseStamp, pal_);
            if (block <= dmc_.timer) dmc_.timer = static_cast<std::uint16_t>(dmc_.timer - block);
            else {
                const unsigned after = block - dmc_.timer - 1U;
                const unsigned period = dmcPeriods[pal_ ? 1 : 0][dmc_.control & 15];
                const unsigned edges = 1U + after / period;
                dmc_.timer = static_cast<std::uint16_t>(period - 1U - after % period);
                dmc_.shift = static_cast<std::uint8_t>(dmc_.shift >> std::min(edges, 8U));
                dmc_.bits = static_cast<std::uint8_t>(((unsigned(dmc_.bits) - 1U + 8U - (edges & 7U)) & 7U) + 1U);
            }
            pendingCycles_ = 0;
            count -= block;
            eventCountdown_ = static_cast<std::uint8_t>(nextEventDistance(triangleMean));
        }
        if (count) advanceCpuCycles(count, triangleMean);
        return;
    }
