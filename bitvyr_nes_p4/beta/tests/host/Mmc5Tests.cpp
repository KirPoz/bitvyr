/**
 * @file Mmc5Tests.cpp
 * @brief Направленные проверки цифрового MMC5 по независимым адресным ожиданиям.
 */
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5RasterPlan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__)
#include <sys/mman.h>
#endif

namespace nes::core {
/** @brief Тестовый доступ к qualified data-path; не добавляет production-полей/наблюдателей. */
struct Mmc5TestAccess {
    using Tile = Mmc5::BackgroundTile;
    using Plan = Mmc5RasterPlan;
    /** @brief Число успешно подготовленных новых планов только в host-эталоне. */
    static inline unsigned preparedPlans=0;
    /**
     * @brief Выбирает аппаратную policy один раз для вызова независимого теста.
     * @param plan Готовый локальный план; его mapper и backing живут до возврата.
     * @param call Синхронная операция, получающая compile-time Policy; указатель не сохраняется.
     * @return Результат операции без дополнительной эмуляции шинного события.
     */
    template<class Call>static decltype(auto) dispatch(Plan& plan,Call&& call){
        using P=Plan::Policy;
        switch(plan.policy()){
        case P::Normal:return call(std::integral_constant<P,P::Normal>{});
        case P::Extended:return call(std::integral_constant<P,P::Extended>{});
        case P::SplitNormal:return call(std::integral_constant<P,P::SplitNormal>{});
        case P::SplitExtended:return call(std::integral_constant<P,P::SplitExtended>{});
        }
        std::abort();
    }
    /**
     * @brief Готовит новый borrowed-план, не меняя цифровое состояние mapper.
     * @param m Mapper единственного host-потока; он переживает plan.
     * @param plan Выходной объект, который должен остаться прежним при отказе.
     * @param ciram Неизменяемые в пределах тестового участка2048 байт либо nullptr.
     * @param control PPUCTRL для паттерн-адреса полного тайла.
     * @return true при готовом плане; это не доказательство выполнения production пути.
     */
    static bool prepare(Mmc5& m,Plan& plan,const std::uint8_t* ciram,std::uint8_t control){
        const bool ready=m.prepareRasterPlan(plan,ciram,control);
        if(ready)++preparedPlans;
        return ready;
    }
    /**
     * @brief Выполняет ровно один data-тайл через новую подготовленную policy.
     * @param plan План только этого synchronous участка без промежуточного MMIO.
     * @param v Loopy v до настоящего Name-fetch.
     * @param x Экранная fetch-колонка0..33, включая prefetch0/1.
     * @param needPlanes false запрещает все CHR byte-loads, сохраняя metadata/latches.
     * @return Те же поля тайла, что возвращает старый независимый helper.
     */
    static Tile planned(Plan& plan,std::uint16_t v,unsigned x,bool needPlanes=true){
        /** @brief Направляет fixture в конкретную compile-time policy, сохраняя одинаковые входные данные. */
        return dispatch(plan,[&](auto tag){return plan.template fetch<decltype(tag)::value>(v,x,needPlanes);});
    }
    /**
     * @brief Передаёт один настоящий read-strobe и данные через новую typed policy.
     * @param plan Свежий план после последней влияющей CPU-записи.
     * @param address Исходный адрес PPU; также проверяются удерживаемые адреса другого домена.
     * @param kind Только три BG-роли: Name, Attribute либо Pattern.
     * @param x Fetch-колонка для Name, без изменения mapper во время callback.
     * @return Прочитанный байт; IRQ и Name-latch остаются у mapper.
     */
    static std::uint8_t partial(Plan& plan,std::uint16_t address,Mmc5::Fetch kind,unsigned x){
        /** @brief Направляет fixture в конкретную compile-time policy, сохраняя одинаковые входные данные. */
        return dispatch(plan,[&](auto tag){
            if(kind==Mmc5::Fetch::BackgroundName)return plan.template readName<decltype(tag)::value>(address,x);
            if(kind==Mmc5::Fetch::BackgroundAttribute)return plan.template readAttribute<decltype(tag)::value>(address);
            return plan.template readPattern<decltype(tag)::value>(address);
        });
    }
    /** @brief Возвращает версию оптимизационного mapping, не продвигая гостевое время. */
    static std::uint32_t version(const Mmc5& m){return m.rasterPlanVersion();}
    /** @brief Создаёт достижимый предел счётчика для bounded wrap-проверки host-теста. */
    static void versionSeed(Mmc5& m,std::uint32_t version){m.rasterVersion_=version;}
    /**
     * @brief Задаёт граничный цифровой срез перед настоящим read-strobe.
     * @param m Mapper пары, не используемый другим потоком.
     * @param seed 0 — обычный read, 1 — начало frame, 2 — scanline IRQ, 3 — конец239-й строки.
     * @details Все последующие detector-переходы выполняет реальный старый/новый read, не тестовая формула.
     */
    static void detectorSeed(Mmc5& m,unsigned seed){
        m.cpuIdle_=0;m.ppuReadSinceM2_=true;m.lastPpuAddress_=0x2000;m.nameRepeat_=seed?3:0;
        m.inFrame_=seed!=1;m.scanline_=seed==3?239:0;m.splitY_=seed==3?239:7;
        m.irqCompare_=1;m.irqEnabled_=true;m.irqPending_=seed==3;
    }
    /** @brief Запускает новую атомарную data-выборку без цифровых /RD edges. */
    static Tile tile(Mmc5& m, std::uint16_t v, std::uint8_t control,
                     const std::uint8_t* ciram, unsigned x, bool needPlanes = true) {
        return m.ppuReadQuietBackgroundTile(v, control, ciram, x, needPlanes);
    }
    /** @brief Независимо собирает прежние четыре typed-read и отдельно возвращает high offset. */
    static Tile reference(Mmc5& m, std::uint16_t v, std::uint8_t control,
                          const std::uint8_t* ciram, unsigned x, std::size_t& highOffset) {
        const auto name = static_cast<std::uint16_t>(0x2000U | (v & 0x0fffU));
        const auto tile = m.ppuReadQuietBackground(name, Mmc5::Fetch::BackgroundName, ciram, x).value;
        const auto attrAddress = static_cast<std::uint16_t>(0x23c0U | (v & 0x0c00U) |
            ((v >> 4U) & 0x38U) | ((v >> 2U) & 7U));
        const auto attr = m.ppuReadQuietBackground(attrAddress, Mmc5::Fetch::BackgroundAttribute, ciram, x).value;
        const auto lowAddress = static_cast<std::uint16_t>(((control & 0x10U) ? 0x1000U : 0U) +
            unsigned(tile) * 16U + ((v >> 12U) & 7U));
        const auto low = m.ppuReadQuietBackground(lowAddress, Mmc5::Fetch::BackgroundPattern, ciram, x);
        const auto high = m.ppuReadQuietBackground(static_cast<std::uint16_t>(lowAddress + 8U),
            Mmc5::Fetch::BackgroundPattern, ciram, x);
        highOffset = high.chrOffset;
        return {tile, attr, low.value, high.value, low.chrOffset};
    }
    /** @brief Снимает все цифровые поля, которые quiet data-path обязан оставить неизменными. */
    static std::array<std::uint32_t, 9> detector(const Mmc5& m) {
        return {m.lastPpuAddress_, m.nameRepeat_, m.cpuIdle_, m.ppuReadSinceM2_,
            m.inFrame_, m.scanline_, m.irqEnabled_, m.irqPending_, m.splitY_};
    }
    /** @brief Снимает весь Name-latch для сравнения следующего pattern/attribute read. */
    static std::array<std::uint32_t, 6> latch(const Mmc5& m) {
        return {m.tileAttribute_, m.tileSplitAttribute_, m.tileSplitY_, m.tileNtOffset_,
            m.tileExtended_, m.tileSplit_};
    }
};
}

