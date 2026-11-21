/**
 * @file MachineEventKernelTests.cpp
 * @brief Направленные срезы нового PPU-оператора и удержанного MMC5-кванта.
 * @details Точный эталон исполняет прежние отдельные точки/инструкции. Проверка
 * работает на одном потоке и не измеряет FPS. Необязательные аргументы задают
 * ожидаемые PHASE/RETAINED (0 либо 1); третий аргумент сохраняет canonical поля.
 * Суффикс --raster-plan 0|1 дополнительно проверяет фактический cache tag E3.
 * Суффикс --raster-interval 0|1 проверяет исполнение арифметического interval.
 */
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "ppu/VideoRenderer.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
unsigned checks=0,phaseAccepted=0,phaseAttempts=0,retainedObserved=0;
unsigned rasterM2=0,rasterBulk=0,rasterWrites=0,rasterCacheHits=0;
unsigned intervalAttempts=0,intervalAccepted=0,intervalRejected=0,intervalMmio=0;
std::FILE* canonical=nullptr;
/**
 * @brief Останавливает тест с номером исходной проверки, без продолжения после расхождения.
 * @param text Заимствованное выражение CHECK; выводится до завершения процесса.
 * @param line Номер строки C++-источника для диагностики, не гостевой timestamp.
 */
[[noreturn]] void fail(const char* text,unsigned line){std::fprintf(stderr,"EVENT_KERNEL line=%u check=%s\n",line,text);std::exit(1);}
#define CHECK(x) do{++checks;if(!(x))fail(#x,__LINE__);}while(false)
/**
 * @brief Пишет поля сравниваемого среза, исключая указатели и padding объектов.
 * @param fields Заимствованный до возврата массив полей; файл canonical принадлежит main.
 * @details Без открытого файла ничего не пишет. Ошибка fwrite останавливает тест.
 */
void dump(const std::vector<std::uint64_t>& fields){
    if(canonical){const auto size=std::uint32_t(fields.size());CHECK(std::fwrite(&size,sizeof(size),1,canonical)==1);
        CHECK(std::fwrite(fields.data(),sizeof(fields[0]),fields.size(),canonical)==fields.size());}
}
}

namespace nes::core {
/**
 * @brief Использует существующую test friendship VideoPacket без нового production API.
 * @details Ручные capacity/protocol метаданные проверяют pure rejection и прежнюю
 * фазу отказа. Это локальный directed fixture, а не допустимый опубликованный кадр.
 */
struct VideoSegmentWriterTestAccess {
    /** @brief Снимает bounded writer metadata; сырой arena проверяется отдельно. */
    static VideoPacket::HotState metadata(const VideoPacket& p){return p.hot();}
    /** @brief Возвращает Recording после проверки искусственного Sealed rejection. */
    static void recording(VideoPacket& p){p.hot().state=VideoPacket::State::Recording;}
    /**
     * @brief Оставляет ровно slots старых credits без раздувания physical event-арены.
     * @param p Recording proto0 с уже существующим fixture-префиксом.
     * @param slots Неотрицательное число свободных8-byte legacy slots.
     */
    static void remaining(VideoPacket& p,unsigned slots){auto& h=p.hot();
        CHECK(h.header.eventCount+slots<=VideoPacket::eventCapacity);
        h.legacyCount=VideoPacket::eventCapacity-h.header.eventCount-slots;
    }
    /**
     * @brief Задаёт несовместимую packet-предпосылку, не вызывая candidate.
     * @param p Packet sole-owner fixture; caller сравнивает чистоту всего хранилища.
     * @param kind 0 — Sealed,1 — protocol1,2 — pending literal prefix,3 — fault,
     * 4 — eventCount за ёмкостью,5 — недопустимые legacy credits.
     */
    static void reject(VideoPacket& p,unsigned kind){auto& h=p.hot();
        switch(kind){
        case 0:h.state=VideoPacket::State::Sealed;break;
        case 1:h.header.protocol=VideoPacket::literalProtocol;h.legacyCount=h.header.eventCount;break;
        case 2:h.literalPrefixCount=1;h.literalPrefix[0]={static_cast<std::uint32_t>(VideoOp::BgLoad),0};break;
        case 3:h.header.fault=VideoFault::InvalidEvent;break;
        case 4:h.header.eventCount=VideoPacket::eventCapacity+1U;break;
        case 5:h.legacyCount=VideoPacket::eventCapacity;break;
        default:CHECK(false);
        }
    }
};
/**
 * @brief Доступ теста к остановленному первичному владельцу, без production API.
 * @details Ручное положение PPU служит только ограниченному дифференциальному
 * оператору; оно не объявляется полноценным save-state или игровым кадром.
 */
struct MachineEventKernelTestAccess {
    /** @brief Создаёт начальную test-память после reset, ДО begin/immutable seed и guest interval. */
    static void initialVideoMemory(Machine& m,Mmc5& digital){
        for(unsigned i=0;i<m.nametables_.size();++i)m.nametables_[i]=std::uint8_t(i*37U+11U);
        for(unsigned i=0;i<digital.exRam_.size();++i)digital.exRam_[i]=std::uint8_t(i*13U+7U);
    }

    /**
     * @brief Сохраняет массив по элементам; адреса и выравнивание не входят в эталон.
     * @param out Владеющий вектор результата, допускающий расширение только в host-тесте.
     * @param values Заимствованные числовые поля остановленного владельца.
     */
    template<class T,std::size_t N>static void append(std::vector<std::uint64_t>& out,const std::array<T,N>& values){for(const auto value:values)out.push_back(value);}

    /** @brief Пробует новый полный slot после synthetic exclusive held-group admission.
     * @param m Primary fixture с уже доказанным owner/packet domain.
     * @param digital Заимствованный mapper именно этой машины.
     * @return true только после фактического full8; false ничего не меняет. */
    static bool tailSlot(Machine& m,Mmc5& digital){return m.timingPpu_.tryMmc5HeldSpriteSlot(digital);}
    /** @brief Передаёт один bounded prefix именно новому held-group loop.
     * @param m Primary fixture без параллельного CPU исполнения.
     * @param digital Заимствованный текущий mapper.
     * @param dots Уже разрешённые PPU точки, не CPU-такты.
     * @return true после полного prefix либо чистый false до продвижения. */
    static bool tailGroup(Machine& m,Mmc5& digital,unsigned dots){return m.timingPpu_.tryMmc5HeldSpriteGroup(dots,digital);}
    /** @brief Получает старый real-dot prefix, затем задаёт различимые secondary/OAM и A/B banks.
     * @param m Остановленный sole-owner fixture с Recording packet.
     * @param digital Mapper этого же fixture; ROM остаётся заимствованной.
     * @param dot Следующая PPU точка0..340 после независимого старого prefix.
     * @param style Биты flips,8x16/table,CHRmode/upper и ошибочных/повторных OAM индексов.
     * @details Не продвигает CPU/APU; это локальный directed endpoint, не save-state. */
    static void tailPrepare(Machine& m,Mmc5& digital,unsigned dot,unsigned style){
        prepare(m,digital,17,style&3U);auto& p=m.timingPpu_;
        while(p.dot_<dot)p.tick();
        p.scanline_=40;p.control_=static_cast<std::uint8_t>(0x10U|((style&4U)?0x20U:0U)|((style&8U)?8U:0U));
        digital.cpuWrite(0x2000U,p.control_);digital.cpuWrite(0x5101U,(style>>4U)&3U);
        digital.cpuWrite(0x5130U,(style>>6U)&3U);
        for(unsigned i=0;i<8U;++i)digital.cpuWrite(static_cast<std::uint16_t>(0x5120U+i),static_cast<std::uint8_t>(i*9U+3U));
        for(unsigned i=0;i<4U;++i)digital.cpuWrite(static_cast<std::uint16_t>(0x5128U+i),static_cast<std::uint8_t>(i*13U+57U));
        for(unsigned i=0;i<8U;++i){
            const unsigned row=(style+i)&((p.control_&0x20U)?15U:7U);
            p.secondary_[i*4U]=static_cast<std::uint8_t>(40U-row);
            p.secondary_[i*4U+1U]=static_cast<std::uint8_t>(i*17U+((style>>2U)&1U));
            p.secondary_[i*4U+2U]=static_cast<std::uint8_t>(((style&1U)?0x40U:0U)|((style&2U)?0x80U:0U)|i);
            p.secondary_[i*4U+3U]=static_cast<std::uint8_t>(i*31U+7U);
            p.secondaryIndex_[i]=(style&128U)&&i==3U?0xFFU:static_cast<std::uint8_t>((style&64U)&&i==((style>>3U)&7U)?0U:(i?i+3U:0U));
        }
        p.oamAddress_=0x75U;p.pendingPixels_=0;
        digital.inFrame_=true;digital.irqPending_=false;digital.nameRepeat_=0;digital.lastPpuAddress_=0x1234U;
        digital.cpuIdle_=0;digital.ppuReadSinceM2_=true;
        digital.scanline_=static_cast<std::uint8_t>(237U+style%3U);digital.splitY_=digital.scanline_;
        digital.irqCompare_=static_cast<std::uint8_t>(digital.scanline_+1U);digital.irqEnabled_=true;
        m.mmc5ReadQuantum_.active=true;m.mmc5ReadQuantum_.frameQualified=true;
    }
    /** @brief Воспроизводит независимые M2 и old tick при любом положении трёхточечной сетки.
     * @param m Остановленная машина без нового group helper.
     * @param digital Текущий mapper, получающий каждый настоящий M2.
     * @param dots Число исполняемых PPU точек.
     * @param lead Положение первой точки относительно M2 modulo3. */
    static void tailScalarM2(Machine& m,Mmc5& digital,unsigned dots,unsigned lead){
        for(unsigned i=0;i<dots;++i){if((i+lead)%3U==0U)digital.clockCpu(1U);m.timingPpu_.tick();}
    }
    /** @brief Создаёт один недопустимый digital/backing guard до operator mutation.
     * @param m Primary fixture, не исполняемый Worker.
     * @param digital Его цифровая модель с неизменной внешней ROM.
     * @param kind Индекс одного из15 detector/backing/debt отказов. */
    static void tailReject(Machine& m,Mmc5& digital,unsigned kind){
        switch(kind){
        case 0:digital.inFrame_=false;break;
        case 1:digital.irqPending_=true;digital.irqEnabled_=false;break;
        case 2:digital.irqPending_=true;digital.irqEnabled_=true;break;
        case 3:digital.nameRepeat_=1;break;
        case 4:digital.nameRepeat_=2;break;
        case 5:digital.nameRepeat_=3;break;
        case 6:digital.lastPpuAddress_=0x2000;break;
        case 7:digital.lastPpuAddress_=0xffff;break;
        case 8:digital.ppuReadSinceM2_=false;break;
        case 9:digital.cpuIdle_=1;break;
        case 10:digital.cpuIdle_=2;break;
        case 11:digital.cpuIdle_=3;break;
        case 12:digital.chr_=nullptr;break;
        case 13:digital.chrBytes_=0;break;
        case 14:m.timingPpu_.pendingPixels_=1;break;
        default:CHECK(false);
        }
    }
    /** @brief Создаёт нарушение outer held-group domain, не меняя ROM или CPU.
     * @param m Остановленная sole-owner машина для pure rejection.
     * @param kind Индекс одного из15 ownership/render/time нарушений. */
    static void tailGroupReject(Machine& m,unsigned kind){auto& p=m.timingPpu_;
        switch(kind){
        case 0:m.mmc5ReadQuantum_.active=false;break;
        case 1:m.outputJob_=false;break;
        case 2:m.outputInitializing_=true;break;
        case 3:m.outputSealed_=true;break;
        case 4:p.pal_=true;break;
        case 5:p.trackBusCycles_=true;break;
        case 6:p.trackBusAddresses_=true;break;
        case 7:p.renderingEnabled_=false;break;
        case 8:p.renderingNext_=false;break;
        case 9:p.renderDelay_=1;break;
        case 10:p.nmiDelay_=1;break;
        case 11:p.nmiPending_=true;break;
        case 12:p.frameReady_=true;break;
        case 13:p.status_|=0x80U;break;
        case 14:p.pendingPixels_=1;break;
        case 15:p.packet_=m.outputBinding_.packets[1];break;
        case 16:m.outputOwner_=false;break;
        default:CHECK(false);
        }
    }
    /** @brief Восстанавливает только synthetic held wiring перед штатным stop/cancel fixture.
     * @param m Уже проверенный и более не исполняемый fixture; ROM ещё принадлежит caller. */
    static void tailStopPrepare(Machine& m){m.outputInitializing_=false;m.outputSealed_=false;m.outputJob_=true;m.outputOwner_=true;
        m.timingPpu_.packet_=m.videoPacket_;
        m.mmc5ReadQuantum_.active=false;m.timingPpu_.pendingPixels_=0;}
    /** @brief Проверяет raw sprite-plane helper напрямую, без новых strobe или времени.
     * @param digital Mapper с заимствованным живым CHR-ROM.
     * @param a Канонический low plane адрес; invalid values также проверяются.
     * @param lo Выход low byte, неизменный при false.
     * @param hi Выход high byte, неизменный при false.
     * @return Исходный pure результат production helper. */
    static bool tailPlanes(Mmc5& digital,std::uint16_t a,std::uint8_t& lo,std::uint8_t& hi){return digital.readQuietSpritePlanes(a,lo,hi);}

