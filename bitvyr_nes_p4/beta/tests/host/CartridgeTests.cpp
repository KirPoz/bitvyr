#include "cartridge/Cartridge.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using nes::core::Cartridge;
using nes::core::LoadResult;
using nes::core::Mirroring;

namespace {
/**
 * @brief Печатает нарушенное CHECK и его строку, затем завершает тест кодом1.
 * @param text Текст условия из CHECK; заимствован на время вывода.
 * @param line Строка проверяемого утверждения, не гостевое время.
 */
[[noreturn]] void fail(const char* text,unsigned line) {
    std::fprintf(stderr,"Cartridge test failed line %u: %s\n",line,text);
    std::exit(1);
}
#define CHECK(condition) do { if (!(condition)) fail(#condition,__LINE__); } while (false)

// Каждое окно PRG8KiB/CHR1KiB имеет собственную метку байтов. Ожидания
// ниже описывают проводку картриджа независимо от реализации mapper.
/**
 * @brief Создаёт собственный iNES с независимыми метками каждого PRG8KiB/CHR1KiB окна.
 * @param mapper Номер mapper в заголовке; в negative cases может быть неподдерживаемым.
 * @param prg16 Число16KiB PRG-банков.
 * @param chr8 Число8KiB CHR-банков.
 * @param flags Младшие flags заголовка, включая наличие512-байтового trainer.
 * @return Собственный vector образа; fixture удерживает его до окончания всех обращений Cartridge.
 */
std::vector<std::uint8_t> image(unsigned mapper,unsigned prg16,unsigned chr8,std::uint8_t flags=0) {
    const unsigned trainer=(flags&4) ? 512 : 0;
    std::vector<std::uint8_t> bytes(16+trainer+prg16*16384+chr8*8192,0);
    std::memcpy(bytes.data(),"NES\x1A",4);
    bytes[4]=static_cast<std::uint8_t>(prg16); bytes[5]=static_cast<std::uint8_t>(chr8);
    bytes[6]=static_cast<std::uint8_t>((mapper<<4)|flags); bytes[7]=static_cast<std::uint8_t>(mapper&0xF0);
    for(unsigned offset=0;offset<prg16*16384;++offset) bytes[16+trainer+offset]=static_cast<std::uint8_t>(offset/8192);
    for(unsigned offset=0;offset<chr8*8192;++offset) bytes[16+trainer+prg16*16384+offset]=static_cast<std::uint8_t>(offset/1024);
    return bytes;
}

/** @brief Проверяет iNES-заголовки, размеры backing и чистые отказы загрузки. */
void headers() {
    Cartridge cart;
    CHECK(cart.load(nullptr,0)==LoadResult::InvalidHeader);
    std::array<std::uint8_t,16> shortImage{};
    CHECK(cart.load(shortImage.data(),shortImage.size())==LoadResult::InvalidHeader);
    auto valid=image(0,1,1);
    CHECK(cart.load(valid.data(),15)==LoadResult::InvalidHeader);
    CHECK(cart.load(valid.data(),valid.size()-1)==LoadResult::Truncated);
    CHECK(!cart.loaded());
    CHECK(cart.cpuRead(0x8000,0xA5)==0xA5);
    auto large=image(0,4,1); CHECK(cart.load(large.data(),large.size())==LoadResult::UnsupportedFormat);
    auto unsupported=image(31,1,1); CHECK(cart.load(unsupported.data(),unsupported.size())==LoadResult::UnsupportedMapper);
    auto arcade=valid; arcade[7]|=1; CHECK(cart.load(arcade.data(),arcade.size())==LoadResult::UnsupportedFormat);
    auto nes2=valid; nes2[7]=8;
    CHECK(cart.load(nes2.data(),nes2.size())==LoadResult::Ok);
    nes2[8]=0x10; CHECK(cart.load(nes2.data(),nes2.size())==LoadResult::UnsupportedFormat);
    nes2[8]=0; nes2[9]=0x0F; CHECK(cart.load(nes2.data(),nes2.size())==LoadResult::UnsupportedFormat);
    auto legacy=image(4,8,16); legacy[13]='M'; legacy[14]='J'; legacy[15]='R';
    CHECK(cart.load(legacy.data(),legacy.size())==LoadResult::Ok);
    CHECK(cart.mapper()==4 && cart.legacyHeader());
    legacy[7]=0x10; CHECK(cart.load(legacy.data(),legacy.size())==LoadResult::UnsupportedFormat);
    auto trainer=image(0,1,1,4);
    for(unsigned i=0;i<512;++i) trainer[16+i]=static_cast<std::uint8_t>(i^0xA5);
    CHECK(cart.load(trainer.data(),trainer.size())==LoadResult::Ok);
    CHECK(cart.cpuRead(0x7000)==0xA5 && cart.cpuRead(0x71FF)==0x5A);
    CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xA000)==1);
    valid[9]=1; CHECK(cart.load(valid.data(),valid.size())==LoadResult::Ok); CHECK(cart.palHint());
    for(auto result:{LoadResult::Ok,LoadResult::InvalidHeader,LoadResult::Truncated,LoadResult::UnsupportedMapper,LoadResult::UnsupportedFormat}) CHECK(std::strlen(cart.errorText(result))>0);
}

