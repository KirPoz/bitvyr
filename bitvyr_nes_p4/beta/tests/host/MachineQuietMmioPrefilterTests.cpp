/** @file MachineQuietMmioPrefilterTests.cpp @brief Отрицательный strict допуск: независимая MMIO/page/owner матрица. */
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
unsigned checks=0;
/** @brief Останавливает тест с независимым условием и строкой исходника. */
[[noreturn]] void fail(const char* reason,unsigned line){std::fprintf(stderr,"quiet MMIO prefilter line %u: %s\n",line,reason);std::exit(1);}
#define CHECK(expression) do{++checks;if(!(expression))fail(#expression,__LINE__);}while(false)
}

namespace nes::core {
/** @brief Вызывает pure predicate напрямую; не вводит производственные counters. */
struct CpuQuietMmioTestAccess {
    /** @brief Возвращает реальное решение helper для одного остановленного CPU. */
    static bool rejected(const Cpu6502& cpu){return cpu.rejectQuietMmioEntry();}
    /** @brief Исполняет прежний private strict entry для проверки полного отказа/продолжения. */
    static unsigned run(Cpu6502& cpu,unsigned maximum){return cpu.runQuietBlock(maximum);}
};
/** @brief Ставит реальные CPU/память для entry matrix через существующую friendship. */
struct MachineEventKernelTestAccess {
    /** @brief Получает настоящий quantum opcode/read инструкции до проверяемого strict entry. */
    static void prepare(Machine& m,std::uint16_t pc){
        m.observeCpuReads();m.cpuReadBudget_=0;m.flushPpu();m.ram_[0x100]=0xA9;m.ram_[0x101]=0;
        auto s=m.cpu_.state();s.pc=0x100;s.p=0x24;s.jammed=false;m.cpu_.setState(s);m.stepInstruction();
        s=m.cpu_.state();s.pc=pc;s.a=0x73;m.cpu_.setState(s);
        CHECK(m.cpuReadBudget_>=8U);
    }
    /** @brief Возвращает только существующий CPU владельца для exact-predicate проверки. */
    static Cpu6502& cpu(Machine& m){return m.cpu_;}
    /** @brief Сохраняет наблюдаемые поля и deferred учёт без getter materialization. */
    static std::vector<std::uint64_t> snapshot(const Machine& m){
        const auto& s=m.cpu_.state();const auto& p=m.timingPpu_;std::vector<std::uint64_t> r={s.pc,s.a,s.x,s.y,s.sp,s.p,s.cycles,s.jammed,m.cycles_,m.pendingCpuReads_,m.cpuReadBudget_,m.openBus_,m.pendingPpuDots_,m.ppuDeferBudget_,p.dot_,std::uint64_t(std::int64_t(p.scanline_)),p.frames_,p.busAddress_,p.v_,p.t_,p.status_};
        for(const auto b:m.ram_)r.push_back(b);
        return r;
    }
    /** @brief Помещает только тестовый RAM-PC; helper обязан сохранить fallback. */
    static void ramCode(Machine& m){m.ram_[0x300]=0x8D;m.ram_[0x301]=0;m.ram_[0x302]=0x20;}
};
}