    /**
     * @brief Снимает цифровые защёлки MMC5, важные для следующего шинного доступа.
     * @param m Исключительно принадлежащий этому host-потоку mapper; указатели backing не копируются.
     * @return Независимый владеющий список полей, банков и ExRAM без padding.
     */
    static std::vector<std::uint64_t> mapper(const Mmc5& m){
        std::vector<std::uint64_t> r={m.prgMode_,m.chrMode_,m.protect1_,m.protect2_,m.exMode_,m.ntMap_,
            m.fillTile_,m.fillAttribute_,m.upperChr_,m.splitControl_,m.splitScroll_,m.splitBank_,m.splitY_,
            m.irqCompare_,m.scanline_,m.multiplierA_,m.multiplierB_,m.ppuControl_,m.ppuMask_,m.lastChrSet_,
            m.nameRepeat_,m.cpuIdle_,m.tileAttribute_,m.tileSplitAttribute_,m.tileSplitY_,m.lastPpuAddress_,
            m.tileNtOffset_,m.irqEnabled_,m.irqPending_,m.inFrame_,m.ppuReadSinceM2_,m.tileExtended_,m.tileSplit_,m.splitSlMode_};
        append(r,m.exRam_);append(r,m.prgBanks_);append(r,m.chrBanks_);
        for(const auto& bases:m.normalChrBases_)append(r,bases);
        return r;
    }
    /**
     * @brief Снимает фазовые данные PPU и ленивые долги, включая невидимый для RGB Sprite0.
     * @param m Остановленная на проверяемом срезе машина; долги намеренно не материализуются.
     * @return Владеющий список значений без заимствованных адресов и C++ padding.
     */
    static std::vector<std::uint64_t> ppu(const Machine& m){
        const auto& p=m.timingPpu_;
        std::vector<std::uint64_t> r={p.trackBusCycles_,p.trackBusAddresses_,p.spriteCacheClocks_,p.spriteCacheValid_,p.spriteZeroRow_,
            p.control_,p.mask_,p.status_,p.oamAddress_,p.openBus_,p.readBuffer_,p.fineX_,p.v_,p.t_,p.busAddress_,
            p.coarseXSignal_,p.scrollSignal_,p.writeToggle_,p.renderingEnabled_,p.renderingNext_,p.renderDelay_,
            p.pendingBackgroundShifts_,p.nextTile_,p.nextAttribute_,p.pendingPixels_,p.pendingPixelX_,p.pendingFill_,p.pendingPaletteIndex_,
            p.evaluateN_,p.evaluateM_,p.secondaryPosition_,p.evaluateLatch_,p.overflowFetches_,p.evaluationDone_,p.evaluationNextDot_,
            std::uint64_t(std::int64_t(p.scanline_)),p.dot_,p.frames_,p.pal_,p.odd_,p.frameReady_,p.nmiLine_,p.nmiPending_,p.suppressVblank_,p.nmiDelay_};
        append(r,p.palette_);append(r,p.secondary_);append(r,p.oam_);append(r,p.secondaryIndex_);append(r,p.spriteRow_);
        append(r,p.spriteZeroMask_);append(r,p.backgroundAtoms_);
        for(const auto& s:p.sprites_){r.push_back(s.low);r.push_back(s.high);r.push_back(s.x);r.push_back(s.attributes);r.push_back(s.index);r.push_back(s.valid);}
#if defined(NES_CORE_PPU_OAM_Y_INDEX) && NES_CORE_PPU_OAM_Y_INDEX
        r.push_back(p.oamYLow_);r.push_back(p.oamYHigh_);r.push_back(std::uint64_t(std::int64_t(p.oamYRow_)));
        r.push_back(p.oamYHeight_);r.push_back(p.oamYDirty_);for(const auto& bucket:p.oamYBuckets_)append(r,bucket);
#endif
        return r;
    }
    /**
     * @brief Снимает CPU и RAM без преждевременной материализации удержанного epoch.
     * @param m Машина, единолично исполняемая тестом; cpuCycles включает уже исполненный долг M2.
     * @param digital Цифровой mapper этой же машины; cpuPeek не подтверждает IRQ и не тактирует шину.
     * @return Владеющий список CPU/RAM/open-bus полей для точного сравнения.
     */
    static std::vector<std::uint64_t> cpu(const Machine& m,const Mmc5& digital){
        const auto& s=m.cpu_.state();std::vector<std::uint64_t> r={s.pc,s.a,s.x,s.y,s.sp,s.p,s.cycles,s.jammed,m.cpuCycles(),m.openBus_};
        append(r,m.ram_);for(unsigned a=0x6000;a<0x8000;++a)r.push_back(digital.cpuPeek(std::uint16_t(a),m.openBus_));return r;
    }
    /**
     * @brief Получает реальный фазовый префикс старым tick, а не воспроизводит формулы оператора.
     * @param m Первичная машина с уже открытым Recording Output writer и живой ROM-арендой.
     * @param digital Mapper этой машины, остающийся живым до stop/cancel.
     * @param dot Начальная точка PPU в искусственно выбранной строке12; один tick означает одну точку.
     * @param mode 0 — normal, 1 — extended, 2 — split, 3 — fill nametable.
     * @details Не продвигает CPU/APU вслед за ручным PPU-префиксом; это локальный
     * differential fixture, а не полный restore машины или замер игрового кадра.
     */
    static void prepare(Machine& m,Mmc5& digital,unsigned dot,unsigned mode){
        auto& p=m.timingPpu_;p.reset();p.bind(m.videoPacket_);p.scanline_=12;p.control_=0x10;p.mask_=0x1e;
        p.renderingEnabled_=p.renderingNext_=true;p.v_=p.t_=0x041d;
        for(unsigned i=0;i<p.palette_.size();++i){p.palette_[i]=std::uint8_t(i+1U);CHECK(m.videoPacket_->add(VideoOp::PaletteWrite,i,0,0,i+1U));}
        // Initial synthetic NT/ExRAM уже заданы до beginOutputFrame; runtime stores не подменяются.
        digital.cpuWrite(0x2000,p.control_);digital.cpuWrite(0x2001,p.mask_);
        digital.cpuWrite(0x5104,mode==1?1:0);digital.cpuWrite(0x5105,mode==3?0xff:0x44);
        digital.cpuWrite(0x5106,7);digital.cpuWrite(0x5107,2);
        digital.cpuWrite(0x5200,mode==2?0x87:0);digital.cpuWrite(0x5201,11);digital.cpuWrite(0x5202,1);
        // Детектор получает настоящий четвёртый read; начальные dot0..dotN также настоящие.
        digital.ppuReadStrobe(0x2000);digital.ppuReadStrobe(0x2000);digital.ppuReadStrobe(0x2000);digital.ppuReadStrobe(0);
        while(p.dot_<dot)p.tick();
        CHECK(p.dot_==dot);CHECK(p.spriteCacheValid_);CHECK(m.videoPacket_->header().fault==VideoFault::None);
    }
    /**
     * @brief Различает первый M2 после read, уже прочитанный M2 и idle1 без потери детектора.
     * @param digital Только mapper проверяемой остановленной машины.
     * @param seed 0 — I0/Ptrue, 1 — I0/Pfalse, 2 — I1/Ptrue, 3 — I1/Pfalse.
     * @details I — число M2 без read, P — признак read после последнего M2. Гостевые clocks не добавляются.
     */
    static void clockSeed(Mmc5& digital,unsigned seed){
        digital.cpuIdle_=seed>=2U?1U:0U;digital.ppuReadSinceM2_=(seed&1U)==0U;
    }
    /** @brief Задаёт валидную opaque-маску Sprite0 вне проверяемого отрезка того же байта. */
    static void outsideSpriteZero(Machine& m){auto& p=m.timingPpu_;p.spriteZeroRow_=true;p.spriteZeroMask_[(p.dot_-1U)>>3U]=0x80;}
    /**
     * @brief Применяет candidate; OFF возвращает false, что проверяется отдельно от fallback.
     * @param m Первичная машина с действующим writer; доступ остаётся на одном host-потоке.
     * @param dots Число уже разрешённых точек до конца текущей восьмиточечной фазы.
     * @return true после исполнения; false обязан сохранить состояние и пакет.
     */
    static bool phase(Machine& m,unsigned dots){return m.timingPpu_.tryMmc5VisiblePhaseSegment(dots);}
    /**
     * @brief Продвигает ровно dots старым поточечным эталоном без нового helper.
     * @param m Машина независимого эталона; CPU/APU здесь намеренно не тактируются.
     * @param dots Число настоящих PPU tick, а не CPU M2 или физических пикселей LCD.
     */
    static void scalar(Machine& m,unsigned dots){while(dots--)m.timingPpu_.tick();}
    /**
     * @brief Создаёт MMC3-срез с уже прошедшим PPU-долгом без ослабления старого prepare.
     * @param m Остановленный primary Output fixture; остаётся у текущего host-потока.
     * @param digital Существующий внешний MMC5 fixture, не выбранный mapper4-картриджем.
     * @param dot Начальная точка строки12; после255 используется прежний scalar tick.
     * @param dots Долг в PPU-точках, а не новый CPU grant; материализуется только flush.
     * @details Старый prepare требует живой sprite-cache в видимой части. Его контракт
     * сохраняется: sprite/prefetch-позиции достигаются отдельным поточечным хвостом.
     */
    static void prepareOrdinaryDebt(Machine& m,Mmc5& digital,unsigned dot,unsigned dots){
        const unsigned visible=std::min(dot,255U);
        prepare(m,digital,visible,0);
        scalar(m,dot-visible);
        CHECK(m.timingPpu_.dot_==dot);
        m.timingPpu_.trackBusCycles_=m.timingPpu_.trackBusAddresses_=true;
        m.pendingPpuDots_=dots;
    }
    /**
     * @brief Исполняет независимый прежний prefix/rest алгоритм для выбранного TimingPpu.
     * @param m Sole-owner MMC3 fixture с уже заданным долгом; CPU/APU не продвигаются.
     * @details Между частями намеренно остаётся старый пересчёт бюджета. Эталон
     * не вызывает изменяемый flushPpuSlow и работает одинаково при PRIVATE OFF/ON.
     */
    static void flushOrdinaryDebtReference(Machine& m){
        CHECK(m.splitVideoEnabled());
        unsigned dots=m.pendingPpuDots_;
        if(!dots)return;
        m.pendingPpuDots_=0;
        const unsigned prefix=m.timingPpu_.deferredPrefix();
        if(prefix&&dots>=prefix){
            m.timingPpu_.tickDots(prefix);m.refreshPpuDeferBudget();dots-=prefix;
        }
        if(dots){m.timingPpu_.tickDots(dots);m.refreshPpuDeferBudget();}
    }
    /**
     * @brief Вызывает настоящий производственный helper материализации долга.
     * @param m Остановленный primary fixture; возврат не означает аппаратный кадр.
     */
    static void flushOrdinaryDebt(Machine& m){m.flushPpuSlow();}
    /**
     * @brief Снимает конечный долг, бюджет и чистые IRQ-прогнозы без padding.
     * @param m Остановленная машина; mapper getters не тактируют A12 и не снимают IRQ.
     * @return Владеющий список чисел для сравнения до и после будущего scalar-хвоста.
     */
    static std::vector<std::uint64_t> ordinaryDebtState(const Machine& m){
        return {m.pendingPpuDots_,m.ppuDeferBudget_,m.cartridge_.irqLine(),
            m.cartridge_.ppuIrqQuietDotsAny(),m.cartridge_.ppuIrqQuietFor(8U,false),
            m.cartridge_.ppuIrqQuietFor(8U,true)};
    }
    /**
     * @brief Изменяет настоящий PPUADDR latch между flush и будущими scalar-точками.
     * @param m Fixture с уже материализованным долгом; CPU-время здесь не подделывается.
     */
    static void ordinaryDebtMmio(Machine& m){
        m.timingPpu_.cpuWrite(6U,0x24U);m.timingPpu_.cpuWrite(6U,0x17U);
    }
    /**
     * @brief Исполняет настоящий ordinary M2 PPU, оставляя старый tick независимым эталоном.
     * @param m Sole-owner машина с открытым writer; локальный тест не продвигает CPU/APU.
     * @param digital Её цифровой mapper, получающий один настоящий M2 перед точками.
     * @param dots Три либо четыре PPU-точки уже прошедшего M2, не граница CPU-записи.
     * @param grouped true вызывает production tickDots; false — исключительно старый tick.
     */
    static void ordinary(Machine& m,Mmc5& digital,unsigned dots,bool grouped){
        digital.clockCpu(1);if(grouped)m.timingPpu_.tickDots(dots);else scalar(m,dots);
    }
    /**
     * @brief Применяет реальные изменяемые условия ordinary reader без подмены его данных.
     * @param m Остановленный локальный fixture; начальный фазовый префикс уже исполнен старым tick.
     * @param digital Mapper этой машины; MMIO изменяет версию реальным cpuWrite.
     * @param policy 0..3 — normal, extended, split-normal, split-extended.
     * @param tracking Бит0 включает bus clocks, бит1 — address drives.
     * @param sprite 0 — нет Sprite0, 1 — возможное попадание, 2 — уже установлен hit.
     */
    static void rasterInputs(Machine& m,Mmc5& digital,unsigned policy,unsigned tracking,unsigned sprite){
        auto& p=m.timingPpu_;digital.cpuWrite(0x5104,policy&1U);digital.cpuWrite(0x5105,0xe4);
        digital.cpuWrite(0x5200,policy>=2U?0x87:0);p.setBusTracking((tracking&1U)!=0,(tracking&2U)!=0);
        p.spriteZeroRow_=sprite!=0;if(sprite){p.spriteZeroMask_.fill(0xff);if(sprite==2)p.status_|=0x40;}
    }
    /**
     * @brief Ставит следующий read возле настоящего четвёртого NT-edge и scanline IRQ.
     * @param digital Mapper пары до M2; дальнейшие переходы исполняет реальный read-strobe.
     */
    static void nearIrq(Mmc5& digital){digital.lastPpuAddress_=0x2000;digital.nameRepeat_=3;
        digital.irqCompare_=1;digital.scanline_=0;digital.inFrame_=true;digital.irqEnabled_=true;digital.irqPending_=false;}
    /** @brief Читает только оптимизационный tag, не включая его в guest-equivalence snapshot. */
    static std::uint32_t rasterTag(const Machine& m){return m.timingPpu_.rasterPlanVersion_;}
    /** @brief Возвращает текущую версию mapping без guest clock или новых data reads. */
    static std::uint32_t rasterVersion(const Mmc5& digital){return digital.rasterPlanVersion();}
    /** @brief Задаёт sticky0 для bounded проверки fallback, не перебирая четыре миллиарда записей. */
    static void zeroRasterVersion(Mmc5& digital){digital.rasterVersion_=0;}
    /** @brief Проверяет invalidate прежней packet binding без сброса guest-состояния. */
    static void rebind(Machine& m){m.timingPpu_.bind(m.videoPacket_);}
    /**
     * @brief Исполняет certified multi-tile endpoint; его M2-fold проверяется старым циклом ниже.
     * @param m Candidate до начала visible восьмиточечной фазы, без CPU/MMIO внутри.
     * @param dots Уже разрешённые24..192 точки, кратные24; не физические LCD-пиксели.
     */
    static void bulk(Machine& m,unsigned dots){m.timingPpu_.replayMmc5InplaceDots(dots);}
    /**
     * @brief Вызывает actual interval endpoint с новым borrowed plan только этого участка.
     * @param m Единственный остановленный владелец; helper не добавляет M2 clocks.
     * @param dots Уже разрешённые PPU-точки, включая намеренно недопустимые границы.
     * @return Production bool, включая pure OFF stub; тест не угадывает PRIVATE macro.
     */
    static bool interval(Machine& m,unsigned dots){Mmc5RasterPlan plan{};auto* mapper=m.mmc5QuietMapper();
        CHECK(mapper);CHECK(mapper->prepareRasterPlan(plan,m.nametables_.data(),m.timingPpu_.control_));
        return m.timingPpu_.tryMmc5RasterInterval(dots,plan);
    }
    /**
     * @brief Ставит допустимые incoming atoms/debt для точного compare со старым scalar.
     * @param m Fixture после настоящего фазового префикса, не runtime save-state.
     * @param shifts Насыщенный сдвиг0..16 до следующего load.
     * @param references true оставляет реальные ROM refs вместо literal bytes.
     * @param empty true сначала публикует pending Draw, затем оставляет stale fill поля.
     */
    static void intervalIncoming(Machine& m,unsigned shifts,bool references,bool empty){auto& p=m.timingPpu_;
        for(unsigned i=0;i<p.backgroundAtoms_.size();++i)p.backgroundAtoms_[i]=references?32U+i*13U:VideoByteRef::literal|(i*31U+7U);
        p.pendingBackgroundShifts_=static_cast<std::uint8_t>(shifts);
        if(empty){p.flushPixels();p.pendingFill_=true;p.pendingPaletteIndex_=3;p.pendingPixelX_=251;}
    }
    /**
     * @brief Различает обе pattern-таблицы и fineY без вычисления нового reader в тесте.
     * @param m Fixture между двумя уже завершёнными PPU точками.
     * @param digital Mapper той же машины; реальный snoop получает текущий control.
     * @param control Новое значение PPUCTRL; старый held address намеренно сохранён.
     * @param fineY Новые старшие fineY биты v для будущего address drive.
     */
    static void intervalPattern(Machine& m,Mmc5& digital,unsigned control,unsigned fineY){auto& p=m.timingPpu_;
        p.control_=static_cast<std::uint8_t>(control);digital.cpuWrite(0x2000,p.control_);
        p.v_=static_cast<std::uint16_t>((p.v_&0x0fffU)|(fineY<<12U));
    }
    /**
     * @brief Вводит неподходящее PPU условие до pure admission, без эмуляции fast алгоритма.
     * @param m Локальный sole-owner fixture, к которому применён настоящий scalar prefix.
     * @param kind 0..17: наблюдатели, временные границы, долги и Sprite0 overlap.
     */
    static void intervalReject(Machine& m,unsigned kind){auto& p=m.timingPpu_;
        switch(kind){
        case 0:p.trackBusCycles_=true;break;case 1:p.trackBusAddresses_=true;break;
        case 2:p.renderDelay_=1;break;case 3:p.nmiDelay_=1;break;
        case 4:p.nmiPending_=true;break;case 5:p.frameReady_=true;break;
        case 6:p.status_|=0x80;break;case 7:p.spriteCacheValid_=false;break;
        case 8:p.pendingFill_=true;break;case 9:p.pendingPaletteIndex_=3;break;
        case 10:p.pendingPixelX_=250;break;case 11:p.pendingBackgroundShifts_=17;break;
        case 12:p.spriteCacheClocks_=256;break;case 13:p.fineX_=8;break;
        case 14:p.spriteZeroRow_=true;p.spriteZeroMask_[(p.dot_-1U)>>3U]|=std::uint8_t(1U<<((p.dot_-1U)&7U));break;
        case 15:m.outputJob_=false;break;case 16:p.pal_=true;break;
        case 17:p.nextAttribute_=4;break;default:CHECK(false);
        }
    }
    /**
     * @brief Ставит Sprite0 в последний пиксель interval либо сразу за ним.
     * @param m Fixture с валидным cache; многобайтовые окна сохраняют настоящую bitmap.
     * @param dots Проверяемая длина, не пересекающая x254.
     * @param inside true создаёт overlap в конце, false — вне текущего span.
     * @param hit true заранее ставит липкий status hit, снимающий повторное наблюдение.
     */
    static void intervalSprite(Machine& m,unsigned dots,bool inside,bool hit){auto& p=m.timingPpu_;
        p.spriteZeroRow_=true;p.spriteZeroMask_.fill(0);const unsigned x=p.dot_-1U+dots-(inside?1U:0U);
        CHECK(x<255U);p.spriteZeroMask_[x>>3U]=std::uint8_t(1U<<(x&7U));if(hit)p.status_|=0x40;
    }
    /**
     * @brief Исполняет настоящий MMIO write со старым scalar PPU в каждом его M2.
     * @param m Независимый reference; owned writer остаётся привязанным и живым.
     * @param address Настоящий CPU-адрес регистра.
     * @param value Фактический записываемый байт.
     * @details Только синхронно снимает admission outputOwner у нового visible-M2
     * kernel, затем восстанавливает его. Packet, lease и videoPolicy не меняются;
     * clockCpuCycle, APU, Cartridge и CPU-write код остаются исходными production.
     */
    static void scalarWrite(Machine& m,std::uint16_t address,std::uint8_t value){
        const bool owner=m.outputOwner_;m.outputOwner_=false;m.write(address,value);m.outputOwner_=owner;
    }
    /** @brief Завершает Draw-префикс для наблюдения Recording на этом же потоке владельца. */
    static void flushPixels(Machine& m){m.timingPpu_.flushPixels();}
    /**
     * @brief Вводит один заведомо неподходящий certificate; отказ обязан быть чистым.
     * @param m Однопоточный fixture без потребителя Recording-пакета.
     * @param kind Индекс0..10: cache, render-delay, NMI-delay, PAL, tracking, fill,
     * Sprite0, dot255, VBlank, writer identity либо sticky Capacity fault.
     */
    static void rejectState(Machine& m,unsigned kind){
        auto& p=m.timingPpu_;
        switch(kind){case 0:p.spriteCacheValid_=false;break;case 1:p.renderDelay_=1;break;case 2:p.nmiDelay_=1;break;
        case 3:p.pal_=true;break;case 4:p.trackBusCycles_=true;break;case 5:p.pendingFill_=true;break;
        case 6:p.spriteZeroRow_=true;p.spriteZeroMask_[(p.dot_-1U)>>3U]=0xff;break;
        case 7:p.dot_=255;break;case 8:p.status_|=0x80;break;case 9:m.outputJob_=false;break;
        case 10:m.videoPacket_->fault(VideoFault::Capacity);break;default:CHECK(false);}
    }
    /** @brief Возвращает guard после чистого отказа, чтобы штатный stop имел действующий writer. */
    static void restoreJob(Machine& m){m.outputJob_=true;}
    /**
     * @brief Настраивает банковый регистр настоящим bus write с обновлением прямых страниц Cartridge.
     * @param m Единственный владелец CPU-bus; эффект содержит настоящий M2 записи.
     * @param address 16-битный адрес NES, не физический host-указатель.
     * @param value Байтовое значение регистра.
     */
    static void write(Machine& m,std::uint16_t address,std::uint8_t value){m.write(address,value);}
    /**
     * @brief Проверяет удержанный grant и точный MMIO-барьер относительно обычного interpreter.
     * @param fast Candidate с активным Output writer; локальный FrameScope владеет его grant.
     * @param reference Независимая машина, исполняющая те же инструкции без FrameScope/GQ.
     * @param a Mapper fast, заимствованный до возврата.
     * @param b Mapper reference, заимствованный до возврата.
     * @param expected -1 допускает любой feature mode, 0 требует OFF, 1 требует actual retained credits.
     * @param prefix M2 инструкций перед записью: 0 либо 2 у LDA immediate.
     * @param budget Верхняя граница допуска в M2.
     * @param writable Сценарий с подключённой и разрешённой записью WRAM.
     * @details Все MMIO сравниваются после обязательного flush; промежуточное
     * отставание устройств до барьера допустимо, CPU/RAM и логическое время уже совпадают.
     */
    static void epoch(Machine& fast,Machine& reference,const Mmc5& a,const Mmc5& b,int expected,unsigned prefix,unsigned budget,bool writable){
        fast.flushCpuReads();reference.flushCpuReads();fast.cpuReadBudget_=reference.cpuReadBudget_=0;
        Machine::Mmc5ReadQuantumFrameScope scope(fast,fast.cpuCycles()+1000U);
        const auto before=cpu(fast,a);const auto start=fast.cpuCycles();
        const auto expectedConsumed=prefix+(writable?6U:0U); // STA4 + NOP2.
        const auto consumed=fast.runMmc5QuietBlock(budget);
        if(consumed!=expectedConsumed)std::fprintf(stderr,"EPOCH_SEED consumed=%u pc=%04x budget=%u pending=%u horizon=%u apuQuiet=%u clocks=%llu apu=%llu ppuDebt=%u defer=%u frame=%u idle=%u P=%u last=%04x repeat=%u\n",
            consumed,unsigned(fast.cpu_.state().pc),unsigned(fast.cpuReadBudget_),unsigned(fast.pendingCpuReads_),
            fast.timingPpu_.mmc5InplaceActiveDots(),fast.timingApu_.quietCpuCycles(),static_cast<unsigned long long>(fast.cycles_),
            static_cast<unsigned long long>(fast.timingApu_.cycles()),unsigned(fast.pendingPpuDots_),unsigned(fast.deferPpuAllowed_),
            unsigned(fast.mmc5ReadQuantum_.frameQualified),unsigned(a.cpuIdle_),unsigned(a.ppuReadSinceM2_),unsigned(a.lastPpuAddress_),unsigned(a.nameRepeat_));
        CHECK(consumed==expectedConsumed);CHECK(fast.cpuCycles()==start+expectedConsumed);
        const bool retained=fast.cpuReadBudget_!=0;
        if(expected>=0)CHECK(retained==(expected!=0 && expectedConsumed<budget));
        if(retained)++retainedObserved;
        if(prefix)CHECK(reference.cpu_.step()==2U);
        if(writable){
            // Быстрый блок уже исполнил STA и NOP. Догоняем его обычной шиной.
            CHECK(reference.cpu_.step()==4U);CHECK(reference.cpu_.step()==2U);
            CHECK(cpu(fast,a)==cpu(reference,b));
        }else{
            // Закрытая защита удерживает STA на обычной шине. Проверяем состояние
            // перед отказом и после каждого шинного шага.
            if(!prefix)CHECK(cpu(fast,a)==before);
            CHECK(cpu(fast,a)==cpu(reference,b));
            CHECK(fast.cpu_.step()==4U);CHECK(reference.cpu_.step()==4U);CHECK(cpu(fast,a)==cpu(reference,b));
            CHECK(fast.cpu_.step()==2U);CHECK(reference.cpu_.step()==2U);CHECK(cpu(fast,a)==cpu(reference,b));
        }
        CHECK(fast.cpu_.step()==4U);CHECK(reference.cpu_.step()==4U);
        fast.flushCpuReads();reference.flushCpuReads();fast.flushPpu();reference.flushPpu();
        CHECK(cpu(fast,a)==cpu(reference,b));CHECK(ppu(fast)==ppu(reference));CHECK(mapper(a)==mapper(b));
        CHECK(fast.pendingCpuReads_==0&&reference.pendingCpuReads_==0);dump(cpu(fast,a));dump(ppu(fast));dump(mapper(a));
    }
    /**
     * @brief Полный grant8 из четырёх NOP обязан закрыться немедленно даже в retained режиме.
     * @param fast Candidate с программой четырёх двухтактовых NOP.
     * @param reference Независимый interpreter без активного retained FrameScope.
     * @param a Цифровой mapper fast; живёт до остановки writer.
     * @param b Цифровой mapper reference с теми же исходными защёлками.
     * @details Проверяет pending=budget=0 до следующей инструкции и точное состояние после восьми M2.
     */
    static void exhausted(Machine& fast,Machine& reference,const Mmc5& a,const Mmc5& b){
        fast.flushCpuReads();reference.flushCpuReads();fast.cpuReadBudget_=reference.cpuReadBudget_=0;
        Machine::Mmc5ReadQuantumFrameScope scope(fast,fast.cpuCycles()+1000U);const auto start=fast.cpuCycles();
        CHECK(fast.runMmc5QuietBlock(8)==8U);CHECK(fast.cpuCycles()==start+8U);
        CHECK(fast.pendingCpuReads_==0&&fast.cpuReadBudget_==0);
        for(unsigned i=0;i<4;++i)CHECK(reference.cpu_.step()==2U);
        fast.flushPpu();reference.flushPpu();CHECK(cpu(fast,a)==cpu(reference,b));
        CHECK(ppu(fast)==ppu(reference));CHECK(mapper(a)==mapper(b));dump(cpu(fast,a));dump(ppu(fast));dump(mapper(a));
    }
};
}