namespace {
using nes::core::Mmc5;
using nes::core::Mmc5TestAccess;
unsigned checks = 0;
unsigned rasterRuns=0,rasterPartialReads=0;

/** @brief Завершает fixture с ненулевым кодом и точной строкой нарушения. */
[[noreturn]] void fail(const char* text, unsigned line) {
    std::fprintf(stderr, "MMC5 test line %u: %s\n", line, text);
    std::exit(1);
}
#define CHECK(condition) do { ++checks; if (!(condition)) fail(#condition, __LINE__); } while (false)

/** @brief Владеет backing отдельно от малого mapper; банки содержат адресные маркеры. */
struct Fixture {
    std::vector<std::uint8_t> prg, chr, ram;
    std::array<std::uint8_t, 2048> ciram{};
    Mmc5 mapper;
    /** @brief Формирует отдельный PRG8k/CHR1k marker; WRAM сохраняет исходный sentinel. */
    explicit Fixture(std::size_t ramBytes = 8192, bool sl = false)
        : prg(1024U * 1024U), chr(1024U * 1024U), ram(ramBytes, 0x6d) {
        for (std::size_t i = 0; i < prg.size(); ++i) prg[i] = static_cast<std::uint8_t>(i / 8192U);
        for (std::size_t i = 0; i < chr.size(); ++i) chr[i] = static_cast<std::uint8_t>(i / 1024U);
        CHECK(mapper.attach(prg.data(), prg.size(), chr.data(), chr.size(),
                            ram.empty() ? nullptr : ram.data(), ram.size(), sl));
    }
    /** @brief Снимает оба независимых аппаратных запрета записи WRAM. */
    void unlock() { mapper.cpuWrite(0x5102, 2); mapper.cpuWrite(0x5103, 1); }
    /** @brief Передаёт три одинаковых NT-read и следующий attribute-read. */
    void line() {
        mapper.ppuReadStrobe(0x0010);
        mapper.ppuReadStrobe(0x2000); mapper.ppuReadStrobe(0x2000); mapper.ppuReadStrobe(0x2000);
        mapper.ppuReadStrobe(0x23c0);
    }
    /** @brief Заполняет ExRAM через настоящий CPU режим2 перед режимом rendering. */
    void ex(unsigned index, std::uint8_t value) {
        mapper.cpuWrite(0x5104, 2); mapper.cpuWrite(static_cast<std::uint16_t>(0x5c00U + index), value);
    }
};

/** @brief Проверяет отказ attach, reset-vector ROM и все четыре PRG режима/выравнивания. */
void prgModes() {
    Fixture f;
    auto& m = f.mapper;
    CHECK(m.cpuRead(0xfffc) == 127);
    CHECK(!m.canUseDirectPrg());
    m.cpuWrite(0x5114, 0x85); m.cpuWrite(0x5115, 0x8b);
    m.cpuWrite(0x5116, 0x91); m.cpuWrite(0x5117, 0x17);
    const unsigned expected[4][4] = {{20,21,22,23}, {10,11,22,23}, {10,11,17,23}, {5,11,17,23}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        m.cpuWrite(0x5100, static_cast<std::uint8_t>(mode));
        for (unsigned page = 0; page < 4; ++page) {
            const auto a = static_cast<std::uint16_t>(0x8000U + page * 8192U);
            CHECK(m.cpuRead(a) == expected[mode][page]);
            CHECK(m.cpuRead(static_cast<std::uint16_t>(a + 8191U)) == expected[mode][page]);
            CHECK(m.prgPage(page) == f.prg.data() + expected[mode][page] * 8192U);
            CHECK(m.writablePrg(a) == nullptr);
        }
    }
    CHECK(m.prgPage(4) == nullptr);
    CHECK(m.cpuRead(0x5800, 0xa6) == 0xa6);
    CHECK(m.cpuRead(0x5100, 0xb3) == 0xb3);
    CHECK(!m.attach(nullptr, f.prg.size(), f.chr.data(), f.chr.size(), f.ram.data(), f.ram.size()));
    CHECK(m.prgPage(0) == nullptr); CHECK(m.cpuRead(0x8000, 0x5a) == 0x5a);
    CHECK(!m.attach(f.prg.data(), 10000, f.chr.data(), f.chr.size(), nullptr, 0));
    CHECK(!m.attach(f.prg.data(), f.prg.size(), f.chr.data(), f.chr.size(), f.ram.data(), 123));
}

/** @brief Проверяет CE alias/open bus каждого поддержанного типа SRAM и защиту. */
void prgRam() {
    for (std::size_t bytes : {std::size_t(0), std::size_t(8192), std::size_t(16384),
                              std::size_t(32768), std::size_t(65536), std::size_t(131072)}) {
        Fixture f(bytes); auto& m = f.mapper;
        for (unsigned b = 0; b < 16; ++b) {
            m.cpuWrite(0x5113, static_cast<std::uint8_t>(b));
            bool connected = bytes != 0;
            unsigned physical = b;
            if (bytes == 8192 || bytes == 32768) connected = (b & 4U) == 0;
            if (bytes == 8192) physical = 0;
            if (bytes == 16384) physical = (b & 4U) ? 1 : 0;
            if (bytes == 32768) physical = b & 3U;
            if (bytes == 65536) physical = b & 7U;
            CHECK(m.cpuRead(0x6007, 0xd2) == (connected ? 0x6d : 0xd2));
            m.cpuWrite(0x6007, 0x90); // Защита записи после включения питания.
            CHECK(m.cpuRead(0x6007, 0xd2) == (connected ? 0x6d : 0xd2));
            m.cpuWrite(0x5102, 0xfe); // Оба младших бита должны совпасть; старшие не влияют.
            m.cpuWrite(0x6007, 0x90);
            CHECK(m.cpuRead(0x6007, 0xd2) == (connected ? 0x6d : 0xd2));
            m.cpuWrite(0x5103, 0xfd);
            m.cpuWrite(0x6007, 0x90);
            CHECK(m.cpuRead(0x6007, 0xd2) == (connected ? 0x90 : 0xd2));
            if (connected) { CHECK(f.ram[physical * 8192U + 7U] == 0x90); f.ram[physical * 8192U + 7U] = 0x6d; }
            m.cpuWrite(0x5102, 1); m.cpuWrite(0x5103, 2);
        }
        f.unlock(); m.cpuWrite(0x5113, 0); m.cpuWrite(0x5114, 0);
        m.cpuWrite(0x8007, 0xc9);
        CHECK(m.cpuRead(0x6007, 0x44) == (bytes ? 0xc9 : 0x44));
        m.cpuWrite(0x5115, 1); m.cpuWrite(0x5100, 2); // Окно RAM16KiB начинается с$8000.
        m.cpuWrite(0x8008, 0x51); m.cpuWrite(0xa009, 0x52);
        if (bytes) { CHECK(f.ram[8] == 0x51); CHECK(m.cpuRead(0xa009) == 0x52); }
        m.cpuWrite(0x5117, 0); CHECK(m.writablePrg(0xe000) == nullptr);
    }
}

/** @brief Проверяет CHR-ширины, обе группы, latch upper bits и точное декодирование PPUCTRL. */
void chrModes() {
    Fixture f; auto& m = f.mapper;
    for (unsigned mode = 0; mode < 4; ++mode) {
        m.cpuWrite(0x5101, static_cast<std::uint8_t>(mode));
        const unsigned width = 8192U >> mode;
        const unsigned last = (width / 1024U) - 1U;
        m.cpuWrite(static_cast<std::uint16_t>(0x5120U + last), 5);
        CHECK(m.chrOffset(0) == 5U * width);
        CHECK(m.chrOffset(static_cast<std::uint16_t>(width - 1)) == 6U * width - 1U);
    }
    m.cpuWrite(0x5101, 3); m.cpuWrite(0x5130, 3); m.cpuWrite(0x5120, 0xff);
    CHECK(m.chrOffset(0x123) == 1023U * 1024U + 0x123U);
    m.cpuWrite(0x5130, 0); CHECK(m.chrOffset(0x123) == 1023U * 1024U + 0x123U);
    m.cpuWrite(0x5120, 2); m.cpuWrite(0x5128, 9);
    m.cpuWrite(0x2000, 0x20); m.cpuWrite(0x2001, 0x18);
    CHECK(m.chrOffset(0, Mmc5::Fetch::SpritePattern) == 2048);
    CHECK(m.chrOffset(0, Mmc5::Fetch::BackgroundPattern) == 9216);
    CHECK(m.chrOffset(0, Mmc5::Fetch::Cpu) == 9216);
    m.cpuWrite(0x5120, 3); CHECK(m.chrOffset(0, Mmc5::Fetch::Cpu) == 3072);
    m.cpuWrite(0x2008, 0); CHECK(m.chrOffset(0, Mmc5::Fetch::BackgroundPattern) == 9216);
    m.cpuWrite(0x2000, 0); CHECK(m.chrOffset(0, Mmc5::Fetch::BackgroundPattern) == 3072);
    m.cpuWrite(0x5128, 12); CHECK(m.chrOffset(0, Mmc5::Fetch::Cpu) == 3072);
    m.cpuWrite(0x2000, 0x20); m.cpuWrite(0x2009, 0);
    CHECK(m.chrOffset(0, Mmc5::Fetch::BackgroundPattern) == 12288);
    m.cpuWrite(0x2001, 0); CHECK(m.chrOffset(0, Mmc5::Fetch::BackgroundPattern) == 3072);
    // Смена режима не сдвигает задним числом сохранённые физические линии.
    m.cpuWrite(0x5101, 2); m.cpuWrite(0x5121, 5); m.cpuWrite(0x5101, 3);
    CHECK(m.chrOffset(0x400) == 11U * 1024U);
}

/** @brief Проверяет все NT-источники, ExRAM CPU/PPU-порты и per-tile расширенные атрибуты. */
void nametables() {
    Fixture f; auto& m = f.mapper;
    m.cpuWrite(0x5105, 0xe4); // Проводка NT0=CIRAM0, NT1=CIRAM1, NT2=ExRAM, NT3=fill.
    m.cpuWrite(0x5106, 0xab); m.cpuWrite(0x5107, 2);
    m.ppuWrite(0x2012, 0x31, f.ciram.data()); m.ppuWrite(0x2412, 0x72, f.ciram.data());
    CHECK(f.ciram[0x12] == 0x31); CHECK(f.ciram[0x412] == 0x72);
    CHECK(m.nametablePeek(0x3012, f.ciram.data()) == 0x31);
    CHECK(m.nametablePeek(0x2c12, f.ciram.data()) == 0xab);
    CHECK(m.nametablePeek(0x2fc0, f.ciram.data()) == 0xaa);
    m.ppuWrite(0x2c12, 1, f.ciram.data()); CHECK(m.nametablePeek(0x2c12, nullptr) == 0xab);
    f.ex(0x12, 0xc5); CHECK(m.cpuRead(0x5c12) == 0xc5);
    CHECK(m.nametablePeek(0x2812, nullptr) == 0); // В mode2 ExRAM не является nametable.
    m.cpuWrite(0x5104, 3); m.cpuWrite(0x5c12, 0); CHECK(m.cpuRead(0x5c12) == 0xc5);
    m.cpuWrite(0x5104, 0); CHECK(m.cpuRead(0x5c12, 0x97) == 0x97);
    CHECK(m.nametablePeek(0x2812, nullptr) == 0xc5);
    m.cpuWrite(0x5c12, 0); CHECK(m.nametablePeek(0x2812, nullptr) == 0xc5);
    m.ppuWrite(0x2813, 0x9e, f.ciram.data()); CHECK(m.nametablePeek(0x2813, nullptr) == 0x9e);
    m.cpuWrite(0x2001, 0x18); f.line(); m.cpuWrite(0x5c13, 0x7e);
    CHECK(m.nametablePeek(0x2813, nullptr) == 0x7e);
    m.cpuWrite(0x5104, 1);
    auto name = m.ppuRead(0x2012, Mmc5::Fetch::BackgroundName, f.ciram.data(), 4);
    CHECK(name.value == 0x31); CHECK(name.chrOffset == Mmc5::InvalidChrOffset);
    auto attr = m.ppuRead(0x23c0, Mmc5::Fetch::BackgroundAttribute, f.ciram.data(), 4);
    CHECK(attr.value == 0xff);
    auto low = m.ppuRead(0x0123, Mmc5::Fetch::BackgroundPattern, f.ciram.data(), 4);
    CHECK(low.chrOffset == 5U * 4096U + 0x123U);
    m.cpuWrite(0x5130, 2);
    auto high = m.ppuRead(0x112b, Mmc5::Fetch::BackgroundPattern, f.ciram.data(), 4);
    CHECK(high.chrOffset == 133U * 4096U + 0x12bU);
    m.cpuWrite(0x5104, 0); // Запрет после Name действует раньше сохранённой tile-защёлки.
    CHECK(m.ppuRead(0x23c0, Mmc5::Fetch::BackgroundAttribute, f.ciram.data()).value == 0);
    CHECK(m.ppuRead(0x0123, Mmc5::Fetch::BackgroundPattern, nullptr).chrOffset == 0x123);
    m.cpuWrite(0x5104, 1);
    m.ppuRead(0x2012, Mmc5::Fetch::BackgroundName, f.ciram.data());
    m.cpuWrite(0x2001, 0);
    CHECK(m.ppuRead(0x23c0, Mmc5::Fetch::BackgroundAttribute, f.ciram.data()).value == 0);
    CHECK(m.ppuRead(0x0123, Mmc5::Fetch::BackgroundPattern, nullptr).chrOffset == 0x123);
    CHECK(m.ppuRead(0x2fc0, Mmc5::Fetch::Cpu, nullptr).value == 0xaa);
    CHECK(m.ppuRead(0x3f00, Mmc5::Fetch::Cpu, nullptr).chrOffset == Mmc5::InvalidChrOffset);
}

/** @brief Проверяет IRQ по fourth-read edge, acknowledge, idle M2 и настоящим CPU-strobes. */
void irq() {
    Fixture f; auto& m = f.mapper;
    m.cpuWrite(0x5203, 1);
    for (unsigned i = 0; i < 3; ++i) m.ppuReadStrobe(0x2000);
    CHECK(!m.inFrame()); m.ppuReadStrobe(0x23c0);
    CHECK(m.inFrame() && m.scanline() == 0 && !m.irqLine());
    f.line(); CHECK(m.scanline() == 1 && !m.irqLine()); // При запрещённом IRQ pending всё равно защёлкивается.
    CHECK((m.cpuPeek(0x5204) & 0xc0U) == 0xc0U);
    m.cpuWrite(0x5204, 0x80); CHECK(m.irqLine());
    m.cpuWrite(0x5204, 0); CHECK(!m.irqLine());
    m.cpuWrite(0x5204, 0x80); CHECK(m.irqLine());
    CHECK((m.cpuRead(0x5204) & 0xc0U) == 0xc0U); CHECK(!m.irqLine());
    m.cpuWrite(0x5203, 2); f.line(); CHECK(m.irqLine());
    CHECK(m.cpuPeek(0xfffa) == 127); CHECK(m.irqLine());
    m.cpuRead(0xfffa); CHECK(!m.irqLine() && !m.inFrame());
    f.line(); f.line(); f.line(); CHECK(m.irqLine());
    m.clockCpu(); m.clockCpu(2); CHECK(m.inFrame());
    m.clockCpu(); CHECK(!m.inFrame()); CHECK(m.irqLine()); // Idle не подтверждает pending IRQ.
    f.line(); CHECK(m.scanline() == 0 && !m.irqLine());
    m.cpuWrite(0x5203, 1); f.line(); CHECK(m.irqLine());
    m.resetSignal(); CHECK(!m.irqLine());
    m.cpuWrite(0x5204, 0x80); CHECK(m.irqLine());
    m.cpuReadStrobe(0x200a, 0x80); CHECK(m.irqLine());
    m.cpuReadStrobe(0x2002, 0x80); CHECK(!m.irqLine() && !m.inFrame());
    f.line(); f.line(); CHECK(m.irqLine());
    m.cpuWrite(0x4014, 2); CHECK(!m.irqLine() && !m.inFrame());
    m.cpuWrite(0x5203, 0); f.line();
    for (unsigned i = 1; i < 240; ++i) f.line();
    CHECK(m.inFrame() && m.scanline() == 239 && !m.irqLine());
    f.line(); CHECK(!m.inFrame() && !m.irqLine());
    m.powerOn();
    m.ppuReadStrobe(0x3000); m.ppuReadStrobe(0x3000); m.ppuReadStrobe(0x3000); m.ppuReadStrobe(0);
    CHECK(!m.inFrame());
    for (unsigned i = 0; i < 4; ++i) m.ppuReadStrobe(0x2000);
    CHECK(m.inFrame() && m.scanline() == 0);
    for (unsigned i = 0; i < 3; ++i) m.ppuReadStrobe(0x2000);
    CHECK(m.scanline() == 1); // Длинная повторяющаяся последовательность снова взводит детектор.
}

/** @brief Проверяет 65536 произведений и отсутствие read-sideeffect у multiplier. */
void multiplier() {
    Fixture f; auto& m = f.mapper;
    for (unsigned a = 0; a < 256; ++a) {
        m.cpuWrite(0x5205, static_cast<std::uint8_t>(a));
        for (unsigned b = 0; b < 256; ++b) {
            m.cpuWrite(0x5206, static_cast<std::uint8_t>(b));
            CHECK(m.cpuRead(0x5205) == (a * b & 255U));
            CHECK(m.cpuRead(0x5206) == (a * b >> 8));
        }
    }
}

/** @brief Проверяет именно reset-detected поля, сохраняя RAM и незатронутые CHR banks. */
void resetDetection() {
    Fixture f; auto& m = f.mapper;
    f.unlock(); m.cpuWrite(0x6000, 0x73);
    m.cpuWrite(0x5101, 3); m.cpuWrite(0x5120, 0x29);
    m.cpuWrite(0x5117, 0x82); m.cpuWrite(0x5100, 0);
    m.cpuWrite(0x5104, 2); m.cpuWrite(0x5c00, 0x45);
    m.cpuWrite(0x5203, 1); m.cpuWrite(0x5204, 0x80);
    f.line(); f.line(); CHECK(m.irqLine());
    m.resetSignal();
    CHECK(!m.irqLine()); CHECK(m.inFrame() && m.scanline() == 1);
    CHECK(m.cpuPeek(0x5204) == 0xc0);
    CHECK(m.cpuRead(0xfffc) == 127); CHECK(m.chrOffset(0) == 0x29U * 1024U);
    CHECK(m.cpuRead(0x5c00) == 0x45); m.cpuWrite(0x5c00, 0); CHECK(m.cpuRead(0x5c00) == 0x45);
    m.cpuWrite(0x6000, 0); CHECK(m.cpuRead(0x6000) == 0x73);
    m.cpuWrite(0x5204, 0x80); CHECK(m.irqLine());
}

/** @brief Проверяет split threshold, ExRAM атрибуты, отдельный CHR-bank и CL/SL-проводку. */
void split() {
    for (bool sl : {false, true}) {
        Fixture f(8192, sl); auto& m = f.mapper;
        f.ex(42, 0x77); f.ex(0x3c2, 0xaa);
        m.cpuWrite(0x5104, 0); m.cpuWrite(0x5201, 9); m.cpuWrite(0x5202, 3);
        m.cpuWrite(0x5200, 0x8c); m.cpuWrite(0x2001, 0x18);
        const auto name = m.ppuRead(0x2000, Mmc5::Fetch::BackgroundName, f.ciram.data(), 10);
        CHECK(name.value == 0x77);
        CHECK(m.ppuRead(0x23c0, Mmc5::Fetch::BackgroundAttribute, f.ciram.data(), 10).value == 0xaa);
        CHECK(m.ppuRead(0x0125, Mmc5::Fetch::BackgroundPattern, f.ciram.data(), 10).chrOffset ==
              3U * 4096U + (sl ? 0x121U : 0x125U));
        m.cpuWrite(0x5200, 0); // Запрет между Name и следующим Pattern действует сразу.
        CHECK(m.ppuRead(0x0125, Mmc5::Fetch::BackgroundPattern, nullptr).chrOffset == 0x125);
        m.cpuWrite(0x5200, 0x8c);
        CHECK(m.ppuRead(0x2000, Mmc5::Fetch::BackgroundName, f.ciram.data(), 12).value == 0);
        m.cpuWrite(0x5200, 0xcc);
        CHECK(m.ppuRead(0x2000, Mmc5::Fetch::BackgroundName, f.ciram.data(), 10).value == 0);
        f.ex(44, 0x53); m.cpuWrite(0x5104, 0);
        CHECK(m.ppuRead(0x2000, Mmc5::Fetch::BackgroundName, f.ciram.data(), 12).value == 0x53);
        m.cpuWrite(0x5104, 2);
        CHECK(m.ppuRead(0x2000, Mmc5::Fetch::BackgroundName, f.ciram.data(), 12).value == 0);
    }
}

/**
 * @brief Сравнивает полный тайл с четырьмя прежними reads и проверяет границу high plane.
 * @details Reference использует отдельную копию только в unit-тесте. Сверяются все
 * per-tile latch и неизменность цифрового detector: новый helper не создаёт /RD/M2.
 */
Mmc5TestAccess::Tile checkQuietTile(Mmc5& mapper, std::uint16_t v,
                                  std::uint8_t control, const std::uint8_t* ciram, unsigned x) {
    auto reference = mapper;
    auto descriptor = mapper;
    auto prepared = mapper;
    auto preparedMetadata = mapper;
    const auto before = Mmc5TestAccess::detector(mapper);
    std::size_t highOffset = Mmc5::InvalidChrOffset;
    const auto expected = Mmc5TestAccess::reference(reference, v, control, ciram, x, highOffset);
    const auto actual = Mmc5TestAccess::tile(mapper, v, control, ciram, x);
    CHECK(actual.tile == expected.tile); CHECK(actual.attribute == expected.attribute);
    CHECK(actual.low == expected.low); CHECK(actual.high == expected.high);
    CHECK(actual.lowChrOffset == expected.lowChrOffset);
    CHECK(highOffset == (actual.lowChrOffset == Mmc5::InvalidChrOffset ?
        Mmc5::InvalidChrOffset : actual.lowChrOffset + 8U));
    CHECK(Mmc5TestAccess::latch(mapper) == Mmc5TestAccess::latch(reference));
    CHECK(Mmc5TestAccess::detector(mapper) == before);
    CHECK(Mmc5TestAccess::detector(reference) == before);
    const auto metadata = Mmc5TestAccess::tile(descriptor, v, control, ciram, x, false);
    CHECK(metadata.tile == actual.tile && metadata.attribute == actual.attribute);
    CHECK(metadata.lowChrOffset == actual.lowChrOffset);
    CHECK(metadata.low == 0 && metadata.high == 0);
    CHECK(Mmc5TestAccess::latch(descriptor) == Mmc5TestAccess::latch(mapper));
    CHECK(Mmc5TestAccess::detector(descriptor) == before);
    Mmc5TestAccess::Plan plan{},metadataPlan{};
    const auto preparedVersion=Mmc5TestAccess::version(prepared);
    const auto beforePrepared=Mmc5TestAccess::latch(prepared);
    std::array<unsigned char,sizeof(plan)> planBefore{};
    std::memcpy(planBefore.data(),&plan,sizeof(plan));
    const bool ready=Mmc5TestAccess::prepare(prepared,plan,ciram,control);
    CHECK(ready==(actual.lowChrOffset!=Mmc5::InvalidChrOffset));
    CHECK(Mmc5TestAccess::version(prepared)==preparedVersion);
    CHECK(Mmc5TestAccess::latch(prepared)==beforePrepared);
    CHECK(Mmc5TestAccess::detector(prepared)==before);
    if(ready){
        const auto value=Mmc5TestAccess::planned(plan,v,x);
        CHECK(value.tile==actual.tile&&value.attribute==actual.attribute);
        CHECK(value.low==actual.low&&value.high==actual.high&&value.lowChrOffset==actual.lowChrOffset);
        CHECK(Mmc5TestAccess::latch(prepared)==Mmc5TestAccess::latch(mapper));
        CHECK(Mmc5TestAccess::detector(prepared)==before);
        CHECK(Mmc5TestAccess::prepare(preparedMetadata,metadataPlan,ciram,control));
        const auto onlyMetadata=Mmc5TestAccess::planned(metadataPlan,v,x,false);
        CHECK(onlyMetadata.tile==actual.tile&&onlyMetadata.attribute==actual.attribute);
        CHECK(onlyMetadata.low==0&&onlyMetadata.high==0&&onlyMetadata.lowChrOffset==actual.lowChrOffset);
        CHECK(Mmc5TestAccess::latch(preparedMetadata)==Mmc5TestAccess::latch(mapper));
        CHECK(Mmc5TestAccess::detector(preparedMetadata)==before);
    }else{
        CHECK(std::memcmp(planBefore.data(),&plan,sizeof(plan))==0);
    }
    return actual;
}

/** @brief Заполняет planes отличающимися адресными маркерами, а не одинаковым номером банка. */
void fillTileData(Fixture& f) {
    for (std::size_t i = 0; i < f.chr.size(); ++i)
        f.chr[i] = static_cast<std::uint8_t>(i * 13U + (i >> 10U) * 19U + (i >> 8U));
    for (unsigned i = 0; i < f.ciram.size(); ++i)
        f.ciram[i] = static_cast<std::uint8_t>(i * 29U + 255U);
    for (unsigned i = 0; i < 1024U; ++i) f.ex(i, static_cast<std::uint8_t>(i * 11U + 0xc5U));
}

/** @brief Проверяет normal банки, размеры ROM, оба набора CHR и границы nametable/fine Y. */
void quietTileNormal() {
    Fixture f; fillTileData(f); auto& m = f.mapper;
    constexpr unsigned addresses[] = {0, 31, 0x3bf, 0x3ff, 0x400, 0x7ff, 0x800, 0xbff, 0xc00, 0xfff};
    for (std::size_t bytes : {std::size_t(1024), std::size_t(4096), std::size_t(65536), f.chr.size()}) {
        CHECK(m.attach(f.prg.data(), f.prg.size(), f.chr.data(), bytes, f.ram.data(), f.ram.size()));
        for (unsigned mode = 0; mode < 4U; ++mode) {
            m.cpuWrite(0x5101, static_cast<std::uint8_t>(mode)); m.cpuWrite(0x5130, 3);
            for (unsigned i = 0; i < 12U; ++i)
                m.cpuWrite(static_cast<std::uint16_t>(0x5120U + i), static_cast<std::uint8_t>(0xf3U + i * 7U));
            m.cpuWrite(0x5105, 0xe4); m.cpuWrite(0x5106, 0xff); m.cpuWrite(0x5107, 2);
            m.cpuWrite(0x5104, 0);
            for (unsigned control : {0U, 0x10U, 0x20U, 0x30U}) {
                m.cpuWrite(0x2000, static_cast<std::uint8_t>(control));
                for (unsigned mask : {0U, 0x18U}) {
                    m.cpuWrite(0x2001, static_cast<std::uint8_t>(mask));
                    for (unsigned address : addresses) for (unsigned fineY : {0U, 1U, 7U})
                        for (unsigned x : {0U, 2U, 31U, 33U})
                            checkQuietTile(m, static_cast<std::uint16_t>(address | (fineY << 12U)),
                                static_cast<std::uint8_t>(control), f.ciram.data(), x);
                }
            }
        }
    }
    // Явное normal-ожидание: BG tall использует B, а не последний записанный A.
    CHECK(m.attach(f.prg.data(), f.prg.size(), f.chr.data(), f.chr.size(), nullptr, 0));
    m.cpuWrite(0x5101, 3); m.cpuWrite(0x5128, 0x39); m.cpuWrite(0x5120, 0xab);
    m.cpuWrite(0x2000, 0x20); m.cpuWrite(0x2001, 0x18);
    m.cpuWrite(0x5105, 0xff); m.cpuWrite(0x5106, 0x3f);
    const auto normal = checkQuietTile(m, 0x7000U, 0x20U, nullptr, 2);
    CHECK(normal.lowChrOffset == 0x39U * 1024U + 0x3f7U);
    CHECK(normal.low == f.chr[normal.lowChrOffset] && normal.high == f.chr[normal.lowChrOffset + 8U]);
    // Явное адресное ожидание независимо от старого helper: последняя plane ROM.
    CHECK(m.attach(f.prg.data(), f.prg.size(), f.chr.data(), 1024U, nullptr, 0));
    m.cpuWrite(0x5105, 0xff); m.cpuWrite(0x5106, 0xff); m.cpuWrite(0x5107, 3);
    const auto end = checkQuietTile(m, 0x7fffU, 0x10U, nullptr, 33U);
    CHECK(end.tile == 0xffU && end.attribute == 0xffU);
    CHECK(end.lowChrOffset == 1015U); CHECK(end.low == f.chr[1015U]); CHECK(end.high == f.chr[1023U]);
}

/** @brief Проверяет ExAttr/split CL/SL, prefetch, threshold, отключение и frame/splitY wrap. */
void quietTilePolicies() {
    for (bool sl : {false, true}) {
        Fixture f(8192, sl); fillTileData(f); auto& m = f.mapper;
        m.cpuWrite(0x5105, 0xe4); m.cpuWrite(0x5106, 0x3f); m.cpuWrite(0x5107, 1);
        m.cpuWrite(0x5202, 0xff);
        for (unsigned exMode = 0; exMode < 4U; ++exMode)
            for (unsigned splitControl : {0U, 0x80U, 0x9fU, 0xbfU, 0xc0U, 0xdfU, 0xffU})
                for (unsigned mask : {0U, 0x18U}) for (unsigned upper : {0U, 3U}) {
                    m.cpuWrite(0x2001, static_cast<std::uint8_t>(mask));
                    m.cpuWrite(0x5104, static_cast<std::uint8_t>(exMode));
                    m.cpuWrite(0x5200, static_cast<std::uint8_t>(splitControl));
                    m.cpuWrite(0x5130, static_cast<std::uint8_t>(upper));
                    m.cpuWrite(0x5201, 239U);
                    for (unsigned v : {0U, 0x7c1fU, 0x73bfU, 0x4fffU})
                        for (unsigned x : {0U, 1U, 2U, 15U, 30U, 31U, 32U, 33U})
                            checkQuietTile(m, static_cast<std::uint16_t>(v), 0x10U, f.ciram.data(), x);
                }
        m.cpuWrite(0x2001, 0); m.cpuWrite(0x5201, 239); m.cpuWrite(0x2001, 0x18);
        m.cpuWrite(0x5104, 1); m.cpuWrite(0x5200, 0x9f); f.line();
        for (unsigned x : {0U, 1U, 2U, 30U, 31U, 32U})
            checkQuietTile(m, 0x7000U, 0, f.ciram.data(), x);
        // Независимое ожидание ExAttr: ExRAM задаёт bank и palette, high остаётся в том же bank.
        m.cpuWrite(0x5200, 0); m.cpuWrite(0x5130, 2); f.ex(0x12, 0xc5);
        m.cpuWrite(0x5104, 1); m.cpuWrite(0x2001, 0x18); f.ciram[0x12] = 0xff;
        const auto ex = checkQuietTile(m, 0x7012U, 0x10U, f.ciram.data(), 4);
        CHECK(ex.tile == 0xffU && ex.attribute == 0xffU);
        CHECK(ex.lowChrOffset == 133U * 4096U + 0xff7U);
        CHECK(ex.low == f.chr[ex.lowChrOffset] && ex.high == f.chr[ex.lowChrOffset + 8U]);
        // Split имеет отдельные Name/attribute и банк, SL меняет только fine Y.
        m.cpuWrite(0x2001, 0); m.cpuWrite(0x5201, 9); m.cpuWrite(0x5202, 3);
        f.ex(42, 0x77); f.ex(0x3c2, 0xaa); m.cpuWrite(0x5104, 0);
        m.cpuWrite(0x5200, 0x8c); m.cpuWrite(0x2001, 0x18);
        const auto split = checkQuietTile(m, 0x5000U, 0x10U, f.ciram.data(), 10);
        CHECK(split.tile == 0x77U && split.attribute == 0xaaU);
        CHECK(split.lowChrOffset == 3U * 4096U + (sl ? 0x771U : 0x775U));
        CHECK(split.low == f.chr[split.lowChrOffset] && split.high == f.chr[split.lowChrOffset + 8U]);
    }
}

/** @brief Проверяет default/detach/отказ attach: Name latch сохраняется без доступа к CHR. */
void quietTileInvalidBacking() {
    Mmc5 m;
    const auto empty = checkQuietTile(m, 0x7fffU, 0x10U, nullptr, 33U);
    CHECK(empty.lowChrOffset == Mmc5::InvalidChrOffset && empty.low == 0 && empty.high == 0);
    Fixture f; f.mapper.detach();
    checkQuietTile(f.mapper, 0x7000U, 0, f.ciram.data(), 2);
    CHECK(!f.mapper.attach(f.prg.data(), f.prg.size(), f.chr.data(), 512U, nullptr, 0));
    checkQuietTile(f.mapper, 0x7fffU, 0x10U, nullptr, 31U);
    CHECK(!f.mapper.attach(f.prg.data(), f.prg.size(), nullptr, 1024U, nullptr, 0));
    checkQuietTile(f.mapper, 0, 0, nullptr, 0);
}

/**
 * @brief Проверяет повторное использование одного плана на3/7/32 тайлах без CPU-записей.
 * @details Эталон выполняет четыре прежних typed-read каждого тайла. Скролл
 * задаётся как вход и переходит через coarseX31; это не эмуляция CPU-времени.
 */
void rasterPlanRuns(){
    for(bool sl:{false,true}){
        Fixture f(8192,sl);fillTileData(f);auto& m=f.mapper;
        for(unsigned chrMode=0;chrMode<4;++chrMode){
            m.cpuWrite(0x5101,static_cast<std::uint8_t>(chrMode));m.cpuWrite(0x5130,3);
            for(unsigned bank=0;bank<12;++bank)m.cpuWrite(static_cast<std::uint16_t>(0x5120+bank),static_cast<std::uint8_t>(bank*19U+0x81U));
            for(unsigned mode=0;mode<4;++mode)for(unsigned ntMap:{0U,0x55U,0xaaU,0xffU,0xe4U})
            for(unsigned control:{0U,0x30U})for(unsigned fineY:{0U,7U})for(unsigned firstX:{0U,29U,31U})
            for(unsigned tiles:{3U,7U,32U}){
                m.cpuWrite(0x2000,static_cast<std::uint8_t>(control));m.cpuWrite(0x2001,0);m.cpuWrite(0x2001,0x18);
                m.cpuWrite(0x5104,static_cast<std::uint8_t>(mode&1U));m.cpuWrite(0x5105,static_cast<std::uint8_t>(ntMap));
                m.cpuWrite(0x5106,0xff);m.cpuWrite(0x5107,2);m.cpuWrite(0x5201,239);m.cpuWrite(0x5202,0x7f);
                m.cpuWrite(0x5200,static_cast<std::uint8_t>(mode>=2?((tiles==32?0xc0U:0x80U)|7U):0U));
                f.line();m.clockCpu(1);m.ppuReadStrobe(0);
                auto actual=m,reference=m;Mmc5TestAccess::Plan plan{};
                CHECK(Mmc5TestAccess::prepare(actual,plan,f.ciram.data(),static_cast<std::uint8_t>(control)));
                const auto version=Mmc5TestAccess::version(actual);
                auto scroll=static_cast<std::uint16_t>((fineY<<12U)|((mode&3U)<<10U)|0x3a0U|firstX);
                for(unsigned index=0;index<tiles;++index){
                    const bool needPlanes=index+3U>=tiles;
                    std::size_t highOffset=Mmc5::InvalidChrOffset;
                    const auto expected=Mmc5TestAccess::reference(reference,scroll,static_cast<std::uint8_t>(control),f.ciram.data(),2U+index,highOffset);
                    const auto value=Mmc5TestAccess::planned(plan,scroll,2U+index,needPlanes);
                    CHECK(value.tile==expected.tile&&value.attribute==expected.attribute&&value.lowChrOffset==expected.lowChrOffset);
                    CHECK(value.low==(needPlanes?expected.low:0)&&value.high==(needPlanes?expected.high:0));
                    CHECK(Mmc5TestAccess::latch(actual)==Mmc5TestAccess::latch(reference));
                    CHECK(Mmc5TestAccess::detector(actual)==Mmc5TestAccess::detector(reference));
                    CHECK(Mmc5TestAccess::version(actual)==version);
                    // Публичное следующее чтение использует настоящий detector, а не план.
                    if(index+1U==tiles){
                        const auto address=static_cast<std::uint16_t>((control&0x10U?0x1000U:0U)+unsigned(expected.tile)*16U+fineY);
                        CHECK(actual.ppuRead(address,Mmc5::Fetch::BackgroundPattern,f.ciram.data(),2U+index).value==
                              reference.ppuRead(address,Mmc5::Fetch::BackgroundPattern,f.ciram.data(),2U+index).value);
                        CHECK(Mmc5TestAccess::detector(actual)==Mmc5TestAccess::detector(reference));
                    }
                    if((scroll&31U)==31U)scroll=static_cast<std::uint16_t>((scroll&0x7fe0U)^0x400U);else ++scroll;
                }
                ++rasterRuns;
            }
        }
    }
}

/**
 * @brief Сравнивает новый настоящий partial-read со старым ppuRead и цифровым продолжением.
 * @details Начало frame, IRQ и конец239-й строки задаются перед /RD. Отдельная
 * policy подготавливается заново после каждой влияющей CPU-записи между planes.
 */
void rasterPlanPartial(){
    for(bool sl:{false,true}){
        Fixture f(8192,sl);fillTileData(f);auto& m=f.mapper;
        m.cpuWrite(0x5101,3);m.cpuWrite(0x5120,3);m.cpuWrite(0x5128,11);
        for(unsigned mode=0;mode<4;++mode)for(unsigned seed=0;seed<4;++seed){
            m.cpuWrite(0x2000,0x10);m.cpuWrite(0x2001,0x18);m.cpuWrite(0x5104,static_cast<std::uint8_t>(mode&1U));
            m.cpuWrite(0x5105,0xe4);m.cpuWrite(0x5200,static_cast<std::uint8_t>(mode>=2?0x87:0));
            m.cpuWrite(0x5201,239);m.cpuWrite(0x5202,5);
            for(auto kind:{Mmc5::Fetch::BackgroundName,Mmc5::Fetch::BackgroundAttribute,Mmc5::Fetch::BackgroundPattern})
            for(unsigned address:{0U,0x1ff7U,0x2000U,0x23ffU,0x2fffU,0x3effU,0x3f00U,0x7f10U}){
                auto actual=m,reference=m;Mmc5TestAccess::detectorSeed(actual,seed);Mmc5TestAccess::detectorSeed(reference,seed);
                Mmc5TestAccess::Plan plan{};CHECK(Mmc5TestAccess::prepare(actual,plan,f.ciram.data(),0x10));
                CHECK(Mmc5TestAccess::partial(plan,static_cast<std::uint16_t>(address),kind,6)==
                      reference.ppuRead(static_cast<std::uint16_t>(address),kind,f.ciram.data(),6).value);
                CHECK(Mmc5TestAccess::latch(actual)==Mmc5TestAccess::latch(reference));
                CHECK(Mmc5TestAccess::detector(actual)==Mmc5TestAccess::detector(reference));
                actual.clockCpu(4);reference.clockCpu(4);
                CHECK(Mmc5TestAccess::partial(plan,0x23c0,Mmc5::Fetch::BackgroundAttribute,6)==
                      reference.ppuRead(0x23c0,Mmc5::Fetch::BackgroundAttribute,f.ciram.data(),6).value);
                CHECK(Mmc5TestAccess::detector(actual)==Mmc5TestAccess::detector(reference));++rasterPartialReads;
            }
            constexpr std::array<std::array<std::uint16_t,2>,8> writes={{{0x5120,0x77},{0x5128,0x33},{0x2000,0x30},
                {0x2001,0},{0x5104,2},{0x5200,0},{0x5130,2},{0x5105,0xff}}};
            for(const auto write:writes){
                auto actual=m,reference=m;Mmc5TestAccess::detectorSeed(actual,seed);Mmc5TestAccess::detectorSeed(reference,seed);
                Mmc5TestAccess::Plan plan{};CHECK(Mmc5TestAccess::prepare(actual,plan,f.ciram.data(),0x10));
                const auto tile=reference.ppuRead(0x2005,Mmc5::Fetch::BackgroundName,f.ciram.data(),6).value;
                CHECK(Mmc5TestAccess::partial(plan,0x2005,Mmc5::Fetch::BackgroundName,6)==tile);
                const auto lowAddress=static_cast<std::uint16_t>(0x1000U+unsigned(tile)*16U+5U);
                CHECK(Mmc5TestAccess::partial(plan,lowAddress,Mmc5::Fetch::BackgroundPattern,6)==
                      reference.ppuRead(lowAddress,Mmc5::Fetch::BackgroundPattern,f.ciram.data(),6).value);
                const auto oldVersion=Mmc5TestAccess::version(actual);
                actual.cpuWrite(write[0],static_cast<std::uint8_t>(write[1]));reference.cpuWrite(write[0],static_cast<std::uint8_t>(write[1]));
                CHECK(Mmc5TestAccess::version(actual)!=oldVersion);
                CHECK(Mmc5TestAccess::prepare(actual,plan,f.ciram.data(),write[0]==0x2000?0x30:0x10));
                CHECK(Mmc5TestAccess::partial(plan,static_cast<std::uint16_t>(lowAddress+8U),Mmc5::Fetch::BackgroundPattern,6)==
                      reference.ppuRead(static_cast<std::uint16_t>(lowAddress+8U),Mmc5::Fetch::BackgroundPattern,f.ciram.data(),6).value);
                CHECK(Mmc5TestAccess::latch(actual)==Mmc5TestAccess::latch(reference));
                CHECK(Mmc5TestAccess::detector(actual)==Mmc5TestAccess::detector(reference));++rasterPartialReads;
            }
        }
    }
}

/**
 * @brief Проверяет invalidation policy/lifecycle, live RAM и sticky нулевую версию.
 * @details Старая borrowed-plan не используется после изменения версии. Нулевая
 * версия допускает свежую подготовку, но не reuse между двумя boundaries.
 */
void rasterPlanEpoch(){
    Fixture f;fillTileData(f);auto& m=f.mapper;Mmc5TestAccess::Plan plan{};
    CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));
    const std::array<std::uint16_t,25> invalidators={0x2000,0x2001,0x5101,0x5104,0x5105,0x5106,0x5107,
        0x5120,0x5121,0x5122,0x5123,0x5124,0x5125,0x5126,0x5127,0x5128,0x5129,0x512a,0x512b,
        0x5130,0x5200,0x5201,0x5202,0x2000,0x5105};
    for(auto address:invalidators){const auto before=Mmc5TestAccess::version(m);m.cpuWrite(address,0);
        CHECK(Mmc5TestAccess::version(m)!=before);CHECK(Mmc5TestAccess::version(m)!=0);}
    auto before=Mmc5TestAccess::version(m);m.resetSignal();CHECK(Mmc5TestAccess::version(m)!=before);
    before=Mmc5TestAccess::version(m);m.powerOn();CHECK(Mmc5TestAccess::version(m)!=before);
    std::vector<std::uint8_t> otherChr(f.chr.size(),0x91);before=Mmc5TestAccess::version(m);
    CHECK(m.attach(f.prg.data(),f.prg.size(),otherChr.data(),otherChr.size(),f.ram.data(),f.ram.size()));
    CHECK(Mmc5TestAccess::version(m)!=before);CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));
    CHECK(Mmc5TestAccess::planned(plan,0,2).low==0x91);
    m.cpuWrite(0x5104,0);m.cpuWrite(0x5105,0);CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));
    before=Mmc5TestAccess::version(m);m.ppuWrite(0x2000,0x72,f.ciram.data());CHECK(Mmc5TestAccess::version(m)==before);
    CHECK(Mmc5TestAccess::partial(plan,0x2000,Mmc5::Fetch::BackgroundName,2)==0x72);
    m.cpuWrite(0x5104,1);m.cpuWrite(0x2001,0x18);f.line();
    CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));before=Mmc5TestAccess::version(m);
    m.cpuWrite(0x5c00,0xc1);CHECK(Mmc5TestAccess::version(m)==before);
    static_cast<void>(Mmc5TestAccess::partial(plan,0x2000,Mmc5::Fetch::BackgroundName,2));
    CHECK(Mmc5TestAccess::partial(plan,0x23c0,Mmc5::Fetch::BackgroundAttribute,2)==0xff);
    before=Mmc5TestAccess::version(m);m.clockCpu(4);m.ppuReadStrobe(0);m.cpuRead(0x5204);
    CHECK(Mmc5TestAccess::version(m)==before);
    Mmc5TestAccess::versionSeed(m,std::numeric_limits<std::uint32_t>::max()-1U);
    m.cpuWrite(0x5105,0xe4);CHECK(Mmc5TestAccess::version(m)==std::numeric_limits<std::uint32_t>::max());
    m.resetSignal();CHECK(Mmc5TestAccess::version(m)==0);m.powerOn();CHECK(Mmc5TestAccess::version(m)==0);
    m.cpuWrite(0x2000,0x30);CHECK(Mmc5TestAccess::version(m)==0);CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0x30));
    m.detach();CHECK(Mmc5TestAccess::version(m)==0);
    std::array<unsigned char,sizeof(plan)> unchanged{};std::memcpy(unchanged.data(),&plan,sizeof(plan));
    CHECK(!Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));CHECK(std::memcmp(unchanged.data(),&plan,sizeof(plan))==0);
    CHECK(m.attach(f.prg.data(),f.prg.size(),f.chr.data(),f.chr.size(),f.ram.data(),f.ram.size()));
    CHECK(Mmc5TestAccess::version(m)==0);CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));
}

