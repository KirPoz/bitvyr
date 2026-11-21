/**
 * @file MachineSignalFused.inl
 * @brief Частный pure planner завершённого MMC5 Signals интервала в единицах M2.
 * @details Включается внутри nes::core только Machine.cpp. Не читает SDK и не
 * меняет primary до commit; CPU admission и actual-prefix materializer используют
 * одну функцию. Для IRQ/NMI границы возвращается только уже доказанный префикс.
 */


/**
 * @details Стабильные BG/sprite/NV части сворачиваются без per-M2 loop.
 * Только короткий непересекающийся boundary выполняет literal digitalM2+3dot FSM.
 * Четвёртый NT /RD и idle3 меняют полный copied detector, включая latent $5204.
 * M2 с IRQ-rise, NMI/frame/oddskip событием не включается в разрешённый префикс.
 */
template<bool KnownSafe>
unsigned Machine::materializeThinMmc5Signals(unsigned requested,ThinMmc5SignalPlan&out)const noexcept {
    static_assert(sizeof(ThinMmc5SignalPlan)<=64U,"Keep two local signal snapshots within128 bytes");
    const auto&p=timingPpu_;const auto&d=*cartridge_.mmc5_;
    ThinMmc5SignalPlan q;
    q.v=p.v_;q.address=p.busAddress_;q.dot=p.dot_;q.row=p.scanline_;q.evaluation=p.evaluationNextDot_;
    q.nextTile=p.nextTile_;q.oam=p.oamAddress_;q.coarse=p.coarseXSignal_;q.scroll=p.scrollSignal_;q.odd=p.odd_;
    q.last=d.lastPpuAddress_;q.repeat=d.nameRepeat_;q.idle=d.cpuIdle_;q.read=d.ppuReadSinceM2_;
    q.inFrame=d.inFrame_;q.pending=d.irqPending_;q.scanline=d.scanline_;q.splitY=d.splitY_;
    q.extended=d.tileExtended_;q.split=d.tileSplit_;
    const bool enabled=KnownSafe||p.renderingEnabled_;
    /** @brief Повторяет clockCpu timeout, не подтверждая pending IRQ latch. */
    [[maybe_unused]] const auto clearIdle=[](ThinMmc5SignalPlan&s)noexcept{
        s.inFrame=false;s.last=0xffffU;s.repeat=0;s.extended=s.split=false;
    };
    /** @brief Полный endFrame перед обновлением текущего /RD. */
    [[maybe_unused]] const auto endFrame=[&](ThinMmc5SignalPlan&s)noexcept{
        s.inFrame=s.pending=false;s.scanline=0;s.last=0xffffU;s.repeat=0;
        s.splitY=d.splitScroll_;s.extended=s.split=false;
    };
    /** @brief Применяет один RD к copied detector; false запрещает пересечь IRQ-rise. */
    const auto rd=[&](ThinMmc5SignalPlan&s,unsigned address)noexcept{
        if constexpr(!KnownSafe){address&=0x3fffU;if(address>=0x3f00U)return true;}
        s.read=true;
        if(s.repeat==3U){
            if constexpr(KnownSafe){
                ++s.scanline;s.splitY=s.splitY==239U?0U:static_cast<std::uint8_t>(s.splitY+1U);
                if(d.irqCompare_&&s.scanline==d.irqCompare_)s.pending=true;
            }else{
            if(!s.inFrame){s.inFrame=true;s.scanline=0;s.pending=false;s.splitY=d.splitScroll_;}
            else if(s.scanline==239U)endFrame(s);
            else{++s.scanline;s.splitY=s.splitY==239U?0U:static_cast<std::uint8_t>(s.splitY+1U);if(d.irqCompare_&&s.scanline==d.irqCompare_)s.pending=true;}
            }
            s.repeat=0;
            if constexpr(!KnownSafe){if(d.irqEnabled_&&s.pending)return false;}
        }
        s.repeat=address>=0x2000U&&address<0x3000U?static_cast<std::uint8_t>(address==s.last?s.repeat+1U:1U):0U;
        s.last=static_cast<std::uint16_t>(address);return true;
    };
    /** @brief Сохраняет coarseX wrap и горизонтальную таблицу. */
    const auto incX=[](ThinMmc5SignalPlan&s)noexcept{
        if((s.v&31U)==31U)s.v=static_cast<std::uint16_t>((s.v&0x7fe0U)^0x400U);else ++s.v;
    };
    /** @brief Сохраняет fineY/coarseY29/31 и вертикальную таблицу. */
    const auto incY=[](ThinMmc5SignalPlan&s)noexcept{
        if((s.v&0x7000U)!=0x7000U)s.v=static_cast<std::uint16_t>(s.v+0x1000U);
        else{unsigned y=(s.v>>5U)&31U;s.v&=0x0fffU;if(y==29U){y=0;s.v^=0x800U;}else if(y==31U)y=0;else ++y;s.v=static_cast<std::uint16_t>((s.v&0x7c1fU)|(y<<5U));}
    };
    /** @brief Исполняет одну literal Signals dot, кроме объявленных hard frontiers. */
    const auto dot=[&](ThinMmc5SignalPlan&s)noexcept{
        if constexpr(!KnownSafe){
        if((s.row==-1&&s.dot==1U)||(s.row==241&&s.dot==1U)||
           (s.row==-1&&s.dot==339U&&s.odd&&enabled))return false;
        }
        if((KnownSafe||unsigned(s.row)<240U)&&s.dot==0U)s.evaluation=1;
        const bool active=KnownSafe||(enabled&&s.row>=-1&&s.row<240);
        const bool bg=(s.dot>=1U&&s.dot<=256U)||(s.dot>=321U&&s.dot<=336U);
        if(s.dot==255U)s.scroll=active;
        if(!active&&bg&&((s.dot-1U)&7U)==6U)s.coarse=false;
        if(active){
            if(s.dot==0U&&(KnownSafe||s.row>=0))s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U));
            if(bg)switch((s.dot-1U)&7U){
            case 0:s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));break;
            case 1:if(!rd(s,s.address))return false;s.nextTile=0;break;
            case 2:s.address=static_cast<std::uint16_t>(0x23c0U|(s.v&0xc00U)|((s.v>>4U)&0x38U)|((s.v>>2U)&7U));break;
            case 3:if(!rd(s,s.address))return false;s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U));break;
            case 4:s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U));break;
            case 5:if(!rd(s,s.address))return false;break;
            case 6:s.coarse=true;if(s.dot==255U)s.scroll=true;s.address=static_cast<std::uint16_t>((s.address+8U)&0x3fffU);break;
            case 7:if(!rd(s,s.address))return false;if(s.coarse)incX(s);s.coarse=false;break;
            }
            if(s.dot==257U){if(s.scroll)s.v=static_cast<std::uint16_t>((s.v&0x7be0U)|(p.t_&0x41fU));s.scroll=false;}
            if constexpr(!KnownSafe){if(s.row==-1&&s.dot>=280U&&s.dot<=304U)s.v=static_cast<std::uint16_t>((s.v&0x41fU)|(p.t_&0x7be0U));}
            if(s.dot>=257U&&s.dot<=320U){
                s.oam=0;
                switch((s.dot-257U)&7U){
                case 0:case 2:s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));break;
                case 3:if(!rd(s,s.address))return false;s.address=0;break;
                case 4:s.address=0;break;
                case 1:case 5:case 7:if(!rd(s,s.address))return false;break;
                case 6:s.address=8;break;
                }
            }
            if(s.dot==337U||s.dot==339U)s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));
            if((s.dot==338U||s.dot==340U)&&!rd(s,s.address))return false;
        }else s.address=s.v&0x3fffU;
        if(!active&&bg&&((s.dot-1U)&7U)==7U&&s.coarse){incX(s);s.coarse=false;}
        if(s.dot==256U&&s.scroll)incY(s);
        if(!active&&s.dot==257U){if(s.scroll)s.v=static_cast<std::uint16_t>((s.v&0x7be0U)|(p.t_&0x41fU));s.scroll=false;}
        if(!active&&unsigned(s.row)<240U&&s.dot>=1U&&s.dot<=256U)s.evaluation=static_cast<std::uint16_t>(s.dot+1U);
        if(++s.dot>340U){s.dot=0;++s.row;if constexpr(!KnownSafe){if(s.row>260){s.row=-1;s.odd=!s.odd;}}}
        return true;
    };
    /** @brief Сворачивает inactive digital M2 и фиксирует единственный idle3 timestamp. */
    const auto idleM2=[&](ThinMmc5SignalPlan&s,unsigned n)noexcept{
        unsigned left=n,offset=0;
        if(s.read){s.idle=0;s.read=false;--left;offset=1;}
        const unsigned room=3U-s.idle;
        if(s.idle<3U&&left>=room){if(s.idleOffset)return false;s.idleOffset=s.elapsed+offset+room;}
        s.idle=static_cast<std::uint8_t>(left>=room?3U:s.idle+left);
        if(s.idle==3U)clearIdle(s);
        return true;
    };
    /**
     * @brief Исполняет короткий BG/sprite prefix/tail в доказанном rendered span.
     * @param s Только copied plan текущего trial; false не публикует его caller.
     * @param count Число 1..8 dots, не больше остатка до следующего aligned tile.
     * @param sprite true выбирает sprite258..320, false BG1..254/321..336.
     * @return false на прежнем RD IRQ-rise; порядок его частичных effects сохранён.
     * @pre enabled, visible row и границы end255/321/337 уже доказаны caller.
     * @details Никаких M2/NMI/rowcross/255/256/257/337 внутри. Начальный drive
     * может быть произвольным: первый RD читает именно s.address до нового drive.
     * После каждого успешного phase dot увеличивается ровно один раз. Снимок
     * при отказе остаётся локальным trial; прежний outer FSM находит safe prefix.
     */
    const auto renderedFragment=[&](ThinMmc5SignalPlan&s,unsigned count,bool sprite)noexcept{
        if(sprite){
            switch((s.dot-257U)&7U){
            case 0:s.oam=0;s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));++s.dot;if(!--count)return true;[[fallthrough]];
            case 1:s.oam=0;if(!rd(s,s.address))return false;++s.dot;if(!--count)return true;[[fallthrough]];
            case 2:s.oam=0;s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));++s.dot;if(!--count)return true;[[fallthrough]];
            case 3:s.oam=0;if(!rd(s,s.address))return false;s.address=0;++s.dot;if(!--count)return true;[[fallthrough]];
            case 4:s.oam=0;s.address=0;++s.dot;if(!--count)return true;[[fallthrough]];
            case 5:s.oam=0;if(!rd(s,s.address))return false;++s.dot;if(!--count)return true;[[fallthrough]];
            case 6:s.oam=0;s.address=8;++s.dot;if(!--count)return true;[[fallthrough]];
            case 7:s.oam=0;if(!rd(s,s.address))return false;++s.dot;return true;
            }
        }else{
            switch((s.dot-1U)&7U){
            case 0:s.address=static_cast<std::uint16_t>(0x2000U|(s.v&0xfffU));++s.dot;if(!--count)return true;[[fallthrough]];
            case 1:if(!rd(s,s.address))return false;s.nextTile=0;++s.dot;if(!--count)return true;[[fallthrough]];
            case 2:s.address=static_cast<std::uint16_t>(0x23c0U|(s.v&0xc00U)|((s.v>>4U)&0x38U)|((s.v>>2U)&7U));++s.dot;if(!--count)return true;[[fallthrough]];
            case 3:if(!rd(s,s.address))return false;s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U));++s.dot;if(!--count)return true;[[fallthrough]];
            case 4:s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U));++s.dot;if(!--count)return true;[[fallthrough]];
            case 5:if(!rd(s,s.address))return false;++s.dot;if(!--count)return true;[[fallthrough]];
            case 6:s.coarse=true;s.address=static_cast<std::uint16_t>((s.address+8U)&0x3fffU);++s.dot;if(!--count)return true;[[fallthrough]];
            case 7:if(!rd(s,s.address))return false;if(s.coarse)incX(s);s.coarse=false;++s.dot;return true;
            }
        }
        return true; // Все phase0..7 исчерпывают bounded count; это не общий dot.
    };
    /** @brief Стабильные rendered dots: short edges + O1 fulltiles, без M2 wrapper. */
    const auto rendered=[&](ThinMmc5SignalPlan&s,unsigned count,bool sprite)noexcept{
        unsigned left=count;
        const unsigned prefix=std::min(left,(1U-s.dot)&7U);
        if(prefix){if(!renderedFragment(s,prefix,sprite))return false;left-=prefix;}
        // Bad-head сохраняет все 8 phase effects/4 RD: сокращается только общий
        // geometry/frontier dispatch. Другой trial/IRQ policy здесь не вводится.
        if(left>=8U&&s.repeat){if(!renderedFragment(s,8U,sprite))return false;left-=8U;}
        const unsigned tiles=left/8U;
        if(tiles){
            if(sprite){s.address=8;s.last=8;s.oam=0;}
            else{
                const unsigned x=(s.v&31U)+tiles;
                s.v=static_cast<std::uint16_t>((s.v&~31U)|(x&31U));if((x/32U)&1U)s.v^=0x400U;
                s.address=static_cast<std::uint16_t>(((p.control_&0x10U)?0x1000U:0U)+((s.v>>12U)&7U)+8U);
                s.last=s.address;s.nextTile=0;s.coarse=false;
            }
            s.repeat=0;s.read=true;s.dot=static_cast<std::uint16_t>(s.dot+8U*tiles);left-=8U*tiles;
        }
        if(left&&!renderedFragment(s,left,sprite))return false;
        s.idle=0;s.read=true;return true;
    };
    while(q.elapsed<requested){
        if constexpr(KnownSafe){
            // Event-free certificate не готовый endpoint: loopy/RD/detector всё ещё исполняются.
            unsigned end=0;bool sprite=false;
            if(q.dot>=1U&&q.dot<255U)end=255U;
            else if(q.dot>=258U&&q.dot<321U){end=321U;sprite=true;}
            else if(q.dot>=321U&&q.dot<337U)end=337U;
            const unsigned n=end?std::min(requested-q.elapsed,(end-q.dot)/3U):0U;
            if(n>=2U){static_cast<void>(rendered(q,n*3U,sprite));q.elapsed+=n;continue;}
            // Каждый 3dot window имеет nonpaletteRD: postM2 idle0/readtrue; idle3 невозможен.
            q.idle=0;q.read=false;
            static_cast<void>(dot(q));static_cast<void>(dot(q));static_cast<void>(dot(q));
            ++q.elapsed;continue;
        }
        unsigned end=0;bool sprite=false;
        // Palette /RD не ставит P; послеCPU2006 literalM2 восстанавливает normal drive.
        if(enabled&&q.row>=0&&q.row<240&&q.read&&q.address<0x3000U){
            if(q.dot>=1U&&q.dot<255U)end=255U;
            else if(q.dot>=258U&&q.dot<321U){end=321U;sprite=true;}
            else if(q.dot>=321U&&q.dot<337U)end=337U;
        }
        const unsigned n=end?std::min(requested-q.elapsed,(end-q.dot)/3U):0U;
        if(n>=2U){
            auto trial=q;
            if(rendered(trial,n*3U,sprite)){trial.elapsed+=n;q=trial;continue;}
            // Предсказанный IRQ может лежать внутри span. Boundary FSM вернёт только safe prefix.
        }
        const bool inactive=!enabled||q.row>=240;
        if(inactive&&!q.coarse&&!q.scroll&&q.address==(q.v&0x3fffU)){
            unsigned dotsToEvent=341U-q.dot;
            if(q.row==241&&q.dot<=1U)dotsToEvent=1U-q.dot;
            if(q.row==-1&&q.dot<=1U)dotsToEvent=1U-q.dot;
            const unsigned nIdle=std::min(requested-q.elapsed,dotsToEvent/3U);
            if(nIdle>=2U){
                auto trial=q;
                if(!idleM2(trial,nIdle))break;
                const unsigned next=trial.dot+3U*nIdle;
                if(unsigned(trial.row)<240U&&trial.dot<=256U)trial.evaluation=static_cast<std::uint16_t>(std::min(257U,next));
                trial.address=trial.v&0x3fffU;trial.dot=static_cast<std::uint16_t>(next%341U);trial.row+=static_cast<std::int16_t>(next/341U);
                if(trial.row>260){trial.row=-1;trial.odd=!trial.odd;}
                trial.elapsed+=nIdle;q=trial;continue;
            }
        }
        auto trial=q;
        if(!idleM2(trial,1U))break;
        if(!dot(trial)||!dot(trial)||!dot(trial))break;
        ++trial.elapsed;q=trial;
    }
    out=q;return q.elapsed;
}
/** @details Прежний ABI entry всегда использует полный generic planner с отказом/rollback. */
unsigned Machine::planThinMmc5Signals(unsigned requested,ThinMmc5SignalPlan&out)const noexcept{
    return materializeThinMmc5Signals<false>(requested,out);
}
