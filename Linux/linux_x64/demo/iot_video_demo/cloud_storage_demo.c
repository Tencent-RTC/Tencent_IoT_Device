// Copyright (c) 2026 Tencent. All rights reserved.

// cloud_storage_demo：云存储 demo

#include "cloud_storage_demo.h"

#if BUILD_IOT_CLOUD_STORAGE

#  include <inttypes.h>
#  include <stdbool.h>
#  include <stdint.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

#  include "av_device.h"
#  include "tc_hal_memory.h"
#  include "tc_hal_thread.h"
#  include "tc_hal_time.h"
#  include "tc_iot_cloud_storage.h"

#  define CLOUD_STORAGE_DEMO_MAX_CHANNELS 4

// 录像事件持续时长范围（秒）
#  define EVENT_DURATION_MIN_SEC 5
#  define EVENT_DURATION_MAX_SEC 15

#  define EVENT_PIC_BUF_SIZE (150 * 1024)

static const uint32_t kRecordEventIds[] = {1, 2, 3, 4, 5, 6};
static const uint32_t kSnapEventIds[] = {100, 101};

typedef struct {
  tc_iot_cloud_storage_plan_type_t plan;
  bool active_continuous_recording;

  // 本通道的推流 sender（按需创建）
  av_sender_t *sender;

  // 当前活动录像事件 id；0 表示空闲
  volatile uint32_t active_event_id;

  // 事件图片预分配 buffer
  uint8_t *pic_buf;

  // 事件停止延时定时器
  ThreadParams stop_event_timer_thread;
  volatile bool stop_event_timer_running;
} cloud_storage_demo_channel_t;

typedef struct {
  bool inited;
  int channel_count;
  // 各通道 sender 使用的媒体源（Linux 仿真）
  const char *audio_file;
  const char *video_file;
  // 共享音频互斥锁（由 cloud_storage_demo_init 注入，转交给各通道 sender）
  HAL_Mutex *audio_mutex;
  cloud_storage_demo_channel_t channels[CLOUD_STORAGE_DEMO_MAX_CHANNELS];
} cloud_storage_demo_t;

static cloud_storage_demo_t s_demo;

static void _on_cs_init_result(tc_iot_error_e error_code, const char *error_message);
static void _on_cs_event_result(uint8_t channel_id,
                                const tc_iot_cloud_storage_event_result_t *result);
static void _on_cs_plan_type_changed(uint8_t channel_id,
                                     tc_iot_cloud_storage_plan_type_t plan_type);

static void *_start_channels(void *arg);
static void _stop_channels(void);

static void _trigger_recording_event(int channel_id, uint32_t event_id);
static void _trigger_snapshot_event(int channel_id, uint32_t event_id);

static void *_event_timer_thread(void *arg);

// ==================== 对外接口 ===============

int cloud_storage_demo_init(const char *audio_file, const char *video_file,
                            HAL_Mutex *audio_mutex) {
  if (s_demo.inited) {
    return 0;
  }
  s_demo.audio_file = audio_file;
  s_demo.video_file = video_file;
  s_demo.audio_mutex = audio_mutex;

#  ifdef BEKEN_PLATFORM
  s_demo.channel_count = 1;
#  else
  s_demo.channel_count = CLOUD_STORAGE_DEMO_MAX_CHANNELS;
#  endif

  memset(s_demo.channels, 0, sizeof(s_demo.channels));
  for (int i = 0; i < CLOUD_STORAGE_DEMO_MAX_CHANNELS; i++) {
    s_demo.channels[i].plan = TC_IOT_CLOUD_STORAGE_PLAN_TYPE_NONE;
  }

  // 为每个通道预分配事件图片 buffer
  for (int i = 0; i < s_demo.channel_count; i++) {
    s_demo.channels[i].pic_buf = (uint8_t *)HAL_Malloc(EVENT_PIC_BUF_SIZE);
    if (!s_demo.channels[i].pic_buf) {
      printf("[cloud_storage_demo] channel_%d pic_buf alloc failed\n", i);
      for (int j = 0; j < i; j++) {
        HAL_Free(s_demo.channels[j].pic_buf);
        s_demo.channels[j].pic_buf = NULL;
      }
      return -1;
    }
  }

  tc_iot_cloud_storage_callback_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.on_init_result = _on_cs_init_result;
  cb.on_event_result = _on_cs_event_result;
  cb.on_plan_type_changed = _on_cs_plan_type_changed;

  tc_iot_error_e rc = tc_iot_cloud_storage_init(&cb);
  if (rc != TC_IOT_ERR_SUCCESS) {
    printf("[cloud_storage_demo] tc_iot_cloud_storage_init failed: %d\n", rc);
    return (int)rc;
  }

  s_demo.inited = true;
  printf("[cloud_storage_demo] init success (channels=%d)\n", s_demo.channel_count);
  return 0;
}

