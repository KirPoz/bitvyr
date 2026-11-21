/** @file ApuExpansion.cpp @brief Полноширинный FIR для больших расширенных DAC-переходов. */
#include "Apu.h"
namespace nes::core {
void Apu::depositWideDelta(std::int32_t delta,unsigned phase) noexcept {
    const auto* coefficient=tables_.impulse[phase].data();
    auto* pending=pendingImpulse_.data()+impulsePosition_;
    for(unsigned j=0;j<impulseSize;++j)pending[j]+=static_cast<std::int64_t>(delta)*coefficient[j];
}
}