namespace {
using namespace nes::core;
using Access=MachineEventKernelTestAccess;
using PacketAccess=VideoSegmentWriterTestAccess;
/**
 * @brief Создаёт собственную NES2 ROM для строгого префикса, отказа и полного grant.
 * @param shape 0 — LDA immediate перед WRAM store, 1 — WRAM store первым opcode,
 * 2 — четыре NOP. После них размещены MMIO/status и loop, без чужих ROM-байтов.
 * @return Владеющие байты NES2 с32KiB PRG,8KiB CHR и заявленной8KiB WRAM.
 */
std::vector<std::uint8_t> romImage(unsigned shape=0){
    std::vector<std::uint8_t> r(16+32768+8192,0xea);std::fill(r.begin(),r.begin()+16,0);
    r[0]='N';r[1]='E';r[2]='S';r[3]=0x1a;r[4]=2;r[5]=1;r[6]=0x50;r[7]=8;r[10]=7;
    const std::array<std::vector<std::uint8_t>,3> code={{
        {0xa9,0x55,0x8d,0,0x60,0xea,0xad,2,0x20,0x4c,0,0xe0},
        {0x8d,0,0x60,0xea,0xad,2,0x20,0x4c,0,0xe0},
        {0xea,0xea,0xea,0xea,0xad,2,0x20,0x4c,0,0xe0}}};
    CHECK(shape<code.size());std::memcpy(r.data()+16+0x6000,code[shape].data(),code[shape].size());
    for(unsigned at:{0x7ffaU,0x7ffcU,0x7ffeU}){r[16+at]=0;r[16+at+1]=0xe0;}
    for(unsigned i=0;i<8192;++i)r[16+32768+i]=std::uint8_t(i*29U+19U);
    return r;
}
/**
 * @brief Создаёт собственный CHR с различимыми банками для MMIO/cache испытаний.
 * @return Та же тестовая CPU-программа с128KiB CHR; нет сторонних игровых байтов.
 * @details Маркер страницы нужен, чтобы stale bank дал другое изображение,
 * а не случайно прошёл на периодических одинаковых1KiB областях исходного fixture.
 */
std::vector<std::uint8_t> rasterRomImage(){
    auto r=romImage();r[5]=16;r.resize(16U+32768U+131072U);
    for(unsigned i=0;i<131072U;++i)r[16U+32768U+i]=std::uint8_t(i*29U+19U+(i>>10U)*37U);
    return r;
}
/**
 * @brief Владеет только ресурсами нужного Output writer; нет второго Worker или replay PPU.
 * @details Все heap allocations предшествуют проверяемому оператору. Сначала stop,
 * затем cancel; факт однопоточного возврата здесь является настоящим completion.
 */
struct Fixture {
    Mmc5 digital;Mmc5Audio expansion;std::array<VideoPacket,2> packets;
    std::array<std::array<std::uint8_t,10240>,2> ram{};AudioPacket audio;
    OutputFrameLease lease{};Machine machine;
    /**
     * @brief Привязывает собственную ROM, цифровой mapper и неалиасные Output диапазоны.
     * @param rom Заимствованные неизменяемые байты: внешний run удерживает их до stop/cancel обеих машин.
     * @details Создаёт единственный Recording writer; ошибка привязки немедленно останавливает тест.
     */
    explicit Fixture(const std::vector<std::uint8_t>& rom,bool writable=true){
        machine.configureMmc5(&digital,&expansion);CHECK(machine.load(rom.data(),rom.size(),Region::Ntsc)==LoadResult::Ok);
        machine.configureSplitAudio(&audio,true);
        OutputFrameBinding bind{};bind.source={&machine,rom.data(),rom.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};
        bind.packets={&packets[0],&packets[1]};bind.snapshots={ram[0].data(),ram[1].data()};
        bind.protectedSpans[0]={&audio,sizeof(audio)};bind.protectedSpans[1]={&digital,sizeof(digital)};bind.protectedSpans[2]={&expansion,sizeof(expansion)};
        CHECK(machine.configureOutputFrames(bind,lease));Access::initialVideoMemory(machine,digital);OutputFrameTicket ticket{lease,0,0,machine.cpuCycles(),0};CHECK(machine.beginOutputFrame(ticket));
        // Настоящий Cartridge write также обновляет его direct PRG page pointers.
        Access::write(machine,0x5114,0x80);Access::write(machine,0x5115,0x81);Access::write(machine,0x5116,0x82);Access::write(machine,0x5117,0x83);
        Access::write(machine,0x5102,writable?2U:1U);Access::write(machine,0x5103,1);
    }
    /** @brief Штатно прекращает незапечатанный writer после всех синхронных обращений. */
    void stop(){Access::restoreJob(machine);CHECK(machine.stopOutputWriter(lease));CHECK(machine.cancelOutputFrames(lease));}
};
/**
 * @brief Снимает семантические packet metadata без padding/host pointers.
 * @param packet Sole-owner Recording либо намеренно повреждённый rejection fixture.
 * @return Владеющий список опубликованных счётчиков и pending literal scratch.
 */
std::vector<std::uint64_t> packetFields(const VideoPacket& packet){const auto p=PacketAccess::metadata(packet);const auto& h=p.header;
    std::vector<std::uint64_t> result={h.generation,h.sequence,h.startCycle,h.endCycle,h.ppuFrame,h.chrRomBytes,h.eventCount,
        h.pal,h.seedPresent,unsigned(h.fault),h.protocol,p.legacyCount,p.literalPrefixCount,unsigned(p.state)};
    for(const auto e:p.literalPrefix){result.push_back(e.tag);result.push_back(e.value);}return result;
}
/**
 * @brief Сравнивает точную публикацию raw records до flushPixels/readRecording.
 * @param a Candidate, который уже вернулся к единственному владельцу.
 * @param b Независимый старый tick-эталон с теми же metadata предпосылками.
 * @details Capacity-corruption обрезается только при чтении арены, не в metadata.
 */
void rawPacketsEqual(const Fixture& a,const Fixture& b){
    CHECK(packetFields(a.packets[0])==packetFields(b.packets[0]));
    CHECK(a.packets[0].legacyEventCount()==b.packets[0].legacyEventCount());
    const auto count=std::min<unsigned>(a.packets[0].header().eventCount,VideoPacket::eventCapacity);
    CHECK(std::memcmp(a.packets[0].events(),b.packets[0].events(),count*sizeof(VideoEvent))==0);
}
/**
 * @brief Проверяет pure bool endpoint и только затем исполняет старый fallback при отказе.
 * @param a Candidate с ещё не использованным interval.
 * @param dots Разрешённые точки; invalid-domain cases также обязаны отказать чисто.
 * @param expected -1 допускает любой ответ;0 требует отказа;1 требует actual execution.
 * @return true только если production interval действительно исполнился.
 * @details При false сравнивается вся event-арена, включая ещё не опубликованные slots.
 */
bool intervalApply(Fixture& a,unsigned dots,int expected){
    const auto beforePpu=Access::ppu(a.machine),beforeMapper=Access::mapper(a.digital),beforePacket=packetFields(a.packets[0]);
    std::vector<VideoEvent> arena(a.packets[0].events(),a.packets[0].events()+VideoPacket::eventCapacity);
    const bool used=Access::interval(a.machine,dots);++intervalAttempts;if(used)++intervalAccepted;else++intervalRejected;
    if(expected>=0)CHECK(used==(expected!=0));
    if(!used){CHECK(Access::ppu(a.machine)==beforePpu);CHECK(Access::mapper(a.digital)==beforeMapper);
        CHECK(packetFields(a.packets[0])==beforePacket);CHECK(std::memcmp(arena.data(),a.packets[0].events(),arena.size()*sizeof(VideoEvent))==0);
        Access::scalar(a.machine,dots);
    }
    return used;
}
/** @brief Сравнивает полный local endpoint и продолжение старым scalar после candidate возврата. */
void intervalCompare(Fixture& a,Fixture& b,unsigned continuation=3){
    CHECK(Access::ppu(a.machine)==Access::ppu(b.machine));CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));rawPacketsEqual(a,b);
    Access::scalar(a.machine,continuation);Access::scalar(b.machine,continuation);
    CHECK(Access::ppu(a.machine)==Access::ppu(b.machine));CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));rawPacketsEqual(a,b);
    dump(Access::ppu(a.machine));dump(Access::mapper(a.digital));
}
/** @brief Сравнивает явно материализованный Recording prefix, включая payload каждого события. */
void packetsEqual(Fixture& a,Fixture& b){
    Access::flushPixels(a.machine);Access::flushPixels(b.machine);CHECK(a.packets[0].readRecording());CHECK(b.packets[0].readRecording());
    const auto& x=a.packets[0].header();const auto& y=b.packets[0].header();CHECK(x.fault==y.fault);CHECK(x.eventCount==y.eventCount);
    CHECK(a.packets[0].legacyEventCount()==b.packets[0].legacyEventCount());
    CHECK(std::memcmp(a.packets[0].events(),b.packets[0].events(),x.eventCount*sizeof(VideoEvent))==0);
}
/**
 * @brief Сравнивает canonical Recording-результат разных допустимых transport encodings.
 * @details У retained replay существующий BgRun заменяет отдельные scalar events;
 * его размер не обязан совпадать. Оба префикса неизменяемы до возврата observers.
 */
