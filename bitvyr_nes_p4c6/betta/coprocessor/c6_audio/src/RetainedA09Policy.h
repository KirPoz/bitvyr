#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::c6rollback {
constexpr std::size_t requestBytes=36,replyBytes=40,metadataBytes=328;
/** @brief Raw status коды selector; любой отказ не означает, что caller может автоматически повторить setboot. */
enum class Code:std::uint32_t {Ok=0,BadRequest=50,Metadata=51,Guard=52,SetBoot=53,PostVerify=54,OwnerBusy=55,Security=56};
/** @brief Читает fixed BootMeta328; callback не меняет request/output/owner и не допускает reentry. */
using ReadMetadata=bool(*)(void*,std::uint8_t*,std::size_t) noexcept;
/** @brief Единственная persistent операция: SDK setboot только fixed OTA0, без reset/eraseAPP/NVS/eFuse. */
using SelectZero=bool(*)(void*) noexcept;
/** @brief Заимствованные SDK-free seams, storage/context остаются у единственного Worker. */
struct Hooks final {void*context{};ReadMetadata read{};SelectZero select{};};
/** @brief Проверяет version/reserved/nonzero freshSHA36 до SDK read; данные не модифицируются. */
bool validRequest(const std::uint8_t*,std::size_t) noexcept;
/**
 * @brief Выполняет fixed retained-a09 policy, повторно проверяет метаданные после setboot.
 * @param request LEu16version1/reserved0 +freshOTADATA_SHA32, ровно36B.
 * @param output Caller-owned40B reply {version32,selectedOffset32,postOTADATA_SHA32}.
 * @param written Меняется только на success; alias с request/output отвергается до callback.
 * @param hooks Стабильные trusted callbacks; false select может означать неизвестную persistent запись.
 * @return Code::Ok — только выбор следующей загрузки. Это НЕ доказательство reboot/actual a09 boot.
 * @details Caller ДО вызова доказывает unbound/noaudio/noRawborrow. После неизвестного отказа
 * запрещены autoretry/reset/free по одному status; Root читает freshBOOTMETA и решает отдельно.
 */
Code select(const std::uint8_t*request,std::size_t requestSize,std::uint8_t*output,
    std::size_t capacity,std::size_t&written,Hooks hooks) noexcept;
}
