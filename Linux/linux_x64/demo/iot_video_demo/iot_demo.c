// Copyright (c) 2026 Tencent. All rights reserved.

// iot_demo：负责 tc_iot_init / tc_iot_login / tc_iot_av_init 与 av_sender
// 生命周期管理；以及跨平台一致的 observer 业务回调。登录的同步等待原语保留
// ifdef 区分（差异极小，不值得为它再拆一层抽象）。
//
// 平台特有的 main / cmd 入口（bk7258 ap_main.c、Linux main.c）在外层各自
// 实现，本文件只暴露 iot_demo_start / iot_demo_stop / iot_demo_call 等接口。

#include "iot_demo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av_device.h"
#include "av_receiver.h"
#include "av_sender.h"
#include "cloud_storage_demo.h"
#include "tc_hal_mutex.h"
#include "tc_iot.h"
#include "tc_iot_av.h"

#if defined(BEKEN_PLATFORM) || defined(OS_XR872)
#  include <os/os.h>
#else
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
#endif

// ==================== 单例状态 ====================

#define MAX_MONITOR_CHANNELS IOT_DEMO_MAX_MONITOR_CHANNELS

typedef struct {
  bool running;
  iot_demo_config_t config;

  // observer 在 tc_iot_av_init 时被 SDK 内部拷贝，理论上栈变量也够；
  // 这里仍然挂在 state 里，方便排查（gdb 一眼能看到当前注册的回调表）。
  tc_iot_av_observer_s observer;

  // 远端帧接收器：转调 av_device 播放 / dump。
  av_receiver_t *receiver;

  // 通话状态：VOIP 通话进行中时阻止监控拉流。
  bool in_call;

  // 当前通话的用户ID，用于区分不同用户的通话
  char current_call_user_id[64];

  // 当前通话的媒体选项（用于 accept 成功后的 sender 启动）
  tc_iot_call_option_t current_call_option;

  // 通话推流 sender
  av_sender_t *call_sender;

  // 监控推流 sender 列表，最多 8 路并发监控。
  av_sender_t *monitor_senders[MAX_MONITOR_CHANNELS];

  // 共享音频互斥锁：注入给所有 sender（及云存 sender），串行化采集回调与拆流销毁。
  // 生命周期与本 demo 一致（start 创建、stop 销毁），覆盖所有 sender 的存活期。
  HAL_Mutex *audio_mutex;
} iot_demo_state_t;

static iot_demo_state_t g_demo;

// ==================== 内部函数前向声明 ====================

// 通用 av 操作回调（call/accept/reject/hangup 共用）
static void _av_op_cb(tc_iot_error_e error_code, const char *error_message);
static void _av_op_cb_hangup(tc_iot_error_e error_code, const char *error_message);
static void _av_op_cb_accept(tc_iot_error_e error_code, const char *error_message);

// sender 生命周期辅助
static av_config_t _make_av_config(int channel_id, tc_iot_media_content_e media_content,
                                   tc_iot_video_quality_e quality, const char *video_file);
static av_sender_t *_start_monitor_sender(int channel_id, tc_iot_video_quality_e quality);
static void _stop_monitor_sender_by_channel(int channel_id);
static void _close_all_monitor_senders(void);
static void _close_call_sender(void);
static void _switch_to_call_sender(tc_iot_media_content_e media_content,
                                   tc_iot_video_quality_e quality);

// observer 回调（业务逻辑跨平台一致，统一放本文件）
static void _on_call_requested(const tc_iot_contact_s *contact, tc_iot_call_option_t option);
static void _on_call_accept(const tc_iot_contact_s *contact);
static void _on_call_reject(const tc_iot_contact_s *contact);
static void _on_call_timeout(const tc_iot_contact_s *contact);
static void _on_call_hangup(const tc_iot_contact_s *contact);
static void _on_monitor_begin(int channel_id, tc_iot_call_option_t option);
static void _on_monitor_end(int channel_id);
static void _on_monitor_switch(int channel_id, tc_iot_video_quality_e quality);
static void _on_ptz_command_received(int channel_id, ptz_command_e ptz_command, int speed);
static void _on_audio_frame_received(const tc_iot_audio_frame *frame);
static void _on_video_frame_received(const tc_iot_video_frame *frame);
static void _setup_observer(tc_iot_av_observer_s *observer);