void canonicalPrefix(Fixture& a,Fixture& b){
    Access::flushPixels(a.machine);Access::flushPixels(b.machine);CHECK(a.packets[0].readRecording());CHECK(b.packets[0].readRecording());
    auto sa=std::make_unique<VideoStorage>();auto sb=std::make_unique<VideoStorage>();
    VideoRenderer va(*sa),vb(*sb);std::vector<std::uint16_t> ra(256U*240U),rb(256U*240U);
    va.setRgb565Output(ra.data());vb.setRgb565Output(rb.data());
    CHECK(va.observeVideo(a.packets[0]));CHECK(vb.observeVideo(b.packets[0]));
    CHECK(sa->indices==sb->indices);CHECK(ra==rb);
    /**
     * @brief Требует хотя бы один ненулевой цветовой индекс в сравнённом canonical prefix.
     * @details Предикат лишь исключает пустой RGB-контроль; не изменяет пиксели и не заменяет полное сравнение обоих буферов.
     */
    CHECK(std::any_of(sa->indices.begin(),sa->indices.end(),[](std::uint8_t value){return value!=0;}));
    std::vector<std::uint64_t> pixels;pixels.reserve(ra.size()*2U);
    for(auto value:sa->indices)pixels.push_back(value);
    for(auto value:ra)pixels.push_back(value);
    dump(pixels);
    const auto& x=a.audio.header();const auto& y=b.audio.header();
    CHECK(x.baseCycle==y.baseCycle&&x.inputCount==y.inputCount&&x.dmcCount==y.dmcCount&&x.expansionCount==y.expansionCount);
    CHECK(std::memcmp(a.audio.inputs(),b.audio.inputs(),x.inputCount*sizeof(AudioPacket::Input))==0);
    CHECK(std::memcmp(a.audio.dmc(),b.audio.dmc(),x.dmcCount*sizeof(AudioPacket::Dmc))==0);
    CHECK(std::memcmp(a.audio.expansions(),b.audio.expansions(),x.expansionCount*sizeof(AudioPacket::Expansion))==0);
}
/**
 * @brief Проверяет все36 prefix/tail срезов каждой политики и продолжение ещё одним scalar dot.
 * @param rom Внешние неизменяемые ROM-байты для всех пар до завершения их stop/cancel.
 * @param expected Ожидаемый PHASE mode:0/OFF,1/ON либо-1 без требования активации.
 */
