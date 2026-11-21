#include "RawPort.h"
#include "../protocol/RawLimits.h"
extern "C" {
#include "transport_drv.h"
int __real_serial_ll_rx_handler(interface_buffer_handle_t* handle);
void nes_hosted_control_rx_observe(interface_buffer_handle_t* handle) __attribute__((weak));
}
/** @brief Linker --wrap отделяет собственный SERIAL1 до stock protobuf decoder; SERIAL0/HCI остаются SDK. */
extern "C" int __wrap_serial_ll_rx_handler(interface_buffer_handle_t* handle) {
    if(!handle||handle->if_num!=1){if(nes_hosted_control_rx_observe)nes_hosted_control_rx_observe(handle);return __real_serial_ll_rx_handler(handle);}
    if(handle->if_type==3&&handle->payload&&handle->payload_len<=NES_RAW_REPLY_PACKET_BYTES&&!handle->flag)
        nes_hosted_raw_rx(handle->payload,handle->payload_len);
    if(handle->free_buf_handle&&handle->priv_buffer_handle){handle->free_buf_handle(handle->priv_buffer_handle);handle->priv_buffer_handle=nullptr;}
    return 0;
}
