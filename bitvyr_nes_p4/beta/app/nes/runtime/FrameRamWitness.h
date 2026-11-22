/**
 * @file FrameRamWitness.h
 * @brief Точный ключ и ограниченная политика диагностической RAM публикации.
 * @details Не включает SDK, не читает Machine/ROM и не передаёт владение.
 * Worker формирует запись до release слота; Main проверяет её после acquire.
 */
#pragma once
#include "OutputFrame.h"
#include <cstdio>

namespace nes::app {
/** @brief Full сохраняет эталон; Sparse120 запрашивает каждый120-й входной кадр. */
enum class FrameRamPolicy:std::uint8_t {Full,Sparse120};

/**
 * @brief RAM availability относится только к этому полному ticket/cut/App ключу.
 * @details valid=false не разрешает читать оставшиеся байты переиспользованного
 * RAM-буфера. Нулевой appGeneration обозначает отсутствие принятого RAM ключа.
 */
struct FrameRamWitness final {
    core::OutputFrameTicket ticket{};
    std::uint64_t appGeneration=0,endCycle=0,ppuFrame=0;
    FrameRamPolicy policy=FrameRamPolicy::Full;
    bool requested=false,valid=false;
};

/** @brief Проверяет enum до его сохранения на единственной joined границе настройки. */
inline bool validFrameRamPolicy(FrameRamPolicy policy)noexcept {
    return policy==FrameRamPolicy::Full||policy==FrameRamPolicy::Sparse120;
}
/** @brief Детерминированный request; без сложения sequence+1 и риска переполнения. */
inline bool frameRamRequested(FrameRamPolicy policy,std::uint64_t sequence)noexcept {
    return policy==FrameRamPolicy::Full||(policy==FrameRamPolicy::Sparse120&&sequence%120U==119U);
}
/**
 * @brief Worker фиксирует ключ до full-copy/metadata ack, пока valid ещё false.
 * @param out Только собственный незапубликованный SlotN; не shared acceptedN.
 */
inline void prepareFrameRamWitness(FrameRamWitness& out,const core::OutputFrameTicket& ticket,
    const core::OutputFrameCut& cut,std::uint64_t appGeneration,FrameRamPolicy policy)noexcept {
    out.ticket=ticket;out.appGeneration=appGeneration;out.endCycle=cut.endCycle;
    out.ppuFrame=cut.ppuFrame;out.policy=policy;out.requested=frameRamRequested(policy,ticket.sequence);out.valid=false;
}
/**
 * @brief Main проверяет все ключи и наличие именно запрошенных байтов, без чтения RAM.
 * @details Отсутствие request законно; запрос с failed/missing full-copy не законен.
 */
inline bool matchesFrameRamWitness(const FrameRamWitness& value,const core::OutputFrameTicket& ticket,
    const core::OutputFrameCut& cut,std::uint64_t appGeneration,FrameRamPolicy policy)noexcept {
    const auto&a=value.ticket;const auto&l=a.lease;const auto&r=ticket.lease;
    return validFrameRamPolicy(policy)&&value.policy==policy&&value.appGeneration==appGeneration&&appGeneration&&
        value.endCycle==cut.endCycle&&value.ppuFrame==cut.ppuFrame&&
        a.sequence==ticket.sequence&&a.inputFrame==ticket.inputFrame&&a.baseCycle==ticket.baseCycle&&a.slot==ticket.slot&&
        l.producer==r.producer&&l.rom==r.rom&&l.romBytes==r.romBytes&&l.romEpoch==r.romEpoch&&
        l.generation==r.generation&&l.transportEpoch==r.transportEpoch&&l.region==r.region&&
        value.requested==frameRamRequested(policy,ticket.sequence)&&value.valid==value.requested;
}
/**
 * @brief Готовит schema3 RAM JSON fragment без heap; unknown RAM остаётся null.
 * @param bytes Размер заимствованного выходного буфера; при усечении возвращает false.
 * @details ramHash используется только при valid; старый числовой0 не кодирует unavailable.
 */
inline bool formatFrameRamWitness(char* destination,std::size_t bytes,const FrameRamWitness& value,std::uint64_t ramHash,bool hashReady)noexcept {
    if(!destination||!bytes||value.policy!=FrameRamPolicy::Sparse120)return false;
    char hash[19]="null";
    if(value.valid&&hashReady)std::snprintf(hash,sizeof(hash),"\"%016llx\"",static_cast<unsigned long long>(ramHash));
    if(!value.appGeneration){
        const auto count=std::snprintf(destination,bytes,"\"ram_policy\":\"sparse120\",\"ram_requested\":false,\"ram_valid\":false,\"ram_status\":\"unavailable\",\"ram_hash\":null,\"ram_key\":null");
        return count>=0&&static_cast<std::size_t>(count)<bytes;
    }
    const auto&t=value.ticket;const auto&l=t.lease;
    const auto count=std::snprintf(destination,bytes,
        "\"ram_policy\":\"sparse120\",\"ram_requested\":%s,\"ram_valid\":%s,\"ram_status\":\"%s\",\"ram_hash\":%s,\"ram_key\":{\"producer\":%llu,\"rom\":%llu,\"rom_bytes\":%llu,\"rom_epoch\":%llu,\"lease_generation\":%llu,\"transport_epoch\":%llu,\"region\":\"%s\",\"sequence\":%llu,\"input_frame\":%llu,\"slot\":%u,\"app_generation\":%llu,\"base_cycle\":%llu,\"end_cycle\":%llu,\"ppu_frame\":%llu}",
        value.requested?"true":"false",value.valid?"true":"false",value.valid?(hashReady?"available":"unobserved"):"not_requested",hash,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(l.producer)),static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(l.rom)),
        static_cast<unsigned long long>(l.romBytes),static_cast<unsigned long long>(l.romEpoch),static_cast<unsigned long long>(l.generation),
        static_cast<unsigned long long>(l.transportEpoch),l.region==core::Region::Pal?"PAL":"NTSC",static_cast<unsigned long long>(t.sequence),
        static_cast<unsigned long long>(t.inputFrame),t.slot,static_cast<unsigned long long>(value.appGeneration),static_cast<unsigned long long>(t.baseCycle),
        static_cast<unsigned long long>(value.endCycle),static_cast<unsigned long long>(value.ppuFrame));
    return count>=0&&static_cast<std::size_t>(count)<bytes;
}
} // namespace nes::app