namespace {
using namespace nes::core;
using Access=MachineEventKernelTestAccess;
/** @brief Создаёт собственную ROM с одинаковой инструкцией в каждой physical8KiB странице. */
std::vector<std::uint8_t> image(unsigned mapper,std::uint8_t opcode,std::uint16_t address){
    std::vector<std::uint8_t> r(16+32768+8192,0xEA);std::memset(r.data(),0,16);std::memcpy(r.data(),"NES\x1A",4);r[4]=2;r[5]=1;r[6]=std::uint8_t(mapper<<4);r[7]=std::uint8_t(mapper&0xF0U);
    for(unsigned bank=0;bank<4;++bank){const auto origin=16+bank*8192;r[origin+0x100]=opcode;r[origin+0x101]=std::uint8_t(address);r[origin+0x102]=std::uint8_t(address>>8);r[origin]=0x20;r[origin+8190]=0x8D;r[origin+8191]=0;}
    r[16+0x7FFC]=0;r[16+0x7FFD]=0x80;return r;
}
/** @brief Проверяет границы MMIO по независимой таблице адресов, а не по формуле helper. */
void matrix(){
    /**
     * @brief Независимая строка MMIO-допуска: адрес и ожидаемый отказ для store/read.
     * @details Таблица фиксирует RAM/MMIO/PRG-границы явно, не вызывает отрицательный prefilter для вычисления собственного ожидания.
     */
    struct Address {std::uint16_t value;bool storeReject,readReject;};
    constexpr Address addresses[]={{0x0000,false,false},{0x1FFF,false,false},{0x2000,true,true},{0x4015,true,true},{0x6000,true,true},{0x7FFF,true,true},{0x8000,true,false},{0xFFFF,true,false}};
    for(const auto mapper:{0U,1U,2U,3U,4U,7U})for(const auto opcode:{0x8CU,0x8DU,0x8EU,0x2CU,0xACU,0xADU,0xAEU})for(const auto item:addresses){
        const auto rom=image(mapper,std::uint8_t(opcode),item.value);auto m=std::make_unique<Machine>();CHECK(m->load(rom.data(),rom.size())==LoadResult::Ok);Access::prepare(*m,0xC100);
        const bool expected=opcode>=0x8CU&&opcode<=0x8EU?item.storeReject:item.readReject;const auto before=Access::snapshot(*m);
        CHECK(CpuQuietMmioTestAccess::rejected(Access::cpu(*m))==expected);CHECK(Access::snapshot(*m)==before);
        const auto consumed=CpuQuietMmioTestAccess::run(Access::cpu(*m),8);
        if(expected){CHECK(consumed==0);CHECK(Access::snapshot(*m)==before);}else CHECK(consumed!=0);
    }
}
/** @brief Сохраняет старый decoder на page crossing, RAM-PC и неподходящих opcode. */
void fallbacks(){
    for(const auto pc:{0x9FFEU,0xBFFEU,0xDFFEU}){
        const auto rom=image(0,0x8D,0x2000);auto m=std::make_unique<Machine>();CHECK(m->load(rom.data(),rom.size())==LoadResult::Ok);Access::prepare(*m,std::uint16_t(pc));CHECK(!CpuQuietMmioTestAccess::rejected(Access::cpu(*m)));const auto before=Access::snapshot(*m);CHECK(CpuQuietMmioTestAccess::run(Access::cpu(*m),8)==0);CHECK(Access::snapshot(*m)==before);
    }
    const auto rom=image(0,0xA9,0x2000);auto m=std::make_unique<Machine>();CHECK(m->load(rom.data(),rom.size())==LoadResult::Ok);Access::prepare(*m,0xC100);CHECK(!CpuQuietMmioTestAccess::rejected(Access::cpu(*m)));CHECK(CpuQuietMmioTestAccess::run(Access::cpu(*m),8)!=0);
    Access::prepare(*m,0x300);Access::ramCode(*m);CHECK(!CpuQuietMmioTestAccess::rejected(Access::cpu(*m)));const auto before=Access::snapshot(*m);CHECK(CpuQuietMmioTestAccess::run(Access::cpu(*m),8)==0);CHECK(Access::snapshot(*m)==before);
    Cpu6502 foreign(*m);CHECK(!CpuQuietMmioTestAccess::rejected(foreign));CHECK(CpuQuietMmioTestAccess::run(foreign,8)==0);
    auto empty=std::make_unique<Machine>();CHECK(!CpuQuietMmioTestAccess::rejected(Access::cpu(*empty)));
}
/** @brief MMC5 остаётся вне generic prefilter даже при совпавших ROM bytes и PCM mode. */
void mmc5(){
    const auto rom=image(5,0x8D,0x2000);auto digital=std::make_unique<Mmc5>();auto audio=std::make_unique<Mmc5Audio>();auto m=std::make_unique<Machine>();m->configureMmc5(digital.get(),audio.get());CHECK(m->load(rom.data(),rom.size())==LoadResult::Ok);auto s=m->cpuState();s.pc=0xC100;Access::cpu(*m).setState(s);const auto before=Access::snapshot(*m);CHECK(!CpuQuietMmioTestAccess::rejected(Access::cpu(*m)));CHECK(Access::snapshot(*m)==before);
}
}

/** @brief Проверяет pure predicate и полное отсутствие эффектов отклонённого strict entry. */
int main(){matrix();fallbacks();mmc5();std::printf("quiet MMIO prefilter passed: checks=%u\n",checks);}
