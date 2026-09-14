// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_IOT_VIDEO_DEMO_AV_SENDER_H_
#define SRC_DEMO_IOT_VIDEO_DEMO_AV_SENDER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tc_hal_mutex.h"
#include "tc_iot_av.h"
#include "tc_iot_def.h"

// av_sender 配置：一个 sender 对应一路通道（channel_id）。
typedef struct {
  // 通道参数
  int channel_id;
  tc_iot_media_content_e media_content;
  tc_iot_video_quality_e quality;

  // ---- Linux 文件流 ----
  const char *audio_file_path;
  const char *video_file_path;
} av_config_t;

typedef struct av_sender_t av_sender_t;

// 共享音频（全局单例：一个 audio channel + 一路采集）的采集回调与拆流销毁跨线程并发。
// 调用方创建一把 HAL_Mutex，在 alloc 时传给每个 sender——所有 sender 必须共用同一把，
// 用于串行化「采集回调用 channel」与「拆流销毁 channel」，消除 use-after-free。
// audio_mutex 的生命周期由调用方管理，必须覆盖所有 sender 的存活期。
av_sender_t *av_sender_alloc(const av_config_t *cfg, HAL_Mutex *audio_mutex);
void av_sender_free(av_sender_t *sender);

int av_sender_start(av_sender_t *sender);
void av_sender_stop(av_sender_t *sender);

int av_sender_get_channel_id(av_sender_t *sender);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_VIDEO_DEMO_AV_SENDER_H_
