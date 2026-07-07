/**
 * @file audio_stream_protocol.h
 * @brief 音频流协议定义 — BinaryProtocolV3 (BP3)
 *
 * ESP32 与服务器之间的 WebSocket 二进制帧格式：
 *   [type:1B][reserved:1B][payload_size:2B BE][payload:NB]
 *
 * 常量定义：
 *   - 帧时长 60ms，采样率 16kHz，单声道
 *   - 每帧 960 个样本（16000 * 0.06）
 *   - Opus 编码后每帧约 25-60 字节
 */

#ifndef _AUDIO_STREAM_PROTOCOL_H_
#define _AUDIO_STREAM_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======================== 音频流参数 ========================
#define AUDIO_STREAM_FRAME_DURATION_MS  60       // 每帧时长（毫秒）
#define AUDIO_STREAM_SAMPLE_RATE        16000    // 采样率（Hz）
#define AUDIO_STREAM_CHANNELS           1        // 通道数（mono）
#define AUDIO_STREAM_FRAME_SAMPLES      (AUDIO_STREAM_SAMPLE_RATE * AUDIO_STREAM_FRAME_DURATION_MS / 1000)  // 每帧样本数 = 960

// ======================== BP3 帧类型 ========================
enum {
    AUDIO_STREAM_TYPE_OPUS = 0x00,   // Opus 编码音频帧
    AUDIO_STREAM_TYPE_JSON = 0x01,   // JSON 文本帧
};

// ======================== BP3 帧结构 ========================
/**
 * BinaryProtocolV3 帧格式：
 *   Byte 0: type（帧类型）
 *   Byte 1: reserved（保留，填 0）
 *   Byte 2-3: payload_size（大端序，payload 字节数）
 *   Byte 4+: payload（实际数据）
 */
typedef struct {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t payload_size;
    uint8_t  payload[];
} __attribute__((packed)) binary_protocol_v3_t;

/**
 * @brief 写入 BP3 帧头
 *
 * @param buf          输出缓冲区（至少 4 字节）
 * @param type         帧类型（AUDIO_STREAM_TYPE_OPUS 或 AUDIO_STREAM_TYPE_JSON）
 * @param payload_size payload 字节数
 */
static inline void bp3_write_header(uint8_t *buf, uint8_t type, uint16_t payload_size) {
    buf[0] = type;
    buf[1] = 0;  // reserved
    buf[2] = (payload_size >> 8) & 0xFF;  // 高字节
    buf[3] = payload_size & 0xFF;          // 低字节
}

#ifdef __cplusplus
}
#endif

#endif