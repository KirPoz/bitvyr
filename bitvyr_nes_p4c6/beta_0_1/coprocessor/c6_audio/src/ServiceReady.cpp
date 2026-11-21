#include "ServiceReady.h"

namespace nes::c6audio {
bool renderReady(const std::uint8_t* bytes,std::size_t count,const wire::Expected& expected,
    const wire::Destination& storage,core::AudioRenderer& renderer,BorrowedResult& result) noexcept {
    result={};
    if(!wire::decode(bytes,count,expected,storage))return false;
    const auto& header=*storage.header;
    const core::AudioPacketView view{&header,storage.input,header.inputCount,storage.dmc,header.dmcCount,
        storage.expansion,header.expansionCount};
    const auto status=renderer.render(view);
    result.status=status;
    result.generation=header.generation;result.sequence=header.sequence;result.ppuFrame=header.ppuFrame;
    result.baseCycle=header.baseCycle;result.endCycle=header.endCycle;result.region=header.region;
    if(status!=core::AudioRenderResult::Ok)return false;
    result.pcm=renderer.samples();result.samples=renderer.sampleCount();return true;
}
} // namespace nes::c6audio