/** @brief Проверяет банки и зеркальность простых mapper по независимым байтовым меткам. */
void simpleMappers() {
    Cartridge cart;
    auto nrom=image(0,1,1,1);
    CHECK(cart.load(nrom.data(),nrom.size())==LoadResult::Ok);
    CHECK(cart.mirroring()==Mirroring::Vertical);
    CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xA000)==1);
    CHECK(cart.cpuRead(0xC000)==0 && cart.cpuRead(0xE000)==1);
    cart.cpuWrite(0x8000,0xFF,1); CHECK(cart.cpuRead(0x8000)==0);
    cart.chrWrite(0,0xFF); CHECK(cart.chrRead(0)==0 && cart.chrRead(0x1FFF)==7);
    auto chrRam=image(0,2,0);
    CHECK(cart.load(chrRam.data(),chrRam.size())==LoadResult::Ok);
    cart.chrWrite(0x1234,0x9A); CHECK(cart.chrRead(0x1234)==0x9A);
    cart.cpuWrite(0x6001,0x3C,2); CHECK(cart.cpuRead(0x6001)==0x3C);
    CHECK(cart.cpuRead(0x5000,0xC3)==0xC3);

    auto uxrom=image(2,16,0);
    CHECK(cart.load(uxrom.data(),uxrom.size())==LoadResult::Ok);
    CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xC000)==30);
    cart.cpuWrite(0xFFFF,2,3); CHECK(cart.cpuRead(0x8000)==4 && cart.cpuRead(0xA000)==5 && cart.cpuRead(0xC000)==30);
    cart.cpuWrite(0x8000,0xFF,4); CHECK(cart.cpuRead(0x8000)==30);
    cart.chrWrite(0x1FFF,0x45); CHECK(cart.chrRead(0x1FFF)==0x45);

    auto axrom=image(7,16,0);
    CHECK(cart.load(axrom.data(),axrom.size())==LoadResult::Ok);
    CHECK(cart.mirroring()==Mirroring::Single0);
    cart.cpuWrite(0x9FFF,0x13,5);
    CHECK(cart.cpuRead(0x8000)==12 && cart.cpuRead(0xE000)==15 && cart.mirroring()==Mirroring::Single1);
    cart.cpuWrite(0x8000,0x03,6); CHECK(cart.mirroring()==Mirroring::Single0);
    auto cnrom=image(3,2,4);
    CHECK(cart.load(cnrom.data(),cnrom.size())==LoadResult::Ok);
    cart.cpuWrite(0x8000,2,7); CHECK(cart.chrRead(0)==16 && cart.chrRead(0x1FFF)==23);
}

/** @brief Передаёт пять последовательных битов MMC1 с заданными адресами и CPU-тактами. */
void serial(Cartridge& cart,std::uint16_t address,unsigned value,std::uint64_t& cycle) {
    for(unsigned bit=0;bit<5;++bit) { cart.cpuWrite(address,static_cast<std::uint8_t>((value>>bit)&1),cycle); cycle+=2; }
}

