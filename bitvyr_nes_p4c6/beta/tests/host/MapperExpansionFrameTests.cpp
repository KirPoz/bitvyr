/** @file MapperExpansionFrameTests.cpp
 * @brief Проверяет Machine, sealed AudioPacket, PCM и Output capability новых mapper69/85.
 * @details Собственные6502 ROM; полный PPU/APU служит независимым scalar reference.
 * C6 codec подключается явно BITVYR_TEST_C6_AUDIO_WIRE, без SDK и COM.
 */
#include "Machine.h"
#include "Palette.h"
#include "apu/AudioRenderer.h"
#include "ppu/VideoRenderer.h"
#if defined(BITVYR_TEST_C6_AUDIO_WIRE) && BITVYR_TEST_C6_AUDIO_WIRE
#include "audio/c6/protocol/AudioPacketWire.h"
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using namespace nes::core;
/** @brief Завершает fixture при первом нарушении без исправления packet или PCM. */
void check(bool okay,const char* expression,unsigned line) {
    if(!okay){std::fprintf(stderr,"Mapper expansion Frame line%u: %s\n",line,expression);std::exit(1);}
}
#define CHECK(x) check(bool(x),#x,__LINE__)
/** @brief Владеет собственным кодом постоянного последнего окна8KiB по адресу$E000. */
struct Program {
    std::vector<std::uint8_t> bytes;
    /** @brief Добавляет настоящую пару LDA-immediate и STA-absolute для MMIO. */
    void set(unsigned address,unsigned value){bytes.insert(bytes.end(),{0xa9,std::uint8_t(value),0x8d,std::uint8_t(address),std::uint8_t(address>>8)});}
    /** @brief Записывает PSG/FM register через два реальных порта выбранного mapper. */
    void sound(unsigned mapper,unsigned index,unsigned value){set(mapper==69?0xc000:0x9010,index);set(mapper==69?0xe000:0x9030,value);}
};
/** @brief Создаёт ROM без native audio: ненулевой PCM обязан поступить от expansion provider. */
std::vector<std::uint8_t> image(unsigned mapper) {
    constexpr unsigned prgBytes=32768;
    std::vector<std::uint8_t> rom(16+prgBytes+8192,0);
    std::memcpy(rom.data(),"NES\x1a",4);rom[4]=2;rom[5]=1;
    rom[6]=std::uint8_t(mapper<<4);rom[7]=std::uint8_t(mapper&0xf0U);
    Program p;p.bytes={0x78,0xd8};p.set(0x4017,0x40);p.set(0x4015,0);
    if(mapper==69){
        p.sound(mapper,0,32);p.sound(mapper,1,0);
        p.sound(mapper,7,0x3e);p.sound(mapper,8,15);
    }else{
        // Собственный быстрый custom patch: AR15 даёт ненулевой первый64-M2 блок.
        constexpr std::uint8_t patch[]{0x21,0x21,0x3f,0,0xf0,0xf0,0x0f,0x0f};
        for(unsigned i=0;i<8;++i)p.sound(mapper,i,patch[i]);
        p.sound(mapper,0x30,0);p.sound(mapper,0x10,0x20);p.sound(mapper,0x20,0x19);
    }
    // Реальные mapper-записи включают WRAM; ROM не полагается на приватный setup поля.
    if(mapper==69){p.set(0x8000,8);p.set(0xa000,0xc0);}else p.set(0xe000,0x80);
    p.set(0x6000,0x6d);
    // Разноцветный фон делает проверку видеобарьеров содержательной после первого кадра.
    p.set(0x2006,0x3f);p.set(0x2006,0);
    for(unsigned color:{0x0fU,0x16U,0x2aU,0x30U})p.set(0x2007,color);
    p.set(0x2000,0);p.set(0x2005,0);p.set(0x2005,0);p.set(0x2001,0x0a);
    // $5FFF и dummy-read индексного перехода в $6000 обязаны сохранить open bus,
    // настоящие M2 и IRQ. Полный scalar PPU/APU не использует owned Frame fast path.
    const unsigned loop=0xe000U+unsigned(p.bytes.size());
    p.bytes.insert(p.bytes.end(),{0xa9,0,0x8d,0x14,0x40,0xe6,0x00,0xa5,0x00,0x49,0x5a,0x85,0x01,
        0xad,0xff,0x5f,0x85,0x02,0xa2,0x01,0xbd,0xff,0x5f,0x85,0x03,0xee,0x00,0x60,
        0xad,0x02,0x20,0x4c,std::uint8_t(loop),std::uint8_t(loop>>8)});
    CHECK(p.bytes.size()<0x1000);
    for(unsigned i=0;i<8192;++i)rom[16+prgBytes+i]=std::uint8_t((i&8U)?0x55U:0xaaU);
    constexpr unsigned fixed=16+prgBytes-8192;
    std::memcpy(rom.data()+fixed,p.bytes.data(),p.bytes.size());
    for(unsigned vector:{0x1ffaU,0x1ffcU,0x1ffeU}){rom[fixed+vector]=0;rom[fixed+vector+1]=0xe0;}
    return rom;
}
/** @brief Владеет двумя Frame слотами и одним C0/PCM consumer до настоящего cancel. */
struct FrameFixture {
    std::array<VideoPacket,2> packets;
    std::array<std::array<std::uint8_t,10240>,2> ram{};
    AudioPacket active,retired;AudioRenderer renderer;
    VideoStorage storage;VideoRenderer video{storage};
    std::array<std::uint16_t,256*240> rgb{};
    OutputFrameBinding binding{};OutputFrameLease lease{};OutputFrameTicket ticket{};OutputFrameCut cut{};
    const VideoPacket* sealed=nullptr;
    Machine machine;
    /** @brief Проверяет actual Output API и expansion capability нового mapper до первого frame. */
    void init(const std::vector<std::uint8_t>& rom,Region region) {
        CHECK(machine.load(rom.data(),rom.size(),region)==LoadResult::Ok);
        machine.configureSplitAudio(&active,true);
        CHECK(machine.splitAudio());CHECK(active.header().capabilities==AudioPacket::externalDacQ16);
        binding.source={&machine,rom.data(),rom.size(),machine.colourTransportEpoch(),1,0,region};
        binding.packets={&packets[0],&packets[1]};binding.snapshots={ram[0].data(),ram[1].data()};
        binding.protectedSpans[0]={&active,sizeof(active)};binding.protectedSpans[1]={&retired,sizeof(retired)};
        binding.protectedSpans[2]={rgb.data(),sizeof(rgb)};binding.protectedSpans[3]={&storage,sizeof(storage)};
        binding.protectedSpans[4]={&video,sizeof(video)};binding.protectedSpans[5]={&renderer,sizeof(renderer)};
        CHECK(machine.configureOutputFrames(binding,lease));
        CHECK(machine.outputFramesEnabled());video.setRgb565Output(rgb.data());
    }
    /** @brief Исполняет и запечатывает frame; consumer работает только после возврата производителя. */
    void frame(unsigned sequence) {
        machine.setController(0,std::uint8_t(sequence));
        ticket={lease,sequence,sequence,machine.cpuCycles(),sequence&1U};
        CHECK(machine.beginOutputFrame(ticket));CHECK(machine.stepFrame()==RunStatus::FrameReady);
        CHECK(machine.sealOutputFrame(ticket,sealed,cut));
        CHECK(machine.copyOutputVisibleRam(ticket,ram[sequence&1U].data(),10240));
        CHECK(machine.retireAudioPacket(retired));
        CHECK(video.render(*sealed)==VideoRenderResult::Ok);
        CHECK(renderer.render(retired)==AudioRenderResult::Ok);
    }
};
/** @brief Сравнивает поля CPU без padding; RAM и PCM сверяются отдельно. */
void sameCpu(const Cpu6502::State&a,const Cpu6502::State&b) {
    CHECK(a.pc==b.pc&&a.a==b.a&&a.x==b.x&&a.y==b.y&&a.sp==b.sp&&a.p==b.p);
    CHECK(a.cycles==b.cycles&&a.jammed==b.jammed);
}
/** @brief Проверяет64-M2 события и настоящий C6 codec в ограниченном8KiB буфере. */
std::size_t boundedPacket(const AudioPacket& packet,unsigned mapper) {
    const auto& h=packet.header();CHECK(h.fault==AudioFault::None);
    CHECK(h.formatVersion==AudioPacket::expansionVersion&&h.capabilities==AudioPacket::externalDacQ16);
    CHECK(h.endCycle>=h.baseCycle);const auto clocks=h.endCycle-h.baseCycle;
    CHECK(h.expansionCount>0&&h.expansionCount<=(clocks+63U)/64U+2U);
    bool nonzero=false;unsigned previous=0;
    for(unsigned i=0;i<h.expansionCount;++i){const auto&e=packet.expansions()[i];
        CHECK(e.offset<=clocks);CHECK(!i||e.offset>=previous+64U);previous=e.offset;
        CHECK(e.level<=0&&e.level>=(mapper==69?-36000:-49152));nonzero|=e.level!=0;
    }
    CHECK(nonzero);
#if defined(BITVYR_TEST_C6_AUDIO_WIRE) && BITVYR_TEST_C6_AUDIO_WIRE
    const auto bytes=nes::c6audio::wire::encodedSize(h);CHECK(bytes>80&&bytes<=8192);
    std::array<std::uint8_t,8192> encoded{};std::size_t written=0;
    CHECK(nes::c6audio::wire::encode(AudioPacketView::from(packet),encoded.data(),encoded.size(),written));
    CHECK(written==bytes);return written;
#else
    return 0; // Локальный P4 проверяет DSP; C6 codec подключается только в своей сборке.
#endif
}
/** @brief Сверяет8 кадров Output с независимым полным PPU/APU на том же ROM. */
void run(unsigned mapper,Region region) {
    const auto rom=image(mapper);auto frame=std::make_unique<FrameFixture>();frame->init(rom,region);
    auto scalar=std::make_unique<Machine>();CHECK(scalar->load(rom.data(),rom.size(),region)==LoadResult::Ok);
    bool pcmNonzero=false;std::size_t maximumWire=0;
    for(unsigned n=0;n<8;++n){
        frame->frame(n);scalar->setController(0,std::uint8_t(n));CHECK(scalar->stepFrame()==RunStatus::FrameReady);
        sameCpu(frame->cut.cpu,scalar->cpuState());CHECK(frame->cut.endCycle==scalar->cpuCycles());
        CHECK(frame->retired.header().endCycle==frame->cut.endCycle);
        CHECK(frame->renderer.sampleCount()==scalar->sampleCount());CHECK(frame->renderer.sampleCount()>0);
        CHECK(std::memcmp(frame->renderer.samples(),scalar->samples(),scalar->sampleCount()*sizeof(std::int16_t))==0);
        for(std::size_t i=0;i<scalar->sampleCount();++i)pcmNonzero|=scalar->samples()[i]!=0;
        for(unsigned i=0;i<2048;++i)CHECK(frame->ram[n&1U][i]==scalar->debugRead(std::uint16_t(i)));
        for(unsigned i=0;i<8192;++i)CHECK(frame->ram[n&1U][2048+i]==scalar->debugRead(std::uint16_t(0x6000+i)));
        const auto* expected=scalar->framebuffer();const auto* actual=frame->video.framebuffer();
        CHECK(expected&&actual);bool nonuniform=false;
        for(unsigned y=0;y<240;++y)for(unsigned x=0;x<256;++x){
            const auto i=y*256+x;CHECK(actual[i]==expected[i]);
            CHECK(frame->rgb[i]==displayRgb565(expected[i],scalar->emphasis(y),region==Region::Pal));
            nonuniform|=expected[i]!=expected[0];
        }
        if(n>0)CHECK(nonuniform);
        CHECK(frame->machine.cartridge().irqLine()==scalar->cartridge().irqLine());
        CHECK(frame->machine.cartridge().selectedExpansionSample()==scalar->cartridge().selectedExpansionSample());
        CHECK(frame->machine.cartridge().selectedExpansionOutputCpuCycles()==scalar->cartridge().selectedExpansionOutputCpuCycles());
        const auto bytes=boundedPacket(frame->retired,mapper);if(bytes>maximumWire)maximumWire=bytes;
        CHECK(frame->packets[n&1U].release());
    }
    CHECK(pcmNonzero);CHECK(frame->machine.cancelOutputFrames(frame->lease));
    std::printf("Mapper%u region%u Frame/nonzero expansion/CPU/RAM/indexRGB/PCM passed8frames; actual_C6_wire_max=%zu (0=not_linked)\n",mapper,unsigned(region),maximumWire);
}
} // namespace
/** @brief Проверяет оба mapper; отсутствие успешного Output capability является ошибкой. */
int main(){for(auto region:{Region::Ntsc,Region::Pal}){run(69,region);run(85,region);}}
