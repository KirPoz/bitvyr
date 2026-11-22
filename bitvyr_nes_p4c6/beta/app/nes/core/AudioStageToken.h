/** @file AudioStageToken.h @brief SDK-free once-token копии аудио до Thin commit ACK; lifetime обеспечивает App. */
#pragma once
#include "apu/AudioPacket.h"
#include <cstdint>

namespace nes::core {
class Machine;
class ThinPpuChannel;
/** @brief Общий источник состояний caller-token и владельца в Thin channel; PRIVATE layout guards отсутствуют. */
enum class AudioStagePhase : std::uint8_t { Empty, Staged, Completed, Abandoned };

/**
 * @brief Caller-owned metadata одного pre-ACK аудиопакета без собственного хранилища PCM/событий.
 * @details Worker единолично stage/render/finalize. Main читает/reset только после actual Worker join.
 * Token и source/destination/Thin channel живут до finalize либо явного joined abandon.
 * Сам объект никогда не освобождает пакеты и не признаёт timeout завершением владельца.
 */
class AudioStageToken final {
public:
    /** @brief Создаёт пустую квитанцию без выделения памяти и без заимствования источника. */
    AudioStageToken() noexcept = default;
    /** @brief Не освобождает заимствованные объекты; caller сначала доказывает actual join. */
    ~AudioStageToken() noexcept = default;
    /** @brief Запрещает вторую квитанцию с тем же правом finalize. */
    AudioStageToken(const AudioStageToken&) = delete;
    /** @brief Запрещает заменить live once-owner чужой квитанцией. */
    AudioStageToken& operator=(const AudioStageToken&) = delete;
    /** @brief Стабильный адрес token является частью once-identity, поэтому перемещение запрещено. */
    AudioStageToken(AudioStageToken&&) = delete;
    /** @brief Не переносит адрес self-binding активной либо завершённой квитанции. */
    AudioStageToken& operator=(AudioStageToken&&) = delete;
    /** @brief Возвращает captured closed header; без valid/completed это только диагностические metadata. */
    const AudioPacket::Header& header() const noexcept { return header_; }
    /** @brief True только для self-bound staged пакета; не подтверждает ACK/Worker join. */
    bool valid() const noexcept { return self_ == this && phase_ == AudioStagePhase::Staged; }
    /** @brief True после единственного post-ACK finalize; не заменяет actual Worker completion. */
    bool completed() const noexcept { return self_ == this && phase_ == AudioStagePhase::Completed; }
    /** @brief True после явного joined abandon без retirement; пакет нельзя публиковать как успешный кадр. */
    bool abandoned() const noexcept { return self_ == this && phase_ == AudioStagePhase::Abandoned; }
    /**
     * @brief Очищает только завершённую/abandoned квитанцию на подтверждённой Main joined-границе.
     * @return false для staged/чужого self; не снимает Core borrow вместо abandon.
     * @pre Main уже получил настоящий Worker done/join; никакой renderer/loan не читает token.
     */
    bool resetJoined() noexcept {
        if (phase_ == AudioStagePhase::Staged || (self_ && self_ != this)) return false;
        header_ = {}; thinSequence_ = leaseGeneration_ = leaseEpoch_ = 0;
        owner_ = nullptr; source_ = nullptr; destination_ = nullptr; thin_ = nullptr; self_ = nullptr;
        phase_ = AudioStagePhase::Empty;
        return true;
    }
private:
    friend class Machine;
    AudioPacket::Header header_{};
    std::uint64_t thinSequence_ = 0, leaseGeneration_ = 0, leaseEpoch_ = 0;
    const Machine* owner_ = nullptr;
    const AudioPacket* source_ = nullptr;
    AudioPacket* destination_ = nullptr;
    const ThinPpuChannel* thin_ = nullptr;
    const AudioStageToken* self_ = nullptr;
    AudioStagePhase phase_ = AudioStagePhase::Empty;
};
}
