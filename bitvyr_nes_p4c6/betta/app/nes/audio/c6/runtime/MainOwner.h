#pragma once
#include "BatchMainSession.h"
#include "../qualification/C6BatchImage.h"
#include "../protocol/RawAsyncAdapter.h"
#include "../transport/P4RawAsyncPort.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <array>
#include <cstdio>
#include <new>

namespace nes::app::c6_async {
/**
 * @brief PRIVATE boot-long Main control в HP; wire8192/дополнительный PCM4096 живут отдельно в PSRAM.
 * @details Обычные поля только Main; отдельный AudioPublished находится в INTERNAL WorkerState.
 * Не создаёт задачи и не освобождается при unknown RX/TX/service/Worker ownership.
 */
class MainOwner final {
public:
    /**
     * @brief Выделяет HP control и PSRAM payload для проверенного профиля C6 до первого запроса.
     * @param qualification Полный RawProfile, ожидаемый из отдельно проверенного C6 image.
     * @details Один warm flag без actual exact HELLO не открывает UNBIND. Allocation/caps и
     * boot-long retention прежние; ROM, Worker join и готовность driver проверяет caller.
     */
    static MainOwner* createQualified(const ::nes::c6audio::service::Binding&binding,publication::AudioPublished&published,std::int16_t*pcm0,
        publication::BatchClient::Backend backend,const publication::RawProfile&qualification)noexcept{
        const auto&hello=qualification.hello;
        if(!publication::RawAsyncAdapter::validProfile(qualification)||hello.generation!=binding.appGeneration||hello.lease!=binding.romLease||
           hello.session!=binding.session||hello.region!=std::uint8_t(binding.region))return nullptr;
        void*memory=heap_caps_aligned_alloc(64,sizeof(MainOwner),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        if(!memory)return nullptr;
        if(!validControl(memory)){heap_caps_free(memory);return nullptr;}
        void*data=heap_caps_aligned_alloc(64,sizeof(Payload),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!data){heap_caps_free(memory);return nullptr;}
        if(!validPayload(data)){heap_caps_free(data);heap_caps_free(memory);return nullptr;}
        auto*channel=::nes::c6audio::p4::initializeMainChannel();
        if(!channel){heap_caps_free(data);heap_caps_free(memory);return nullptr;}
        auto*payload=::new(data)Payload;
        auto*owner=::new(memory)MainOwner(*channel,binding,published,pcm0,*payload,backend,qualification);
        std::printf("NES_C6_AUDIO_ALLOC control=%p control_bytes=%u control_hp_dma=1 payload=%p payload_bytes=%u payload_psram=1 wire_bytes=8192 pcm_bytes=4096 channel_bytes=%u no_rpc=1\n",
            memory,unsigned(sizeof(MainOwner)),data,unsigned(sizeof(Payload)),unsigned(sizeof(::nes::c6raw::Channel)));
        return owner;
    }
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    MainOwner(const MainOwner&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    MainOwner&operator=(const MainOwner&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    MainOwner(MainOwner&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    MainOwner&operator=(MainOwner&&)=delete;
    /** @brief Один cold/run шаг, без waits/alloc/log; первые HELLO/INIT/BIND выполняются лишь здесь. */
    bool pollOne()noexcept{
        if(mode_==Mode::Poisoned)return false;
        if(mode_==Mode::Unbound)return true;
        if(mode_==Mode::Empty){if(!adapter_.beginHello())return fail();mode_=Mode::Hello;return true;}
        if(mode_==Mode::WarmStart){if(!adapter_.beginHello())return fail();mode_=Mode::WarmHello;return true;}
        if(mode_==Mode::WarmHello){
            publication::ReplyView reply{};const auto result=publication::RawAsyncAdapter::poll(&adapter_,reply);
            if(result==publication::TransportState::Pending)return true;
            if(result!=publication::TransportState::Complete||!adapter_.warmQualified()||!session_.beginReboundBinding())return fail();
            mode_=Mode::WarmBind;return true;
        }
        if(mode_==Mode::Hello){
            publication::ReplyView reply{};const auto result=publication::RawAsyncAdapter::poll(&adapter_,reply);
            if(result==publication::TransportState::Pending)return true;
            if(result!=publication::TransportState::Complete||!adapter_.qualified()||
               !session_.begin(binding_,payload_->wire.data(),payload_->wire.size(),pcm0_,payload_->pcm.data(),payload_->pcm.size()))return fail();
            mode_=Mode::Cold;return true;
        }
        if(!session_.pollOne())return fail();
        if(mode_==Mode::Unbinding){
            if(session_.unbound()){
                if(!adapter_.unboundAckVerified())return fail();
                mode_=Mode::Unbound;
            }
            return true;
        }
        if((mode_==Mode::Cold||mode_==Mode::WarmBind)&&session_.initialized())mode_=Mode::Ready;
        return true;
    }
    /** @brief Проверенные read-only HELLO и INIT/BIND завершены; не означает Worker или PCM ready. */
    bool ready()const noexcept{return mode_==Mode::Ready;}
    /**
     * @brief На actual noJob/final-drained границе начинает checked old-stream UNBIND.
     * @details Legacy ae1c profile отказывает без нового RPC. Caller удерживает запрет
     * нового Worker start до нового ready; числовой noJob сам не берёт RTOS semaphore.
     */
    bool beginUnbind(bool noJobInFlight)noexcept{
        if(mode_!=Mode::Ready||!adapter_.warmQualified()||!session_.beginUnbind(noJobInFlight))return false;
        mode_=Mode::Unbinding;return true;
    }
    /** @brief Exact local session/adapter ACK завершён; не означает PPA/Worker либо C6 driver-sourceDone. */
    bool unbound()const noexcept{return mode_==Mode::Unbound&&session_.unbound()&&adapter_.unboundAckVerified();}
    /**
     * @brief Без allocation переиспользует пустые PCM/wire и начинает новый HELLO→BIND, без INIT.
     * @details C6 capability гарантирует bounded ранний HELLO Ready: compute ждёт старый
     * ACK-sourceDone. На P4 нет таймера, который выдаёт это событие за завершённое.
     * Ошибка после commit удерживает owner; методы не освобождают ROM/storage.
     */
    bool rebindEmpty(const ::nes::c6audio::service::Binding&next,bool noJobInFlight)noexcept{
        if(!noJobInFlight||!unbound())return false;
        const auto nextHello=helloKey(next);
        if(!adapter_.canWarmRebind(nextHello)||!session_.rebindEmpty(next,true))return false;
        if(!adapter_.reprofileWarm(nextHello))return fail();
        binding_=next;mode_=Mode::WarmStart;return true;
    }
    /** @brief Возвращает Main-only session для exact joined-N/PCM/backpressure операций. */
    BatchMainSession& session()noexcept{return session_;}
    /** @brief Immutable App/audio binding копируется до start, не читается Worker через owner. */
    const ::nes::c6audio::service::Binding&binding()const noexcept{return binding_;}
    /** @brief C6 numeric counters доступны только Main; это nested component times, не NES FPS. */
    const ::nes::c6audio::client::Stats&stats()const noexcept{return client_.stats();}
    /** @brief Timeout не освобождает Channel/PSRAM/PCM и не вызывает C6 reset. */
    bool noteTimeout()noexcept{adapter_.noteTimeout();session_.noteTimeout();return fail();}
private:
    /** @brief Только payload: прежние два массива без control metadata и без дополнительной копии. */
    struct alignas(16) Payload final {
        std::array<std::uint8_t,8192>wire{};
        std::array<std::int16_t,core::Apu::sampleCapacity>pcm{};
    };
    static_assert(sizeof(Payload)==12288,"C6 payload holds one wire window and the second PCM buffer");
    /** @brief Проверяет alignment и оба конца целого control extent: именно HP DRAM, internal, DMA и byte accessible. */
    static bool validControl(void*memory)noexcept{
        const auto*last=static_cast<std::uint8_t*>(memory)+sizeof(MainOwner)-1;
        return reinterpret_cast<std::uintptr_t>(memory)%64U==0&&esp_ptr_in_dram(memory)&&esp_ptr_in_dram(last)&&
            esp_ptr_internal(memory)&&esp_ptr_internal(last)&&esp_ptr_dma_capable(memory)&&esp_ptr_dma_capable(last)&&
            esp_ptr_byte_accessible(memory)&&esp_ptr_byte_accessible(last);
    }
    /** @brief Проверяет оба конца отдельного PSRAM payload; аппаратный TX envelope остаётся внутри прежнего Channel. */
    static bool validPayload(void*memory)noexcept{
        const auto*last=static_cast<std::uint8_t*>(memory)+sizeof(Payload)-1;
        return reinterpret_cast<std::uintptr_t>(memory)%64U==0&&esp_ptr_external_ram(memory)&&esp_ptr_external_ram(last)&&
            esp_ptr_byte_accessible(memory)&&esp_ptr_byte_accessible(last);
    }
    /** @brief Числовые cold этапы принадлежат только Main. */
    enum class Mode:std::uint8_t {Empty,Hello,Cold,Ready,Unbinding,Unbound,WarmStart,WarmHello,WarmBind,Poisoned};
    /** @brief Формирует только HELLO identity; freshness полного binding проверяет checked session/client. */
    static ::nes::c6raw::Key helloKey(const ::nes::c6audio::service::Binding&binding)noexcept{
        return {binding.appGeneration,binding.romLease,binding.session,0,0,0,0,0,0,std::uint8_t(binding.region)};
    }
    /** @brief Создаёт только refs/metadata и заимствует отдельный payload; транспорт не запускается конструктором. */
    MainOwner(::nes::c6raw::Channel&channel,const ::nes::c6audio::service::Binding&binding,
              publication::AudioPublished&published,std::int16_t*pcm0,Payload&payload,publication::BatchClient::Backend backend,const publication::RawProfile&qualification)noexcept:
        adapter_(channel,qualification,::nes::c6audio::p4::clockMicroseconds),
        client_(&adapter_,publication::RawAsyncAdapter::start,publication::RawAsyncAdapter::poll,backend),
        session_(client_,published),binding_(binding),pcm0_(pcm0),payload_(&payload){}
    /** @brief Sticky отказ сохраняет boot-long owner и все поздние callbacks. */
    bool fail()noexcept{mode_=Mode::Poisoned;return false;}
    publication::RawAsyncAdapter adapter_;publication::BatchClient client_;BatchMainSession session_;
    ::nes::c6audio::service::Binding binding_{};std::int16_t*pcm0_{};Payload*payload_{};Mode mode_=Mode::Empty;
};
} // namespace nes::app::c6_async