#if defined(_WIN32) || defined(__unix__)
/**
 * @brief Владеет настоящей недоступной CHR-областью: любое ошибочное byte-read завершит тест.
 * @details Модель использует только адрес/размер для mapping. Область живёт до detach;
 * защита ОС проверяется отдельно от отказа attach/null backing.
 */
struct UnreadableChr {
    static constexpr std::size_t bytes=1024U*1024U;
    std::uint8_t* data=nullptr;
    /** @brief Резервирует целый допустимый CHR-ROM с PROT_NONE/PAGE_NOACCESS без чтения байтов. */
    UnreadableChr(){
#if defined(_WIN32)
        data=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS));
        CHECK(data!=nullptr);
        MEMORY_BASIC_INFORMATION info{};
        CHECK(VirtualQuery(data,&info,sizeof(info))==sizeof(info));
        CHECK((info.Protect&PAGE_NOACCESS)!=0&&info.State==MEM_COMMIT);
#else
        void* memory=mmap(nullptr,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(memory!=MAP_FAILED);data=static_cast<std::uint8_t*>(memory);
#endif
    }
    /** @brief Освобождает только свою область после явного mapper detach в вызывающем тесте. */
    ~UnreadableChr(){
#if defined(_WIN32)
        CHECK(VirtualFree(data,0,MEM_RELEASE)!=0);
#else
        CHECK(munmap(data,bytes)==0);
#endif
    }
};
#endif