// login 同步等待
static void _on_login(tc_iot_error_e error_code, const char *error_message);
static bool _wait_login_result(int32_t timeout_ms);
static void _login_result_init(void);
static void _login_result_destroy(void);

// 工具函数
static const char *_safe_str(const char *user_id);
static size_t _copy_str_safe(char *dest, size_t dest_size, const char *src);
static void _clear_current_call_user_id(void);

static void _on_demo_log(tc_iot_log_level_e level, const char *log) {
  (void)level;
  if (log) {
    printf("%s", log);
  }
}

static void _set_expected_av_params(void) {
#if defined(OS_XR872)
  tc_iot_call_experiment_api("set_expected_av_params",
                             "{\"expected_audio_codec\":\"G722\","
                             "\"expected_video_codec\":\"MJPEG\","
                             "\"expected_video_rotation\":0,"
                             "\"max_fps\":5,"
                             "\"max_video_width\":240,"
                             "\"max_video_height\":320}",
                             NULL);
#else
  (void)0; /* 其他平台暂无需设置 */
#endif
}

// ==================== 公开接口 ====================
int iot_demo_start(const iot_demo_config_t *cfg) {
  if (!cfg || !cfg->product_id || !cfg->device_id || !cfg->device_secret) {
    return -1;
  }
  if (g_demo.running) {
    return 0;
  }

  g_demo.config = *cfg;

  printf("[demo] starting: product=%s device=%s\n", cfg->product_id, cfg->device_id);

  // 1) tc_iot_init
  tc_iot_config_s tc_cfg;
  memset(&tc_cfg, 0, sizeof(tc_cfg));
  tc_cfg.storage_path = "./";
  // tc_cfg.log_level = TC_IOT_LOG_LEVEL_DEBUG;
  // tc_cfg.on_log = _on_demo_log;
  tc_cfg.on_device_event = NULL;

  int rc = tc_iot_init(&tc_cfg);
  if (rc != TC_IOT_ERR_SUCCESS) {
    printf("[demo] tc_iot_init failed: %d\n", rc);
    return -1;
  }

  _set_expected_av_params();

  // 2) tc_iot_login
  tc_iot_device_info_s dev;
  dev.product_id = cfg->product_id;
  dev.device_id = cfg->device_id;
  dev.device_secret = cfg->device_secret;
  dev.region = cfg->region ? cfg->region : "ap-guangzhou";

  _login_result_init();

  rc = tc_iot_login(&dev, _on_login);
  if (rc != TC_IOT_ERR_SUCCESS) {
    printf("[demo] tc_iot_login failed: %d\n", rc);
    tc_iot_deinit();
    return -1;
  }
#ifdef BEKEN_PLATFORM
  const int32_t kLoginTimeoutMs = 30000;
#else
  const int32_t kLoginTimeoutMs = 5000;
#endif
  if (!_wait_login_result(kLoginTimeoutMs)) {
    printf("[demo] tc_iot_login timeout\n");
    tc_iot_deinit();
    return -1;
  }

  // 3) tc_iot_av_init —— observer 注册（SDK 内部会拷贝，临时变量即可）
  memset(&g_demo.observer, 0, sizeof(g_demo.observer));
  _setup_observer(&g_demo.observer);

  rc = tc_iot_av_init(&g_demo.observer);
  if (rc != TC_IOT_ERR_SUCCESS) {
    printf("[demo] tc_iot_av_init failed: %d\n", rc);
    tc_iot_deinit();
    return -1;
  }

  // 4) av_device 初始化（dump 文件） + av_receiver（远端帧播放转调）
  av_device_config_t dev_cfg;
  memset(&dev_cfg, 0, sizeof(dev_cfg));
  dev_cfg.dump_audio_path = cfg->dump_audio_path;
  dev_cfg.dump_video_path = cfg->dump_video_path;
  dev_cfg.mp4_file_path = cfg->mp4_file_path;
  av_device_init(&dev_cfg);

  // 共享音频互斥锁：必须在任何推流 sender 创建之前创建，注入给所有 sender。
  g_demo.audio_mutex = HAL_MutexCreate();
  if (!g_demo.audio_mutex) {
    printf("[demo] audio_mutex create failed\n");
    av_device_deinit();
    tc_iot_av_deinit();
    tc_iot_deinit();
    return -1;
  }

  g_demo.receiver = av_receiver_alloc();
  if (!g_demo.receiver) {
    printf("[demo] av_receiver_alloc failed\n");
    HAL_MutexDestroy(g_demo.audio_mutex);
    g_demo.audio_mutex = NULL;
    av_device_deinit();
    tc_iot_av_deinit();
    tc_iot_deinit();
    return -1;
  }

  g_demo.running = true;

  cloud_storage_demo_init(cfg->audio_file_path, cfg->video_file_path, g_demo.audio_mutex);

  printf("[demo] started\n");
  return 0;
}

