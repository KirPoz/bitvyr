/* Собственное расширение; включается только в частную копию pinned sdio_drv.c. */
#include "../transport/RawPort.h"
#include "../protocol/RawLimits.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
/** @brief Два DMA-конверта; SDK получает zerocopy источник и возвращает его единственным callback. */
typedef struct { uint8_t data[1536] __attribute__((aligned(64))); void* context; void(*done)(void*); unsigned busy; } nes_raw_slot_t;
static DRAM_ATTR nes_raw_slot_t nes_raw_slots[2];
static portMUX_TYPE nes_raw_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief SDK закончил чтение собственного DMA-конверта; callback не подтверждает remote delivery. */
static void nes_raw_source_done(void* pointer) {
    nes_raw_slot_t* slot=(nes_raw_slot_t*)pointer;
    void(*done)(void*)=slot->done;void* context=slot->context;
    portENTER_CRITICAL(&nes_raw_lock);slot->busy=0;portEXIT_CRITICAL(&nes_raw_lock);
    if(done)done(context);
}
/** @brief Неблокирующее принятие в обычную lower-priority очередь: HCI сохраняет более высокий приоритет. */
int nes_hosted_raw_try_tx(unsigned index,const uint8_t* data,size_t bytes,void* context,void(*done)(void*)) {
    if(index>=2||!data||!bytes||bytes>NES_RAW_REQUEST_PACKET_BYTES||bytes>MAX_PAYLOAD_SIZE||bytes+sizeof(struct esp_payload_header)>curr_transfer_size||!done||!is_transport_tx_ready())return 0;
    nes_raw_slot_t* slot=&nes_raw_slots[index];
    portENTER_CRITICAL(&nes_raw_lock);if(slot->busy){portEXIT_CRITICAL(&nes_raw_lock);return 0;}slot->busy=1;portEXIT_CRITICAL(&nes_raw_lock);
    slot->context=context;slot->done=done;memset(slot->data,0,sizeof(slot->data));
    memcpy(slot->data+sizeof(struct esp_payload_header),data,bytes);
    interface_buffer_handle_t handle={0};handle.if_type=ESP_SERIAL_IF;handle.if_num=1;handle.payload_zcopy=H_BUFF_ZEROCOPY;
    handle.payload_len=(uint16_t)bytes;handle.payload=slot->data;handle.priv_buffer_handle=slot;handle.free_buf_handle=nes_raw_source_done;
    if(g_h.funcs->_h_queue_item(to_slave_queue[PRIO_Q_OTHERS],&handle,0)!=0){portENTER_CRITICAL(&nes_raw_lock);slot->busy=0;portEXIT_CRITICAL(&nes_raw_lock);return 0;}
    g_h.funcs->_h_post_semaphore(sem_to_slave_queue);return 1;
}