void phases(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<4;++mode)for(unsigned seed=0;seed<4;++seed)
    for(unsigned phase=0;phase<8;++phase)for(unsigned dots=1;dots<=8-phase;++dots){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17+phase,mode);Access::prepare(b->machine,b->digital,17+phase,mode);
        Access::clockSeed(a->digital,seed);Access::clockSeed(b->digital,seed);
        const auto before=Access::ppu(a->machine);const auto mapperBefore=Access::mapper(a->digital);
        const bool used=Access::phase(a->machine,dots);++phaseAttempts;if(used)++phaseAccepted;
        if(expected>=0)CHECK(used==(expected!=0));
        if(!used){CHECK(Access::ppu(a->machine)==before);CHECK(Access::mapper(a->digital)==mapperBefore);Access::scalar(a->machine,dots);}
        Access::scalar(b->machine,dots);CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        packetsEqual(*a,*b);Access::scalar(a->machine,1);Access::scalar(b->machine,1);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));a->stop();b->stop();
    }
}
/**
 * @brief Проверяет ненулевой Sprite0 за границей отрезка и последнюю разрешённую точку254.
 * @param rom Заимствованная ROM, живущая дольше обеих независимых машин.
 * @param expected Ожидаемый PHASE mode:0/OFF,1/ON либо-1 для автоматического режима.
 * @details После точки254 отдельно исполняет255, сохраняя исходную scroll-защёлку.
 */
void positiveEdges(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned kind=0;kind<2;++kind){auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        const unsigned dot=kind?254U:18U,dots=kind?1U:2U;
        Access::prepare(a->machine,a->digital,dot,0);Access::prepare(b->machine,b->digital,dot,0);
        Access::clockSeed(a->digital,0);Access::clockSeed(b->digital,0);
        if(!kind){Access::outsideSpriteZero(a->machine);Access::outsideSpriteZero(b->machine);}
        const bool used=Access::phase(a->machine,dots);++phaseAttempts;if(used)++phaseAccepted;
        if(expected>=0)CHECK(used==(expected!=0));
        if(!used)Access::scalar(a->machine,dots);
        Access::scalar(b->machine,dots);CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        packetsEqual(*a,*b);Access::scalar(a->machine,1);Access::scalar(b->machine,1);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        packetsEqual(*a,*b);dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));a->stop();b->stop();
    }
}
/**
 * @brief Проверяет чистые отказы и точный новый cross-tile admission вместо старого phase-only отказа.
 * @param rom Неизменяемый backing временных fixtures, удерживаемый до их завершения.
 * @param phaseExpected PHASE feature;0 запрещает вход даже при включённом INTERVAL.
 * @param intervalExpected INTERVAL feature;ON вправе исполнить старый cross-tile rejection.
 */
void rejects(const std::vector<std::uint8_t>& rom,int phaseExpected,int intervalExpected){
    for(unsigned kind=0;kind<11;++kind){auto a=std::make_unique<Fixture>(rom);Access::prepare(a->machine,a->digital,17,0);Access::rejectState(a->machine,kind);
        const auto p=Access::ppu(a->machine),m=Access::mapper(a->digital);const auto count=a->packets[0].header().eventCount;
        CHECK(!Access::phase(a->machine,1));CHECK(Access::ppu(a->machine)==p);CHECK(Access::mapper(a->digital)==m);CHECK(a->packets[0].header().eventCount==count);a->stop();}
    auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
    Access::prepare(a->machine,a->digital,23,0);Access::prepare(b->machine,b->digital,23,0);
    const auto p=Access::ppu(a->machine),m=Access::mapper(a->digital),packet=packetFields(a->packets[0]);
    CHECK(!Access::phase(a->machine,0));CHECK(Access::ppu(a->machine)==p);
    CHECK(Access::mapper(a->digital)==m);CHECK(packetFields(a->packets[0])==packet);
    const bool used=Access::phase(a->machine,3);
    if(phaseExpected>=0&&intervalExpected>=0)CHECK(used==(phaseExpected!=0&&intervalExpected!=0));
    if(!used){CHECK(Access::ppu(a->machine)==p);CHECK(Access::mapper(a->digital)==m);
        CHECK(packetFields(a->packets[0])==packet);Access::scalar(a->machine,3);}
    Access::scalar(b->machine,3);intervalCompare(*a,*b,1);a->stop();b->stop();
}
/**
 * @brief Проверяет ordinary M2 по каждой фазе, обоим clock/address проводам и Sprite0.
 * @param rom Неизменяемая ROM с различимыми CHR-банками, живущая до stop каждого fixture.
 * @param expected -1 не требует E3 admission;0/1 сверяет фактический tag нового cache.
 * @details Reference всегда исполняет старые tick/read, включая четвёртый NT-edge,
 * IRQ, Sprite0 и продолжение через следующий M2; новый read-алгоритм не копируется.
 */