void iot_demo_stop(void) {
  if (!g_demo.running) {
    return;
  }
  g_demo.running = false;
  g_demo.in_call = false;
  _clear_current_call_user_id();
  memset(&g_demo.current_call_option, 0, sizeof(g_demo.current_call_option));

  cloud_storage_demo_deinit();

  // 关闭所有推流 sender
  _close_all_monitor_senders();
  _close_call_sender();

  // 关闭远端帧接收器与设备
  if (g_demo.receiver) {
    av_receiver_free(g_demo.receiver);
    g_demo.receiver = NULL;
  }
  av_device_deinit();

  // 所有 sender 已停、采集已停，销毁共享音频互斥锁。
  if (g_demo.audio_mutex) {
    HAL_MutexDestroy(g_demo.audio_mutex);
    g_demo.audio_mutex = NULL;
  }

  tc_iot_av_deinit();
  tc_iot_deinit();

  _login_result_destroy();

  printf("[demo] stopped\n");
}

// ==================== 通话操作 ====================

int iot_demo_call(const char *user_id, tc_iot_media_content_e media_content,
                  tc_iot_video_quality_e video_quality) {
  if (!user_id || user_id[0] == '\0') {
    printf("[demo] call: user_id required\n");
    return -1;
  }

  tc_iot_contact_s contact = {0};
  contact.user_id = user_id;
  contact.user_name = "";
  contact.avatar_url = "";

  tc_iot_call_option_t option = {0};
  option.media_content = media_content;
  option.video_quality = video_quality;

  printf("[demo] call user_id=%s media=%d quality=%d\n", user_id, media_content, video_quality);

  // 主叫冷启动：先记选项并停监控，等 on_call_accepted（对端接听）再开编码。
  g_demo.current_call_option = option;
  g_demo.in_call = true;
  _copy_str_safe(g_demo.current_call_user_id, sizeof(g_demo.current_call_user_id), user_id);
  printf("[demo] call initiated, blocking monitor streams; encoder deferred to on_call_accepted\n");

  _close_all_monitor_senders();
  _close_call_sender();

  tc_iot_error_e rc = tc_iot_av_call(&contact, option, _av_op_cb);
  if (rc != TC_IOT_ERR_SUCCESS) {
    printf("[demo] tc_iot_av_call failed: %d\n", rc);
    g_demo.in_call = false;
    _clear_current_call_user_id();
    memset(&g_demo.current_call_option, 0, sizeof(g_demo.current_call_option));
    _close_call_sender();
    return (int)rc;
  }
  return 0;
}

int iot_demo_accept(const char *user_id, tc_iot_media_content_e media_content,
                    tc_iot_video_quality_e video_quality) {
  if (!user_id || user_id[0] == '\0') {
    return -1;
  }

  tc_iot_contact_s contact = {0};
  contact.user_id = user_id;

  tc_iot_call_option_t option = {0};
  option.media_content = media_content;
  option.video_quality = video_quality;
  g_demo.current_call_option = option;
  printf("[demo] accept user_id=%s media=%d quality=%d\n", user_id, media_content, video_quality);

  tc_iot_error_e rc = tc_iot_av_accept(&contact, _av_op_cb_accept);
  printf("[demo] tc_iot_av_accept user_id=%s rc=%d\n", user_id, rc);
  if (rc != TC_IOT_ERR_SUCCESS) {
    memset(&g_demo.current_call_option, 0, sizeof(g_demo.current_call_option));
  }

  return (int)rc;
}