/** @brief Проверяет serial/RMW/reset и выбор PRG/CHR MMC1. */
void mmc1() {
    Cartridge cart;
    auto rom=image(1,8,8);
    CHECK(cart.load(rom.data(),rom.size())==LoadResult::Ok);
    std::uint64_t cycle=10;
    CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xC000)==14);
    serial(cart,0xE000,3,cycle); CHECK(cart.cpuRead(0x8000)==6 && cart.cpuRead(0xC000)==14);
    serial(cart,0x8000,0x0A,cycle); CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xC000)==6); CHECK(cart.mirroring()==Mirroring::Vertical);
    serial(cart,0x8000,0x03,cycle); CHECK(cart.cpuRead(0x8000)==4 && cart.cpuRead(0xC000)==6); CHECK(cart.mirroring()==Mirroring::Horizontal);
    serial(cart,0xA000,3,cycle); CHECK(cart.chrRead(0)==8 && cart.chrRead(0x1000)==12); // В режиме8KiB младший бит банка не влияет на выбор.
    serial(cart,0x8000,0x1C,cycle); serial(cart,0xA000,3,cycle); serial(cart,0xC000,7,cycle);
    CHECK(cart.chrRead(0)==12 && cart.chrRead(0x1000)==28);
    CHECK(cart.mirroring()==Mirroring::Single0);
    serial(cart,0x8000,0x1D,cycle); CHECK(cart.mirroring()==Mirroring::Single1);
    cart.cpuWrite(0x6000,0x61,cycle); cycle+=2;
    serial(cart,0xE000,0x13,cycle); CHECK(cart.cpuRead(0x6000,0xA5)==0xA5);
    cart.cpuWrite(0x6000,0x99,cycle); cycle+=2;
    serial(cart,0xE000,3,cycle); CHECK(cart.cpuRead(0x6000)==0x61);

    cart.reset();
    // В серии соседних CPU-тактов учитывается только первая запись.
    for(unsigned i=0;i<5;++i) cart.cpuWrite(0xE000,1,100+i);
    CHECK(cart.cpuRead(0x8000)==0);
    for(unsigned i=0;i<4;++i) cart.cpuWrite(0xE000,0,110+i*2);
    CHECK(cart.cpuRead(0x8000)==2);
    cart.reset();
    // Вторую запись RMW игнорируют для каждого бита пятибитной передачи.
    for(unsigned bit=0;bit<5;++bit) {
        const auto value=static_cast<std::uint8_t>((2U>>bit)&1U);
        cart.cpuWrite(0xE000,value,200+bit*8); cart.cpuWrite(0xE000,value^1U,201+bit*8);
    }
    CHECK(cart.cpuRead(0x8000)==4);

    cycle=1000;
    serial(cart,0x8000,0x12,cycle); // Выбраны PRG32KiB, CHR4KiB и вертикальная зеркальность.
    serial(cart,0xA000,3,cycle); serial(cart,0xC000,7,cycle);
    cart.cpuWrite(0xA000,1,2000); cart.cpuWrite(0xA000,0x80,2001); // Бит сброса учитывается даже после записи на соседнем такте.
    cycle=2100; serial(cart,0xE000,2,cycle);
    CHECK(cart.cpuRead(0x8000)==4 && cart.cpuRead(0xC000)==14);
    CHECK(cart.mirroring()==Mirroring::Vertical);
    CHECK(cart.chrRead(0)==12 && cart.chrRead(0x1000)==28); // Сброс сохраняет выбранные CHR-банк и режим.

    cart.reset();
    cycle=3000;
    for(unsigned bit=0;bit<4;++bit) { cart.cpuWrite(0x8000,static_cast<std::uint8_t>((5U>>bit)&1U),cycle); cycle+=2; }
    cart.cpuWrite(0xE000,0,cycle); // Адрес назначения выбирает только пятая запись.
    CHECK(cart.cpuRead(0x8000)==10 && cart.mirroring()==Mirroring::Single0);
}

