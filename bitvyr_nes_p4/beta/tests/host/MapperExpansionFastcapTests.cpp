/** @file MapperExpansionFastcapTests.cpp
 * @brief Проверяет новые PRG-RAM/BG-span/Plain-sprite допуски69/85 настоящими гостевыми операциями.
 * @details Полный PPU/APU и отдельный scalar executable дают независимые oracle;
 * directed sprite packet доказывает реальный bulk slot по SpriteTile вместо трёх scalar ops.
 */
#include "Machine.h"
#include "Palette.h"
#include "apu/AudioRenderer.h"
#include "ppu/VideoRenderer.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#ifndef FASTCAP_SCALAR
#define FASTCAP_SCALAR 0
#endif
namespace {
using namespace nes::core;
unsigned checks=0;std::FILE* canonical=nullptr;
/** @brief Останавливает тест при первом наблюдаемом расхождении. */
void check(bool okay,const char* text,unsigned line){++checks;if(!okay){std::fprintf(stderr,"FASTCAP line%u: %s\n",line,text);std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
/** @brief Сериализует явное поле little-endian без padding и адресов C++ объектов. */
void number(std::uint64_t v,unsigned bytes){if(canonical)for(unsigned i=0;i<bytes;++i)CHECK(std::fputc(int((v>>(8*i))&255U),canonical)!=EOF);}
/** @brief Сохраняет только байты завершённого результата. */
void bytes(const void* data,std::size_t n){if(canonical)CHECK(std::fwrite(data,1,n,canonical)==n);}
/** @brief Собирает оригинальный6502 код постоянного окна$E000. */
struct Program {
 std::vector<std::uint8_t> code;unsigned base=0xe000;
 /** @brief Возвращает следующую CPU-метку в фиксированном банке. */
 unsigned address()const{return base+unsigned(code.size());}
 /** @brief Добавляет opcode или operand. */
 void byte(unsigned x){code.push_back(std::uint8_t(x));}
 /** @brief Добавляет immediate/zero-page operand. */
 void imm(unsigned op,unsigned x){byte(op);byte(x);}
 /** @brief Добавляет абсолютный адрес little-endian. */
 void abs(unsigned op,unsigned x){byte(op);byte(x);byte(x>>8);}
 /** @brief Записывает константу настоящими LDA/STA. */
 void set(unsigned a,unsigned x){imm(0xa9,x);abs(0x8d,a);}
 /** @brief Проверяет диапазон и записывает относительный переход. */
 void branch(unsigned op,unsigned target){const int d=int(target)-int(address()+2);CHECK(d>=-128&&d<=127);imm(op,unsigned(d));}
 /** @brief Выбирает регистр mapper69. */
 void fme(unsigned reg,unsigned value){set(0x8000,reg);set(0xa000,value);}
 /** @brief Настраивает чистый expansion tone без native triangle. */
 void sound(unsigned mapper,unsigned reg,unsigned value){set(mapper==69?0xc000:0x9010,reg);set(mapper==69?0xe000:0x9030,value);}
};
/** @brief Создаёт ROM с независимыми CHR-страницами, IRQ bank switching, DMC и OAM DMA. */
std::vector<std::uint8_t> image(unsigned mapper,bool chrRam){
 constexpr unsigned prg=32768,chr=16384,fixed=16+prg-8192;
 std::vector<std::uint8_t> r(16+prg+(chrRam?0:chr),0);std::memcpy(r.data(),"NES\x1a",4);
 r[4]=2;r[5]=chrRam?0:2;r[6]=std::uint8_t(mapper<<4);r[7]=std::uint8_t(mapper&0xf0U);
 for(unsigned i=0;i<prg-8192;++i)r[16+i]=std::uint8_t((i/8192)&1U?0xaa:0x55);
 if(!chrRam)for(unsigned page=0;page<16;++page)for(unsigned i=0;i<1024;++i)
  r[16+prg+page*1024+i]=std::uint8_t((i&8U)?((page&1U)?0x55:0):((page&1U)?0xaa:0xff));
 Program p;p.byte(0x78);p.byte(0xd8);p.imm(0xa2,0xff);p.byte(0x9a);p.set(0x4017,0x40);p.set(0x2000,0);p.set(0x2001,0);
 if(mapper==69){for(unsigned i=0;i<8;++i)p.fme(i,i);p.fme(8,0xc0);}
 else {for(unsigned i=0;i<8;++i)p.set(0xa000U+(i/2U)*0x1000U+(i&1U)*0x10U,i);p.set(0xe000,0x80);}
 if(mapper==69){p.sound(mapper,0,32);p.sound(mapper,1,0);p.sound(mapper,7,0x3e);p.sound(mapper,8,15);}
 else {constexpr unsigned patch[]{0x21,0x21,0x3f,0,0xf0,0xf0,0x0f,0x0f};for(unsigned i=0;i<8;++i)p.sound(mapper,i,patch[i]);p.sound(mapper,0x30,0);p.sound(mapper,0x10,0x20);p.sound(mapper,0x20,0x19);}
 // Две физические CHR-RAM страницы имеют настоящие данные до первого render.
 if(chrRam){p.set(0x2006,0);p.set(0x2006,0);p.imm(0xa2,0);p.imm(0xa0,8);p.imm(0xa9,0xaa);const auto fill=p.address();p.abs(0x8d,0x2007);p.byte(0xe8);p.branch(0xd0,fill);p.byte(0x88);p.branch(0xd0,fill);}
 p.set(0x2006,0x20);p.set(0x2006,0);p.imm(0xa2,0);p.imm(0xa0,4);p.imm(0xa9,0);const auto names=p.address();p.abs(0x8d,0x2007);p.byte(0xe8);p.branch(0xd0,names);p.byte(0x88);p.branch(0xd0,names);
 p.set(0x2006,0x3f);p.set(0x2006,0);for(unsigned i=0;i<32;++i)p.set(0x2007,(i&3U)==1?0x16:(i&3U)==2?0x21:(i&3U)==3?0x30:0x0f);
 p.imm(0xa2,0);p.imm(0xa9,0xff);const auto oam=p.address();p.abs(0x9d,0x0200);p.byte(0xe8);p.branch(0xd0,oam);
 p.set(0x0200,31);p.set(0x0201,1);p.set(0x0202,0);p.set(0x0203,64);p.set(0x0204,80);p.set(0x0205,2);p.set(0x0206,0x40);p.set(0x0207,128);
 p.set(0x2003,0);p.set(0x4014,2);p.set(0x2006,0x20);p.set(0x2006,0);p.set(0x2005,0);p.set(0x2005,0);
 p.set(0x4010,0x4f);p.set(0x4012,0);p.set(0x4013,1);p.set(0x4015,0x10);p.set(0x2001,0x1e);
 if(mapper==69){p.fme(14,0);p.fme(15,0x10);p.fme(13,0x81);}else{p.set(0xe010,0xf8);p.set(0xf000,3);}p.byte(0x58);
 const auto loop=p.address();p.abs(0xad,0x6000);p.byte(0x18);p.imm(0x69,1);p.abs(0x8d,0x6001);p.abs(0xee,0x6000);p.abs(0xad,0x2002);p.set(0x4014,2);p.abs(0x4c,loop);
 CHECK(p.code.size()<0x800);std::memcpy(r.data()+fixed,p.code.data(),p.code.size());
 Program irq;irq.base=0xe800;irq.byte(0x48);irq.imm(0xe6,3);
 if(mapper==69)irq.fme(13,0);else irq.set(0xf010,0);
 irq.imm(0xa5,2);irq.imm(0x49,1);irq.imm(0x85,2);
 if(mapper==69){irq.set(0x8000,0);irq.imm(0xa5,2);irq.abs(0x8d,0xa000);irq.set(0x8000,11);irq.imm(0xa5,2);irq.abs(0x8d,0xa000);}else{irq.abs(0x8d,0xa000);irq.abs(0x8d,0x9000);}
 if(chrRam){irq.set(0x2006,0);irq.set(0x2006,0);irq.imm(0xa5,2);irq.imm(0x49,0xff);irq.abs(0x8d,0x2007);irq.set(0x2006,0x20);irq.set(0x2006,0);irq.set(0x2005,0);irq.set(0x2005,0);}
 if(mapper==69){irq.fme(14,0);irq.fme(15,0x10);irq.fme(13,0x81);}irq.byte(0x68);irq.byte(0x40);
 std::memcpy(r.data()+fixed+0x800,irq.code.data(),irq.code.size());r[fixed+0x900]=0x40;
 r[fixed+0x1ffa]=0;r[fixed+0x1ffb]=0xe9;r[fixed+0x1ffc]=0;r[fixed+0x1ffd]=0xe0;r[fixed+0x1ffe]=0;r[fixed+0x1fff]=0xe8;return r;
}
}
namespace nes::core {
/** @brief Native friendship использует существующие поля, не добавляет product hooks или состояния. */
struct MachineEventKernelTestAccess {
 /** @brief Читает RAM только у завершённого sole-owner, обходя запрещённый detached debug API. */
 static std::uint8_t ram(const Machine&m,unsigned address){return m.ram_[address&2047U];}
 /** @brief Проверяет настоящую pure CPU-RAM проводку69/85 на остановленном срезе. */
 static std::uint8_t cpuRam(const Machine&m,unsigned address){return m.cartridge_.cpuRead(std::uint16_t(address),m.openBus_);}
 /** @brief Отключает deferred CPU/PPU clocks только у независимого full PPU reference. */
 static void scalar(Machine&m){
#if FASTCAP_SCALAR
  (void)m; // Отдельный pure-scalar Core уже исключает deferred clocks из профиля.
#else
  m.observeCpuReads();m.flushPpu();m.cpuReadBudget_=0;m.deferPpuAllowed_=false;
#endif
 }
#if !FASTCAP_SCALAR
 /** @brief Проверяет фактическую Plain bulk-ветку через отличающийся опубликованный SpriteTile. */
 static void spriteSlot(Machine&m,VideoPacket&packet){
  auto&p=m.timingPpu_;p.packet_=&packet;p.capture_=nullptr;p.trackBusCycles_=p.trackBusAddresses_=false;
  p.scanline_=40;p.dot_=265;p.control_=0;p.mask_=0x1e;p.renderingEnabled_=p.renderingNext_=true;
  p.secondary_[4]=39;p.secondary_[5]=1;p.secondary_[6]=0x40;p.secondary_[7]=23;p.secondaryIndex_[1]=1;
  CHECK(m.videoSpriteMode(16,false,false)==Machine::VideoSpriteMode::Plain);
  CHECK(m.canVideoBgSpan(false,false,8,0,0));CHECK(!m.canVideoBgSpan(true,false,8,0,0));CHECK(!m.canVideoBgSpan(false,true,8,0,0));
  p.spriteFetchGroup<false>(8);CHECK(p.dot_==273);CHECK(packet.header().eventCount==2);CHECK(packet.events()[0].op()==VideoOp::SpriteTile);
  CHECK(p.sprites_[1].valid&&p.sprites_[1].x==23&&p.sprites_[1].index==1);p.packet_=nullptr;
 }
 /** @brief Выполняет настоящий LDA $6000 на Timing владельце и проверяет retained debt до getter barrier. */
 static void retained(Machine&m){
  CHECK(m.configureObservableVideo(true));
  if(m.cartridge_.mapper()==69){m.cartridge_.cpuWrite(0x8000,8,0);m.cartridge_.cpuWrite(0xa000,0xc0,0);}else m.cartridge_.cpuWrite(0xe000,0x80,0);
  m.ram_[0x200]=0xad;m.ram_[0x201]=0;m.ram_[0x202]=0x60;
  auto s=m.cpu_.state();s.pc=0x200;s.p=0x24;m.cpu_.setState(s);m.stepInstruction();CHECK(m.pendingCpuReads_>0&&m.cpuReadBudget_>0);
  m.observeCpuReads();m.flushPpu();
 }
#endif
};
}
namespace {
using Access=MachineEventKernelTestAccess;
/** @brief Переводит independent scalar index в RGB общей палитры при emphasis0. */
std::uint16_t rgb(unsigned index){const auto c=rgbPalette[index&63U];return std::uint16_t(((c>>16)&248U)<<8|((c>>8)&252U)<<3|(c&255U)>>3);}
/** @brief Сохраняет CPU, RAM/PRG-RAM, CHR mapping-visible bytes, каждый RGB/PCM байт. */
void dump(Machine&m,const std::uint8_t*indices,const std::uint16_t*colours,const std::int16_t*pcm,std::size_t samples,unsigned variant,unsigned n){
 const auto&s=m.cpuState();number(variant,4);number(n,4);number(s.pc,2);number(s.a,1);number(s.x,1);number(s.y,1);number(s.sp,1);number(s.p,1);number(s.cycles,8);number(s.jammed,1);number(m.cpuCycles(),8);
 for(unsigned i=0;i<2048;++i){number(Access::ram(m,i),1);}
 for(unsigned i=0x6000;i<0x8000;++i){number(Access::cpuRam(m,i),1);}
 for(unsigned i=0;i<8192;++i){number(m.cartridge().chrRead(std::uint16_t(i)),1);}
 bytes(indices,256*240);
 for(unsigned i=0;i<256*240;++i){number(colours?colours[i]:rgb(indices[i]),2);}
 number(samples,8);for(unsigned i=0;i<samples;++i){number(std::uint16_t(pcm[i]),2);}
}
#if !FASTCAP_SCALAR
/** @brief Владеет двумя real Output слотами до cancel после всех release. */
struct Frame {
 std::array<VideoPacket,2> packets;std::array<std::array<std::uint8_t,10240>,2>ram{};
 AudioPacket active,retired;AudioRenderer sound;VideoStorage storage;VideoRenderer video{storage};std::array<std::uint16_t,256*240>colours{};
 OutputFrameBinding binding{};OutputFrameLease lease{};OutputFrameCut cut{};const VideoPacket*sealed=nullptr;Machine machine;
 /** @brief Проверяет непересекающиеся Output spans и expansion audio capability. */
 void init(const std::vector<std::uint8_t>&r){CHECK(machine.load(r.data(),r.size(),Region::Ntsc)==LoadResult::Ok);machine.configureSplitAudio(&active,true);binding.source={&machine,r.data(),r.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};binding.packets={&packets[0],&packets[1]};binding.snapshots={ram[0].data(),ram[1].data()};binding.protectedSpans[0]={&active,sizeof(active)};binding.protectedSpans[1]={&retired,sizeof(retired)};binding.protectedSpans[2]={colours.data(),sizeof(colours)};binding.protectedSpans[3]={&storage,sizeof(storage)};binding.protectedSpans[4]={&video,sizeof(video)};binding.protectedSpans[5]={&sound,sizeof(sound)};CHECK(machine.configureOutputFrames(binding,lease));video.setRgb565Output(colours.data());}
 /** @brief Исполняет и потребляет один законченный кадр с реальными guest clocks. */
 void frame(unsigned n){OutputFrameTicket ticket{lease,n,n,machine.cpuCycles(),n&1U};CHECK(machine.beginOutputFrame(ticket));CHECK(machine.stepFrame()==RunStatus::FrameReady);CHECK(machine.sealOutputFrame(ticket,sealed,cut));CHECK(machine.copyOutputVisibleRam(ticket,ram[n&1U].data(),10240));CHECK(machine.retireAudioPacket(retired));CHECK(video.render(*sealed)==VideoRenderResult::Ok);CHECK(sound.render(retired)==AudioRenderResult::Ok);}
};
/** @brief Считает только настоящие headers, не принимая raw payload за op. */
/** @brief Считает реальные span-тайлы и записи банков/CHR между видимыми draw одного кадра. */
struct Coverage {unsigned spans=0,tiles=0,midBank=0,midChr=0;};
/** @brief Пропускает payload целиком, чтобы случайные raw bytes не стали ложными событиями. */
Coverage spans(const VideoPacket&p){Coverage out;bool visible=false,pendingBank=false,pendingChr=false;
 for(unsigned i=0;i<p.header().eventCount;){const auto&e=p.events()[i];unsigned slots=1;
  if(e.op()==VideoOp::BgView&&visible)pendingBank=true;
  if(e.op()==VideoOp::ChrWrite&&visible)pendingChr=true;
  if(e.op()==VideoOp::BgSpan||e.op()==VideoOp::BgRun||e.op()==VideoOp::Draw){
   if(pendingBank){++out.midBank;pendingBank=false;}if(pendingChr){++out.midChr;pendingChr=false;}visible=true;
  }
  switch(e.op()){case VideoOp::BgRun:slots=1+(e.arg0()+1)/2;break;case VideoOp::BgView:slots=6;break;
   case VideoOp::BgSpan:++out.spans;out.tiles+=e.arg0();[[fallthrough]];case VideoOp::SpriteTile:slots=2;break;
   case VideoOp::SpriteRow:slots=7;break;default:break;}
  CHECK(slots<=p.header().eventCount-i);i+=slots;
 }return out;}
/** @brief Сравнивает явные CPU fields без padding. */
void cpuEqual(const Cpu6502::State&a,const Cpu6502::State&b){CHECK(a.pc==b.pc&&a.a==b.a&&a.x==b.x&&a.y==b.y&&a.sp==b.sp&&a.p==b.p&&a.cycles==b.cycles&&a.jammed==b.jammed);}
/** @brief Проверяет новые ветви отдельно от большой игровой программы. */
void directed(const std::vector<std::uint8_t>&r){
 auto m=std::make_unique<Machine>();CHECK(m->load(r.data(),r.size(),Region::Ntsc)==LoadResult::Ok);
 CHECK((m->cartridge().statusPollCapabilities()&Cartridge::NoCpuClockEvent)==0);
 auto p=std::make_unique<VideoPacket>();CHECK(p->begin(1,0,7,false,r.data(),std::uint32_t(r.size())));Access::spriteSlot(*m,*p);
 auto retained=std::make_unique<Machine>();CHECK(retained->load(r.data(),r.size(),Region::Ntsc)==LoadResult::Ok);
 Access::retained(*retained);
}
#endif
/** @brief Проверяет16 кадров каждой платы/CHR memory модели и видимое исполнение IRQ/DMC/DMA. */
void run(unsigned mapper,bool chrRam,unsigned variant){
 const auto r=image(mapper,chrRam);auto scalar=std::make_unique<Machine>();CHECK(scalar->load(r.data(),r.size(),Region::Ntsc)==LoadResult::Ok);Access::scalar(*scalar);
#if !FASTCAP_SCALAR
 directed(r);auto frame=std::make_unique<Frame>();frame->init(r);Coverage coverage{};bool varied=false;
#endif
 for(unsigned n=0;n<16;++n){scalar->setController(0,std::uint8_t(n));CHECK(scalar->stepFrame()==RunStatus::FrameReady);const auto* pixels=scalar->framebuffer();CHECK(pixels);
#if FASTCAP_SCALAR
 dump(*scalar,pixels,nullptr,scalar->samples(),scalar->sampleCount(),variant,n);
#else
 frame->machine.setController(0,std::uint8_t(n));frame->frame(n);cpuEqual(frame->cut.cpu,scalar->cpuState());CHECK(frame->cut.endCycle==scalar->cpuCycles());
 CHECK(frame->sound.sampleCount()==scalar->sampleCount());CHECK(std::memcmp(frame->sound.samples(),scalar->samples(),scalar->sampleCount()*sizeof(std::int16_t))==0);
 CHECK(std::memcmp(frame->video.framebuffer(),pixels,256*240)==0);
 for(unsigned i=0;i<256*240;++i){CHECK(frame->colours[i]==rgb(pixels[i]));if(pixels[i]!=pixels[0])varied=true;}
 for(unsigned i=0;i<2048;++i){const auto a=Access::ram(frame->machine,i),b=Access::ram(*scalar,i);if(a!=b)std::fprintf(stderr,"RAM mapper%u chrRam%u frame%u address%04x actual%02x scalar%02x\n",mapper,unsigned(chrRam),n,i,a,b);CHECK(a==b);}
 for(unsigned i=0x6000;i<0x8000;++i)CHECK(Access::cpuRam(frame->machine,i)==Access::cpuRam(*scalar,i));
 CHECK(frame->machine.cartridge().irqLine()==scalar->cartridge().irqLine());CHECK(frame->machine.cartridge().selectedExpansionSample()==scalar->cartridge().selectedExpansionSample());
 const auto c=spans(*frame->sealed);coverage.spans+=c.spans;coverage.tiles+=c.tiles;coverage.midBank+=c.midBank;coverage.midChr+=c.midChr;dump(frame->machine,frame->video.framebuffer(),frame->colours.data(),frame->sound.samples(),frame->sound.sampleCount(),variant,n);CHECK(frame->packets[n&1U].release());
#endif
 }
 CHECK(scalar->debugRead(3)>0);CHECK(scalar->debugRead(0x6000)!=0);CHECK(scalar->sampleCount()>0);
#if !FASTCAP_SCALAR
 CHECK(coverage.spans>0&&varied);CHECK(coverage.midBank>0);CHECK(!chrRam||coverage.midChr>0);CHECK(frame->machine.cancelOutputFrames(frame->lease));std::printf("FASTCAP mapper=%u chrRam=%u frames=16 bgSpans=%u spanTiles=%u midBank=%u midChr=%u PlainSpriteTile=1 retainedPRGRAM=1 exactCPU_RAM_RGB_PCM=1\n",mapper,unsigned(chrRam),coverage.spans,coverage.tiles,coverage.midBank,coverage.midChr);
#endif
}
}
/** @brief Принимает только optional canonical path; не обращается к устройству или SDK. */
int main(int argc,char**argv){if(argc==3&&!std::strcmp(argv[1],"--canonical")){canonical=std::fopen(argv[2],"wb");CHECK(canonical);}else if(argc!=1)return 2;unsigned variant=0;for(unsigned mapper:{69U,85U})for(bool ram:{false,true})run(mapper,ram,variant++);if(canonical)CHECK(std::fclose(canonical)==0);std::printf("FASTCAP checks=%u scalar=%u\n",checks,FASTCAP_SCALAR);}