int iot_demo_reject(const char *user_id) {
  if (!user_id || user_id[0] == '\0') {
    return -1;
  }
  tc_iot_contact_s contact = {0};
  contact.user_id = user_id;
  tc_iot_av_reject(&contact, _av_op_cb);
  printf("[demo] tc_iot_av_reject user_id=%s\n", user_id);
  return 0;
}

int iot_demo_hangup(const char *user_id) {
  if (!user_id || user_id[0] == '\0') {
    return -1;
  }
  tc_iot_contact_s contact = {0};
  contact.user_id = user_id;
  tc_iot_error_e rc = tc_iot_av_hangup(&contact, _av_op_cb_hangup);
  printf("[demo] tc_iot_av_hangup user_id=%s rc=%d\n", user_id, rc);
  return (int)rc;
}

uint64_t iot_demo_seek_mp4_second(uint64_t target_second) {
  uint64_t target_ms = target_second * 1000;
  uint64_t actual_ms = av_device_seek_mp4_ms(target_ms);
  if (actual_ms == 0) {
    printf("[demo] seek failed: target_second=%llu (target_ms=%llu)\n",
           (unsigned long long)target_second, (unsigned long long)target_ms);
    return 0;
  }
  printf("[demo] seek done: target_second=%llu (target_ms=%llu) actual_ms=%llu\n",
         (unsigned long long)target_second, (unsigned long long)target_ms,
         (unsigned long long)actual_ms);
  return actual_ms;
}

static void _av_op_cb(tc_iot_error_e error_code, const char *error_message) {
  printf("[demo] av op: code=%d msg=%s\n", error_code, error_message ? error_message : "");
}

static void _av_op_cb_hangup(tc_iot_error_e error_code, const char *error_message) {
  printf("[demo] av op hangup: code=%d msg=%s\n", error_code, error_message ? error_message : "");
  g_demo.in_call = false;
  _clear_current_call_user_id();
  _close_call_sender();
}

static void _av_op_cb_accept(tc_iot_error_e error_code, const char *error_message) {
  if (error_code == TC_IOT_ERR_SUCCESS) {
    printf("[demo] accept succeeded, starting call sender with media=%d quality=%d\n",
           g_demo.current_call_option.media_content, g_demo.current_call_option.video_quality);
    g_demo.in_call = true;
    _switch_to_call_sender(g_demo.current_call_option.media_content,
                           g_demo.current_call_option.video_quality);
  } else {
    printf("[demo] accept failed, error_code=%d, error_message=%s\n", (int)error_code,
           error_message ? error_message : "");
    memset(&g_demo.current_call_option, 0, sizeof(g_demo.current_call_option));
    if (!g_demo.in_call) {
      _clear_current_call_user_id();
    }
  }
}

// ==================== login 同步等待 ====================

#if defined(BEKEN_PLATFORM) || defined(PLATFORM_FREERTOS)
#  define LOGIN_WAIT_SLEEP_MS() rtos_delay_milliseconds(100)
#elif defined(OS_XR872)
#  define LOGIN_WAIT_SLEEP_MS() OS_MSleep(100)
#endif

#if defined(BEKEN_PLATFORM) || defined(PLATFORM_FREERTOS) || defined(OS_XR872)
typedef struct {
  volatile bool called;
  tc_iot_error_e error_code;
} login_result_t;

static login_result_t s_login_result;

static void _on_login(tc_iot_error_e error_code, const char *error_message) {
  (void)error_message;
  s_login_result.error_code = error_code;
  s_login_result.called = true;
}

static bool _wait_login_result(int32_t timeout_ms) {
  int32_t elapsed = 0;
  while (!s_login_result.called) {
    if (elapsed >= timeout_ms) {
      return false;
    }
    LOGIN_WAIT_SLEEP_MS();
    elapsed += 100;
  }
  return true;
}

static void _login_result_init(void) {
  memset(&s_login_result, 0, sizeof(s_login_result));
}

static void _login_result_destroy(void) {}
#else
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool called;
  tc_iot_error_e error_code;
} login_result_t;

static login_result_t s_login_result = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                                        .cond = PTHREAD_COND_INITIALIZER};

