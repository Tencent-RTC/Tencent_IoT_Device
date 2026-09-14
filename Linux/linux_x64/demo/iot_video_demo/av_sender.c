// Copyright (c) 2026 Tencent. All rights reserved.

#include "av_sender.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "av_device.h"
#include "tc_hal_mutex.h"
#include "tc_iot_av.h"
#include "tc_iot_def.h"

#define VIDEO_MAX_FRAME_SIZE (200 * 1024)
struct av_sender_t {
  av_config_t config;

  bool running;
  bool video_first_idr_seen;
  bool audio_acquired;

  tc_iot_av_channel_t *video_channel;

  // 共享音频互斥锁：由 av_sender_alloc 注入，所有 sender 共用同一把（见下方线程模型说明）。
  HAL_Mutex *audio_mutex;
};

// 音频是全局共享的：tc_iot_create_audio_channel 只能创建一次，且麦克风只有一只，
// 因此整个进程只维护「一个 audio channel + 一路音频采集」，由引用计数管理生命周期。
// 采集回调 _on_audio_captured 是全局唯一的（不绑定具体 sender），把帧推到 g_audio_channel。
//
// 线程模型：g_audio_channel / g_audio_channel_refcount 被两类线程访问——
//   1) 平台采集线程（如 bk7258 的 voc_rd）在回调 _on_audio_captured 里读 g_audio_channel
//      并 push（内部会做 AAC 编码，耗时）；
//   2) 控制线程（信令 / demo 主流程）在 _acquire_audio / _release_audio 里增删引用计数、
//      创建 / 销毁 g_audio_channel。
// 二者跨核并发。互斥锁由调用方创建、经 av_sender_alloc 注入每个 sender（所有 sender 共用同一把），
// 控制线程从 self->audio_mutex 取用，采集回调则通过采集 user_data 拿到同一把锁。
// 它保证「采集回调对 channel 的使用」与「拆流对 channel 的销毁」互斥：release 在持锁时把
// g_audio_channel 置空，销毁只发生在置空之后，且回调只在持锁、且 g_audio_channel 非空时才
// push——从而彻底消除 voc_rd 用到已释放 channel 的 UAF。
// 平台 start/stop 采集必须在锁外调用（部分平台 stop 会 join 采集线程，持锁会死锁）。
static tc_iot_av_channel_t *g_audio_channel = NULL;
static int g_audio_channel_refcount = 0;
static bool g_audio_is_aac = false;

// ==================== 内部函数前向声明 ====================

static int _acquire_audio(HAL_Mutex *audio_mutex, const char *audio_file_path);
static void _release_audio(HAL_Mutex *audio_mutex);
static bool _need_audio(tc_iot_media_content_e media_content);
static bool _need_video(tc_iot_media_content_e media_content);
static void _on_audio_captured(const uint8_t *data, uint32_t len, void *user_data);
static void _on_video_captured(const uint8_t *data, uint32_t len, bool is_key_frame,
                               tc_iot_video_codec_e codec, void *user_data);

// ==================== 公开接口 ====================

av_sender_t *av_sender_alloc(const av_config_t *cfg, HAL_Mutex *audio_mutex) {
  if (!cfg) {
    return NULL;
  }
  av_sender_t *self = (av_sender_t *)calloc(1, sizeof(av_sender_t));
  if (!self) {
    return NULL;
  }
  self->config = *cfg;
  self->audio_mutex = audio_mutex;
  printf("[av_sender] alloc: ch=%d media_content=%d quality=%d\n", cfg->channel_id,
         cfg->media_content, cfg->quality);
  return self;
}

void av_sender_free(av_sender_t *self) {
  if (!self) {
    return;
  }
  av_sender_stop(self);
  free(self);
}

int av_sender_start(av_sender_t *self) {
  if (!self) {
    return -1;
  }
  if (self->running) {
    return 0;
  }

  bool need_audio = _need_audio(self->config.media_content);
  bool need_video = _need_video(self->config.media_content);

  self->video_first_idr_seen = false;
  self->running = true;

  // 音频：获取全局共享 channel 并（在首个使用者处）启动唯一的音频采集。
  if (need_audio) {
    if (_acquire_audio(self->audio_mutex, self->config.audio_file_path) != 0) {
      av_sender_stop(self);
      return -1;
    }
    self->audio_acquired = true;
  }

  // 视频：每路通道独立创建 channel 与采集线程。
  if (need_video) {
    self->video_channel =
        tc_iot_create_video_channel(self->config.channel_id, self->config.quality);
    if (!self->video_channel) {
      av_sender_stop(self);
      return -1;
    }
    if (av_device_start_video(self->config.video_file_path, _on_video_captured, self) != 0) {
      printf("[av_sender] start video capture ch=%d failed\n", self->config.channel_id);
      av_sender_stop(self);
      return -1;
    }
  }

  printf("[av_sender] ch=%d started (audio=%d video=%d)\n", self->config.channel_id, need_audio,
         need_video);
  return 0;
}