void cloud_storage_demo_deinit(void) {
  if (!s_demo.inited) {
    return;
  }

  _stop_channels();
  tc_iot_cloud_storage_deinit();

  // 释放图片 buffer
  for (int i = 0; i < s_demo.channel_count; i++) {
    if (s_demo.channels[i].pic_buf) {
      HAL_Free(s_demo.channels[i].pic_buf);
      s_demo.channels[i].pic_buf = NULL;
    }
  }

  s_demo.inited = false;
  printf("[cloud_storage_demo] deinit done\n");
}

int cloud_storage_demo_trigger_event(int channel_id) {
  if (!s_demo.inited) {
    printf("[cloud_storage_demo] not initialized\n");
    return -1;
  }
  if (channel_id < 0 || channel_id >= s_demo.channel_count) {
    printf("[cloud_storage_demo] invalid channel_id=%d (max=%d)\n", channel_id,
           s_demo.channel_count - 1);
    return -1;
  }

  // 随机选择事件类型：80% 录像，20% 抓图
  bool do_recording = ((uint32_t)rand() % 10 < 8);
  if (do_recording) {
    cloud_storage_demo_channel_t *channel = &s_demo.channels[channel_id];
    if ((channel->plan != TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME) &&
        (channel->plan != TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT)) {
      printf("[cloud_storage_demo] channel_%d disabled (no plan)\n", channel_id);
      return -1;
    }

    if (channel->active_event_id != 0) {
      printf("[cloud_storage_demo] channel_%d busy (event=%" PRIu32 " in progress)\n", channel_id,
             channel->active_event_id);
      return -1;
    }
    uint32_t event_id =
        kRecordEventIds[rand() % (int)(sizeof(kRecordEventIds) / sizeof(kRecordEventIds[0]))];
    _trigger_recording_event(channel_id, event_id);
  } else {
    uint32_t event_id =
        kSnapEventIds[rand() % (int)(sizeof(kSnapEventIds) / sizeof(kSnapEventIds[0]))];
    _trigger_snapshot_event(channel_id, event_id);
  }

  return 0;
}

// ==================== 云存回调: _on_cs_init_result ====================

static void _on_cs_init_result(tc_iot_error_e error_code, const char *error_message) {
  printf("[cloud_storage_demo] on_init_result: code=%d msg=%s\n", error_code,
         error_message ? error_message : "");
  if (error_code != TC_IOT_ERR_SUCCESS) {
    return;
  }

  static ThreadParams tp;
  memset(&tp, 0, sizeof(tp));
  tp.thread_name = "cs_start";
  tp.thread_func = _start_channels;
  tp.stack_size = HAL_GetDefaultThreadStackSize();
  tp.priority = THREAD_PRIORITY_NORMAL;

  if (HAL_ThreadCreate(&tp) != 0) {
    printf("[cloud_storage_demo] failed to create _start_channels thread\n");
  }
}

static void _on_cs_event_result(uint8_t channel_id,
                                const tc_iot_cloud_storage_event_result_t *result) {
  if (!result) {
    return;
  }
  printf("[cloud_storage_demo] event_result channel_id=%u event_id=%" PRIu32
         " "
         "report=%d pic=%d\n",
         channel_id, result->event_id, result->event_report_result, result->picture_upload_result);
}

static void _on_cs_plan_type_changed(uint8_t channel_id,
                                     tc_iot_cloud_storage_plan_type_t plan_type) {
  if (channel_id >= CLOUD_STORAGE_DEMO_MAX_CHANNELS) {
    return;
  }
  s_demo.channels[channel_id].plan = plan_type;

  const char *name = "UNKNOWN";
  if (plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_NONE) {
    name = "NONE";
  } else if (plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME) {
    name = "FULL_TIME";
  } else if (plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT) {
    name = "EVENT";
  } else if (plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_IMAGE) {
    name = "IMAGE";
  }
  printf("[cloud_storage_demo] plan_type_changed channel_id=%u plan=%s\n", channel_id, name);

  cloud_storage_demo_channel_t *channel = &s_demo.channels[channel_id];
  if (plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME ||
      plan_type == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT) {
    if (!channel->sender) {
      av_config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.channel_id = channel_id;
      cfg.media_content = TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
      cfg.quality = TC_IOT_VIDEO_QUALITY_HD;
      cfg.audio_file_path = s_demo.audio_file;
      cfg.video_file_path = s_demo.video_file;
      av_sender_t *sender = av_sender_alloc(&cfg, s_demo.audio_mutex);
      if (sender && av_sender_start(sender) == 0) {
        channel->sender = sender;
      } else {
        printf("[cloud_storage_demo] channel_%u sender start failed\n", channel_id);
        if (sender) {
          av_sender_free(sender);
        }
      }
    }
  }
}

