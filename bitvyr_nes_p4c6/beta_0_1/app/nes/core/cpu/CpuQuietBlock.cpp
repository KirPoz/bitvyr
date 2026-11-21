/**
 * @file CpuQuietBlock.cpp
 * @brief Исполнение уже допущенных quiet-инструкций с локальными регистрами и точной публикацией.
 * @ingroup nes_cpu
 */
#include "Machine.h"
#include "RomWindow.h"
// Проверка actual count использует только существующий FUSED prepare-before-apply.
// Только локальный codegen: guest-переходы, память и Repeat-журнал не меняются.
#define NES_CPU_SIZE_PEEK_INLINE
#include <type_traits>
namespace nes::core {
namespace {
/** @brief Узкий план неизменяемой six-op петли; все адреса — CPU virtual, не указатели. */
struct Mmc5AffineLoopPlan final {
    std::uint16_t counter=0,stable=0,store=0; ///< ВнутренняяRAM; physical индексы получаются через&0x7ff.
    unsigned cycles=0; ///< Реальная стоимость полного периода в M2.
    int direction=1; ///< +1 для INC,−1 для DEC до последующего ADC.
};
/** @brief Итог только изменяемого байта/A и ADC flags; Machine clocks сюда не входят. */
struct Mmc5AffineValue final {std::uint8_t value=0,status=0;};

/**
 * @brief Декодирует один direct Zp/Abs operand без speculative выхода за ROM range.
 * @param code,bytes Заимствованный immutable code range текущего strict grant.
 * @param cursor Смещение opcode; при успехе указывает на следующую инструкцию.
 * @param zp,absolute Разрешённые official opcodes одного семейства.
 * @param address,cycles Результат: virtual address и read/store стоимость 3/4M2.
 * @return false при другом opcode/обрезанном operand; гостевое состояние не меняется.
 */
static bool readMmc5AffineOperand(const std::uint8_t*code,unsigned bytes,unsigned&cursor,
    unsigned zp,unsigned absolute,std::uint16_t&address,unsigned&cycles)noexcept {
    if(cursor>=bytes)return false;
    const auto op=code[cursor++];
    if(op==zp){if(cursor>=bytes)return false;address=code[cursor++];cycles=3;return true;}
    if(op!=absolute||bytes-cursor<2U)return false;
    address=static_cast<std::uint16_t>(unsigned(code[cursor])|(unsigned(code[cursor+1U])<<8U));cursor+=2U;cycles=4;return true;
}

/**
 * @brief Проверяет generic INC/DEC p;CLC;LDA p|q;ADC q|p;STA p;JMP head.
 * @param code,bytes Полный range12..16 байт одной уже квалифицированной physicalROM page.
 * @param head Настоящий CPU target текущего JMP; имя ROM/CRC/PC-константы не используются.
 * @param out Получает plan только после всех grammar/physical alias/range проверок.
 * @return false сохраняет out и требует прежний decoder.
 * @pre Caller квалифицировал immutable ROM и owned strict MMC5 horizon; helper не создаёт credits.
 */
static bool decodeMmc5AffineLoop(const std::uint8_t*code,unsigned bytes,std::uint16_t head,
    Mmc5AffineLoopPlan&out)noexcept {
    if(!code||bytes<12U||bytes>16U)return false;
    Mmc5AffineLoopPlan plan;unsigned cursor=0,cost=0;std::uint16_t left=0,right=0;
    const auto modify=code[0];
    if(modify!=0xe6U&&modify!=0xeeU&&modify!=0xc6U&&modify!=0xceU)return false;
    if(!readMmc5AffineOperand(code,bytes,cursor,modify==0xe6U?0xe6U:0xc6U,modify==0xeeU?0xeeU:0xceU,plan.counter,cost))return false;
    plan.direction=(modify&0x20U)?1:-1;plan.cycles=cost+2U; // RMW5/6 вместоread3/4.
    if(cursor>=bytes||code[cursor++]!=0x18U)return false;
    plan.cycles+=2U; // CLC всегда сбрасывает carry перед суммой.
    if(!readMmc5AffineOperand(code,bytes,cursor,0xa5U,0xadU,left,cost))return false;
    plan.cycles+=cost;
    if(!readMmc5AffineOperand(code,bytes,cursor,0x65U,0x6dU,right,cost))return false;
    plan.cycles+=cost;
    if(!readMmc5AffineOperand(code,bytes,cursor,0x85U,0x8dU,plan.store,cost))return false;
    plan.cycles+=cost;
    if(bytes-cursor!=3U||code[cursor]!=0x4cU||
       (unsigned(code[cursor+1U])|(unsigned(code[cursor+2U])<<8U))!=head)return false;
    if(plan.counter>=0x2000U||left>=0x2000U||right>=0x2000U||plan.store>=0x2000U)return false;
    const auto p=plan.counter&0x7ffU;const bool leftP=(left&0x7ffU)==p,rightP=(right&0x7ffU)==p;
    if(leftP==rightP||(plan.store&0x7ffU)!=p)return false;
    plan.stable=leftP?right:left;plan.cycles+=3U;out=plan;return true;
}

/**
 * @brief Вычисляет p_k=p0+k*(q+sign) mod256 и flags последнего двоичного ADC.
 * @param p,q Текущие physical RAM bytes на target head до будущих периодов.
 * @param direction +1 для INC,−1 для DEC, квалифицирован decoder.
 * @param iterations Число полных будущих периодов,1..floor(240/19);0 — тождественный результат.
 * @param status Канонический P; остальные кромеC/V/N/Z биты сохраняются, BCD вNES отсутствует.
 * @return Итог p/A и P, без RAM-записи и без тактирования устройств.
 */
static Mmc5AffineValue advanceMmc5Affine(std::uint8_t p,std::uint8_t q,int direction,
    unsigned iterations,std::uint8_t status)noexcept {
    if(!iterations)return {p,status};
    const int n=static_cast<int>(iterations),step=int(q)+direction;
    const auto previous=static_cast<std::uint8_t>(int(p)+(n-1)*step);
    const auto incremented=static_cast<std::uint8_t>(int(previous)+direction);
    const unsigned sum=unsigned(incremented)+unsigned(q);const auto value=static_cast<std::uint8_t>(sum);
    const auto flags=static_cast<std::uint8_t>((status&~0xc3U)|((sum>>8U)&1U)|
        ((~(incremented^q)&(incremented^value)&0x80U)>>1U)|(value&0x80U)|(unsigned(value==0)<<1U));
    return {value,flags};
}

} // namespace

#if defined(__GNUC__) || defined(__clang__)
#define NES_CPU_ALU_INLINE [[gnu::always_inline]] inline
#else
#define NES_CPU_ALU_INLINE inline
#endif
#define NES_CPU_ALU_STATE state
#define NES_CPU_ALU_SIGNATURE_setFlag NES_CPU_ALU_INLINE void Cpu6502::aluSetFlag(State& state, Flag bit, bool set) noexcept
#define NES_CPU_ALU_CALL_setFlag(...) aluSetFlag(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_flag NES_CPU_ALU_INLINE bool Cpu6502::aluFlag(const State& state, Flag bit) noexcept
#define NES_CPU_ALU_CALL_flag(...) aluFlag(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_setNz NES_CPU_ALU_INLINE void Cpu6502::aluSetNz(State& state, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_setNz(...) aluSetNz(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_adc NES_CPU_ALU_INLINE void Cpu6502::aluAdc(State& state, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_adc(...) aluAdc(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_compare NES_CPU_ALU_INLINE void Cpu6502::aluCompare(State& state, std::uint8_t left, std::uint8_t right) noexcept
#define NES_CPU_ALU_CALL_compare(...) aluCompare(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_modify NES_CPU_ALU_INLINE std::uint8_t Cpu6502::aluModify(State& state, Op op, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_modify(...) aluModify(state, __VA_ARGS__)
#define NES_CPU_ALU_SIGNATURE_applyRead NES_CPU_ALU_INLINE void Cpu6502::aluApplyRead(State& state, Op op, std::uint8_t value) noexcept
#define NES_CPU_ALU_CALL_applyRead(...) aluApplyRead(state, __VA_ARGS__)
#define NES_CPU_ALU_EMIT_setFlag
#define NES_CPU_ALU_EMIT_flag
#define NES_CPU_ALU_EMIT_setNz
#define NES_CPU_ALU_EMIT_adc
#define NES_CPU_ALU_EMIT_compare
#define NES_CPU_ALU_EMIT_modify
#define NES_CPU_ALU_EMIT_applyRead
#include "CpuAlu.inl"
#undef NES_CPU_ALU_EMIT_setFlag
#undef NES_CPU_ALU_EMIT_flag
#undef NES_CPU_ALU_EMIT_setNz
#undef NES_CPU_ALU_EMIT_adc
#undef NES_CPU_ALU_EMIT_compare
#undef NES_CPU_ALU_EMIT_modify
#undef NES_CPU_ALU_EMIT_applyRead
#undef NES_CPU_ALU_STATE
#undef NES_CPU_ALU_SIGNATURE_setFlag
#undef NES_CPU_ALU_CALL_setFlag
#undef NES_CPU_ALU_SIGNATURE_flag
#undef NES_CPU_ALU_CALL_flag
#undef NES_CPU_ALU_SIGNATURE_setNz
#undef NES_CPU_ALU_CALL_setNz
#undef NES_CPU_ALU_SIGNATURE_adc
#undef NES_CPU_ALU_CALL_adc
#undef NES_CPU_ALU_SIGNATURE_compare
#undef NES_CPU_ALU_CALL_compare
#undef NES_CPU_ALU_SIGNATURE_modify
#undef NES_CPU_ALU_CALL_modify
#undef NES_CPU_ALU_SIGNATURE_applyRead
#undef NES_CPU_ALU_CALL_applyRead
#undef NES_CPU_ALU_INLINE

#if defined(__GNUC__) || defined(__clang__)
#define NES_CPU_QUIET_INLINE __attribute__((always_inline))
#else
#define NES_CPU_QUIET_INLINE
#endif
// У H7 GCC меняет inline-эвристику общего decoder. Ограничиваем исправление
// этим опытом; OFF сохраняет прежнюю политику codegen обычной прошивки.
#define NES_CPU_BASE_PEEK_INLINE NES_CPU_SIZE_PEEK_INLINE
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
/** @details Только отрицательный допуск: abs stores разрешены strict decoder
 * лишь в RAM<2000, abs reads/BIT — в RAM<2000 либо PRG>=8000. Сам IRQ/MMIO
 * продолжает обычный interpreter. Проверяем одну исходную physical ROM-page,
 * не удерживаем байты между вызовами и не используем CPU-visible MMC5 PCM read.
 * noinline позволяет поместить этот небольшой фильтр во Flash отдельно от
 * resident generic decoder, не раздувая его большим новым prepared-tail.
 */
bool Cpu6502::rejectQuietMmioEntry() const noexcept {
    if(this!=&bus_.cpu_||!bus_.cartridge_.loaded()||bus_.cartridge_.mapper()==5)return false;
    const unsigned pc=state_.pc,offset=pc&8191U;
    if(pc<0x8000U||offset>8189U)return false;
    const auto* const bytes=bus_.quietMemory().prg[(pc>>13U)&3U]+offset;
    switch(bytes[0]){
    case 0x8C:case 0x8D:case 0x8E: // STY/STA/STX absolute.
        return unsigned(bytes[1])+(unsigned(bytes[2])<<8U)>=0x2000U;
    case 0x2C:case 0xAC:case 0xAD:case 0xAE:{ // BIT/LDY/LDA/LDX absolute.
        const unsigned address=unsigned(bytes[1])+(unsigned(bytes[2])<<8U);
        return address>=0x2000U&&address<0x8000U;
    }
    default:return false;
    }
}
/** @details Сохраняет прежний ELF/SPM символ; тело decoder остаётся в том же mapped object. */
std::uint32_t Cpu6502::runQuietBlock(unsigned maxCycles) noexcept {
    const auto mapper=bus_.cartridge_.mapper();
    if(mapper==69U||mapper==85U)return runQuietBlockWithPolicy<false,true>(maxCycles);
    if(rejectQuietMmioEntry())return 0;
    return runQuietBlockWithPolicy<false,false>(maxCycles);
}

/** @details Политика задана при компиляции: одно исходное тело сохраняет обе семантики без runtime bool-ветвей обычного пути. */
template<bool Mmc5Span,bool BankedWram>
std::uint32_t Cpu6502::runQuietBlockWithPolicy(unsigned maxCycles) noexcept {
    if (this != &bus_.cpu_ || inStep_ || state_.jammed || bus_.cpuReadBudget_ < 8 ||
        maxCycles < 8 || bus_.dmaPending_ || bus_.dmcStalls_ ||
        !bus_.cartridge_.loaded() ||
        nmiPending_ || nmiReady_ || irqPending_) return 0;
    // Mapper source/enable/direction не меняются внутри обычных RAM/PRG-
    // инструкций. Каждая значимая запись Legacy остаётся на скалярной шине.
    const bool writeStrobesActive = !bus_.cartridge_.cpuWriteStrobeIsNoop();
    const unsigned budget = maxCycles < bus_.cpuReadBudget_ ? maxCycles : bus_.cpuReadBudget_;
    bool selfRepeatWindow=true;
    if(bus_.audioPacket_){
        const auto now=bus_.cpuCycles(),origin=bus_.audioPacket_->header().baseCycle;
        selfRepeatWindow=bus_.audioPacket_->fault()==AudioFault::None&&now>=origin&&
            now-origin<=65535U-budget;
    }
    const auto memory = bus_.quietMemory();
    auto* const ram = memory.ram;
    [[maybe_unused]]
    const auto* const prg = memory.prg;
    // Политика R/W RAM живёт один grant. MMIO/mapper-записи закрывают decoder до смены mapping; IRQ/DMC/audio horizon остаётся прежним.
    static_assert(!Mmc5Span||!BankedWram,"MMC5 and banked WRAM grants are disjoint");
    [[maybe_unused]] auto* const genericWram6000=[&]() noexcept -> std::uint8_t* {
        if constexpr(BankedWram)return bus_.cartridge_.quietWritableWram6000().data;
        else return nullptr;
    }();
    if constexpr(BankedWram)if(!genericWram6000)return runQuietBlockWithPolicy<Mmc5Span,false>(maxCycles);

    // Capability живёт ровно один grant. MMIO/mapper writes закрывают блок;
    // null/отключённый CE остаётся на scalar open-bus шине. Никаких 2KiB masks.
    const auto* const wram6000=Mmc5Span?bus_.quietMmc5WramPage():nullptr;
    auto* const writableWram6000 = [&]() noexcept -> std::uint8_t* {
        if constexpr (Mmc5Span) {
            // Цветовой реплей сохраняет полный поток шинных записей.
            // В Thin-сигналах WRAM может наблюдать только гостевой CPU.
            if (!wram6000 || bus_.colourCapture_ || (bus_.thin_ && !bus_.thinSignalMode())) return nullptr;
            auto* const page = bus_.cartridge_.quietWritableWram6000().data;
            return page == wram6000 ? page : nullptr;
        }
        return genericWram6000;
    }();
    const auto* const dataPrg=bus_.quietCpuReadPages(); // Фиксируется один раз на блок.
    // Полный обычный цикл памяти — неподвижная точка только при точном возврате
    // регистров, шинных защёлок и каждого записанного байта к захваченному состоянию.
    // Существующий quiet-горизонт исключает всех асинхронных CPU-наблюдателей.
    // Каждый after() вызывается до publish: local.cycles неизменен; JAM
    // здесь не исполняется; pendingIrq=true прерывает путь до этого вызова.
    // Меняться могут только перечисленные захваченные поля. Журнал RAM остаётся полным.
    /**
     * @brief Локальный quiet-свидетель фиксированной точки: регистровый снимок, базовый адрес, открытая шина и ограниченный журнал исходных RAM-байтов.
     * @ingroup nes_cpu
     */
    struct RepeatState final {
        /**
         * @brief Регистры PC/A/X/Y/SP/P доказательства обычного Repeat-цикла; не подменяет полный RAM-журнал, базовый адрес и открытую шину.
         * @ingroup nes_cpu
         */
        struct Snapshot { std::uint16_t pc; std::uint8_t a, x, y, sp, p; };
        static_assert(sizeof(Snapshot) == 8, "Quiet register snapshot layout");
        /**
         * @brief Запись ограниченного Repeat-журнала: физический RAM-адрес и исходный байт до первой записи в него.
         * @ingroup nes_cpu
         */
        struct Write { std::uint16_t address; std::uint8_t original; };
        enum class Phase : std::uint8_t { Unarmed, Watching, Stopped };
        std::uint8_t* ram;
        Snapshot saved; // Полностью инициализируется до перехода phase в Watching.
        Write writes[8];
        std::uint32_t started = 0;
        std::uint16_t base = 0;
        std::uint8_t open = 0;
        std::uint8_t size = 0;
        Phase phase = Phase::Unarmed;
        /**
         * @brief Создаёт локальный свидетель повторяемого quiet-цикла для текущей RAM.
         * @details Снимок регистров и журнал ещё не используются до фазы Watching; журнал хранит не более восьми уникальных записей.
         * @param memory Изменяемая внутренняя CPU RAM текущего блока; журнал хранит исходные значения затронутых байтов.
         */
        explicit RepeatState(std::uint8_t* memory) noexcept : ram(memory) {}
        /**
         * @brief Сохраняет исходный RAM-байт в журнале до его фактической quiet-записи.
         * @details Повторная запись того же адреса не расширяет журнал; переполнение останавливает доказательство, а не гостевую запись.
         * @param address Нормализованный физический индекс внутренней 2-КиБ CPU RAM; журналирование не выполняет MMIO.
         */
        void beforeWrite(unsigned address) noexcept {
            // Журнал Repeat охватывает лишь внутренние 2 КиБ; внешняя запись запрещает folding вместо ложного alias через mask7FF.
            if constexpr(BankedWram||Mmc5Span)if(address>=0x6000U){phase=Phase::Stopped;return;}
            if (phase != Phase::Watching) return;
            address &= 0x7ffU;
            for (unsigned i = 0; i < size; ++i)
                if (writes[i].address == address) return;
            // Достижимый size0..8; явная верхняя граница также ограничивает
            // каждую индексную хост-запись объектом журнала для генератора кода.
            if (size >= 8) { phase = Phase::Stopped; return; }
            writes[size++] = {static_cast<std::uint16_t>(address), ram[address]};
        }
        /**
         * @brief Проверяет завершившийся quiet-цикл и возвращает лишь доказанное число пропускаемых тактов.
         * @details Первая попытка сравнения одноразовая, включая неуспех. Период не расширяет исходный бюджет и не удаляет полезные RAM/шинные эффекты.
         * @param currentBase Текущая опубликованная эффективная база CPU.
         * @param currentOpen Текущий последний байт открытой CPU-шины.
         * @param consumed Уже учтённые такты текущего quiet-блока.
         * @param instructionPc PC фактически завершённой инструкции.
         * @param remaining Оставшийся бюджет уже допущенного quiet-горизонта в CPU-тактах.
         * @param selfJmp При включённой JMP-self опции отмечает фактически завершённый прямой переход в себя.
         * @return Число доказанно пропускаемых CPU-тактов внутри remaining либо 0 при отказе; одна проверка окончательна для блока.
         * @param state Регистровое состояние CPU в данной точке; const-ссылка читается, изменяемая ссылка получает только ALU-эффекты.
         */
        unsigned after(const State& state, std::uint16_t currentBase,
                       std::uint8_t currentOpen, bool /*текущее ожидающее прерывание*/,
                       std::uint32_t consumed, std::uint16_t instructionPc,
                       unsigned remaining
                       ,bool selfJmp
                       ) noexcept {
            if (phase == Phase::Stopped) return 0;
            if (phase == Phase::Watching && state.pc == saved.pc) {
                phase = Phase::Stopped; // Одна проверка, включая неуспешную.
                if (state.a != saved.a || state.x != saved.x || state.y != saved.y ||
                    state.sp != saved.sp || state.p != saved.p ||
                    currentBase != base || currentOpen != open) return 0;
                for (unsigned i = 0; i < size; ++i)
                    if (ram[writes[i].address] != writes[i].original) return 0;
                const unsigned length = consumed - started;
                return length ? (remaining / length) * length : 0;
            }
            if (phase == Phase::Unarmed && (state.pc < instructionPc ||
                (selfJmp && state.pc == instructionPc))) {
                saved = {state.pc, state.a, state.x, state.y, state.sp, state.p};
                base = currentBase; open = currentOpen; started = consumed;
                size = 0; phase = Phase::Watching;
            }
            return 0;
        }
    } repeat(ram);

    auto local = state_;
    auto savedBase = baseAddress_;
    auto open = bus_.openBus_;
    const bool stableIrq = irqLine_;
    bool pendingIrq = false;
    std::uint32_t consumed = 0;
    // Допустимый предшественник — только реально завершённое в этом блоке тело.
    std::uint16_t previousPc = 0, idleBranchPc = 0;
    unsigned previousCycles = 0, idleBranchCycles = 0;
    Op previousOp = Op::JAM;
    bool yieldIdle = false;
    const auto quietDeadline = bus_.cpuCycles() + budget;
    // Ограниченный опыт не меняет обычную resident-политику: только MMC5
    // использует подготовленные 2..7 тактов уже полученного grant.
    constexpr bool preparedTail = Mmc5Span;
    // Новый вызов всё ещё требует 8; полный actual count проверяется до apply.
    // constexpr оставляет generic-политике прежнее условие без runtime-ветви.
    while (budget - consumed >= (preparedTail ? 2U : 8U)) {


    const auto instructionPc = local.pc;
    /**
     * @brief Проверяет допуск адреса к обычной RAM/PRG; область MMIO не становится обычной памятью.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto ordinary = [&]() noexcept {
        if constexpr(BankedWram) {
    return [genericWram6000,wram6000](std::uint16_t a) noexcept {
        if constexpr(Mmc5Span)if(a>=0x6000U&&a<0x8000U)return wram6000!=nullptr;
        return ((a<0x2000||a>=0x8000) || (genericWram6000&&Cartridge::QuietWram6000::contains(a)))&&(!Mmc5Span||(a!=0xfffaU&&a!=0xfffbU));
    };
        } else {
    return [
        wram6000
    ](std::uint16_t a) noexcept {
        if constexpr(Mmc5Span)if(a>=0x6000U&&a<0x8000U)return wram6000!=nullptr;
        return (a<0x2000||a>=0x8000)&&(!Mmc5Span||(a!=0xfffaU&&a!=0xfffbU));
    };
        }
    }();
    /**
     * @brief Читает только уже допущенную обычную RAM/PRG без продвижения clock; MMIO фильтруется до вызова.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto basePeek = [&]() noexcept {
        if constexpr(BankedWram) {
    return [ram,dataPrg,genericWram6000](std::uint16_t a) noexcept NES_CPU_BASE_PEEK_INLINE { return a < 0x2000 ? ram[a & 0x7ffU] : a < 0x8000U ? genericWram6000[a-Cartridge::QuietWram6000::base] : dataPrg[(a >> 13) & 3U][a & 8191U]; };
        } else {
    return [ram,dataPrg](std::uint16_t a) noexcept NES_CPU_BASE_PEEK_INLINE { return a < 0x2000 ? ram[a & 0x7ffU] : dataPrg[(a >> 13) & 3U][a & 8191U]; };
        }
    }();
    /** @brief Generic-политика получает исходную literal lambda без H5/H7 captures и ветвей. */
    const auto peek=[&]()noexcept{
        if constexpr(Mmc5Span){
            return [basePeek
        ,wram6000
            ](std::uint16_t a) noexcept NES_CPU_SIZE_PEEK_INLINE {
        if(a>=0x6000U&&a<0x8000U)return wram6000[a&8191U];
                return basePeek(a);
            };
        }else return basePeek;
    }();
    auto cursor = local.pc;
    if (!ordinary(cursor)) goto publish;
    // Одна инструкция использует максимум опкод и два байта операндов.
    // Не читать байты заранее; RAM и два последних байта страницы сохраняют peek().
    const unsigned codeOffset = cursor & 8191U;
    const std::uint8_t* codeCursor = nullptr;
    if (cursor >= 0x8000U && codeOffset <= 8192U - 3U &&
        (!Mmc5Span||(ordinary(static_cast<std::uint16_t>(cursor+1U))&&ordinary(static_cast<std::uint16_t>(cursor+2U))))) {
        codeCursor = dataPrg[(cursor >> 13U) & 3U] + codeOffset;
    }
    const auto opcodePc = cursor++;
    const auto opcode = codeCursor ? *codeCursor++ : peek(opcodePc);
    Op op=Op::JAM;
    Mode mode=Mode::Imp;
    [[maybe_unused]] bool branch=false;
    bool updateBase=false,take=false,oldInterruptMask=false;
    unsigned count=1;
    std::uint8_t last=0,stackStatus=0;
    std::uint16_t base=savedBase,location=0,stackReturn=0;
    /**
     * @brief Нормализует SP до восьми бит и адресует аппаратную stack-страницу $0100; память не читает и SP не изменяет.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto stackAddress = [](unsigned sp) noexcept {
        return static_cast<std::uint16_t>(0x100U | static_cast<std::uint8_t>(sp));
    };

    /**
     * @brief Планирует исходные шинные обращения для статических op/mode.
     * @details Не применяет ALU и записи RAM: общий обработчик операции вызывается
     * только после прежних проверок ordinary, dummy и stack-alias.
     * @param opTag Тег операции собственного OpcodeTable.inl.
     * @param modeTag Тег режима адресации той же таблицы.
     * @return true при полном допуске исходной инструкции.
     */
    const auto prepareKnown=[&](auto opTag,auto modeTag)noexcept NES_CPU_QUIET_INLINE{
        constexpr auto op=decltype(opTag)::value;
        constexpr auto mode=decltype(modeTag)::value;
    switch (op) {
    case Op::BRK: case Op::JAM:
    case Op::SHX: case Op::SHY: case Op::AHX: case Op::TAS: return false;
    case Op::JMP: if (mode != Mode::Abs) return false; break;
    default: break;
    }
    const bool store = op == Op::STA || op == Op::STX || op == Op::STY || op == Op::SAX;
    const bool rmw = mode != Mode::Acc && (op == Op::ASL || op == Op::LSR || op == Op::ROL ||
        op == Op::ROR || op == Op::INC || op == Op::DEC || op == Op::SLO || op == Op::RLA ||
        op == Op::SRE || op == Op::RRA || op == Op::DCP || op == Op::ISC);
    // Активный source3 тактирует обе RMW-записи даже при установленном IRQ.
    // Опубликовать до эффекта; неизменённые скалярные записи дают все стробы.
    if (writeStrobesActive && (store || rmw)) return false;
    const bool stack = op == Op::PHA || op == Op::PHP || op == Op::PLA ||
        op == Op::PLP || op == Op::JSR || op == Op::RTS || op == Op::RTI;
    const bool stackPush = op == Op::PHA || op == Op::PHP || op == Op::JSR;
    if (stackPush && writeStrobesActive) return false;
    constexpr bool branch = mode == Mode::Rel;
    const unsigned operands = mode == Mode::Imp || mode == Mode::Acc ? 0U :
        (mode == Mode::Abs || mode == Mode::AbsX || mode == Mode::AbsY ? 2U : 1U);
    // Допущенный трёхбайтовый ROM-диапазон доказывает только адреса опкода/операндов.
    // Data/dummy/stack/branch-доступы сохраняют исходные проверки.
    if (!codeCursor && operands && (!ordinary(cursor) || (operands == 2 && !ordinary(static_cast<std::uint16_t>(cursor+1U))))) return false;


    /**
     * @brief Учитывает очередной байт и его шинный count/last в локальной последовательности без дополнительного CPU-clock.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto sample = [&](std::uint16_t a) noexcept NES_CPU_QUIET_INLINE { ++count; return last = peek(a); };
    /**
     * @brief Получает ровно следующий реально требуемый байт операнда и продвигает локальный курсор.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto fetch = [&]() noexcept NES_CPU_QUIET_INLINE {
        std::uint8_t value;
        if (codeCursor) { ++count;
            value = last = *codeCursor++;
        }
        else value = sample(cursor);
        ++cursor;
        return value;
    };





    if (stack) {
        // Спланировать каждое шинное чтение до изменения RAM или локальных регистров.
        // Quiet-горизонт исключает асинхронных наблюдателей. Значимый
        // строб записи уже отверг каждую инструкцию push.
        if (!ordinary(cursor)) return false;
        if (op == Op::JSR) {
            const std::uint16_t low = fetch();
            stackReturn = cursor; // Сохраняется адрес старшего байта операнда.
            // JSR читает старший байт ПОСЛЕ обеих записей. Перекрывающийся
            // RAM/stack-операнд оставить на скалярной хронологической шине, даже если
            // его исходное значение случайно равно сохраняемому.
            if (cursor < 0x2000 && ((cursor & 0x7ffU) == stackAddress(local.sp) ||
                (cursor & 0x7ffU) == stackAddress(local.sp - 1U))) return false;
            sample(stackAddress(local.sp));
            count += 2; // Записи PCH/PCL до финального чтения старшего байта операнда.
            location = static_cast<std::uint16_t>(low | (std::uint16_t(fetch()) << 8));
        } else {
            sample(cursor); // Фиктивное чтение кода без продвижения PC.
            if (stackPush) ++count;
            else {
                sample(stackAddress(local.sp)); // Фиктивное чтение перед увеличением SP.
                if (op == Op::PLA || op == Op::PLP) sample(stackAddress(local.sp + 1U));
                else {
                    unsigned pulled = 1;
                    if (op == Op::RTI) stackStatus = sample(stackAddress(local.sp + pulled++));
                    const std::uint16_t low = sample(stackAddress(local.sp + pulled++));
                    location = static_cast<std::uint16_t>(low | (std::uint16_t(sample(stackAddress(local.sp + pulled))) << 8));
                    if (op == Op::RTS) {
                        if (!ordinary(location)) return false;
                        sample(location); // Фиктивное чтение адреса возврата перед PC+1.
                    }
                }
            }
        }
    } else if (branch) {
        const auto displacement = static_cast<std::int8_t>(fetch());
        switch (op) {
        case Op::BPL: take = !aluFlag(local, N); break; case Op::BMI: take = aluFlag(local, N); break;
        case Op::BVC: take = !aluFlag(local, V); break; case Op::BVS: take = aluFlag(local, V); break;
        case Op::BCC: take = !aluFlag(local, C); break; case Op::BCS: take = aluFlag(local, C); break;
        case Op::BNE: take = !aluFlag(local, Z); break; case Op::BEQ: take = aluFlag(local, Z); break;
        default: return false;
        }
        if (take) {
            location = static_cast<std::uint16_t>(cursor + displacement);
            if (!ordinary(cursor)) return false;
            sample(cursor);
            if ((cursor & 0xff00U) != (location & 0xff00U)) {
                const auto dummy = static_cast<std::uint16_t>((cursor & 0xff00U) | (location & 0xffU));
                if (!ordinary(dummy)) return false;
                sample(dummy);
            }
        }
    } else if (mode == Mode::Imp || mode == Mode::Acc) {
        if (!ordinary(cursor)) return false;
        sample(cursor);
    } else {
        switch (mode) {
        case Mode::Imm: location = cursor++; break;
        case Mode::Zp: location = fetch(); break;
        case Mode::ZpX: case Mode::ZpY: {
            const auto zero = fetch(); sample(zero);
            location = static_cast<std::uint8_t>(zero + (mode == Mode::ZpX ? local.x : local.y)); break;
        }
        case Mode::Abs: case Mode::AbsX: case Mode::AbsY: {
            const std::uint16_t lo = fetch(); base = static_cast<std::uint16_t>(lo | (std::uint16_t(fetch()) << 8));
            location = static_cast<std::uint16_t>(base + (mode == Mode::AbsX ? local.x : mode == Mode::AbsY ? local.y : 0U));
            if (mode != Mode::Abs && (store || rmw || (base & 0xff00U) != (location & 0xff00U))) {
                const auto dummy = static_cast<std::uint16_t>((base & 0xff00U) | (location & 0xffU));
                if (!ordinary(dummy)) return false;
                sample(dummy);
            }
            break;
        }
        case Mode::XInd: {
            const auto zero = fetch(); sample(zero);
            const auto pointer = static_cast<std::uint8_t>(zero + local.x);
            const std::uint16_t lo = sample(pointer);
            location = static_cast<std::uint16_t>(lo | (std::uint16_t(sample(static_cast<std::uint8_t>(pointer+1U))) << 8));
            break;
        }
        case Mode::IndY: {
            const auto pointer = fetch(); const std::uint16_t lo = sample(pointer);
            base = static_cast<std::uint16_t>(lo | (std::uint16_t(sample(static_cast<std::uint8_t>(pointer+1U))) << 8));
            location = static_cast<std::uint16_t>(base + local.y);
            if (store || rmw || (base & 0xff00U) != (location & 0xff00U)) {
                const auto dummy = static_cast<std::uint16_t>((base & 0xff00U) | (location & 0xffU));
                if (!ordinary(dummy)) return false;
                sample(dummy);
            }
            break;
        }
        default: return false;
        }
        if (op != Op::JMP) {
            if constexpr(BankedWram || Mmc5Span) {
                if ((store || rmw) ? (location>=0x2000U && !(writableWram6000&&Cartridge::QuietWram6000::contains(location))) : !ordinary(location)) return false;
            } else {
                if ((store || rmw) ? location >= 0x2000 : !ordinary(location)) return false;
            }
            if (store) ++count;
            else { sample(location); if (rmw) count += 2; }
            updateBase = true;
            if (mode != Mode::AbsX && mode != Mode::AbsY && mode != Mode::IndY) base = location;
        }
    }
    // Каждый потенциально эффектный адрес отвергнут выше. При отказе не изменены
    // такты, архитектурные регистры, RAM или защёлка открытой шины.
    oldInterruptMask = aluFlag(local, I);

        return true;
    };
    /**
     * @brief Применяет одну общую ALU/write-ветвь для всех режимов операции.
     * @details P остаётся каноническим. Журнал, stack-порядок и последний байт
     * совпадают с исходным интерпретатором; динамического вызова handler нет.
     * @param opTag Статический тег уже допущенной операции.
     */
    const auto applyKnown=[&](auto opTag)noexcept NES_CPU_QUIET_INLINE{
        constexpr auto op=decltype(opTag)::value;
    const bool store = op == Op::STA || op == Op::STX || op == Op::STY || op == Op::SAX;
    const bool rmw = mode != Mode::Acc && (op == Op::ASL || op == Op::LSR || op == Op::ROL ||
        op == Op::ROR || op == Op::INC || op == Op::DEC || op == Op::SLO || op == Op::RLA ||
        op == Op::SRE || op == Op::RRA || op == Op::DCP || op == Op::ISC);
    const bool stack = op == Op::PHA || op == Op::PHP || op == Op::PLA ||
        op == Op::PLP || op == Op::JSR || op == Op::RTS || op == Op::RTI;
    constexpr bool branch=op==Op::BPL||op==Op::BMI||op==Op::BVC||op==Op::BVS||op==Op::BCC||op==Op::BCS||op==Op::BNE||op==Op::BEQ;
    if (stack) {
        switch (op) {
        case Op::PHA: case Op::PHP:
            last = op == Op::PHA ? local.a : static_cast<std::uint8_t>(local.p | U | B);
            
            repeat.beforeWrite(stackAddress(local.sp));
            ram[stackAddress(local.sp)] = last;
            --local.sp;
            break;
        case Op::PLA: case Op::PLP:
            ++local.sp;
            if (op == Op::PLA) { local.a = last; aluSetNz(local, last); }
            else local.p = static_cast<std::uint8_t>((last | U) & ~B);
            break;
        case Op::JSR:
            
            repeat.beforeWrite(stackAddress(local.sp));
            ram[stackAddress(local.sp)] = static_cast<std::uint8_t>(stackReturn >> 8);
            
            repeat.beforeWrite(stackAddress(local.sp - 1U));
            ram[stackAddress(local.sp - 1U)] = static_cast<std::uint8_t>(stackReturn);
            local.sp = static_cast<std::uint8_t>(local.sp - 2U);
            cursor = location;
            break;
        case Op::RTS:
            local.sp = static_cast<std::uint8_t>(local.sp + 2U);
            cursor = static_cast<std::uint16_t>(location + 1U);
            break;
        case Op::RTI:
            local.sp = static_cast<std::uint8_t>(local.sp + 3U);
            local.p = static_cast<std::uint8_t>((stackStatus | U) & ~B);
            cursor = location;
            break;
        default: break;
        }
    } else if (branch) { if (take) cursor = location; }
    else if (op == Op::JMP) cursor = location;
    else if (mode == Mode::Imp) {
#define NES_CPU_QUIET_IMPLIED_OP op
#include "QuietImplied.inl"
#undef NES_CPU_QUIET_IMPLIED_OP
    } else if (mode == Mode::Acc) local.a=aluModify(local, op,local.a);
    else if (store) {
        if constexpr(BankedWram || Mmc5Span) {
            last = op == Op::STX ? local.x : op == Op::STY ? local.y :
                op == Op::SAX ? static_cast<std::uint8_t>(local.a & local.x) : local.a;
            repeat.beforeWrite(location);
            if (location < 0x2000U) ram[location & 0x7ffU] = last;
            else writableWram6000[location - Cartridge::QuietWram6000::base] = last;
    
        } else {
        last = op == Op::STX ? local.x : op == Op::STY ? local.y :
            op == Op::SAX ? static_cast<std::uint8_t>(local.a & local.x) : local.a;
        
            repeat.beforeWrite(location & 0x7ffU);
            ram[location & 0x7ffU] = last;
    
        }
    } else if (rmw) {
        if constexpr(BankedWram || Mmc5Span) {
            repeat.beforeWrite(location);
            // Обе WRAM записи RMW используют тот же физический байт и два исходных M2.
            if (location >= 0x6000U) writableWram6000[location - Cartridge::QuietWram6000::base] = last;
            last = aluModify(local, op, last);
            if (location < 0x2000U) ram[location & 0x7ffU] = last;
            else writableWram6000[location - Cartridge::QuietWram6000::base] = last;
    
        } else {
        last = aluModify(local, op,last); 
            repeat.beforeWrite(location & 0x7ffU);
            ram[location & 0x7ffU] = last;
    
        }
    } else aluApplyRead(local, op,last);

    };
    // Один opcode-dispatch выбирает общий статический шинный маршрут.
    // После него общий operation-dispatch не дублирует ALU по режимам адресации.
    // Guards используют constexpr значения. Обычный if сохраняет syntactic goto
    // для GCC unused-label анализа template, а оптимизатор сворачивает те же константы.
    switch(opcode){
#define INS(code, operation, addressing)     case code: {         constexpr auto vOp=Op::operation; constexpr auto vMode=Mode::addressing;         op=vOp; mode=vMode; branch=vMode==Mode::Rel;         constexpr bool vStore=vOp==Op::STA||vOp==Op::STX||vOp==Op::STY||vOp==Op::SAX;         constexpr bool vRmw=vMode!=Mode::Acc&&(vOp==Op::ASL||vOp==Op::LSR||vOp==Op::ROL||vOp==Op::ROR||vOp==Op::INC||vOp==Op::DEC||vOp==Op::SLO||vOp==Op::RLA||vOp==Op::SRE||vOp==Op::RRA||vOp==Op::DCP||vOp==Op::ISC);         constexpr bool vStack=vOp==Op::PHA||vOp==Op::PHP||vOp==Op::PLA||vOp==Op::PLP||vOp==Op::JSR||vOp==Op::RTS||vOp==Op::RTI;         if((!Mmc5Span&&(vOp==Op::SLO||vOp==Op::RLA||vOp==Op::SRE||vOp==Op::RRA||vOp==Op::DCP||vOp==Op::ISC||vOp==Op::LAX||vOp==Op::SAX||vOp==Op::ANC||vOp==Op::ALR||vOp==Op::ARR||vOp==Op::AXS||vOp==Op::XAA||vOp==Op::LAS))||vOp==Op::BRK||vOp==Op::JAM||vOp==Op::SHX||vOp==Op::SHY||vOp==Op::AHX||vOp==Op::TAS||(vOp==Op::JMP&&vMode!=Mode::Abs))goto publish;         else if(vStack||vMode==Mode::Rel||vOp==Op::JMP)goto address_##operation;         else if(vStore)goto address_##addressing##_W;         else if(vRmw)goto address_##addressing##_M;         else goto address_##addressing##_R;     }
#include "OpcodeTable.inl"
#undef INS
    }
    goto publish;
    address_BRK:
        if(!prepareKnown(std::integral_constant<Op,Op::BRK>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ORA:
        if(!prepareKnown(std::integral_constant<Op,Op::ORA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_JAM:
        if(!prepareKnown(std::integral_constant<Op,Op::JAM>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SLO:
        if(!prepareKnown(std::integral_constant<Op,Op::SLO>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_NOP:
        if(!prepareKnown(std::integral_constant<Op,Op::NOP>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ASL:
        if(!prepareKnown(std::integral_constant<Op,Op::ASL>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_PHP:
        if(!prepareKnown(std::integral_constant<Op,Op::PHP>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ANC:
        if(!prepareKnown(std::integral_constant<Op,Op::ANC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BPL:
        if(!prepareKnown(std::integral_constant<Op,Op::BPL>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_CLC:
        if(!prepareKnown(std::integral_constant<Op,Op::CLC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_JSR:
        if(!prepareKnown(std::integral_constant<Op,Op::JSR>{},std::integral_constant<Mode,Mode::Abs>{}))goto publish;
        goto addressDone;
    address_AND:
        if(!prepareKnown(std::integral_constant<Op,Op::AND>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_RLA:
        if(!prepareKnown(std::integral_constant<Op,Op::RLA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BIT:
        if(!prepareKnown(std::integral_constant<Op,Op::BIT>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ROL:
        if(!prepareKnown(std::integral_constant<Op,Op::ROL>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_PLP:
        if(!prepareKnown(std::integral_constant<Op,Op::PLP>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BMI:
        if(!prepareKnown(std::integral_constant<Op,Op::BMI>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_SEC:
        if(!prepareKnown(std::integral_constant<Op,Op::SEC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_RTI:
        if(!prepareKnown(std::integral_constant<Op,Op::RTI>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_EOR:
        if(!prepareKnown(std::integral_constant<Op,Op::EOR>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SRE:
        if(!prepareKnown(std::integral_constant<Op,Op::SRE>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LSR:
        if(!prepareKnown(std::integral_constant<Op,Op::LSR>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_PHA:
        if(!prepareKnown(std::integral_constant<Op,Op::PHA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ALR:
        if(!prepareKnown(std::integral_constant<Op,Op::ALR>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_JMP:
        if(!prepareKnown(std::integral_constant<Op,Op::JMP>{},std::integral_constant<Mode,Mode::Abs>{}))goto publish;
        goto addressDone;
    address_BVC:
        if(!prepareKnown(std::integral_constant<Op,Op::BVC>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_CLI:
        if(!prepareKnown(std::integral_constant<Op,Op::CLI>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_RTS:
        if(!prepareKnown(std::integral_constant<Op,Op::RTS>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ADC:
        if(!prepareKnown(std::integral_constant<Op,Op::ADC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_RRA:
        if(!prepareKnown(std::integral_constant<Op,Op::RRA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ROR:
        if(!prepareKnown(std::integral_constant<Op,Op::ROR>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_PLA:
        if(!prepareKnown(std::integral_constant<Op,Op::PLA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ARR:
        if(!prepareKnown(std::integral_constant<Op,Op::ARR>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BVS:
        if(!prepareKnown(std::integral_constant<Op,Op::BVS>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_SEI:
        if(!prepareKnown(std::integral_constant<Op,Op::SEI>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_STA:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SAX:
        if(!prepareKnown(std::integral_constant<Op,Op::SAX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_STY:
        if(!prepareKnown(std::integral_constant<Op,Op::STY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_STX:
        if(!prepareKnown(std::integral_constant<Op,Op::STX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_DEY:
        if(!prepareKnown(std::integral_constant<Op,Op::DEY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TXA:
        if(!prepareKnown(std::integral_constant<Op,Op::TXA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_XAA:
        if(!prepareKnown(std::integral_constant<Op,Op::XAA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BCC:
        if(!prepareKnown(std::integral_constant<Op,Op::BCC>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_AHX:
        if(!prepareKnown(std::integral_constant<Op,Op::AHX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TYA:
        if(!prepareKnown(std::integral_constant<Op,Op::TYA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TXS:
        if(!prepareKnown(std::integral_constant<Op,Op::TXS>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TAS:
        if(!prepareKnown(std::integral_constant<Op,Op::TAS>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SHY:
        if(!prepareKnown(std::integral_constant<Op,Op::SHY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SHX:
        if(!prepareKnown(std::integral_constant<Op,Op::SHX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LDY:
        if(!prepareKnown(std::integral_constant<Op,Op::LDY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LDA:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LDX:
        if(!prepareKnown(std::integral_constant<Op,Op::LDX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LAX:
        if(!prepareKnown(std::integral_constant<Op,Op::LAX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TAY:
        if(!prepareKnown(std::integral_constant<Op,Op::TAY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TAX:
        if(!prepareKnown(std::integral_constant<Op,Op::TAX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BCS:
        if(!prepareKnown(std::integral_constant<Op,Op::BCS>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_CLV:
        if(!prepareKnown(std::integral_constant<Op,Op::CLV>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_TSX:
        if(!prepareKnown(std::integral_constant<Op,Op::TSX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_LAS:
        if(!prepareKnown(std::integral_constant<Op,Op::LAS>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_CPY:
        if(!prepareKnown(std::integral_constant<Op,Op::CPY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_CMP:
        if(!prepareKnown(std::integral_constant<Op,Op::CMP>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_DCP:
        if(!prepareKnown(std::integral_constant<Op,Op::DCP>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_DEC:
        if(!prepareKnown(std::integral_constant<Op,Op::DEC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_INY:
        if(!prepareKnown(std::integral_constant<Op,Op::INY>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_DEX:
        if(!prepareKnown(std::integral_constant<Op,Op::DEX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_AXS:
        if(!prepareKnown(std::integral_constant<Op,Op::AXS>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BNE:
        if(!prepareKnown(std::integral_constant<Op,Op::BNE>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_CLD:
        if(!prepareKnown(std::integral_constant<Op,Op::CLD>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_CPX:
        if(!prepareKnown(std::integral_constant<Op,Op::CPX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_SBC:
        if(!prepareKnown(std::integral_constant<Op,Op::SBC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_ISC:
        if(!prepareKnown(std::integral_constant<Op,Op::ISC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_INC:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_INX:
        if(!prepareKnown(std::integral_constant<Op,Op::INX>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_BEQ:
        if(!prepareKnown(std::integral_constant<Op,Op::BEQ>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_SED:
        if(!prepareKnown(std::integral_constant<Op,Op::SED>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_Imp_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_Imp_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_Imp_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Imp>{}))goto publish;
        goto addressDone;
    address_XInd_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::XInd>{}))goto publish;
        goto addressDone;
    address_XInd_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::XInd>{}))goto publish;
        goto addressDone;
    address_XInd_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::XInd>{}))goto publish;
        goto addressDone;
    address_Zp_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Zp>{}))goto publish;
        goto addressDone;
    address_Zp_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Zp>{}))goto publish;
        goto addressDone;
    address_Zp_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Zp>{}))goto publish;
        goto addressDone;
    address_Imm_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Imm>{}))goto publish;
        goto addressDone;
    address_Imm_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Imm>{}))goto publish;
        goto addressDone;
    address_Imm_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Imm>{}))goto publish;
        goto addressDone;
    address_Acc_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Acc>{}))goto publish;
        goto addressDone;
    address_Acc_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Acc>{}))goto publish;
        goto addressDone;
    address_Acc_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Acc>{}))goto publish;
        goto addressDone;
    address_Abs_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Abs>{}))goto publish;
        goto addressDone;
    address_Abs_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Abs>{}))goto publish;
        goto addressDone;
    address_Abs_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Abs>{}))goto publish;
        goto addressDone;
    address_Rel_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_Rel_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_Rel_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Rel>{}))goto publish;
        goto addressDone;
    address_IndY_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::IndY>{}))goto publish;
        goto addressDone;
    address_IndY_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::IndY>{}))goto publish;
        goto addressDone;
    address_IndY_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::IndY>{}))goto publish;
        goto addressDone;
    address_ZpX_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::ZpX>{}))goto publish;
        goto addressDone;
    address_ZpX_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::ZpX>{}))goto publish;
        goto addressDone;
    address_ZpX_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::ZpX>{}))goto publish;
        goto addressDone;
    address_AbsY_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::AbsY>{}))goto publish;
        goto addressDone;
    address_AbsY_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::AbsY>{}))goto publish;
        goto addressDone;
    address_AbsY_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::AbsY>{}))goto publish;
        goto addressDone;
    address_AbsX_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::AbsX>{}))goto publish;
        goto addressDone;
    address_AbsX_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::AbsX>{}))goto publish;
        goto addressDone;
    address_AbsX_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::AbsX>{}))goto publish;
        goto addressDone;
    address_Ind_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::Ind>{}))goto publish;
        goto addressDone;
    address_Ind_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::Ind>{}))goto publish;
        goto addressDone;
    address_Ind_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::Ind>{}))goto publish;
        goto addressDone;
    address_ZpY_R:
        if(!prepareKnown(std::integral_constant<Op,Op::LDA>{},std::integral_constant<Mode,Mode::ZpY>{}))goto publish;
        goto addressDone;
    address_ZpY_W:
        if(!prepareKnown(std::integral_constant<Op,Op::STA>{},std::integral_constant<Mode,Mode::ZpY>{}))goto publish;
        goto addressDone;
    address_ZpY_M:
        if(!prepareKnown(std::integral_constant<Op,Op::INC>{},std::integral_constant<Mode,Mode::ZpY>{}))goto publish;
        goto addressDone;
    addressDone:
    // prepareKnown меняет лишь локальные план/RAW-count/last/cursor.
    // При отказе ни ALU/RAM/Repeat, ни published PC/base/open ещё не тронуты.
    // Для прежнего remaining>=8 условие всегда false; новая ветвь касается
    // только 2..7 уже допущенных CPU-тактов, не расширяя IRQ/NMI/DMC horizon.
    if constexpr(preparedTail)if(count>budget-consumed)goto publish;
    switch(op){
    case Op::BRK: applyKnown(std::integral_constant<Op,Op::BRK>{}); break;
    case Op::ORA: applyKnown(std::integral_constant<Op,Op::ORA>{}); break;
    case Op::JAM: applyKnown(std::integral_constant<Op,Op::JAM>{}); break;
    case Op::SLO: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::SLO>{}); break;
    case Op::NOP: applyKnown(std::integral_constant<Op,Op::NOP>{}); break;
    case Op::ASL: applyKnown(std::integral_constant<Op,Op::ASL>{}); break;
    case Op::PHP: applyKnown(std::integral_constant<Op,Op::PHP>{}); break;
    case Op::ANC: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::ANC>{}); break;
    case Op::BPL: applyKnown(std::integral_constant<Op,Op::BPL>{}); break;
    case Op::CLC: applyKnown(std::integral_constant<Op,Op::CLC>{}); break;
    case Op::JSR: applyKnown(std::integral_constant<Op,Op::JSR>{}); break;
    case Op::AND: applyKnown(std::integral_constant<Op,Op::AND>{}); break;
    case Op::RLA: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::RLA>{}); break;
    case Op::BIT: applyKnown(std::integral_constant<Op,Op::BIT>{}); break;
    case Op::ROL: applyKnown(std::integral_constant<Op,Op::ROL>{}); break;
    case Op::PLP: applyKnown(std::integral_constant<Op,Op::PLP>{}); break;
    case Op::BMI: applyKnown(std::integral_constant<Op,Op::BMI>{}); break;
    case Op::SEC: applyKnown(std::integral_constant<Op,Op::SEC>{}); break;
    case Op::RTI: applyKnown(std::integral_constant<Op,Op::RTI>{}); break;
    case Op::EOR: applyKnown(std::integral_constant<Op,Op::EOR>{}); break;
    case Op::SRE: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::SRE>{}); break;
    case Op::LSR: applyKnown(std::integral_constant<Op,Op::LSR>{}); break;
    case Op::PHA: applyKnown(std::integral_constant<Op,Op::PHA>{}); break;
    case Op::ALR: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::ALR>{}); break;
    case Op::JMP: applyKnown(std::integral_constant<Op,Op::JMP>{}); break;
    case Op::BVC: applyKnown(std::integral_constant<Op,Op::BVC>{}); break;
    case Op::CLI: applyKnown(std::integral_constant<Op,Op::CLI>{}); break;
    case Op::RTS: applyKnown(std::integral_constant<Op,Op::RTS>{}); break;
    case Op::ADC: applyKnown(std::integral_constant<Op,Op::ADC>{}); break;
    case Op::RRA: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::RRA>{}); break;
    case Op::ROR: applyKnown(std::integral_constant<Op,Op::ROR>{}); break;
    case Op::PLA: applyKnown(std::integral_constant<Op,Op::PLA>{}); break;
    case Op::ARR: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::ARR>{}); break;
    case Op::BVS: applyKnown(std::integral_constant<Op,Op::BVS>{}); break;
    case Op::SEI: applyKnown(std::integral_constant<Op,Op::SEI>{}); break;
    case Op::STA: applyKnown(std::integral_constant<Op,Op::STA>{}); break;
    case Op::SAX: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::SAX>{}); break;
    case Op::STY: applyKnown(std::integral_constant<Op,Op::STY>{}); break;
    case Op::STX: applyKnown(std::integral_constant<Op,Op::STX>{}); break;
    case Op::DEY: applyKnown(std::integral_constant<Op,Op::DEY>{}); break;
    case Op::TXA: applyKnown(std::integral_constant<Op,Op::TXA>{}); break;
    case Op::XAA: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::XAA>{}); break;
    case Op::BCC: applyKnown(std::integral_constant<Op,Op::BCC>{}); break;
    case Op::AHX: applyKnown(std::integral_constant<Op,Op::AHX>{}); break;
    case Op::TYA: applyKnown(std::integral_constant<Op,Op::TYA>{}); break;
    case Op::TXS: applyKnown(std::integral_constant<Op,Op::TXS>{}); break;
    case Op::TAS: applyKnown(std::integral_constant<Op,Op::TAS>{}); break;
    case Op::SHY: applyKnown(std::integral_constant<Op,Op::SHY>{}); break;
    case Op::SHX: applyKnown(std::integral_constant<Op,Op::SHX>{}); break;
    case Op::LDY: applyKnown(std::integral_constant<Op,Op::LDY>{}); break;
    case Op::LDA: applyKnown(std::integral_constant<Op,Op::LDA>{}); break;
    case Op::LDX: applyKnown(std::integral_constant<Op,Op::LDX>{}); break;
    case Op::LAX: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::LAX>{}); break;
    case Op::TAY: applyKnown(std::integral_constant<Op,Op::TAY>{}); break;
    case Op::TAX: applyKnown(std::integral_constant<Op,Op::TAX>{}); break;
    case Op::BCS: applyKnown(std::integral_constant<Op,Op::BCS>{}); break;
    case Op::CLV: applyKnown(std::integral_constant<Op,Op::CLV>{}); break;
    case Op::TSX: applyKnown(std::integral_constant<Op,Op::TSX>{}); break;
    case Op::LAS: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::LAS>{}); break;
    case Op::CPY: applyKnown(std::integral_constant<Op,Op::CPY>{}); break;
    case Op::CMP: applyKnown(std::integral_constant<Op,Op::CMP>{}); break;
    case Op::DCP: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::DCP>{}); break;
    case Op::DEC: applyKnown(std::integral_constant<Op,Op::DEC>{}); break;
    case Op::INY: applyKnown(std::integral_constant<Op,Op::INY>{}); break;
    case Op::DEX: applyKnown(std::integral_constant<Op,Op::DEX>{}); break;
    case Op::AXS: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::AXS>{}); break;
    case Op::BNE: {
        applyKnown(std::integral_constant<Op,Op::BNE>{});
        if constexpr(Mmc5Span){
            // Rel не меняет base/P: cursor/last уже являются точным post-BNE
            // tuple. Pure helper не читает local State и не публикует clocks.
            // Общий commit ниже остаётся единственным, включая delayed-I poll.
            if(take&&(!stableIrq||aluFlag(local,I))){
                const unsigned length=static_cast<std::uint16_t>(instructionPc-cursor);
                const unsigned remaining=budget-consumed-count;
                if((length==2U||length==3U)&&remaining>=36U){
                    const auto pcAx=std::uint32_t(cursor)|(std::uint32_t(local.a)<<16U)|(std::uint32_t(local.x)<<24U);
                    const auto baseOpenP=(std::uint32_t(savedBase)<<16U)|(std::uint32_t(last)<<8U)|local.p;
                    const auto branchInfo=std::uint32_t(instructionPc)|(std::uint32_t(count)<<16U)|(std::uint32_t(local.jammed)<<24U);
                    // Сохраняем доказанный oldhelper cap80, не расширяя весь grant240.
                    const unsigned foldRoom=remaining>80U?80U:remaining;
                    const unsigned folded=bus_.mmc5LocalReadFold(pcAx,baseOpenP,branchInfo,foldRoom,ram,wram6000,dataPrg);
                    consumed+=folded;
                }
            }
        }
        break;
    }
    case Op::CLD: applyKnown(std::integral_constant<Op,Op::CLD>{}); break;
    case Op::CPX: applyKnown(std::integral_constant<Op,Op::CPX>{}); break;
    case Op::SBC: applyKnown(std::integral_constant<Op,Op::SBC>{}); break;
    case Op::ISC: if constexpr(Mmc5Span)applyKnown(std::integral_constant<Op,Op::ISC>{}); break;
    case Op::INC: applyKnown(std::integral_constant<Op,Op::INC>{}); break;
    case Op::INX: applyKnown(std::integral_constant<Op,Op::INX>{}); break;
    case Op::BEQ: applyKnown(std::integral_constant<Op,Op::BEQ>{}); break;
    case Op::SED: applyKnown(std::integral_constant<Op,Op::SED>{}); break;
    }
    if (updateBase) savedBase=base;
    local.pc=cursor;
    consumed += count;
    open=last;
    const bool delayedI=op==Op::CLI || op==Op::SEI || op==Op::PLP;
    pendingIrq=stableIrq && !(delayedI ? oldInterruptMask : aluFlag(local,I));
    if (pendingIrq) break;
    // Формула не зависит от mapper: non-banked quiet grant уже исключил MMIO,
    // DMA/DMC-наблюдение RAM и пересечение IRQ/NMI/PPU границы. Новый допуск
    // сохраняет внутренние physical-alias проверки и неизменяемый ROM source.
    // BankedWram остаётся прежним; публичные layout, clocks и packet не меняются.
#if defined(NES_EXPERIMENTAL_GENERIC_AFFINE_QUIET) && NES_EXPERIMENTAL_GENERIC_AFFINE_QUIET
    if constexpr(Mmc5Span||!BankedWram){
#else
    if constexpr(Mmc5Span){
#endif
        // Только будущие целые периоды уже выданного grant. Текущий JMP исполнен обычно.
        if(op==Op::JMP&&mode==Mode::Abs&&local.pc<instructionPc&&budget-consumed>=19U&&
           !writeStrobesActive&&(!Mmc5Span||(bus_.mmc5ReadQuantum_.active&&bus_.cartridge_.mapper()==5U))){
            const unsigned head=local.pc,end=unsigned(instructionPc)+3U,span=end-head;
            if(head>=0x8000U&&end<=0x10000U&&span>=12U&&span<=16U&&
               (head>>13U)==((end-1U)>>13U)&&!(head<=0xfffbU&&end>0xfffaU)){
                // Одноразовое proof физическойROM, не сохранённый decode-cache или PRG-RAM.
                const auto source=bus_.quietRomWindow(static_cast<std::uint16_t>(head));
                Mmc5AffineLoopPlan plan;
                if(source.immutable&&source.physicalPage&&decodeMmc5AffineLoop(source.physicalPage+(head&8191U),span,static_cast<std::uint16_t>(head),plan)){
                    const unsigned iterations=(budget-consumed)/plan.cycles;
                    if(iterations){
                        const unsigned physical=plan.counter&0x7ffU;
                        const auto value=advanceMmc5Affine(ram[physical],ram[plan.stable&0x7ffU],plan.direction,iterations,local.p);
                        repeat.beforeWrite(physical);ram[physical]=value.value;local.a=value.value;local.p=value.status;
                        savedBase=plan.store;open=static_cast<std::uint8_t>(head>>8U);
                        consumed+=iterations*plan.cycles;
                        // local.pc ужеhead; commonpublish единожды применяет cycles/pending/credits.
                    }
                }
            }
        }
    }
    if (!Mmc5Span&&branch && take && previousCycles && previousPc == local.pc) {
        const auto length = static_cast<std::uint16_t>(instructionPc - previousPc);
        const bool counted = previousOp == Op::DEX || previousOp == Op::DEY ||
            previousOp == Op::INX || previousOp == Op::INY;
        const bool reads = previousOp == Op::LDA || previousOp == Op::LDX || previousOp == Op::LDY ||
            previousOp == Op::BIT || previousOp == Op::CMP || previousOp == Op::CPX || previousOp == Op::CPY;
        if (length >= 1 && length <= 3 && (reads || (counted && op == Op::BNE)) &&
            budget - consumed >= 2U * (previousCycles + count)) {
            idleBranchPc = instructionPc; idleBranchCycles = count;
            yieldIdle = true;
            goto publish;
        }
    }
    if(!Mmc5Span)consumed += repeat.after(local, savedBase, open, pendingIrq,
                             consumed, instructionPc, budget - consumed
                             ,selfRepeatWindow && op == Op::JMP
                             );
    // Доказанный цикл заканчивается той же инструкцией и состоянием предшественника;
    // сохранить настоящую последнюю инструкцию для существующего двухинструкционного helper.
    previousPc = instructionPc; previousCycles = count; previousOp = op;
    }
publish:
    if (!consumed) return 0;
    local.cycles += consumed;
    state_=local;
    baseAddress_=savedBase;
    bus_.pendingCpuReads_=static_cast<std::uint8_t>(bus_.pendingCpuReads_+consumed);
    bus_.cpuReadBudget_=static_cast<std::uint8_t>(bus_.cpuReadBudget_-consumed);
    bus_.openBus_=open;
    irqAtLastCycleStart_=stableIrq; nmiAtLastCycleStart_=false;
    irqPending_=pendingIrq; nmiReady_=false;
    if (!Mmc5Span&&yieldIdle) {
        // Опубликовать до вызова проверенного helper. Он получает только
        // остаток исходного горизонта, а не новое будущее окно.
        const auto beforeIdle = bus_.cpuCycles();
        bus_.collapseIdleLoop(previousPc, idleBranchPc, previousCycles, idleBranchCycles, quietDeadline);
        consumed += static_cast<std::uint32_t>(bus_.cpuCycles() - beforeIdle);
    }
    if (!Mmc5Span&&!yieldIdle && (previousOp == Op::BEQ || previousOp == Op::BVC ||
        (previousOp == Op::BPL && baseAddress_ == 0x2002U)))
        consumed += bus_.collapseStatusPollLoop(previousPc, previousCycles, quietDeadline);
    return consumed;
}
/** @brief Публикует единственный внешний template-вход Machine для строгого MMC5 grant. */
template std::uint32_t Cpu6502::runQuietBlockWithPolicy<true,false>(unsigned maxCycles) noexcept;
#undef NES_CPU_QUIET_INLINE
} // пространство имён nes::core
#undef NES_CPU_SIZE_PEEK_INLINE