void rasterOrdinary(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<4;++mode)for(unsigned phase=0;phase<8;++phase)
    for(unsigned dots:{3U,4U})for(unsigned tracking=0;tracking<4;++tracking)for(unsigned sprite=0;sprite<3;++sprite){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17+phase,mode);Access::prepare(b->machine,b->digital,17+phase,mode);
        Access::rasterInputs(a->machine,a->digital,mode,tracking,sprite);Access::rasterInputs(b->machine,b->digital,mode,tracking,sprite);
        if(sprite==1){Access::nearIrq(a->digital);Access::nearIrq(b->digital);}
        Access::ordinary(a->machine,a->digital,dots,true);Access::ordinary(b->machine,b->digital,dots,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        const auto tag=Access::rasterTag(a->machine);
        if(expected>=0)CHECK((tag!=0)==(expected!=0));
        if(tag)CHECK(tag==Access::rasterVersion(a->digital));
        Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        if(tag){CHECK(Access::rasterTag(a->machine)==tag);++rasterCacheHits;}
        dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));++rasterM2;a->stop();b->stop();
    }
}
/**
 * @brief Проверяет special255/256 внутри ordinary3/4-dot и переход к sprite-fetch.
 * @param rom Живой неизменяемый backing fixtures с различимыми CHR-банками.
 * @param expected Ожидаемый E3 admission; fallback сравнивается с тем же старым tick.
 * @details Оба окна заканчиваются после dot256: E_EV255, coarseX и incrementY
 * должны материализоваться до последующего M2 dot257..259. Sprite0 отдельно
 * исключает безопасный aggregate, поэтому проверяется и per-dot pixel путь.
 */
void rasterRightEdge(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<4;++mode)for(unsigned tracking:{0U,3U})
    for(unsigned sprite:{0U,1U})for(unsigned dots:{3U,4U}){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        const unsigned dot=257U-dots;
        Access::prepare(a->machine,a->digital,dot,mode);Access::prepare(b->machine,b->digital,dot,mode);
        Access::rasterInputs(a->machine,a->digital,mode,tracking,sprite);Access::rasterInputs(b->machine,b->digital,mode,tracking,sprite);
        Access::ordinary(a->machine,a->digital,dots,true);Access::ordinary(b->machine,b->digital,dots,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        if(expected>=0)CHECK((Access::rasterTag(a->machine)!=0)==(expected!=0));
        Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));++rasterM2;a->stop();b->stop();
    }
}
/**
 * @brief Проверяет actual BgRun от трёх тайлов до192 точек с coarseX переходом.
 * @param rom Живой backing собственных fixtures; packet observers не владеют ROM.
 * @param expected Ожидаемое включение E3; BgRun обязан существовать и при old typed fallback.
 * @details Сравниваются canonical RGB/indices и полный выход PPU/mapper, поскольку
 * transport events намеренно отличаются от scalar эталона. Затем исполняется ещё M2.
 */
void rasterRuns(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<4;++mode)for(unsigned tiles:{3U,6U,12U,24U}){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17,mode);Access::prepare(b->machine,b->digital,17,mode);
        Access::rasterInputs(a->machine,a->digital,mode,0,0);Access::rasterInputs(b->machine,b->digital,mode,0,0);
        Access::clockSeed(a->digital,0);Access::clockSeed(b->digital,0);
        Access::bulk(a->machine,tiles*8U);
        for(unsigned dot=0;dot<tiles*8U;dot+=3U)Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        bool usedRun=false;
        // Только реальные headers: raw Seed/View/Sprite payload не op-коды.
        for(unsigned i=0;i<a->packets[0].header().eventCount;){const auto& e=a->packets[0].events()[i];
            if(e.op()==VideoOp::BgRun||e.op()==VideoOp::Mmc5Span){usedRun=true;break;}
            unsigned slots=1U;
            switch(e.op()){case VideoOp::Mmc5Seed:slots=641U;break;case VideoOp::BgView:slots=6U;break;
            case VideoOp::BgSpan:case VideoOp::SpriteTile:slots=2U;break;
            case VideoOp::SpriteRow:slots=7U;break;default:break;}
            CHECK(slots<=a->packets[0].header().eventCount-i);i+=slots;
        }
        CHECK(usedRun);if(expected>=0)CHECK((Access::rasterTag(a->machine)!=0)==(expected!=0));
        canonicalPrefix(*a,*b);Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        canonicalPrefix(*a,*b);dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));++rasterBulk;a->stop();b->stop();
    }
}
/**
 * @brief Меняет реальные регистры между low/high и сверяет ordinary scalar continuation.
 * @param rom Различимые CHR-банки делают stale descriptor наблюдаемым по данным/RGB.
 * @param expected CLI-признак E3; после следующего M2 cache tag обязан получить новую версию.
 * @details Write начинается на dot20, исполняет20..22 и только затем меняет mapper
 * на dot23, строго между low22 и high24. Эталон использует тот же production MMIO,
 * но admission prepared kernel в его write-M2 временно выключен friend helper.
 */
void rasterMmio(const std::vector<std::uint8_t>& rom,int expected){
    const std::array<std::pair<std::uint16_t,std::uint8_t>,10> writes={{{0x5127,7},{0x512b,9},{0x2000,0x30},{0x2001,0},
        {0x5104,2},{0x5200,0},{0x5130,2},{0x5105,0xff},{0x5106,0x35},{0x5c00,0xc1}}};
    for(unsigned mode=0;mode<4;++mode)for(const auto& write:writes){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17,mode);Access::prepare(b->machine,b->digital,17,mode);
        Access::rasterInputs(a->machine,a->digital,mode,0,0);Access::rasterInputs(b->machine,b->digital,mode,0,0);
        Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        const auto before=Access::rasterVersion(a->digital);
        Access::write(a->machine,write.first,write.second);Access::scalarWrite(b->machine,write.first,write.second);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        CHECK(Access::cpu(a->machine,a->digital)==Access::cpu(b->machine,b->digital));packetsEqual(*a,*b);
        if(write.first!=0x5c00)CHECK(Access::rasterVersion(a->digital)!=before);
        else CHECK(Access::rasterVersion(a->digital)==before);
        Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
        // Rendering-off завершает трёхточечный delayed transition старым tick,
        // поэтому новый tag обязан обновиться лишь при следующем eligible M2.
        if(expected==1&&write.first!=0x2001)CHECK(Access::rasterTag(a->machine)==Access::rasterVersion(a->digital));
        for(unsigned i=0;i<3;++i){Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);}
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));
        canonicalPrefix(*a,*b);dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));++rasterWrites;a->stop();b->stop();
    }
}
/**
 * @brief Проверяет packet rebind и sticky0 fallback на действующем ordinary reader.
 * @param rom Собственная ROM, удерживаемая до stop/cancel обоих владельцев.
 * @param expected Ожидаемый E3 mode; нулевой mapper epoch всегда запрещает cache hit.
 */
void rasterCacheBoundaries(const std::vector<std::uint8_t>& rom,int expected){
    auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
    Access::prepare(a->machine,a->digital,17,3);Access::prepare(b->machine,b->digital,17,3);
    Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
    if(expected>=0)CHECK((Access::rasterTag(a->machine)!=0)==(expected!=0));
    Access::rebind(a->machine);Access::rebind(b->machine);CHECK(Access::rasterTag(a->machine)==0);
    Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
    CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));packetsEqual(*a,*b);
    Access::zeroRasterVersion(a->digital);Access::zeroRasterVersion(b->digital);
    for(unsigned i=0;i<4;++i){Access::ordinary(a->machine,a->digital,3,true);Access::ordinary(b->machine,b->digital,3,false);
        CHECK(Access::rasterTag(a->machine)==0);CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));CHECK(Access::mapper(a->digital)==Access::mapper(b->digital));}
    packetsEqual(*a,*b);dump(Access::ppu(a->machine));dump(Access::mapper(a->digital));a->stop();b->stop();
}
/**
 * @brief Перебирает ровно384 основные phase/length/policy точки арифметического оператора.
 * @param rom Неизменяемый backing, живущий дольше обоих локальных writers.
 * @param expected CLI-признак actual interval,0/OFF либо1/ON, -1 без требования admission.
 * @details FineY/PPUCTRL и armed detector чередуются внутри этой матрицы;
 * отдельные incoming/fault случаи не умножают её на полное декартово произведение.
 */
void rasterIntervalPhases(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<2;++mode)for(unsigned phase=0;phase<8;++phase)for(unsigned dots=1;dots<=24;++dots){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17+phase,mode);Access::prepare(b->machine,b->digital,17+phase,mode);
        Access::rasterInputs(a->machine,a->digital,mode,0,0);Access::rasterInputs(b->machine,b->digital,mode,0,0);
        Access::intervalPattern(a->machine,a->digital,((phase+dots)&1U)?0x10U:0U,(dots&1U)?7U:0U);
        Access::intervalPattern(b->machine,b->digital,((phase+dots)&1U)?0x10U:0U,(dots&1U)?7U:0U);
        if(((phase+dots)&3U)==0U){Access::nearIrq(a->digital);Access::nearIrq(b->digital);}
        intervalApply(*a,dots,expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
        a->stop();b->stop();
    }
}
/**
 * @brief Проверяет debt0/1/7/8/15/16, literal/ROM refs и пустой stale Draw отдельно от основной матрицы.
 * @param rom Собственные ROM-байты со значимыми pattern markers.
 * @param expected Ожидаемый bool feature, не layout macro в test consumer.
 * @details Старый loadBackgroundAtoms разрешает ссылки независимо. Partial mix
 * ROM refs должен остаться fallback, а shift0/8/16 и отсутствие load допускают copy.
 */
void rasterIntervalIncoming(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<2;++mode)for(unsigned shifts:{0U,1U,7U,8U,15U,16U})
    for(bool references:{false,true})for(unsigned kind=0;kind<3;++kind){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        const unsigned phase=kind==2?7U:0U,dots=kind==1?24U:1U;
        Access::prepare(a->machine,a->digital,17+phase,mode);Access::prepare(b->machine,b->digital,17+phase,mode);
        Access::intervalIncoming(a->machine,shifts,references,false);Access::intervalIncoming(b->machine,shifts,references,false);
        const bool mixReject=references&&kind!=2&&shifts!=0&&shifts!=8&&shifts!=16;
        intervalApply(*a,dots,mixReject?0:expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
        a->stop();b->stop();
    }
    for(unsigned mode=0;mode<2;++mode)for(unsigned phase:{0U,3U,7U})for(unsigned dots:{1U,8U,24U}){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17+phase,mode);Access::prepare(b->machine,b->digital,17+phase,mode);
        Access::intervalIncoming(a->machine,8,false,true);Access::intervalIncoming(b->machine,8,false,true);
        intervalApply(*a,dots,expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
        a->stop();b->stop();
    }
}
/**
 * @brief Проверяет Sprite0 вне span, overlap в последнем байте и липкий hit.
 * @param rom Живой CHR backing; scalar выполняет настоящую opacity-проверку.
 * @param expected Ожидаемый feature admission для безопасных случаев.
 */
void rasterIntervalSprite(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned mode=0;mode<2;++mode)for(unsigned dots:{1U,9U,24U})for(unsigned kind=0;kind<3;++kind){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,24,mode);Access::prepare(b->machine,b->digital,24,mode);
        Access::intervalSprite(a->machine,dots,kind!=0,kind==2);Access::intervalSprite(b->machine,dots,kind!=0,kind==2);
        intervalApply(*a,dots,kind==1?0:expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
        a->stop();b->stop();
    }
}
/**
 * @brief Проверяет pure отказы packet/PPU/domain и последующее старое исполнение.
 * @param rom Общая неизменяемая ROM fixtures, не пользовательская игра.
 * @details Снимок отказа включает полную event-арену: speculative raw stores
 * не скрываются за неизменным header.eventCount. Повреждённый metadata не observer-кадр.
 */