/** @brief Записывает выбранный регистр MMC3 через настоящую пару адресов. */
void setMmc3(Cartridge& cart,unsigned index,unsigned value,std::uint8_t mode=0) {
    cart.cpuWrite(0x8000,static_cast<std::uint8_t>(index|mode),0);
    cart.cpuWrite(0x8001,static_cast<std::uint8_t>(value),0);
}

/** @brief Формирует квалифицированный фронт A12 после заданной низкой длительности. */
void qualifiedEdge(Cartridge& cart,unsigned lowDots=8) {
    cart.ppuAddress(0);
    for(unsigned i=0;i<lowDots;++i) cart.ppuCycle();
    cart.ppuAddress(0x1000);
}

/** @brief Проверяет банки, A12-фильтр, reload/pending/enable IRQ MMC3. */
void mmc3() {
    Cartridge cart;
    auto rom=image(4,4,8);
    CHECK(cart.load(rom.data(),rom.size())==LoadResult::Ok);
    CHECK(cart.cpuRead(0x8000)==0 && cart.cpuRead(0xA000)==1 && cart.cpuRead(0xC000)==6 && cart.cpuRead(0xE000)==7);
    setMmc3(cart,6,4); CHECK(cart.cpuRead(0x8000)==4 && cart.cpuRead(0xC000)==6);
    cart.cpuWrite(0x8000,0x46,0); CHECK(cart.cpuRead(0x8000)==6 && cart.cpuRead(0xC000)==4);
    setMmc3(cart,7,3,0x40); CHECK(cart.cpuRead(0xA000)==3);
    setMmc3(cart,0,5); setMmc3(cart,1,8); setMmc3(cart,2,11);
    CHECK(cart.chrRead(0)==4 && cart.chrRead(0x400)==5 && cart.chrRead(0x800)==8 && cart.chrRead(0x1000)==11);
    cart.cpuWrite(0x8000,0x80,0); CHECK(cart.chrRead(0)==11 && cart.chrRead(0x1000)==4 && cart.chrRead(0x1400)==5);
    cart.cpuWrite(0xA000,0,0); CHECK(cart.mirroring()==Mirroring::Vertical);
    cart.cpuWrite(0xA000,1,0); CHECK(cart.mirroring()==Mirroring::Horizontal);
    cart.cpuWrite(0x6000,0x5A,0); CHECK(cart.cpuRead(0x6000)==0x5A);
    cart.cpuWrite(0xA001,0xC0,0); cart.cpuWrite(0x6000,0xAA,0); CHECK(cart.cpuRead(0x6000)==0x5A);
    cart.cpuWrite(0xA001,0,0); CHECK(cart.cpuRead(0x6000,0xA5)==0xA5);
    cart.cpuWrite(0x6000,0xFF,0); cart.cpuWrite(0xA001,0x80,0); CHECK(cart.cpuRead(0x6000)==0x5A);

    cart.reset(); cart.ppuAddress(0x1000); // Сначала устанавливается высокий адрес, затем проверяется длительность низкого A12.
    cart.cpuWrite(0xC000,1,0); cart.cpuWrite(0xC001,0,0); cart.cpuWrite(0xE001,0,0);
    qualifiedEdge(cart,7); CHECK(!cart.irqLine());
    qualifiedEdge(cart,8); CHECK(!cart.irqLine()); // Перезагрузка счётчика значением1.
    qualifiedEdge(cart,7); CHECK(!cart.irqLine()); // Короткий низкий A12 не должен уменьшать счётчик.
    qualifiedEdge(cart,8); CHECK(cart.irqLine());
    cart.cpuWrite(0xE000,0,0); CHECK(!cart.irqLine());
    cart.cpuWrite(0xC000,0,0); cart.cpuWrite(0xC001,0,0); cart.cpuWrite(0xE001,0,0);
    qualifiedEdge(cart); CHECK(cart.irqLine()); // Проверяется повторная нулевая перезагрузка mapper4/submapper0.
    cart.cpuWrite(0xE000,0,0); cart.cpuWrite(0xE001,0,0); qualifiedEdge(cart); CHECK(cart.irqLine());
    cart.cpuWrite(0xE000,0,0); cart.cpuWrite(0xC000,3,0); cart.cpuWrite(0xC001,0,0);
    for(unsigned i=0;i<4;++i) qualifiedEdge(cart);
    CHECK(!cart.irqLine()); cart.cpuWrite(0xE001,0,0);
    qualifiedEdge(cart); CHECK(!cart.irqLine()); qualifiedEdge(cart); CHECK(!cart.irqLine()); qualifiedEdge(cart); CHECK(!cart.irqLine());
    qualifiedEdge(cart); CHECK(cart.irqLine()); // Запрет IRQ блокирует выход, но не работающий счётчик.

    auto four=image(4,4,8,8);
    CHECK(cart.load(four.data(),four.size())==LoadResult::Ok);
    cart.cpuWrite(0xA000,0,0); CHECK(cart.mirroring()==Mirroring::FourScreen);
    cart.cpuWrite(0xA000,1,0); CHECK(cart.mirroring()==Mirroring::FourScreen);
}

