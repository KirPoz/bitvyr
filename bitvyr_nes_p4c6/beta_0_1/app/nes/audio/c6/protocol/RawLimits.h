#pragma once

/* Единый C/C++ wire contract частного directional-MTU профиля, без SDK/ABI structs.
 * Request1400 сохраняет stock Hosted TX/RX1536. Только собственный C6 reply1792
 * получает DMA2048; общий P4 copy pool2048 обслуживает RX и прежний TX1536.
 * Два направления не должны объявлять одинаковую увеличенную request-capacity.
 */
#define NES_RAW_HEADER_BYTES 64U
#define NES_RAW_REQUEST_PACKET_BYTES 1400U
#define NES_RAW_REPLY_PACKET_BYTES 1792U
#define NES_RAW_HOSTED_HEADER_BYTES 12U
#define NES_RAW_RX_BLOCK_BYTES 2048U
#define NES_RAW_STREAM_PACKET_COUNT 20U
#define NES_RAW_STREAM_BYTES (NES_RAW_RX_BLOCK_BYTES * NES_RAW_STREAM_PACKET_COUNT)
#define NES_RAW_DIRECTIONAL_PROFILE 2U
#define NES_RAW_HELLO_CAPABILITIES 0x1ffU
#define NES_RAW_HELLO_BODY_BYTES 100U