static void _on_login(tc_iot_error_e error_code, const char *error_message) {
  (void)error_message;
  pthread_mutex_lock(&s_login_result.mutex);
  s_login_result.error_code = error_code;
  s_login_result.called = true;
  pthread_cond_signal(&s_login_result.cond);
  pthread_mutex_unlock(&s_login_result.mutex);
}

static bool _wait_login_result(int32_t timeout_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&s_login_result.mutex);
  while (!s_login_result.called) {
    int rc = pthread_cond_timedwait(&s_login_result.cond, &s_login_result.mutex, &ts);
    if (rc == ETIMEDOUT) {
      pthread_mutex_unlock(&s_login_result.mutex);
      return false;
    }
  }
  pthread_mutex_unlock(&s_login_result.mutex);
  return true;
}

static void _login_result_init(void) {
  s_login_result.called = false;
  s_login_result.error_code = TC_IOT_ERR_SUCCESS;
  pthread_mutex_init(&s_login_result.mutex, NULL);
  pthread_cond_init(&s_login_result.cond, NULL);
}

static void _login_result_destroy(void) {
  pthread_mutex_destroy(&s_login_result.mutex);
  pthread_cond_destroy(&s_login_result.cond);
}
#endif

// ==================== sender 生命周期辅助 ====================

static av_config_t _make_av_config(int channel_id, tc_iot_media_content_e media_content,
                                   tc_iot_video_quality_e quality, const char *video_file) {
  av_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.channel_id = channel_id;
  cfg.media_content = media_content;
  cfg.quality = quality;
  cfg.audio_file_path = g_demo.config.audio_file_path;
  cfg.video_file_path = video_file ? video_file : g_demo.config.video_file_path;
  return cfg;
}

// 创建并启动一路 monitor sender，存入列表空槽；失败返回 NULL。
static av_sender_t *_start_monitor_sender(int channel_id, tc_iot_video_quality_e quality) {
  int slot = -1;
  bool has_active_sender = false;
  for (int i = 0; i < MAX_MONITOR_CHANNELS; i++) {
    if (g_demo.monitor_senders[i]) {
      has_active_sender = true;
      if (av_sender_get_channel_id(g_demo.monitor_senders[i]) == channel_id) {
        printf("[demo] monitor ch=%d already running\n", channel_id);
        return g_demo.monitor_senders[i];
      }
    }
    if (slot < 0 && !g_demo.monitor_senders[i]) {
      slot = i;
    }
  }
  if (slot < 0) {
    printf("[demo] monitor ch=%d blocked: no free slot (max=%d)\n", channel_id,
           MAX_MONITOR_CHANNELS);
    return NULL;
  }

  // 音频是全局共享单一采集源：只让首路监控通道推音频，后续并发通道只推视频，
  // 避免多路重复推送同一路音频导致对端音频叠加/重复。
  tc_iot_media_content_e media_content =
      has_active_sender ? TC_IOT_MEDIA_CONTENT_VIDEO : TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;

  // 所有 monitor 通道共用默认视频文件。
  av_config_t cfg = _make_av_config(channel_id, media_content, quality, NULL);

  av_sender_t *sender = av_sender_alloc(&cfg, g_demo.audio_mutex);
  if (!sender) {
    printf("[demo] monitor ch=%d av_sender_alloc failed\n", channel_id);
    return NULL;
  }
  if (av_sender_start(sender) != 0) {
    printf("[demo] monitor ch=%d av_sender_start failed\n", channel_id);
    av_sender_free(sender);
    return NULL;
  }
  g_demo.monitor_senders[slot] = sender;
  printf("[demo] monitor ch=%d started (slot=%d, audio=%d)\n", channel_id, slot,
         media_content == TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO);
  return sender;
}

// ==================== 推流管理 ====================

static void _stop_monitor_sender_by_channel(int channel_id) {
  for (int i = 0; i < MAX_MONITOR_CHANNELS; i++) {
    if (g_demo.monitor_senders[i] &&
        av_sender_get_channel_id(g_demo.monitor_senders[i]) == channel_id) {
      av_sender_free(g_demo.monitor_senders[i]);
      g_demo.monitor_senders[i] = NULL;
      printf("[demo] monitor ch=%d stopped (slot=%d)\n", channel_id, i);
      return;
    }
  }
  printf("[demo] monitor ch=%d not found\n", channel_id);
}

