/** @file MapperExpansionWramQuietTests.cpp
 * @brief Проверяет call-local WRAM69/85 на настоящем quiet budget и независимых scalar M2.
 * @details Никаких future credits: leading NOP получает бюджет самой Machine;
 * reference отключает defer и исполняет каждую6502 инструкцию обычным Cpu6502::step.
 * Полный Frame CPU/RAM/RGB/PCM oracle дополнительно остаётся в Fastcap fixtures.
 */
#include "Machine.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
namespace {
using namespace nes::core;
unsigned checks=0,cases=0,positive=0,negative=0;
/** @brief Показывает первое нарушение, не исправляя память или эталон. */
void check(bool okay,const char* text,unsigned line){++checks;if(!okay){std::fprintf(stderr,"WRAM_QUIET line%u case%u: %s\n",line,cases,text);std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
/** @brief Отдельные режимы настоящего mapper gate и размера NES2 RAM. */
enum class Gate {Enabled,Disabled,Rom,RomLast,NoRam,ShortRam};
/** @brief Одна исходная инструкция либо короткий цикл; expected не является runtime разрешением. */
struct Case {const char*name;std::vector<std::uint8_t> body;unsigned a=0x5a,x=0,y=0,pointer=0x6000;bool eligible=true,loop=false,crossCode=false;Gate gate=Gate::Enabled;};
/** @brief Создаёт собственную ROM с постоянным E000 кодом, distinct PRG и штатными vectors. */
std::vector<std::uint8_t> image(unsigned mapper,const Case&c){
 constexpr unsigned prg=65536,fixed=16+prg-8192;std::vector<std::uint8_t>r(16+prg+8192,0);std::memcpy(r.data(),"NES\x1a",4);r[4]=4;r[5]=1;r[6]=std::uint8_t(mapper<<4);r[7]=std::uint8_t(mapper&0xf0U);
 if(c.gate==Gate::NoRam||c.gate==Gate::ShortRam){r[7]|=8;r[8]=mapper==85?0x20:0;r[10]=c.gate==Gate::ShortRam?5:0;}
 for(unsigned i=0;i<prg;++i)r[16+i]=std::uint8_t(0x10+i/8192);
 std::vector<std::uint8_t>code{0xea};code.insert(code.end(),c.body.begin(),c.body.end());if(!c.loop)code.insert(code.end(),{0xea,0xea,0xea,0xea,0xad,0x02,0x20});CHECK(code.size()<128);std::memcpy(r.data()+fixed,code.data(),code.size());
 if(c.crossCode){constexpr std::uint8_t follow[]{0x5a,0xea,0xea,0xea,0xea,0xad,0x02,0x20};std::memcpy(r.data()+16,follow,sizeof(follow));}
 r[fixed+0x100]=0x40;for(unsigned v:{0x1ffaU,0x1ffcU,0x1ffeU}){r[fixed+v]=0;r[fixed+v+1]=v==0x1ffc?0xe0:0xe1;}return r;
}
}
namespace nes::core {
/** @brief Использует уже существующую CPU friendship; не добавляет product instrumentation. */
struct CpuQuietMmioTestAccess {
 /** @brief Передаёт только настоящий уже выданный budget, не создавая нового горизонта. */
 static unsigned block(Cpu6502&cpu,unsigned maximum){return cpu.runQuietBlock(maximum);}
 /** @brief Сохраняет наблюдаемые interrupt polling latches без clock/getter side effects. */
 static std::array<bool,6> irq(const Cpu6502&cpu){return {cpu.irqLine_,cpu.irqPending_,cpu.nmiPending_,cpu.nmiReady_,cpu.irqAtLastCycleStart_,cpu.nmiAtLastCycleStart_};}
};
/** @brief Доступ к завершённым native владельцам; открытая шина и долги не скрываются getter-барьером. */
struct MachineEventKernelTestAccess {
 /** @brief Выбирает независимый обычный full PPU/APU путь reference без deferred CPU clocks. */
 static void scalar(Machine&m){m.observeCpuReads();m.flushPpu();m.cpuReadBudget_=0;m.deferPpuAllowed_=false;}
 /** @brief Читает логические уже исполненные M2 без их повторного исполнения. */
 static std::uint64_t logical(const Machine&m){return m.cycles_+m.pendingCpuReads_;}
 /** @brief Возвращает реальный остаток budget, сформированный первой NOP. */
 static unsigned budget(const Machine&m){return m.cpuReadBudget_;}
 /** @brief Исполняет generic decoder выбранного primary CPU, не подменяя mapper policy. */
 static unsigned block(Machine&m,unsigned maximum=255){return CpuQuietMmioTestAccess::block(m.cpu_,maximum);}
 /** @brief Материализует только завершённые clocks до канонического сравнения. */
 static void barrier(Machine&m){m.observeCpuReads();m.flushPpu();m.cpuReadBudget_=0;}
 /** @brief Настраивает исходный synthetic mapper/память до первой гостевой NOP, без save-state в продукте. */
 static void prepare(Machine&m,unsigned mapper,const Case&c){
  for(unsigned i=0;i<8192;++i)m.cartridge_.ram()[i]=std::uint8_t(0x31+(i&255U)+17U*(i>>11U));
  m.ram_[0]=0x47;m.ram_[1]=0x92;m.ram_[0x20]=std::uint8_t(c.pointer);m.ram_[0x21]=std::uint8_t(c.pointer>>8);
  const auto value=c.gate==Gate::Disabled?0x40U:c.gate==Gate::Rom?1U:c.gate==Gate::RomLast?0x3fU:0xc0U;
  if(mapper==69){m.cartridge_.cpuWrite(0x8000,8,0);m.cartridge_.cpuWrite(0xa000,std::uint8_t(value),0);}else m.cartridge_.cpuWrite(0xe000,c.gate==Gate::Disabled?0:0x80,0);
  if(c.crossCode){m.cartridge_.ram()[0x1ffe]=0xea;m.cartridge_.ram()[0x1fff]=0xa9;}
  auto state=m.cpu_.state();state.pc=c.crossCode?0x7ffe:0xe000;state.a=std::uint8_t(c.a);state.x=std::uint8_t(c.x);state.y=std::uint8_t(c.y);state.p=0x24;m.cpu_.setState(state);
 }
 /** @brief Применяет непосредственный setup gate только к остановленному fixture, не во время grant. */
 static void gate(Machine&m,unsigned mapper,bool enabled){barrier(m);if(mapper==69){m.cartridge_.cpuWrite(0x8000,8,0);m.cartridge_.cpuWrite(0xa000,enabled?0xc0:0x40,0);}else m.cartridge_.cpuWrite(0xe000,enabled?0x80:0,0);}
 /** @brief Ставит собственную RAM программу на текущем реальном cut до новой выдачи credits. */
 static void program(Machine&m,const std::vector<std::uint8_t>&code){barrier(m);CHECK(code.size()<256);std::copy(code.begin(),code.end(),m.ram_.begin()+0x200);auto state=m.cpu_.state();state.pc=0x200;state.p=0x24;m.cpu_.setState(state);}
 /** @brief Задаёт настоящие digital IRQ регистры исходного synthetic cut. */
 static void unmask(Machine&m){auto state=m.cpu_.state();state.p=std::uint8_t(state.p&~4U);m.cpu_.setState(state);}
 /** @brief Подводит mapper 69 или 85 к ближайшему IRQ перед проверкой горизонта исполнения. */
 static void irqNear(Machine&m,unsigned mapper){barrier(m);if(mapper==69){m.cartridge_.cpuWrite(0x8000,14,0);m.cartridge_.cpuWrite(0xa000,2,0);m.cartridge_.cpuWrite(0x8000,15,0);m.cartridge_.cpuWrite(0xa000,0,0);m.cartridge_.cpuWrite(0x8000,13,0);m.cartridge_.cpuWrite(0xa000,0x81,0);}else{m.cartridge_.cpuWrite(0xe010,0xfd,0);m.cartridge_.cpuWrite(0xf000,6,0);}}
 /** @brief Сохраняет raw CPU/RAM/bus tuple до чистого отрицательного admission без flush. */
 static std::vector<std::uint64_t> raw(const Machine&m){const auto&s=m.cpu_.state();std::vector<std::uint64_t>v{s.pc,s.a,s.x,s.y,s.sp,s.p,s.cycles,std::uint64_t(s.jammed),logical(m),m.pendingCpuReads_,m.cpuReadBudget_,m.openBus_};for(auto x:CpuQuietMmioTestAccess::irq(m.cpu_))v.push_back(x);for(auto x:m.ram_)v.push_back(x);for(unsigned i=0;i<8192;++i)v.push_back(m.cartridge_.ram()[i]);return v;}
 /** @brief Сравнивает полные архитектурные данные, открытый bus, IRQ, аудио и все8KiB WRAM. */
 static void same(Machine&a,Machine&b){
  barrier(a);barrier(b);const auto&x=a.cpu_.state();const auto&y=b.cpu_.state();CHECK(x.pc==y.pc&&x.a==y.a&&x.x==y.x&&x.y==y.y&&x.sp==y.sp&&x.p==y.p&&x.cycles==y.cycles&&x.jammed==y.jammed);
  CHECK(logical(a)==logical(b));CHECK(a.openBus_==b.openBus_);CHECK(CpuQuietMmioTestAccess::irq(a.cpu_)==CpuQuietMmioTestAccess::irq(b.cpu_));CHECK(a.ram_==b.ram_);CHECK(std::memcmp(a.cartridge_.ram(),b.cartridge_.ram(),8192)==0);CHECK(a.cartridge_.irqLine()==b.cartridge_.irqLine());CHECK(a.audioIrqLine()==b.audioIrqLine());CHECK(a.cartridge_.selectedExpansionSample()==b.cartridge_.selectedExpansionSample());CHECK(a.cartridge_.selectedExpansionOutputCpuCycles()==b.cartridge_.selectedExpansionOutputCpuCycles());CHECK(a.sampleCount()==b.sampleCount());CHECK(std::memcmp(a.samples(),b.samples(),a.sampleCount()*sizeof(std::int16_t))==0);
 }
 /** @brief Читает уже остановленный физический WRAM и внутренний RAM sentinel. */
 static unsigned ram(const Machine&m,unsigned address){return address>=0x6000?m.cartridge_.ram()[address-0x6000]:m.ram_[address&2047U];}
};
}
namespace {
using Access=MachineEventKernelTestAccess;
/** @brief Владеет одной ROM и двумя независимыми Machine до конца сравнения. */
struct Pair {
 std::vector<std::uint8_t>rom;std::unique_ptr<Machine>fast=std::make_unique<Machine>(),reference=std::make_unique<Machine>();
 /** @brief ПодключаетTiming только fast владельцу; reference сохраняет full PPU и собственные M2. */
 Pair(unsigned mapper,const Case&c,Region region):rom(image(mapper,c)){CHECK(fast->load(rom.data(),rom.size(),region)==LoadResult::Ok);CHECK(reference->load(rom.data(),rom.size(),region)==LoadResult::Ok);CHECK(fast->configureObservableVideo(true));Access::scalar(*reference);Access::prepare(*fast,mapper,c);Access::prepare(*reference,mapper,c);}
 /** @brief Исполняет один настоящий opcode на каждой стороне. */
 void step(){fast->stepInstruction();reference->stepInstruction();}
 /** @brief Догоняет независимый scalar только полными инструкциями до реального результата quiet. */
 void catchup(){const auto target=Access::logical(*fast);unsigned guard=0;while(Access::logical(*reference)<target){reference->stepInstruction();CHECK(++guard<1000);}CHECK(Access::logical(*reference)==target);}
};
/** @brief Проверяет один реальный read/write/RMW либо чистый отказ и scalar continuation. */
void run(unsigned mapper,const Case&c,Region region=Region::Ntsc){++cases;if(c.gate==Gate::ShortRam){const auto r=image(mapper,c);Machine rejected;CHECK(rejected.load(r.data(),r.size(),region)==LoadResult::UnsupportedFormat);CHECK(!rejected.cartridge().loaded());CHECK(!const_cast<Cartridge&>(rejected.cartridge()).quietWritableWram6000().data);++negative;return;}Pair pair(mapper,c,region);const bool full=c.gate!=Gate::NoRam&&c.gate!=Gate::ShortRam&&c.gate!=Gate::Disabled&&c.gate!=Gate::Rom&&c.gate!=Gate::RomLast;CHECK(bool(pair.fast->cartridge().loaded()));
 auto capability=const_cast<Cartridge&>(pair.fast->cartridge()).quietWritableWram6000();CHECK(bool(capability.data)==full);CHECK(!Cartridge::QuietWram6000::contains(0x5fff));CHECK(Cartridge::QuietWram6000::contains(0x6000));CHECK(Cartridge::QuietWram6000::contains(0x7fff));CHECK(!Cartridge::QuietWram6000::contains(0x8000));
 pair.step();const auto initial=Access::raw(*pair.fast);const auto budget=Access::budget(*pair.fast);CHECK(budget>=8);const auto changed=Access::block(*pair.fast);
 if(c.eligible){CHECK(changed>0);CHECK(changed<=budget);++positive;}else{CHECK(changed==0);CHECK(Access::raw(*pair.fast)==initial);++negative;}
 pair.catchup();Access::same(*pair.fast,*pair.reference);CHECK(Access::ram(*pair.fast,0)==0x47);CHECK(Access::ram(*pair.fast,1)==0x92);
 if(c.loop)CHECK(Access::ram(*pair.fast,0x6000)>0x31);
 pair.step();Access::same(*pair.fast,*pair.reference);
}
/** @brief Меняет enable настоящим CPU STA между grants: прежний указатель не является постоянным разрешением. */
void transition(unsigned mapper){++cases;Case c{"transition",{0xad,0,0x60,0xa9,std::uint8_t(mapper==69?0x40:0),0x8d,std::uint8_t(mapper==69?0:0),std::uint8_t(mapper==69?0xa0:0xe0),0xad,0,0x60}};Pair pair(mapper,c,Region::Ntsc);pair.step();CHECK(Access::block(*pair.fast)>0);pair.catchup();Access::same(*pair.fast,*pair.reference);pair.step();Access::same(*pair.fast,*pair.reference);CHECK(!const_cast<Cartridge&>(pair.fast->cartridge()).quietWritableWram6000().data);
 // Новая NOP выдаёт только допустимые реальным устройством credits; disabled-read обязан остаться scalar.
 const std::vector<std::uint8_t>code{0xea,0xad,0,0x60,0xea,0xea,0xea,0xea,0xad,2,0x20};Access::program(*pair.fast,code);Access::program(*pair.reference,code);pair.step();const auto before=Access::raw(*pair.fast);CHECK(Access::block(*pair.fast)==0);CHECK(before==Access::raw(*pair.fast));pair.step();Access::same(*pair.fast,*pair.reference);
 Access::gate(*pair.fast,mapper,true);Access::gate(*pair.reference,mapper,true);Access::program(*pair.fast,code);Access::program(*pair.reference,code);pair.step();CHECK(Access::block(*pair.fast)>0);pair.catchup();Access::same(*pair.fast,*pair.reference);
}
/** @brief Проверяет IRQ-front0, реальные OAM DMA и DMC перед следующим grant без искусственного расширения horizon. */
void events(unsigned mapper){++cases;Case c{"events",{0xad,0,0x60,0x8d,1,0x60,0xee,0,0x60}};Pair pair(mapper,c,Region::Ntsc);Access::irqNear(*pair.fast,mapper);Access::irqNear(*pair.reference,mapper);Access::unmask(*pair.fast);Access::unmask(*pair.reference);pair.step();const auto before=Access::raw(*pair.fast);CHECK(Access::block(*pair.fast)==0);CHECK(before==Access::raw(*pair.fast));pair.step();Access::same(*pair.fast,*pair.reference);CHECK(pair.fast->cartridge().irqLine());const auto pending=Access::raw(*pair.fast);CHECK(Access::block(*pair.fast)==0);CHECK(pending==Access::raw(*pair.fast));pair.step();Access::same(*pair.fast,*pair.reference);
 // Выключение IRQ — явный setup нового проверяемого cut, одинаковый для обеих сторон.
 Case fresh{"DMC/DMA",{0xad,0,0x60}};Pair io(mapper,fresh,Region::Ntsc);
 const std::vector<std::uint8_t>program{0xa9,0x4f,0x8d,0x10,0x40,0xa9,0,0x8d,0x12,0x40,0xa9,1,0x8d,0x13,0x40,0xa9,0x10,0x8d,0x15,0x40,0xa9,2,0x8d,0x14,0x40,0xea,0xad,0,0x60,0x8d,1,0x60,0xee,0,0x60,0xea,0xea,0xea,0xea,0xad,2,0x20};
 Access::program(*io.fast,program);Access::program(*io.reference,program);
 for(unsigned instruction=0;instruction<10;++instruction){io.step();Access::same(*io.fast,*io.reference);}
 io.step(); // First NOP drains OAM/DMC and obtains real credits; do not erase its budget before admission.
 const auto b=Access::budget(*io.fast);CHECK(b>=8);const auto used=Access::block(*io.fast);CHECK(used>0&&used<=b);io.catchup();Access::same(*io.fast,*io.reference);io.step();Access::same(*io.fast,*io.reference);
}
}
/** @brief Исполняет оба mapper/оба CPU региона и негативные memory/IRQ/DMA cut cases. */
int main(){
 for(unsigned mapper:{69U,85U})for(Region region:{Region::Ntsc,Region::Pal}){
  // Cold RRA/ISC сохраняют полноценное исполнение через scalar: quiet обязан
  // отказать без side effects, затем pair.step сравнивает реальные M2/RAM/IRQ/PCM.
  for(unsigned address:{0x6000U,0x67ffU,0x6800U,0x6fffU,0x7000U,0x77ffU,0x7800U,0x7fffU})for(unsigned op:{0xadU,0x8dU,0xeeU,0x0eU,0x6fU,0xefU}){
   const bool eligible=op!=0x6fU&&op!=0xefU;
   run(mapper,{"offset",{std::uint8_t(op),std::uint8_t(address),std::uint8_t(address>>8)},0x5a,0,0,0x6000,eligible},region);
  }
  run(mapper,{"indexed samepage",{0xbd,0xf0,0x60},0x5a,15},region);run(mapper,{"indexed crossed",{0xbd,0xf0,0x60},0x5a,16},region);run(mapper,{"WRAM-ROM indexed read",{0xbd,0xff,0x7f},0x5a,1},region);
  run(mapper,{"indirect crossed",{0xb1,0x20},0x5a,0,16,0x60f0},region);run(mapper,{"indirect store",{0x91,0x20},0x5a,0,16,0x60f0},region);run(mapper,{"indexed indirect read",{0xa1,0x20},0x5a,0,0,0x6800},region);run(mapper,{"indexed indirect store",{0x81,0x20},0x5a,0,0,0x6800},region);
  run(mapper,{"5F dummy LDA",{0xbd,0xff,0x5f},0x5a,1,0,0x6000,false},region);run(mapper,{"5F dummy STA",{0x9d,0xff,0x5f},0x5a,1,0,0x6000,false},region);run(mapper,{"5F dummy ASL",{0x1e,0xff,0x5f},0x5a,1,0,0x6000,false},region);run(mapper,{"8000 store",{0x9d,0xff,0x7f},0x5a,1,0,0x6000,false},region);
  run(mapper,{"5F direct",{0xad,0xff,0x5f},0x5a,0,0,0x6000,false},region);run(mapper,{"Repeat external writes",{0xee,0,0x60,0x4c,1,0xe0},0x5a,0,0,0x6000,true,true},region);
  run(mapper,{"operand WRAM-ROM crossing",{},0x5a,0,0,0x6000,true,false,true},region);
  for(Gate gate:{Gate::Disabled,Gate::NoRam,Gate::ShortRam})for(unsigned op:{0xadU,0x8dU,0xeeU})run(mapper,{"closed RAM",{std::uint8_t(op),0,0x60},0x5a,0,0,0x6000,false,false,false,gate},region);
  if(mapper==69)for(Gate gate:{Gate::Rom,Gate::RomLast})for(unsigned op:{0xadU,0x8dU,0xeeU})run(mapper,{"ROM window",{std::uint8_t(op),0,0x60},0x5a,0,0,0x6000,false,false,false,gate},region);
 }
 for(unsigned mapper:{69U,85U}){transition(mapper);events(mapper);}
 std::printf("WRAM_QUIET cases=%u positive=%u negative=%u checks=%u full8192RamCompared eightBoundaryOffsets no2Kalias scalarCPU_RAM_bus_IRQ_PCM=exact\n",cases,positive,negative,checks);
}