void rasterIntervalRejects(const std::vector<std::uint8_t>& rom){
    for(unsigned kind=0;kind<18;++kind){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17,0);Access::prepare(b->machine,b->digital,17,0);
        Access::intervalReject(a->machine,kind);Access::intervalReject(b->machine,kind);
        intervalApply(*a,8,0);Access::scalar(b->machine,8);intervalCompare(*a,*b);
        a->stop();b->stop();
    }
    for(unsigned kind=0;kind<6;++kind){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17,0);Access::prepare(b->machine,b->digital,17,0);
        PacketAccess::reject(a->packets[0],kind);PacketAccess::reject(b->packets[0],kind);
        intervalApply(*a,8,0);Access::scalar(b->machine,8);intervalCompare(*a,*b);
        PacketAccess::recording(a->packets[0]);PacketAccess::recording(b->packets[0]);a->stop();b->stop();
    }
    constexpr std::array<std::array<unsigned,2>,5> geometry={{{17,0},{17,25},{8,3},{254,2},{255,1}}};
    for(const auto shape:geometry){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,shape[0],0);Access::prepare(b->machine,b->digital,shape[0],0);
        intervalApply(*a,shape[1],0);Access::scalar(b->machine,shape[1]);intervalCompare(*a,*b,1);
        a->stop();b->stop();
    }
    auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
    Access::prepare(a->machine,a->digital,17,2);Access::prepare(b->machine,b->digital,17,2);
    intervalApply(*a,24,0);Access::scalar(b->machine,24);intervalCompare(*a,*b);a->stop();b->stop();
}
/**
 * @brief Выводит точный capacity count из старого scalar и проверяет exact/-1 границу.
 * @param rom Общий неизменяемый backing fixtures и временного reference oracle.
 * @param expected Feature admission в случае ровно достаточных credits.
 * @details Тест не повторяет новую формулу phases/records; delta старых legacy
 * credits измеряется настоящим tick до настройки budget новой независимой пары.
 */
void rasterIntervalCapacity(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned phase:{0U,3U,7U})for(unsigned dots:{1U,8U,24U}){
        auto oracle=std::make_unique<Fixture>(rom);Access::prepare(oracle->machine,oracle->digital,17+phase,0);
        const unsigned before=oracle->packets[0].legacyEventCount();Access::scalar(oracle->machine,dots);
        const unsigned required=oracle->packets[0].legacyEventCount()-before;CHECK(required);oracle->stop();
        for(unsigned shortage=0;shortage<2;++shortage){
            auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
            Access::prepare(a->machine,a->digital,17+phase,0);Access::prepare(b->machine,b->digital,17+phase,0);
            PacketAccess::remaining(a->packets[0],required-shortage);PacketAccess::remaining(b->packets[0],required-shortage);
            intervalApply(*a,dots,shortage?0:expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
            a->stop();b->stop();
        }
    }
}
/**
 * @brief Проверяет настоящий MMIO между двумя независимо подготовленными interval chunks.
 * @param rom Различимые CHR-банки делают stale plan/data видимыми в scalar compare.
 * @param expected Ожидаемое E3b выполнение, кроме включения split с обязательным fallback.
 */
void rasterIntervalMmio(const std::vector<std::uint8_t>& rom,int expected){
    constexpr std::array<std::pair<std::uint16_t,std::uint8_t>,6> writes={{{0x5127,7},{0x2000,0x30},
        {0x5104,1},{0x5105,0xff},{0x5c00,0xc1},{0x5200,0x87}}};
    for(unsigned mode=0;mode<2;++mode)for(const auto& write:writes)for(unsigned dots:{1U,9U,24U}){
        auto a=std::make_unique<Fixture>(rom);auto b=std::make_unique<Fixture>(rom);
        Access::prepare(a->machine,a->digital,17,mode);Access::prepare(b->machine,b->digital,17,mode);
        intervalApply(*a,3,expected);Access::scalar(b->machine,3);rawPacketsEqual(*a,*b);
        Access::write(a->machine,write.first,write.second);Access::scalarWrite(b->machine,write.first,write.second);
        CHECK(Access::cpu(a->machine,a->digital)==Access::cpu(b->machine,b->digital));
        intervalApply(*a,dots,write.first==0x5200?0:expected);Access::scalar(b->machine,dots);intervalCompare(*a,*b);
        canonicalPrefix(*a,*b);++intervalMmio;a->stop();b->stop();
    }
}
}

namespace {
unsigned tailSlotCases=0,tailSlotAccepted=0,tailGroupCases=0,tailPureRejects=0,tailFutureCases=0;
/** @brief Сравнивает immediate state/records без flush или нормализации долга.
 * @param a Candidate после возврата sole-owner.
 * @param b Независимый old tick oracle на том же срезе. */
void tailEqual(Fixture& a,Fixture& b){CHECK(Access::ppu(a.machine)==Access::ppu(b.machine));
    CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));rawPacketsEqual(a,b);}
/** @brief Доводит оба old tick oracle через337/339/next0/next1 и следующий scanline IRQ edge.
 * @param a Candidate с уже завершённым новым prefix.
 * @param b Oracle с тем же endpoint; далее оба идут только старым tick.
 * @param lead Положение следующей точки относительно M2 modulo3. */
void tailFuture(Fixture& a,Fixture& b,unsigned lead){
    for(unsigned n:{16U,80U,304U}){
        Access::tailScalarM2(a.machine,a.digital,n,lead);Access::tailScalarM2(b.machine,b.digital,n,lead);
        lead=(lead+n)%3U;tailEqual(a,b);
        CHECK(a.digital.cpuRead(0x5204U,0x15U)==b.digital.cpuRead(0x5204U,0x15U));
        CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));
    }
    for(unsigned i=0;i<4U;++i){a.digital.clockCpu(1U);b.digital.clockCpu(1U);CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));}
    CHECK(!a.digital.inFrame()&&!b.digital.inFrame());++tailFutureCases;
}
/** @brief Проверяет no-mutation false до независимого scalar fallback.
 * @param a Заимствованный fixture; packet arena снимок принадлежит этой функции.
 * @param dots PPU точки для bounded group.
 * @param slot true вызывает непосредственно full8 helper; false — весь held group.
 * @param expected -1 допускает оба ответа;0/1 требуют отказа/actual execution.
 * @return Исходный результат production helper; fallback выполняет caller. */
bool tailApply(Fixture& a,unsigned dots,bool slot,int expected,Mmc5* foreign=nullptr){
    const auto p=Access::ppu(a.machine),m=Access::mapper(a.digital),h=packetFields(a.packets[0]);
    std::vector<VideoEvent> arena(a.packets[0].events(),a.packets[0].events()+VideoPacket::eventCapacity);
    const bool used=slot?Access::tailSlot(a.machine,a.digital):Access::tailGroup(a.machine,foreign?*foreign:a.digital,dots);
    if(expected>=0)CHECK(used==(expected!=0));
    if(!used){CHECK(Access::ppu(a.machine)==p);CHECK(Access::mapper(a.digital)==m);CHECK(packetFields(a.packets[0])==h);
        CHECK(std::memcmp(arena.data(),a.packets[0].events(),arena.size()*sizeof(VideoEvent))==0);}
    return used;
}
/** @brief Все8 slots/phases/lengths и63dot swallowing case используют независимый old tick.
 * @param rom Владеющая собственная ROM, живая дольше всех fixtures.
 * @param expected -1 допускает OFF/ON;0/1 доказывают actual group activation. */
void tailGroups(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned slot=0;slot<8U;++slot)for(unsigned phase=0;phase<8U;++phase)for(unsigned n=1;n<=16U;++n){
        const unsigned dot=257U+slot*8U+phase;if(dot+n>321U)continue;
        auto a=std::make_unique<Fixture>(rom),b=std::make_unique<Fixture>(rom);
        Access::tailPrepare(a->machine,a->digital,dot,(slot+phase+n)*13U);Access::tailPrepare(b->machine,b->digital,dot,(slot+phase+n)*13U);
        const bool used=tailApply(*a,n,false,dot==257U?0:expected);if(!used)Access::scalar(a->machine,n);
        Access::scalar(b->machine,n);tailEqual(*a,*b);++tailGroupCases;
        if(phase==0U&&n==8U)tailFuture(*a,*b,0U);
        Access::tailStopPrepare(a->machine);Access::tailStopPrepare(b->machine);a->stop();b->stop();
    }
    for(const auto shape:std::array<std::array<unsigned,2>,7>{{{{258,63}},{{259,62}},{{260,61}},{{265,56}},{{266,55}},{{313,8}},{{320,1}}}}){
        auto a=std::make_unique<Fixture>(rom),b=std::make_unique<Fixture>(rom);
        Access::tailPrepare(a->machine,a->digital,shape[0],255U);Access::tailPrepare(b->machine,b->digital,shape[0],255U);
        if(!tailApply(*a,shape[1],false,expected)){Access::scalar(a->machine,shape[1]);}
        Access::scalar(b->machine,shape[1]);tailEqual(*a,*b);
        tailFuture(*a,*b,0U);++tailGroupCases;
        Access::tailStopPrepare(a->machine);Access::tailStopPrepare(b->machine);a->stop();b->stop();
    }
}
/** @brief Полный слот проверяет все A/B CHR modes, OAM flips/heights и три M2 lead.
 * @param rom Неизменяемая собственная ROM с различимыми CHR страницами.
 * @param expected -1 допускает OFF/ON;0/1 доказывают actual full8 execution. */
void tailSlots(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned slot=0;slot<8U;++slot)for(unsigned style=0;style<256U;style+=5U)for(unsigned lead=0;lead<3U;++lead){
        auto a=std::make_unique<Fixture>(rom),b=std::make_unique<Fixture>(rom);
        const unsigned dot=257U+slot*8U;Access::tailPrepare(a->machine,a->digital,dot,style);Access::tailPrepare(b->machine,b->digital,dot,style);
        const bool used=tailApply(*a,8U,true,slot?expected:0);if(used)++tailSlotAccepted;else Access::tailScalarM2(a->machine,a->digital,8U,lead);
        Access::tailScalarM2(b->machine,b->digital,8U,lead);tailEqual(*a,*b);++tailSlotCases;
        if(style%25U==0U)tailFuture(*a,*b,(lead+8U)%3U);
        Access::tailStopPrepare(a->machine);Access::tailStopPrepare(b->machine);a->stop();b->stop();
    }
}
/** @brief Неподходящие detector/held/render/backing состояния отвергаются без единой записи.
 * @param rom Владеющая ROM для rejection fixtures; дополнительные boundary ROM локальны. */