static void _close_all_monitor_senders(void) {
  for (int i = 0; i < MAX_MONITOR_CHANNELS; i++) {
    if (g_demo.monitor_senders[i]) {
      av_sender_free(g_demo.monitor_senders[i]);
      g_demo.monitor_senders[i] = NULL;
    }
  }
}

static void _close_call_sender(void) {
  if (g_demo.call_sender) {
    av_sender_free(g_demo.call_sender);
    g_demo.call_sender = NULL;
  }
}

// 关闭所有 monitor sender 与旧 call_sender，启动新的 call_sender（channel_id=0）。
static void _switch_to_call_sender(tc_iot_media_content_e media_content,
                                   tc_iot_video_quality_e quality) {
  _close_all_monitor_senders();
  _close_call_sender();

  av_config_t cfg = _make_av_config(0, media_content, quality, g_demo.config.video_file_path);
  g_demo.call_sender = av_sender_alloc(&cfg, g_demo.audio_mutex);
  if (!g_demo.call_sender) {
    printf("[demo] call_sender av_sender_alloc failed\n");
    return;
  }
  if (av_sender_start(g_demo.call_sender) != 0) {
    printf("[demo] call_sender av_sender_start failed\n");
    av_sender_free(g_demo.call_sender);
    g_demo.call_sender = NULL;
  }
}

// ==================== observer 业务回调 ====================
//
// 收到对端事件后控制本地 sender 的采集启停，并把远端音视频帧投递到 av_device
// 进行播放 / dump。
//
// 多通道监控推流：每个 on_monitor_begin 使用不同的 channel_id 进行独立推流，
// av_sender 内部维护最多 8 路的 channel 数组，采集端共用，同一帧数据推送到
// 所有活跃的视频通道。通话进行中（in_call=true）则拒绝所有监控拉流请求。

static void _on_call_requested(const tc_iot_contact_s *contact, tc_iot_call_option_t option) {
  printf("[demo] on_call_requested: user_id=%s media=%d quality=%d\n", _safe_str(contact->user_id),
         option.media_content, option.video_quality);

  g_demo.current_call_option = option;

  // 记录请求通话的用户ID（如果当前没有通话）
  if (!g_demo.in_call) {
    _copy_str_safe(g_demo.current_call_user_id, sizeof(g_demo.current_call_user_id),
                   contact->user_id);
  }

  // 测试逻辑：奇数次接受、偶数次拒绝，方便联调时同时验证两条路径。
  static int flag = 0;
  if (flag++ % 2 == 0) {
    tc_iot_error_e rc = tc_iot_av_accept(contact, _av_op_cb_accept);
    if (rc == TC_IOT_ERR_SUCCESS) {
      printf("[demo] tc_iot_av_accept called successfully, waiting for callback\n");
    } else {
      printf("[demo] tc_iot_av_accept failed immediately: %d\n", rc);
      memset(&g_demo.current_call_option, 0, sizeof(g_demo.current_call_option));
    }
  } else {
    tc_iot_av_reject(contact, _av_op_cb);
  }
}

static void _on_call_accept(const tc_iot_contact_s *contact) {
  printf("[demo] on_call_accept: user_id=%s\n", _safe_str(contact->user_id));
  g_demo.in_call = true;
  _copy_str_safe(g_demo.current_call_user_id, sizeof(g_demo.current_call_user_id),
                 contact->user_id);

  // 主叫：对端接听后再开编码。被叫若已在 accept.cb 启过 call_sender 则跳过。
  if (g_demo.call_sender) {
    printf("[demo] on_call_accept: call_sender already running, skip\n");
    return;
  }
  printf("[demo] on_call_accept: starting call sender media=%d quality=%d\n",
         g_demo.current_call_option.media_content, g_demo.current_call_option.video_quality);
  _switch_to_call_sender(g_demo.current_call_option.media_content,
                         g_demo.current_call_option.video_quality);
}

static void _on_call_reject(const tc_iot_contact_s *contact) {
  printf("[demo] on_call_reject: user_id=%s\n", _safe_str(contact->user_id));
  g_demo.in_call = false;
  _clear_current_call_user_id();
  _close_call_sender();
}