void av_sender_stop(av_sender_t *self) {
  if (!self || !self->running) {
    return;
  }

  self->running = false;
  if (_need_video(self->config.media_content)) {
    av_device_stop_video(_on_video_captured, self);
  }

  if (self->video_channel) {
    tc_iot_destroy_video_channel(self->video_channel);
    self->video_channel = NULL;
  }
  // 音频：释放全局共享引用，最后一个使用者会停止采集并销毁 channel。
  if (self->audio_acquired) {
    _release_audio(self->audio_mutex);
    self->audio_acquired = false;
  }

  printf("[av_sender] ch=%d stopped\n", self->config.channel_id);
}

int av_sender_get_channel_id(av_sender_t *self) {
  return self ? self->config.channel_id : -1;
}

// ==================== 内部工具 ====================

// 获取全局共享音频资源：引用计数 0→1 时创建 channel 并启动唯一的音频采集，
// 其余情况只递增计数。成功返回 0。
static int _acquire_audio(HAL_Mutex *audio_mutex, const char *audio_file_path) {
  HAL_MutexLock(audio_mutex);

  // 已有使用者：只增引用计数（channel 已就绪，采集已在跑）。
  if (g_audio_channel_refcount > 0) {
    g_audio_channel_refcount++;
    int refcount = g_audio_channel_refcount;
    HAL_MutexUnlock(audio_mutex);
    printf("[av_sender] acquire shared audio, refcount=%d\n", refcount);
    return 0;
  }

  // 首个使用者：在锁内创建 channel 并发布 g_audio_channel（此时采集尚未启动，无回调）。
  tc_iot_av_channel_t *channel = tc_iot_create_audio_channel();
  if (!channel) {
    HAL_MutexUnlock(audio_mutex);
    printf("[av_sender] create shared audio channel failed\n");
    return -1;
  }
  g_audio_channel = channel;
  g_audio_channel_refcount = 1;
  HAL_MutexUnlock(audio_mutex);

  // 启动采集放在锁外：采集线程回调也会抢这把锁，若持锁启动且平台 start 内部等待采集线程即死锁。
  // 把同一把锁作为采集 user_data 传给全局回调 _on_audio_captured，供其自我加锁。
  if (av_device_start_audio(audio_file_path, _on_audio_captured, audio_mutex) != 0) {
    printf("[av_sender] start shared audio capture failed\n");
    HAL_MutexLock(audio_mutex);
    tc_iot_av_channel_t *to_destroy = g_audio_channel;
    g_audio_channel = NULL;
    g_audio_channel_refcount = 0;
    HAL_MutexUnlock(audio_mutex);
    if (to_destroy) {
      tc_iot_destroy_audio_channel(to_destroy);
    }
    return -1;
  }
  g_audio_is_aac = audio_file_path && strlen(audio_file_path) >= 4 &&
                   strcasecmp(audio_file_path + strlen(audio_file_path) - 4, ".mp4") == 0;
  printf("[av_sender] shared audio created (channel + capture) codec=%s\n",
         g_audio_is_aac ? "AAC" : "PCM");
  printf("[av_sender] acquire shared audio, refcount=1\n");
  return 0;
}

