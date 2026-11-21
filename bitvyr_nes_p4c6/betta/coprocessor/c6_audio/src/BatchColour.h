#pragma once
#include "RawWire.h"
#include "IndexColourRun.h"
#include "ppu/c6/ResolvedColourCodec.h"
#include <array>

namespace nes::c6raw::batch {
namespace colour=::nes::c6_colour;
constexpr std::size_t requestHeaderBytes=8,replyHeaderBytes=36,rowBytes=294,maxRows=4,maxRecords=32;
constexpr std::uint8_t beginFrame=1,endFrame=2;
enum class Command : std::uint8_t { Seed=1,Run=2,End=3 };
enum class Status : std::uint32_t { Ok=0,Malformed=20,Identity=21,Sequence=22,Grammar=23,Faulted=24,Kernel=25,Capacity=26 };
using Clock=std::uint32_t(*)();
/** @brief Подсчёты wall cycles C6 по этапам; CRC/transport вне process измеряются адаптером отдельно. */
struct Metrics {std::uint32_t validate{},prepare{},kernel{},pack{},pixels{};};
/** @brief Ровно один C6 worker владеет frame/row/cache; буферы replies принадлежат внешним двум transport slots. */
class Stream final {
public:
    /** @brief Обнуляет только собственную логическую сессию; caller обязан завершить/join старые transport owners. */
    void reset() noexcept;
    /** @brief Проверяет весь TLV/ordinal до mutation; при ошибке fault sticky до reset, partial results не публикуются. */
    Status process(const Key&,const std::uint8_t*,std::size_t,std::uint8_t*,std::size_t,
                   std::size_t& written,Metrics&,Clock=nullptr) noexcept;
    /** @brief Показывает незавершённую строку; BOOTMETA допускается только после quiesce. */
    bool rowOpen() const noexcept {return rowOpen_;}
    /** @brief Отдельный quiesce witness: frame не завершён даже если между пакетами нет открытой row. */
    bool frameOpen() const noexcept {return frameOpen_;}
    /** @brief Ошибка не выдаётся за завершение pending frame/transport. */
    bool faulted() const noexcept {return fault_;}
private:
    colour::index_only::Context context_{};
    colour::index_only::Result result_{};
    colour::RowSeed seed_{};
    Key owner_{};
    std::array<std::uint8_t,32> dirty_{},completed_{};
    std::uint32_t nextOrdinal_{};
    std::uint16_t y_{},prefix_{};
    bool rowOpen_{},frameOpen_{},fault_{},haveFrame_{};
};
/** @brief Заимствованная проверенная строка ответа; indices доступны только до возврата transport slot. */
struct RowView {std::uint16_t y{},prefix{};std::uint8_t emphasis{};bool pal{};const std::uint8_t *dirty{},*indices{};};
/** @brief Проверяет весь reply до любых pixel stores: размер/ordinal/region/dirty bounds/6bit indices. */
bool decodeReply(const std::uint8_t*,std::size_t,std::uint32_t ordinal,bool pal,
                 std::array<RowView,maxRows>&,unsigned& count) noexcept;
/** @brief Пишет маленькое LE поле без невыровненных loads. */
void put(std::uint8_t*,std::uint32_t,unsigned) noexcept;
/** @brief Читает маленькое LE поле после проверки его границ. */
std::uint32_t get(const std::uint8_t*,unsigned) noexcept;
}