// ============ 通道启停: _start_channels / _stop_channels ==================

static void *_start_channels(void *arg) {
  (void)arg;
  printf("[cloud_storage_demo] === Cloud Storage Plan ===\n");
  for (int i = 0; i < s_demo.channel_count; i++) {
    tc_iot_cloud_storage_plan_type_t plan = tc_iot_cloud_storage_get_plan_type((uint8_t)i);
    s_demo.channels[i].plan = plan;
    const char *plan_name = "UNKNOWN";
    if (plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME) {
      plan_name = "FULL_TIME";
    } else if (plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT) {
      plan_name = "EVENT";
    } else if (plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_IMAGE) {
      plan_name = "IMAGE";
    } else if (plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_NONE) {
      plan_name = "NONE";
    }
    printf("channel_%d: %s\n", i, plan_name);
  }
  printf("[cloud_storage_demo] ===========================\n");

  for (int i = 0; i < s_demo.channel_count; i++) {
    if (s_demo.channels[i].plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME ||
        s_demo.channels[i].plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT) {
      // 每个通道创建独立 sender 推流。
      av_config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.channel_id = i;
      cfg.media_content = TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
      cfg.quality = TC_IOT_VIDEO_QUALITY_HD;
      cfg.audio_file_path = s_demo.audio_file;
      cfg.video_file_path = s_demo.video_file;

      av_sender_t *sender = av_sender_alloc(&cfg, s_demo.audio_mutex);
      if (!sender || av_sender_start(sender) != 0) {
        printf("[cloud_storage_demo] channel_%d sender start failed\n", i);
        if (sender) {
          av_sender_free(sender);
        }
        s_demo.channels[i].plan = TC_IOT_CLOUD_STORAGE_PLAN_TYPE_NONE;
        continue;
      }
      s_demo.channels[i].sender = sender;
    }

    if (s_demo.channels[i].plan == TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME) {
      tc_iot_error_e rc = tc_iot_cloud_storage_start_continuous_recording((uint8_t)i);
      printf("[cloud_storage_demo] channel_%d start_continuous_recording rc=%d\n", i, rc);
      if (rc == TC_IOT_ERR_SUCCESS) {
        s_demo.channels[i].active_continuous_recording = true;
      }
    }
  }
  return NULL;
}

static void _stop_channels(void) {
  for (int i = 0; i < s_demo.channel_count; i++) {
    if (s_demo.channels[i].stop_event_timer_running) {
      s_demo.channels[i].stop_event_timer_running = false;
      HAL_SleepMs(1500);
    }

    // 如果有活动事件，强制停止
    if (s_demo.channels[i].active_event_id != 0) {
      tc_iot_cloud_storage_stop_event_recording((uint8_t)i, s_demo.channels[i].active_event_id);
      s_demo.channels[i].active_event_id = 0;
    }

    if (s_demo.channels[i].active_continuous_recording) {
      tc_iot_cloud_storage_stop_continuous_recording((uint8_t)i);
      s_demo.channels[i].active_continuous_recording = false;
    }

    if (s_demo.channels[i].sender) {
      av_sender_free(s_demo.channels[i].sender);
      s_demo.channels[i].sender = NULL;
    }
  }
}

// ==================== 事件触发: _trigger_recording_event ====================