static void _on_call_timeout(const tc_iot_contact_s *contact) {
  printf("[demo] on_call_timeout: user_id=%s\n", _safe_str(contact->user_id));
  g_demo.in_call = false;
  _clear_current_call_user_id();
  _close_call_sender();
}

static void _on_call_hangup(const tc_iot_contact_s *contact) {
  printf("[demo] on_call_hangup: user_id=%s\n", _safe_str(contact->user_id));

  // 要挂断的用户，是当前通话的用户时，才停止推流
  if (contact->user_id && g_demo.current_call_user_id[0] != '\0' &&
      strcmp(contact->user_id, g_demo.current_call_user_id) == 0) {
    printf("[demo] current call user %s hung up, stopping call\n", _safe_str(contact->user_id));
    g_demo.in_call = false;
    _clear_current_call_user_id();
    _close_call_sender();
  } else {
    printf("[demo] ignore hangup from user %s (current call user is %s)\n",
           _safe_str(contact->user_id), g_demo.current_call_user_id);
  }
}

static void _on_monitor_begin(int channel_id, tc_iot_call_option_t option) {
  printf("[demo] _on_monitor_begin ch=%d media=%d quality=%d\n", channel_id, option.media_content,
         option.video_quality);

  // 通话进行中，阻止监控拉流。
  if (g_demo.in_call) {
    printf("[demo] monitor ch=%d blocked: in_call=%d\n", channel_id, g_demo.in_call);
    return;
  }

  _start_monitor_sender(channel_id, option.video_quality);
}

static void _on_monitor_end(int channel_id) {
  printf("[demo] _on_monitor_end ch=%d\n", channel_id);
  _stop_monitor_sender_by_channel(channel_id);
}

static void _on_monitor_switch(int channel_id, tc_iot_video_quality_e quality) {
  printf("[demo] on_monitor_switch ch=%d quality=%d\n", channel_id, quality);

  // 关闭旧 sender，按新质量创建新 sender。
  _stop_monitor_sender_by_channel(channel_id);
  _start_monitor_sender(channel_id, quality);
}

static void _on_ptz_command_received(int channel_id, ptz_command_e ptz_command, int speed) {
  printf("[demo] on_ptz_command ch=%d cmd=%d speed=%d\n", channel_id, ptz_command, speed);
}

static void _on_audio_frame_received(const tc_iot_audio_frame *frame) {
  av_receiver_on_audio_frame(g_demo.receiver, frame);
}

static void _on_video_frame_received(const tc_iot_video_frame *frame) {
  av_receiver_on_video_frame(g_demo.receiver, frame);
}

static void _setup_observer(tc_iot_av_observer_s *observer) {
  observer->on_call_requested = _on_call_requested;
  observer->on_call_accepted = _on_call_accept;
  observer->on_call_rejected = _on_call_reject;
  observer->on_call_timeout = _on_call_timeout;
  observer->on_call_hangup = _on_call_hangup;
  observer->on_monitor_begin = _on_monitor_begin;
  observer->on_monitor_end = _on_monitor_end;
  observer->on_audio_frame_received = _on_audio_frame_received;
  observer->on_video_frame_received = _on_video_frame_received;
  observer->on_monitor_switch = _on_monitor_switch;
  observer->on_ptz_command_received = _on_ptz_command_received;
}

// ==================== 工具函数 ====================

static const char *_safe_str(const char *user_id) {
  return user_id ? user_id : "NULL";
}

static size_t _copy_str_safe(char *dest, size_t dest_size, const char *src) {
  if (!src || dest_size == 0) {
    if (dest && dest_size > 0) {
      dest[0] = '\0';
    }
    return 0;
  }

  size_t copy_len = dest_size - 1;
  strncpy(dest, src, copy_len);
  dest[copy_len] = '\0';

  // 返回实际复制的字符数（不包括结尾的'\0'）
  size_t src_len = strlen(src);
  return src_len < copy_len ? src_len : copy_len;
}

static void _clear_current_call_user_id(void) {
  memset(g_demo.current_call_user_id, 0, sizeof(g_demo.current_call_user_id));
}