/**
 * @brief Проверяет descriptor-only через реальный no-access backing и независимые адресные ожидания.
 * @details Normal A/B, ExAttr и split CL/SL должны сохранить весь Name-latch и digital
 * detector, вернуть нулевые planes и правильный offset. Даже один CHR load аварийно
 * завершит процесс, поэтому fixture доказывает elision, а не только совпадение байтов.
 */
void quietTileNoChrReads(){
#if defined(_WIN32) || defined(__unix__)
    unsigned policies=0;
    for(bool sl:{false,true}){
        UnreadableChr guarded;
        Fixture f(8192,sl);auto& m=f.mapper;
        CHECK(m.attach(f.prg.data(),f.prg.size(),guarded.data,guarded.bytes,f.ram.data(),f.ram.size(),sl));
        f.ciram[0]=0x3f;f.ciram[0x3c0]=0x96;
        f.ex(0,0xc5);f.ex(42,0x77);f.ex(0x3c2,0xaa);
        m.cpuWrite(0x5104,0);m.cpuWrite(0x5101,3);
        m.cpuWrite(0x5120,5);m.cpuWrite(0x5128,9);m.cpuWrite(0x2001,0x18);
        /** @brief Сверяет raw metadata с независимой константой, не выполняя reference CHR-read. */
        const auto metadata=[&](std::uint16_t v,unsigned x,unsigned tile,unsigned attribute,
                                 std::size_t offset,const std::array<std::uint32_t,6>& latch){
            const auto before=Mmc5TestAccess::detector(m);
            const auto value=Mmc5TestAccess::tile(m,v,0,f.ciram.data(),x,false);
            CHECK(value.tile==tile&&value.attribute==attribute);
            CHECK(value.lowChrOffset==offset&&value.low==0&&value.high==0);
            CHECK(Mmc5TestAccess::latch(m)==latch);CHECK(Mmc5TestAccess::detector(m)==before);
            Mmc5TestAccess::Plan plan{};
            CHECK(Mmc5TestAccess::prepare(m,plan,f.ciram.data(),0));
            const auto planned=Mmc5TestAccess::planned(plan,v,x,false);
            CHECK(planned.tile==tile&&planned.attribute==attribute);
            CHECK(planned.lowChrOffset==offset&&planned.low==0&&planned.high==0);
            CHECK(Mmc5TestAccess::latch(m)==latch);CHECK(Mmc5TestAccess::detector(m)==before);
            ++policies;
        };
        m.cpuWrite(0x2000,0);
        metadata(0x7000,4,0x3f,0x96,5U*1024U+0x3f7U,{0xc5,0,0,0,0,0});
        m.cpuWrite(0x2000,0x20);
        metadata(0x7000,4,0x3f,0x96,9U*1024U+0x3f7U,{0xc5,0,0,0,0,0});
        m.cpuWrite(0x5104,1);m.cpuWrite(0x5130,2);
        metadata(0x7000,4,0x3f,0xff,133U*4096U+0x3f7U,{0xc5,0,0,0,1,0});
        // Настоящее выключение PPUMASK превосходит предыдущий ExAttr latch.
        m.cpuWrite(0x2001,0);
        metadata(0x7000,4,0x3f,0x96,5U*1024U+0x3f7U,{0xc5,0,0,0,0,0});
        m.cpuWrite(0x2001,0x18);m.cpuWrite(0x2000,0);
        m.cpuWrite(0x5104,0);m.cpuWrite(0x5201,9);m.cpuWrite(0x5202,3);m.cpuWrite(0x5200,0x8c);
        metadata(0x5000,10,0x77,0xaa,3U*4096U+(sl?0x771U:0x775U),{0xc5,0xaa,9,42,0,1});
        // Регистровое выключение split не оставляет прежнюю CHR подстановку.
        m.cpuWrite(0x5200,0);
        metadata(0x7000,10,0x3f,0x96,5U*1024U+0x3f7U,{0xc5,0xaa,9,42,0,0});
        m.detach();
    }
    std::printf("MMC5_NO_CHR_READ_PASS policies=%u protection=no_access\n",policies);
#else
    std::puts("MMC5_NO_CHR_READ_SKIP unsupported_host=1");
#endif
}
}

/** @brief Запускает только переносимые unit-проверки; это не FPS/полная MMC5-приёмка. */
int main() {
    prgModes(); prgRam(); chrModes(); nametables(); irq(); multiplier(); resetDetection(); split();
    quietTileNormal(); quietTilePolicies(); quietTileInvalidBacking();
    rasterPlanRuns();rasterPlanPartial();rasterPlanEpoch();
    quietTileNoChrReads();
    CHECK(Mmc5TestAccess::preparedPlans>0&&rasterRuns>0&&rasterPartialReads>0);
    std::printf("MMC5_RASTER_PLAN_DIRECT_PASS prepared=%u runs=%u partial=%u plan_bytes=%zu\n",
        Mmc5TestAccess::preparedPlans,rasterRuns,rasterPartialReads,sizeof(Mmc5TestAccess::Plan));
    std::printf("MMC5_DIGITAL_PASS checks=%u mapper_bytes=%zu\n", checks, sizeof(Mmc5));
}