static void _trigger_recording_event(int channel_id, uint32_t event_id) {
  cloud_storage_demo_channel_t *channel = &s_demo.channels[channel_id];

  // 抓图
  uint32_t pic_len = 0;
  bool has_pic =
      (av_device_get_event_picture(event_id, channel->pic_buf, EVENT_PIC_BUF_SIZE, &pic_len) == 0);

  tc_iot_cloud_storage_event_params_t params;
  memset(&params, 0, sizeof(params));
  params.event_id = event_id;
  params.pre_record_seconds = 0;
  if (has_pic) {
    params.picture_data = channel->pic_buf;
    params.picture_size = pic_len;
  }
  params.extra_info = "{\"source\":\"cloud_storage_demo\",\"trigger\":\"manual\"}";

  tc_iot_error_e rc = tc_iot_cloud_storage_start_event_recording((uint8_t)channel_id, &params);
  printf("[cloud_storage_demo] channel_%d recording_start event_id=%" PRIu32 " pic=%" PRIu32
         " "
         "rc=%d\n",
         channel_id, event_id, pic_len, rc);
  if (rc != TC_IOT_ERR_SUCCESS) {
    return;
  }
  channel->active_event_id = event_id;

  // 启动延时停止线程
  uint32_t duration = EVENT_DURATION_MIN_SEC +
                      (uint32_t)(rand() % (EVENT_DURATION_MAX_SEC - EVENT_DURATION_MIN_SEC + 1));
  printf("[cloud_storage_demo] channel_%d event=%" PRIu32 " duration=%" PRIu32 "s\n", channel_id,
         event_id, duration);

  channel->stop_event_timer_running = true;

  // 低16位=channel_id，高16位=duration
  intptr_t arg = (intptr_t)((duration << 16) | (channel_id & 0xFFFF));

  ThreadParams *tp = &channel->stop_event_timer_thread;
  memset(tp, 0, sizeof(*tp));
  tp->thread_name = "cs_event_stop";
  tp->thread_func = _event_timer_thread;
  tp->user_arg = (void *)arg;
  tp->stack_size = HAL_GetDefaultThreadStackSize();
  tp->priority = THREAD_PRIORITY_NORMAL;

  if (HAL_ThreadCreate(tp) != 0) {
    channel->stop_event_timer_running = false;
    tc_iot_cloud_storage_stop_event_recording((uint8_t)channel_id, event_id);
    channel->active_event_id = 0;
  }
}

// ==================== 事件触发: _trigger_snapshot_event ====================

static void _trigger_snapshot_event(int channel_id, uint32_t event_id) {
  cloud_storage_demo_channel_t *channel = &s_demo.channels[channel_id];

  uint32_t pic_len = 0;
  bool has_pic =
      (av_device_get_event_picture(event_id, channel->pic_buf, EVENT_PIC_BUF_SIZE, &pic_len) == 0);

  tc_iot_cloud_storage_event_params_t params;
  memset(&params, 0, sizeof(params));
  params.event_id = event_id;
  if (has_pic) {
    params.picture_data = channel->pic_buf;
    params.picture_size = pic_len;
  }
  params.extra_info =
      "{\"source\":\"cloud_storage_demo\",\"trigger\":"
      "\"manual\",\"kind\":\"snapshot\"}";

  tc_iot_error_e rc = tc_iot_cloud_storage_report_event_snapshot((uint8_t)channel_id, &params);
  printf("[cloud_storage_demo] channel_%d snapshot event_id=%" PRIu32 " pic=%" PRIu32 " rc=%d\n",
         channel_id, event_id, pic_len, rc);
}

// ==================== 事件定时器线程: _event_timer_thread ====================

static void *_event_timer_thread(void *arg) {
  intptr_t val = (intptr_t)arg;
  int channel_id = (int)(val & 0xFFFF);
  uint32_t duration_sec = (uint32_t)((val >> 16) & 0xFFFF);

  for (uint32_t i = 0; i < duration_sec; i++) {
    if (!s_demo.channels[channel_id].stop_event_timer_running) {
      break;
    }
    HAL_SleepMs(1000);
  }

  // 停止录像事件
  uint32_t event_id = s_demo.channels[channel_id].active_event_id;
  if (event_id != 0 && s_demo.channels[channel_id].stop_event_timer_running) {
    tc_iot_error_e rc = tc_iot_cloud_storage_stop_event_recording((uint8_t)channel_id, event_id);
    printf("[cloud_storage_demo] channel_%d recording_stop event_id=%" PRIu32 " rc=%d\n",
           channel_id, event_id, rc);
    s_demo.channels[channel_id].active_event_id = 0;
  }

  s_demo.channels[channel_id].stop_event_timer_running = false;

  return NULL;
}

#else  // !BUILD_IOT_CLOUD_STORAGE

int cloud_storage_demo_init(const char *audio_file, const char *video_file,
                            HAL_Mutex *audio_mutex) {
  (void)audio_file;
  (void)video_file;
  (void)audio_mutex;
  return 0;
}

void cloud_storage_demo_deinit(void) {}

int cloud_storage_demo_trigger_event(int channel_id) {
  (void)channel_id;
  return -1;
}

#endif  // BUILD_IOT_CLOUD_STORAGE
