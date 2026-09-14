// Copyright (c) 2026 Tencent. All rights reserved.

/**
 * @file audio_device.h
 * @brief 音频设备平台抽象层：统一采集/播放接口，各平台分别实现。
 */

#ifndef SRC_DEMO_IOT_AITALK_DEMO_AUDIO_DEVICE_H_
#define SRC_DEMO_IOT_AITALK_DEMO_AUDIO_DEVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "tc_iot_def.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t sample_rate;            /* 16000 */
  uint8_t channels;                /* 1 */
  uint8_t bits_per_sample;         /* 16 */
  uint32_t frame_duration_ms;      /* 60 */
  tc_iot_audio_codec_e send_codec; /* 采集输出编码 (PCM/OPUS) */
  tc_iot_audio_codec_e recv_codec; /* 播放输入编码 (PCM/OPUS) */
} audio_device_config_t;

/* 采集回调：设备产出已编码帧时调用 */
typedef void (*audio_device_on_capture_t)(const uint8_t *data, uint32_t size,
                                          tc_iot_audio_codec_e codec, uint64_t pts_ms,
                                          void *user_data);

/* 生命周期 */
int audio_device_open(const audio_device_config_t *config);
void audio_device_close(void);

/* 采集 (push 模式，回调在内部线程触发) */
int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data);
void audio_device_stop_capture(void);

/* 播放（codec != PCM 时内部解码） */
int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec);

/* 编码配置查询（各平台实现） */
tc_iot_audio_codec_e audio_device_default_codec(void);
bool audio_device_codec_supported(tc_iot_audio_codec_e codec);
uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec);

/* 跨平台毫秒级休眠（各平台实现），供 demo 等待逻辑使用 */
void audio_device_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_AITALK_DEMO_AUDIO_DEVICE_H_
