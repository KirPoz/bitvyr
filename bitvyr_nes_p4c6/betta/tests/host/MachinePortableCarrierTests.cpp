/**
 * @file MachinePortableCarrierTests.cpp
 * @brief Проверяет единый selected DAC в scalar/full APU и actualOutput/DMC.
 * @details Повторно использует только owned Output fixture существующего теста.
 * Независимый scalar эталон — отдельная обычная Machine без packet/GQ backend.
 * Portable PCM сравнивается с тем же portable scalar, raw профиль — с raw scalar.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main existing_wram_output_test_main
#include "MachineMmc5WramOutputTests.cpp"
#undef main
#pragma GCC diagnostic pop
#include <algorithm>

namespace {
bool expectPortable=false;
unsigned selectedDifferences=0;

/** @brief Проверяет фактический PRIVATE выбор без PRIVATE macro у consumer. */
void selectedProvider(){
    const auto rom=image(0);auto cartridge=std::make_unique<Cartridge>();Mmc5 digital;Mmc5Audio audio;
    cartridge->bindMmc5State(&digital,&audio);CHECK(cartridge->load(rom.data(),rom.size())==LoadResult::Ok);
    audio.cpuWrite(0x5015,3);audio.cpuWrite(0x5000,0x1F);audio.cpuWrite(0x5002,1);audio.cpuWrite(0x5003,0xF8);
    audio.cpuWrite(0x5004,0x9B);audio.cpuWrite(0x5006,2);audio.cpuWrite(0x5007,0xF8);audio.cpuWrite(0x5011,0x39);
    for(unsigned i=0;i<80;++i){
        std::array<std::uint8_t,sizeof(Mmc5Audio)> before{};std::memcpy(before.data(),&audio,sizeof(audio));
        const auto raw=audio.sample(),portable=audio.portableSample();
        CHECK(cartridge->expansionSample()==raw);
        CHECK(cartridge->selectedExpansionSample()==(expectPortable?portable:raw));
        CHECK(cartridge->selectedExpansionOutputCpuCycles()==(expectPortable?audio.nextPortableOutputCpuCycles():audio.nextOutputCpuCycles()));
        CHECK(std::memcmp(before.data(),&audio,sizeof(audio))==0);
        selectedDifferences+=raw!=portable;audio.clockCpu(1U+i%7U);
    }
    Cartridge empty;CHECK(empty.expansionSample()==0);CHECK(empty.selectedExpansionSample()==0);CHECK(empty.selectedExpansionOutputCpuCycles()==1);
    CHECK(selectedDifferences>0);
}

/** @brief Собирает собственную программу с pulse-несущей, native DMC и PCM snoop. */
std::vector<std::uint8_t> carrierImage(unsigned period,bool snoop){
    auto rom=image(0);std::fill(rom.begin()+16,rom.begin()+16+32768,0x3E);
    Program p;p.byte(0x78);p.byte(0xD8);p.set(0x4017,0x40);
    p.set(0x5114,0x80);p.set(0x5115,0x81);p.set(0x5116,0x82);p.set(0x5117,0x83);
    p.set(0x5102,2);p.set(0x5103,1);p.set(0x5113,0);
    p.set(0x5015,3);p.set(0x5000,0x1F);p.set(0x5002,period);p.set(0x5003,0xF8);
    p.set(0x5004,0x9B);p.set(0x5006,period);p.set(0x5007,0xF8);
    p.set(0x5010,snoop?1:0);p.set(0x5011,0x39);
    p.set(0x4010,0x4F);p.set(0x4012,0);p.set(0x4013,1);p.set(0x4015,0x10);
    p.set(0x2000,0);p.set(0x2001,0x18);
    const auto loop=p.pc();p.pair(0xE6,0x27);p.absolute(0xEE,0x6000);
    // Здесь нет лишнего mapper MMIO: packet Machine вправе использовать GQ,
    // пока native DMC/PPU horizons это разрешают; scalar Machine остаётся full.
    for(unsigned i=0;i<12;++i)p.byte(0xEA);
    p.absolute(0x4C,loop);CHECK(p.code.size()<0x600U);
    std::memcpy(rom.data()+16+0x6000,p.code.data(),p.code.size());
    rom[16+0x7FFA]=0;rom[16+0x7FFB]=0xE0;rom[16+0x7FFC]=0;rom[16+0x7FFD]=0xE0;rom[16+0x7FFE]=0;rom[16+0x7FFF]=0xE0;
    return rom;
}

/** @brief Сверяет один профиль с независимым scalar, включая оба DMC provider пути. */
void scalarVersusOutput(unsigned period,bool snoop){
    const auto rom=carrierImage(period,snoop);auto f=std::make_unique<Fixture>();f->init(rom);
    auto scalar=std::make_unique<Machine>();Mmc5 digital;Mmc5Audio audio;scalar->configureMmc5(&digital,&audio);CHECK(scalar->load(rom.data(),rom.size())==LoadResult::Ok);
    std::uint64_t dmc=0;
    for(unsigned frame=0;frame<8;++frame){
        f->frame(frame);scalar->setController(0,std::uint8_t(frame));CHECK(scalar->stepFrame()==RunStatus::FrameReady);
        const auto& actual=f->cut.cpu;const auto old=scalar->cpuState();
        CHECK(actual.pc==old.pc&&actual.a==old.a&&actual.x==old.x&&actual.y==old.y&&actual.sp==old.sp&&actual.p==old.p&&actual.cycles==old.cycles&&actual.jammed==old.jammed);
        CHECK(f->cut.endCycle==scalar->cpuCycles());
        CHECK(f->sound.sampleCount()==scalar->sampleCount());
        CHECK(std::memcmp(f->sound.samples(),scalar->samples(),scalar->sampleCount()*sizeof(std::int16_t))==0);
        CHECK(f->expansion.sample()==audio.sample()); // Raw digital phase сохраняется в portable.
        CHECK(f->expansion.cpuRead(0x5015,0)==audio.cpuRead(0x5015,0));
        for(unsigned i=0;i<2048;++i)CHECK(f->ram[frame&1U][i]==scalar->debugRead(std::uint16_t(i)));
        for(unsigned i=0;i<f->retired.header().dmcCount;++i){const auto&e=f->retired.dmc()[i];CHECK(e.value==0x3E);CHECK(e.stall>0);}
        dmc+=f->retired.header().dmcCount;CHECK(f->packets[frame&1U].release());
    }
    CHECK(dmc>0);CHECK(f->machine.cancelOutputFrames(f->lease));
}
}

/** @brief Проверяет actual feature activation, mixed provider и scalar/packet PCM. */
int main(int argc,char** argv){
    CHECK(argc==3&&std::strcmp(argv[1],"--portable-carrier")==0);CHECK(std::strcmp(argv[2],"0")==0||std::strcmp(argv[2],"1")==0);expectPortable=argv[2][0]=='1';
    selectedProvider();for(const unsigned period:{0U,1U,2U,15U})for(const bool snoop:{false,true})scalarVersusOutput(period,snoop);
    std::printf("MACHINE_PORTABLE_CARRIER_PASS expected=%u selected_differences=%u checks=%u\n",unsigned(expectPortable),selectedDifferences,checks);
}
