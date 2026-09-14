// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_H_
#define SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tc_iot_def.h"

// monitor 多通道最多并发路数
#define IOT_DEMO_MAX_MONITOR_CHANNELS 8

typedef struct {
  // 设备三元组
  const char *product_id;
  const char *device_id;
  const char *device_secret;
  // 可为 NULL，默认 "ap-guangzhou"
  const char *region;

  // ---- Linux 仿真：媒体源与远端帧 dump ----
  // 默认音频文件（所有 sender 共用，因为只有一份 PCM）
  const char *audio_file_path;
  // 默认视频文件（call sender / monitor 共用）
  const char *video_file_path;
  // Linux 仿真：MP4 文件路径（提供给 av_device_init 提前解封装，用于 seek 测试）
  const char *mp4_file_path;

  // 远端帧 dump（av_receiver / av_device 使用）
  const char *dump_audio_path;
  const char *dump_video_path;
} iot_demo_config_t;

int iot_demo_start(const iot_demo_config_t *cfg);
void iot_demo_stop(void);

int iot_demo_call(const char *user_id, tc_iot_media_content_e media_content,
                  tc_iot_video_quality_e video_quality);
int iot_demo_accept(const char *user_id, tc_iot_media_content_e media_content,
                    tc_iot_video_quality_e video_quality);
int iot_demo_reject(const char *user_id);
int iot_demo_hangup(const char *user_id);

uint64_t iot_demo_seek_mp4_second(uint64_t target_second);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_H_