// 释放全局共享音频资源：引用计数减 1，减到 0 时停止采集并销毁 channel。
static void _release_audio(HAL_Mutex *audio_mutex) {
  HAL_MutexLock(audio_mutex);
  if (g_audio_channel_refcount <= 0) {
    HAL_MutexUnlock(audio_mutex);
    return;
  }
  g_audio_channel_refcount--;
  int refcount = g_audio_channel_refcount;
  tc_iot_av_channel_t *to_destroy = NULL;
  if (refcount == 0) {
    // 在锁内把全局指针置空：此后 _on_audio_captured 抢到锁只会看到 NULL 直接返回，
    // 不再访问即将销毁的 channel；而正在 push 的回调持有锁，release 会阻塞到其结束。
    to_destroy = g_audio_channel;
    g_audio_channel = NULL;
  }
  HAL_MutexUnlock(audio_mutex);

  printf("[av_sender] release shared audio, refcount=%d\n", refcount);

  if (to_destroy) {
    // 停止采集与销毁 channel 放在锁外：部分平台 stop 会 join 采集线程，而采集线程回调仍会
    // 抢这把锁，持锁 stop 会死锁。此处安全：g_audio_channel 已在锁内置空，to_destroy 为
    // 局部私有，任何后续回调都拿不到它，故无 UAF。
    // user_data 传同一把锁：与 _acquire_audio 里 start 的 user_data 一致，stop 才能匹配上。
    av_device_stop_audio(_on_audio_captured, audio_mutex);
    tc_iot_destroy_audio_channel(to_destroy);
    printf("[av_sender] shared audio destroyed (capture + channel)\n");
  }
}

static bool _need_audio(tc_iot_media_content_e media_content) {
  return media_content == TC_IOT_MEDIA_CONTENT_AUDIO ||
         media_content == TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
}

static bool _need_video(tc_iot_media_content_e media_content) {
  return media_content == TC_IOT_MEDIA_CONTENT_VIDEO ||
         media_content == TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
}

// ==================== 采集回调 → 推流 ====================

// 全局唯一的音频采集回调：所有视频通道共享此音频流，帧只推送到共享 channel 一次。
// 由平台采集线程调用。user_data 是 _acquire_audio 注入的共享音频互斥锁；全程持锁：
// 与 _release_audio 的销毁互斥，保证 push 期间 g_audio_channel 不会被销毁（消除 UAF）。
static void _on_audio_captured(const uint8_t *data, uint32_t len, void *user_data) {
  HAL_Mutex *audio_mutex = (HAL_Mutex *)user_data;
  if (!data || len == 0) {
    return;
  }

  HAL_MutexLock(audio_mutex);
  if (!g_audio_channel) {
    HAL_MutexUnlock(audio_mutex);
    return;
  }

  tc_iot_audio_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.data = (uint8_t *)data;
  frame.data_size = len;
  frame.pts_ms = av_device_now_ms();
  frame.codec = g_audio_is_aac ? TC_IOT_AUDIO_CODEC_AAC : TC_IOT_AUDIO_CODEC_PCM;
  frame.sample_rate = (tc_iot_audio_sample_rate_e)MIC_SAMPLE_RATE;
  frame.channels = (tc_iot_audio_channel_e)MIC_CHANNELS;
  frame.frame_duration_ms = 20;

  tc_iot_error_e err = tc_iot_push_audio_frame(g_audio_channel, &frame);
  HAL_MutexUnlock(audio_mutex);

  if (err != TC_IOT_ERR_SUCCESS) {
    printf("[av_sender] push shared audio frame failed: %d\n", err);
  }
}

static void _on_video_captured(const uint8_t *data, uint32_t len, bool is_key_frame,
                               tc_iot_video_codec_e codec, void *user_data) {
  av_sender_t *self = (av_sender_t *)user_data;
  if (!self || !self->running || !self->video_channel || !data || len == 0) {
    return;
  }
  if (len > VIDEO_MAX_FRAME_SIZE) {
    return;
  }

  // 等待第一个 IDR 帧后再开始上行，保证对端能解码。
  if (!self->video_first_idr_seen) {
    if (!is_key_frame) {
      return;
    }
    self->video_first_idr_seen = true;
    printf("[av_sender] ch=%d first IDR seen, video send enabled\n", self->config.channel_id);
  }

  tc_iot_video_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.data = (uint8_t *)data;
  frame.data_size = len;
  frame.pts_ms = av_device_now_ms();
  frame.codec = codec;
  frame.type = is_key_frame ? TC_IOT_VIDEO_FRAME_TYPE_IDR : TC_IOT_VIDEO_FRAME_TYPE_P;
  frame.rotation = TC_IOT_VIDEO_ROTATION_0;
  frame.width = VIDEO_WIDTH;
  frame.height = VIDEO_HEIGHT;

  tc_iot_error_e err = tc_iot_push_video_frame(self->video_channel, &frame);
  if (err != TC_IOT_ERR_SUCCESS) {
    printf("[av_sender] push video frame ch=%d failed: %d\n", self->config.channel_id, err);
  }
}