/** @brief Проверяет отказ Legacy160 вне разрешённых backing и сохранность RAM. */
void legacyGuard(const char* path) {
    Cartridge cart;
    auto fake=image(160,16,32);
    CHECK(cart.load(fake.data(),fake.size())==LoadResult::UnsupportedMapper);
    CHECK(!cart.loaded());
    if(!path) { std::puts("Legacy160 exact-image positive test skipped: pass local Aladdin .nes path to enable"); return; }
    std::FILE* input=std::fopen(path,"rb"); CHECK(input!=nullptr);
    std::vector<std::uint8_t> bytes(524304);
    CHECK(std::fread(bytes.data(),1,bytes.size(),input)==bytes.size()); CHECK(std::fgetc(input)==EOF); std::fclose(input);
    CHECK(cart.load(bytes.data(),bytes.size())==LoadResult::Ok && cart.mapper()==160);
    CHECK(cart.cpuRead(0xFFFC)==0xEF && cart.cpuRead(0xFFFD)==0xE0);
    cart.cpuWrite(0x6000,0xA5,0); CHECK(cart.cpuRead(0x6000)==0xA5);
    cart.cpuWrite(0xD000,0x80,0); cart.cpuWrite(0x6000,0x5A,0);
    cart.cpuWrite(0xD000,0,0); CHECK(cart.cpuRead(0x6000)==0xA5); // Переключение ROM не должно портить скрытую RAM.
    cart.cpuWrite(0xD000,0x1E,0); cart.cpuWrite(0x8002,0x1E,0); cart.cpuWrite(0x8003,0x1F,0);
    CHECK(cart.cpuRead(0xFFFC)==0xEF && cart.cpuRead(0xFFFD)==0xE0);
    cart.cpuWrite(0x5800,0x68,0); cart.cpuWrite(0x5801,0x37,0);
    for(unsigned i=0;i<8;++i) cart.cpuCycle();
    CHECK(cart.cpuRead(0x5800)==0x58 && cart.cpuRead(0x5801)==0x16);
    bytes[100]^=1;
    CHECK(cart.load(bytes.data(),bytes.size())==LoadResult::UnsupportedMapper && !cart.loaded());
    bytes[100]^=1; bytes.push_back(0);
    CHECK(cart.load(bytes.data(),bytes.size())==LoadResult::UnsupportedMapper);
}
}

/**
 * @brief Проверяет iNES-отказы, простые mapper, MMC1 serial/RMW/reset и MMC3 A12/IRQ.
 * @details Байтовые маркеры и ожидаемые masks заданы независимо от проверяемой проводки; CHECK завершает процесс кодом1.
 */
int main(int argc,char** argv) {
    headers(); simpleMappers(); mmc1(); mmc3(); legacyGuard(argc>1 ? argv[1] : nullptr);
    std::puts("Cartridge tests passed: headers/trainer, NROM/UxROM/CNROM/AxROM/MMC1/MMC3, IRQ/RAM, exactLegacy160guard");
}