void tailRejects(const std::vector<std::uint8_t>& rom){
    for(unsigned kind=0;kind<15U;++kind){
        auto a=std::make_unique<Fixture>(rom);Access::tailPrepare(a->machine,a->digital,265U,128U);Access::tailReject(a->machine,a->digital,kind);
        CHECK(!tailApply(*a,8U,true,0));++tailPureRejects;Access::tailStopPrepare(a->machine);a->stop();
    }
    for(unsigned kind=0;kind<17U;++kind){
        auto a=std::make_unique<Fixture>(rom);Access::tailPrepare(a->machine,a->digital,258U,7U);Access::tailGroupReject(a->machine,kind);
        CHECK(!tailApply(*a,63U,false,0));++tailPureRejects;Access::tailStopPrepare(a->machine);a->stop();
    }
    for(unsigned dot:{257U,321U,329U,337U}){
        auto a=std::make_unique<Fixture>(rom);Access::tailPrepare(a->machine,a->digital,dot,9U);
        CHECK(!tailApply(*a,8U,false,0));++tailPureRejects;Access::tailStopPrepare(a->machine);a->stop();
    }
    auto a=std::make_unique<Fixture>(rom);Access::tailPrepare(a->machine,a->digital,265U,255U);
    for(unsigned address=0;address<0x2000U;++address){
        std::uint8_t lo=0x5aU,hi=0xa5U;const auto before=Access::mapper(a->digital);
        const bool ok=Access::tailPlanes(a->digital,std::uint16_t(address),lo,hi);
        CHECK(ok==((address&8U)==0U));CHECK(Access::mapper(a->digital)==before);
        if(!ok){CHECK(lo==0x5aU&&hi==0xa5U);}else{
            CHECK(lo==a->digital.ppuRead(std::uint16_t(address),Mmc5::Fetch::SpritePattern,nullptr,0).value);
            CHECK(hi==a->digital.ppuRead(std::uint16_t(address+8U),Mmc5::Fetch::SpritePattern,nullptr,0).value);
        }
    }
    for(unsigned address:{0x2000U,0x3f00U,0xffffU}){std::uint8_t lo=0x5aU,hi=0xa5U;
        CHECK(!Access::tailPlanes(a->digital,std::uint16_t(address),lo,hi));CHECK(lo==0x5aU&&hi==0xa5U);}
    Access::tailStopPrepare(a->machine);a->stop();
    std::array<std::uint8_t,8192> prg{};
    for(std::size_t bytes:{std::size_t(1024U),std::size_t(1024U*1024U)}){
        std::vector<std::uint8_t> chr(bytes);for(std::size_t i=0;i<bytes;++i)chr[i]=static_cast<std::uint8_t>(i*29U+(i>>10U)*37U);
        Mmc5 digital;CHECK(digital.attach(prg.data(),prg.size(),chr.data(),chr.size(),nullptr,0));
        for(unsigned mode=0;mode<4U;++mode){
            digital.cpuWrite(0x5101U,std::uint8_t(mode));digital.cpuWrite(0x5130U,3U);
            for(unsigned i=0;i<8U;++i)digital.cpuWrite(std::uint16_t(0x5120U+i),std::uint8_t(i*17U+13U));
            for(unsigned i=0;i<4U;++i)digital.cpuWrite(std::uint16_t(0x5128U+i),std::uint8_t(i*23U+71U));
            for(unsigned address=0;address<0x2000U;++address)if(!(address&8U)){
                std::uint8_t lo=0,hi=0;const auto before=Access::mapper(digital);
                CHECK(Access::tailPlanes(digital,std::uint16_t(address),lo,hi));CHECK(Access::mapper(digital)==before);
                CHECK(lo==digital.ppuRead(std::uint16_t(address),Mmc5::Fetch::SpritePattern,nullptr,0).value);
                CHECK(hi==digital.ppuRead(std::uint16_t(address+8U),Mmc5::Fetch::SpritePattern,nullptr,0).value);
            }
        }
    }
}
/** @brief Сверяет packet/owner guard и прежний fallback; saturated credits не требуют новых записей.
 * @param rom Неизменяемая собственная ROM; каждый pair имеет отдельные writer/mapper ресурсы.
 * @param expected OFF/ON для допустимого saturated Recording, остальные случаи обязаны отказать. */
void tailPacketGuards(const std::vector<std::uint8_t>& rom,int expected){
    for(unsigned kind=0;kind<6U;++kind){
        auto a=std::make_unique<Fixture>(rom),b=std::make_unique<Fixture>(rom),foreign=std::make_unique<Fixture>(rom);
        Access::tailPrepare(a->machine,a->digital,258U,67U);Access::tailPrepare(b->machine,b->digital,258U,67U);
        if(kind==0U){PacketAccess::reject(a->packets[0],0U);PacketAccess::reject(b->packets[0],0U);}
        if(kind==1U){PacketAccess::reject(a->packets[0],3U);PacketAccess::reject(b->packets[0],3U);}
        if(kind==2U){Access::tailGroupReject(a->machine,15U);Access::tailGroupReject(b->machine,15U);}
        if(kind==3U){Access::tailGroupReject(a->machine,16U);Access::tailGroupReject(b->machine,16U);}
        if(kind==4U){PacketAccess::remaining(a->packets[0],0U);PacketAccess::remaining(b->packets[0],0U);}
        const auto foreignBefore=Access::mapper(foreign->digital);
        const bool used=tailApply(*a,63U,false,kind==4U?expected:0,kind==5U?&foreign->digital:nullptr);
        if(!used){Access::scalar(a->machine,63U);}Access::scalar(b->machine,63U);tailEqual(*a,*b);
        CHECK(Access::mapper(foreign->digital)==foreignBefore);
        CHECK(packetFields(a->packets[1])==packetFields(b->packets[1]));
        CHECK(std::memcmp(a->packets[1].events(),b->packets[1].events(),VideoPacket::eventCapacity*sizeof(VideoEvent))==0);
        if(kind==0U){PacketAccess::recording(a->packets[0]);PacketAccess::recording(b->packets[0]);}
        Access::tailStopPrepare(a->machine);Access::tailStopPrepare(b->machine);a->stop();b->stop();foreign->stop();++tailGroupCases;
    }
}
/**
 * @brief Сравнивает350 MMC3 debt-срезов с независимым прежним prefix/rest эталоном.
 * @details Проверяет visible/sprite/prefetch/строчную границу, ноль/один legacy
 * credit, прежнюю ошибку и Sealed. Полная арена сравнивается до продолжения;
 * нормальный writer получает реальный PPUADDR MMIO, затем ещё3+17 scalar-точек.
 * PRIVATE-флаг намеренно не виден consumer: обе политики обязаны сохранить
 * те же PPU, budget/IRQ forecasts и packet/legacy credits. Активация нового
 * branch отдельно подтверждается source/target-проверкой, без счётчика в Core.
 * @return Число завершённых случаев; первый CHECK прекращает процесс при различии.
 */
unsigned ordinaryDebtJoins(){
    auto rom=romImage();rom[6]=0x40; // Собственная прежняя ROM выбирает mapper4.
    unsigned cases=0;
    for(unsigned dot:{18U,19U,22U,23U,24U,63U,248U,252U,255U,258U,319U,321U,329U,337U})
    for(unsigned dots:{1U,3U,8U,17U,40U})for(unsigned fault:{0U,1U,2U,3U,4U}){
        auto a=std::make_unique<Fixture>(rom),b=std::make_unique<Fixture>(rom);
        Access::prepareOrdinaryDebt(a->machine,a->digital,dot,dots);
        Access::prepareOrdinaryDebt(b->machine,b->digital,dot,dots);
        if(fault==1U){PacketAccess::remaining(a->packets[0],0U);PacketAccess::remaining(b->packets[0],0U);}
        if(fault==2U){PacketAccess::remaining(a->packets[0],1U);PacketAccess::remaining(b->packets[0],1U);}
        if(fault==3U){PacketAccess::reject(a->packets[0],3U);PacketAccess::reject(b->packets[0],3U);}
        if(fault==4U){PacketAccess::reject(a->packets[0],0U);PacketAccess::reject(b->packets[0],0U);}
        Access::flushOrdinaryDebt(a->machine);Access::flushOrdinaryDebtReference(b->machine);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));
        CHECK(Access::ordinaryDebtState(a->machine)==Access::ordinaryDebtState(b->machine));
        rawPacketsEqual(*a,*b); // Включает legacy credits, metadata и опубликованные records.
        CHECK(std::memcmp(a->packets[0].events(),b->packets[0].events(),
            VideoPacket::eventCapacity*sizeof(VideoEvent))==0);
        if(!fault){Access::ordinaryDebtMmio(a->machine);Access::ordinaryDebtMmio(b->machine);}
        Access::scalar(a->machine,3U);Access::scalar(b->machine,3U);
        Access::scalar(a->machine,17U);Access::scalar(b->machine,17U);
        CHECK(Access::ppu(a->machine)==Access::ppu(b->machine));
        CHECK(Access::ordinaryDebtState(a->machine)==Access::ordinaryDebtState(b->machine));
        rawPacketsEqual(*a,*b);
        dump(Access::ppu(a->machine));dump(Access::ordinaryDebtState(a->machine));dump(packetFields(a->packets[0]));
        if(fault==4U){PacketAccess::recording(a->packets[0]);PacketAccess::recording(b->packets[0]);}
        a->stop();b->stop();++cases;
    }
    CHECK(cases==350U);return cases;
}
} // namespace

/**
 * @brief Запускает directed correctness gates; явные ожидания доказывают actual activation флагов.
 * @param argc 1 — без ожиданий; далее PHASE/RETAINED, canonical и --raster-plan/--raster-interval 0|1.
 * @param argv Числовые ожидания строго0 либо1; canonical открывается и закрывается только этим host-тестом.
 * @return Успешное завершение процесса после всех проверок; первый отказ CHECK прекращает процесс с кодом1.
 */
int main(int argc,char** argv){
    CHECK(argc==1||argc>=3);int phase=-1,retained=-1,raster=-1,interval=-1,tail=-1;
    if(argc>=3){phase=std::atoi(argv[1]);retained=std::atoi(argv[2]);CHECK((phase==0||phase==1)&&(retained==0||retained==1));}
    for(int i=3;i<argc;++i){
        if(std::strcmp(argv[i],"--tail-slots")==0){CHECK(i+1<argc);tail=std::atoi(argv[++i]);CHECK(tail==0||tail==1);}
        else if(std::strcmp(argv[i],"--raster-plan")==0){CHECK(i+1<argc);raster=std::atoi(argv[++i]);CHECK(raster==0||raster==1);}
        else if(std::strcmp(argv[i],"--raster-interval")==0){CHECK(i+1<argc);interval=std::atoi(argv[++i]);CHECK(interval==0||interval==1);}
        else {CHECK(!canonical);canonical=std::fopen(argv[i],"wb");CHECK(canonical);}
    }
    const unsigned ordinaryDebtCases=ordinaryDebtJoins();
    const auto tailRom=rasterRomImage();tailGroups(tailRom,tail);tailSlots(tailRom,tail);tailRejects(tailRom);tailPacketGuards(tailRom,tail);
    const auto rom=romImage();phases(rom,phase);positiveEdges(rom,phase);rejects(rom,phase,interval);
    const auto rasterRom=rasterRomImage();rasterOrdinary(rasterRom,raster);rasterRightEdge(rasterRom,raster);rasterRuns(rasterRom,raster);
    rasterMmio(rasterRom,raster);rasterCacheBoundaries(rasterRom,raster);
    rasterIntervalPhases(rasterRom,interval);rasterIntervalIncoming(rasterRom,interval);rasterIntervalSprite(rasterRom,interval);
    rasterIntervalRejects(rasterRom);rasterIntervalCapacity(rasterRom,interval);rasterIntervalMmio(rasterRom,interval);
    for(unsigned shape=0;shape<3;++shape){const auto program=romImage(shape);
        for(unsigned budget:{8U,80U})for(bool writable:{false,true}){
            if(shape==2&&(budget!=8U||!writable))continue;
            auto a=std::make_unique<Fixture>(program,writable);auto b=std::make_unique<Fixture>(program,writable);
            Access::prepare(a->machine,a->digital,41,0);Access::prepare(b->machine,b->digital,41,0);
            Access::clockSeed(a->digital,0);Access::clockSeed(b->digital,0);
            if(shape==2)Access::exhausted(a->machine,b->machine,a->digital,b->digital);
            else Access::epoch(a->machine,b->machine,a->digital,b->digital,retained,shape?0U:2U,budget,writable);
            canonicalPrefix(*a,*b);a->stop();b->stop();
        }
    }
    if(canonical)CHECK(std::fclose(canonical)==0);
    std::printf("ORDINARY_DEBT_JOIN_PASS cases=%u\n",ordinaryDebtCases);
    std::printf("MMC5_TAIL_SLOT_PASS slots=%u accepted=%u groups=%u rejects=%u future=%u\n",tailSlotCases,tailSlotAccepted,tailGroupCases,tailPureRejects,tailFutureCases);
    std::printf("EVENT_KERNEL_PASS checks=%u phase_attempts=%u phase_accepted=%u retained_observed=%u\n",checks,phaseAttempts,phaseAccepted,retainedObserved);
    std::printf("MMC5_RASTER_KERNEL_PASS ordinary_m2=%u bulk=%u mmio=%u cache_reused=%u\n",rasterM2,rasterBulk,rasterWrites,rasterCacheHits);
    std::printf("MMC5_RASTER_INTERVAL_PASS attempts=%u accepted=%u rejected=%u mmio=%u\n",intervalAttempts,intervalAccepted,intervalRejected,intervalMmio);
}
