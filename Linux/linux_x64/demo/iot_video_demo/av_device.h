// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_IOT_VIDEO_DEMO_AV_DEVICE_H_
#define SRC_DEMO_IOT_VIDEO_DEMO_AV_DEVICE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "iot_demo_config.h"
#include "tc_iot_def.h"

typedef struct {
  // Linux 用：远端音频写入的 dump 文件，NULL 表示不 dump
  const char *dump_audio_path;
  // Linux 用：远端视频写入的 dump 文件，NULL 表示不 dump
  const char *dump_video_path;
  // Linux 用：MP4 文件路径。提供后 av_device_init 会提前解封装，使 seek 命令在采集启动前可用
  const char *mp4_file_path;
} av_device_config_t;

typedef void (*audio_device_callback_t)(const uint8_t *data, uint32_t len, void *user_data);
typedef void (*video_device_callback_t)(const uint8_t *data, uint32_t len, bool is_key_frame,
                                        tc_iot_video_codec_e codec, void *user_data);

void av_device_init(const av_device_config_t *cfg);
void av_device_deinit(void);

// 开始音频采集。Linux 仿真用 audio_file_path 指定本路采集的 PCM 文件（或 .mp4 文件）；
// 真实硬件平台忽略 audio_file_path（共用同一只麦克风，按 callback 广播）。
int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data);
void av_device_stop_audio(audio_device_callback_t callback, void *user_data);

// 开始视频采集。Linux 仿真用 video_file_path 指定本路采集的 H.264 文件（或 .mp4 文件）；
// 真实硬件平台忽略 video_file_path（共用同一只摄像头，按 callback 广播）。
int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data);
void av_device_stop_video(video_device_callback_t callback, void *user_data);

void av_device_playout_audio(const uint8_t *data, uint32_t len);
void av_device_playout_video(const uint8_t *data, uint32_t len);
void av_device_playout_mjpg(const uint8_t *data, uint32_t len);

void av_device_request_idr(void);

// 返回单调递增的毫秒时间戳，用于音视频帧 PTS（各平台 av_device_*.c 自行实现）。
uint64_t av_device_now_ms(void);

// ==================== MP4 解封装 ====================
// 获取 mp4 文件的总时长（毫秒）
uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path);

// seek 到 mp4 文件的指定时长位置（毫秒），返回实际 seek 到的毫秒偏移。失败返回 0。
uint64_t av_device_seek_mp4_ms(uint64_t target_ms);

// ==================== 事件抓拍图（供云存 demo 使用）====================
// 调用方提供 buf，图片数据写入 buf 内。返回 0 成功，-1 失败。
int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_VIDEO_DEMO_AV_DEVICE_H_
