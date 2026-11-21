#pragma once
#include "../protocol/AudioPacketWire.h"
#include <atomic>

namespace nes::c6audio::async {
/** @brief Полная App/audio identity immutable публикации, без указателя на Machine. */
struct PublishedKey final {
    std::uint64_t appGeneration{},romLease{},romEpoch{},session{},inputFrame{};
    wire::Expected audio{};std::uint8_t arena{};
};
/** @brief Сверяет все домены, включая region/capabilities/полный64-bit frame/time. */
inline bool same(const PublishedKey&a,const PublishedKey&b)noexcept{
    const auto&x=a.audio;const auto&y=b.audio;
    return a.appGeneration==b.appGeneration&&a.romLease==b.romLease&&a.romEpoch==b.romEpoch&&a.session==b.session&&a.inputFrame==b.inputFrame&&a.arena==b.arena&&
        x.generation==y.generation&&x.sequence==y.sequence&&x.ppuFrame==y.ppuFrame&&x.baseCycle==y.baseCycle&&x.endCycle==y.endCycle&&x.region==y.region&&x.capabilities==y.capabilities;
}
/**
 * @brief Доверенная App квитанция именно joined кадра N, не обещание idle всей Machine.
 * @details Создаётся только после actual semaphore+exact identity N; CPU1 N+1 уже может
 * работать. Этот тип сам не берёт semaphore и не разрешает reset/destroy Machine N+1.
 */
struct JoinedFrame final {PublishedKey key{};bool actualDoneTaken{},identityValidated{};};
/**
 * @brief PRIVATE одноэлементный release/acquire handoff только staged immutable records.
 * @details Worker публикует ПОСЛЕ actual Core stage и больше не пишет destination records.
 * Main получает только view/header; token/finalize/Machine остаются Worker. Состояние
 * не означает WorkerDone, ACK, PCM ready или PPA completion. Stop/timeout не освобождает.
 * Используются прежние две AudioPacket arena; этот объект не содержит их копий.
 */
class AudioPublished final {
public:
    /** @brief Empty не означает наличие выделенных arenas; ресурсы предоставляет App. */
    AudioPublished()noexcept=default;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    AudioPublished(const AudioPublished&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    AudioPublished&operator=(const AudioPublished&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    AudioPublished(AudioPublished&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    AudioPublished&operator=(AudioPublished&&)=delete;
    /**
     * @brief Worker фиксирует ticket после stage, но до ACK/true join.
     * @param view Физически доступные immutable used arrays живут до releaseInput.
     * @details Header копируется отдельно; источник не Machine и не активный packet.
     * Проверка actual stage publication остаётся обязанностью доверенного App caller.
     */
    bool publish(const PublishedKey&key,const core::AudioPacketView&view)noexcept{
        if(state_.load(std::memory_order_acquire)!=Empty||!view.readable()||!key.appGeneration||!key.romLease||!key.romEpoch||!key.session||key.arena>1)return false;
        const auto&h=view.header();const auto&a=key.audio;
        if(h.fault!=core::AudioFault::None||h.generation!=a.generation||h.sequence!=a.sequence||h.ppuFrame!=a.ppuFrame||h.baseCycle!=a.baseCycle||h.endCycle!=a.endCycle||
           h.region!=a.region||h.capabilities!=a.capabilities||!a.generation||a.endCycle<=a.baseCycle||a.ppuFrame!=key.inputFrame+1U||key.inputFrame==~std::uint64_t{0})return false;
        key_=key;header_=h;input_=view.inputs();dmc_=view.dmc();expansion_=view.expansions();
        state_.store(Published,std::memory_order_release);return true;
    }
    /**
     * @brief Main читает release-published immutable key без предсказания actual endCycle.
     * @param out Отдельный caller-owned key; при отсутствии Published не изменяется.
     * @details Это не admission: caller независимо проверяет App/ROM/session/input/arena,
     * audio generation/sequence/ppuFrame/base/region/capabilities и bounded endCycle,
     * затем вызывает acquire с этим exact key. Primary Machine не читается.
     */
    bool peekPublishedKey(PublishedKey&out)const noexcept{
        if(state_.load(std::memory_order_acquire)!=Published)return false;
        out=key_;return true;
    }
    /** @brief Main acquire возвращает неизменный view только exact identity; отказ не снимает borrow. */
    bool acquire(const PublishedKey&expected,core::AudioPacketView&out)noexcept{
        if(state_.load(std::memory_order_acquire)!=Published||!same(key_,expected))return false;
        out={&header_,input_,header_.inputCount,dmc_,header_.dmcCount,expansion_,header_.expansionCount};
        state_.store(Borrowed,std::memory_order_release);return true;
    }
    /**
     * @brief Main отмечает full DATA acceptance И actual input-TX sourceDone.
     * @details Не разрешает PCM reuse или remote CONSUMED. false не отзывает source.
     * При отдельной encoded wire copy сохранение исходной arena до этого момента консервативно.
     */
    bool releaseInput(const PublishedKey&expected,bool allDataAcceptedAndTxDone)noexcept{
        if(state_.load(std::memory_order_acquire)!=Borrowed||!same(key_,expected)||!allDataAcceptedAndTxDone)return false;
        state_.store(Released,std::memory_order_release);return true;
    }
    /**
     * @brief Только actual joined App разрешает следующую публикацию в той же arena.
     * @details Неверный exact joined-N proof, fault или timeout никогда не очищают borrow. Уничтожение
     * ticket/header запрещено, пока Main читает acquired view; releaseInput завершает чтение.
     */
    bool reclaimJoined(const PublishedKey&expected,const JoinedFrame&joined)noexcept{
        if(!joined.actualDoneTaken||!joined.identityValidated||!same(expected,joined.key)||state_.load(std::memory_order_acquire)!=Released||!same(key_,expected))return false;
        state_.store(Empty,std::memory_order_release);return true;
    }
    /**
     * @brief Exact reclaim receipt для Frame Main, единственного publisher этого объекта.
     * @pre Другой task не может одновременно publish: Frame Worker пишет только Payload.
     * @details Empty начального/чужого key не подходит. Empty после exact key возможен
     * лишь через releaseInput(DATA+TXdone) и reclaimJoined(matching actual JoinedFrame).
     */
    bool reclaimedMainFrame(const PublishedKey&expected)const noexcept{
        return state_.load(std::memory_order_acquire)==Empty&&same(key_,expected);
    }
    /** @brief Числовая занятость не является proof completion другого владельца. */
    bool held()const noexcept{return state_.load(std::memory_order_acquire)!=Empty;}
private:
    enum:unsigned {Empty,Published,Borrowed,Released};
    std::atomic<unsigned>state_{Empty};PublishedKey key_{};core::AudioPacket::Header header_{};
    const core::AudioPacket::Input*input_{};const core::AudioPacket::Dmc*dmc_{};const core::AudioPacket::Expansion*expansion_{};
};
static_assert(std::atomic<unsigned>::is_always_lock_free,"single-word handoff must not allocate hidden lock");
} // namespace nes::c6audio::async
