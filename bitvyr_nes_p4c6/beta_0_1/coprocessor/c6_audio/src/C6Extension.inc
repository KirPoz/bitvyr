/* Собственное расширение; включается только в частную копию pinned sdio_slave_api.c. */
#include "RawPort.h"
#include "RawLimits.h"
/** @brief Память DMA принадлежит статическому слоту до actual sdio_slave_send_get_finished. */
typedef struct { uint8_t data[NES_RAW_RX_BLOCK_BYTES] __attribute__((aligned(4))); void* context; void(*done)(void*); unsigned busy; } nes_raw_slot_t;
static DRAM_ATTR nes_raw_slot_t nes_raw_slots[2];
static portMUX_TYPE nes_raw_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief Возвращает1 только для собственного завершённого DMA; чужие буферы остаются stock allocator. */
static int nes_raw_dma_done(void* pointer) {
    for(unsigned i=0;i<2;++i){nes_raw_slot_t* slot=&nes_raw_slots[i];if(pointer!=slot->data)continue;
        if(!slot->busy)return 1;
        void(*done)(void*)=slot->done;void* context=slot->context;
        portENTER_CRITICAL(&nes_raw_lock);slot->busy=0;portEXIT_CRITICAL(&nes_raw_lock);if(done)done(context);return 1;}
    return 0;
}
/** @brief Не ждёт кредит/очередь и не вызывает malloc; два raw слота ограничивают конкуренцию с HCI. */
int nes_hosted_raw_try_tx(unsigned index,const uint8_t* data,size_t bytes,void* context,void(*done)(void*)) {
#if SIMPLIFIED_SDIO_SLAVE
    (void)index;(void)data;(void)bytes;(void)context;(void)done;return 0;
#else
    if(index>=2||!data||!bytes||bytes>NES_RAW_REPLY_PACKET_BYTES||!done||if_handle_g.state<ACTIVE||is_host_power_saving()||!sdio_send_queue_sem)return 0;
    nes_raw_slot_t* slot=&nes_raw_slots[index];
    portENTER_CRITICAL(&nes_raw_lock);if(slot->busy){portEXIT_CRITICAL(&nes_raw_lock);return 0;}slot->busy=1;portEXIT_CRITICAL(&nes_raw_lock);
    if(xSemaphoreTake(sdio_send_queue_sem,0)!=pdTRUE){portENTER_CRITICAL(&nes_raw_lock);slot->busy=0;portEXIT_CRITICAL(&nes_raw_lock);return 0;}
    slot->context=context;slot->done=done;memset(slot->data,0,sizeof(slot->data));
    interface_buffer_handle_t handle={0};handle.if_type=ESP_SERIAL_IF;handle.if_num=1;handle.payload_len=(uint16_t)bytes;
    memcpy(slot->data+sizeof(struct esp_payload_header),data,bytes);update_tx_header(slot->data,&handle);
    const esp_err_t result=sdio_slave_send_queue(slot->data,bytes+sizeof(struct esp_payload_header),slot->data,0);
    if(result!=ESP_OK){xSemaphoreGive(sdio_send_queue_sem);portENTER_CRITICAL(&nes_raw_lock);slot->busy=0;portEXIT_CRITICAL(&nes_raw_lock);return 0;}
    return 1;
#endif
}
