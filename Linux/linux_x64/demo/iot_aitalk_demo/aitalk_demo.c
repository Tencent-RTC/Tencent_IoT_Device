// Copyright (c) 2026 Tencent. All rights reserved.

// aitalk_demo: platform-independent AITalk lifecycle management.
// Handles tc_iot_init / tc_iot_login / tc_iot_aitalk_init / start_speak
// and wires MIC capture → tc_iot_aitalk_send_audio, bot audio → SPK playout.
//
// Platform-specific entry (BK7258 ap_main.c / Linux main.c) calls
// aitalk_demo_start() / aitalk_demo_stop().

#include "aitalk_demo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "audio_device.h"
#include "tc_iot.h"
#include "tc_iot_aitalk.h"

// ==================== 日志宏 ====================

#ifndef AITALK_LOG
#  define AITALK_LOG(fmt, ...) printf("[aitalk] " fmt "\n", ##__VA_ARGS__)
#endif

// ==================== 单例状态 ====================

typedef struct {
  bool running;
  aitalk_demo_config_t config;
  bool sdk_inited;
  bool aitalk_inited;
  bool device_opened;
  volatile bool error_signaled;
} aitalk_demo_state_t;

static aitalk_demo_state_t g_state;

typedef struct {
  volatile bool called;
  tc_iot_error_e error_code;
} async_result_t;

static async_result_t s_login_result;
static async_result_t s_start_speak_result;

// ==================== 前向声明 ====================

static void _login_result_init(void);
static void _on_login(tc_iot_error_e error_code, const char *error_message);
static bool _wait_login_result(int32_t timeout_ms);

static void _on_recv_bot_audio(const tc_iot_audio_frame *frame, void *user_data);
static void _on_recv_bot_text(const char *text, void *user_data);
static void _on_recv_asr_text(const char *text, void *user_data);
static void _on_launch_call(const tc_iot_contact_s *contact, void *user_data);
static void _on_bot_state_changed(tc_iot_aitalk_bot_state_e old_state,
                                  tc_iot_aitalk_bot_state_e new_state, void *user_data);
static void _on_aitalk_error(tc_iot_error_e error_code, const char *error_msg, void *user_data);
static void _on_register_contacts(tc_iot_error_e error_code, const char *error_message);

static void _start_speak_result_init(void);
static void _on_start_speak(tc_iot_error_e error_code, const char *error_message);
static bool _wait_start_speak_result(int32_t timeout_ms);

static void _on_mic_captured(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec,
                             uint64_t pts_ms, void *user_data);

// ==================== 公开接口 ====================

int aitalk_demo_start(const aitalk_demo_config_t *cfg) {
  if (!cfg || !cfg->product_id || !cfg->device_id || !cfg->device_secret) {
    return -1;
  }
  if (g_state.running) {
    AITALK_LOG("already running");
    return 0;
  }

  g_state.config = *cfg;
  g_state.error_signaled = false;

  AITALK_LOG("starting: product=%s device=%s bot=%s", cfg->product_id, cfg->device_id,
             cfg->bot_id ? cfg->bot_id : "");

  // 1) tc_iot_init
  tc_iot_config_s tc_cfg;
  memset(&tc_cfg, 0, sizeof(tc_cfg));
  tc_cfg.storage_path = "./";
  tc_cfg.log_level = TC_IOT_LOG_LEVEL_DEBUG;

  int rc = tc_iot_init(&tc_cfg);
  if (rc != TC_IOT_ERR_SUCCESS) {
    AITALK_LOG("tc_iot_init failed: %d", rc);
    return -1;
  }
  g_state.sdk_inited = true;

  // 2) tc_iot_login
  tc_iot_device_info_s dev;
  memset(&dev, 0, sizeof(dev));
  dev.product_id = cfg->product_id;
  dev.device_id = cfg->device_id;
  dev.device_secret = cfg->device_secret;
  dev.region = cfg->region ? cfg->region : "ap-guangzhou";

  _login_result_init();

  rc = tc_iot_login(&dev, _on_login);
  if (rc != TC_IOT_ERR_SUCCESS) {
    AITALK_LOG("tc_iot_login failed: %d", rc);
    goto fail;
  }

  const int32_t kLoginTimeoutMs = 30000;
  if (!_wait_login_result(kLoginTimeoutMs)) {
    AITALK_LOG("login timeout");
    goto fail;
  }

  // 3) tc_iot_aitalk_init
  {
    tc_iot_aitalk_observer_s observer;
    memset(&observer, 0, sizeof(observer));
    observer.on_receive_bot_audio = _on_recv_bot_audio;
    observer.on_receive_bot_text = _on_recv_bot_text;
    observer.on_receive_asr_text = _on_recv_asr_text;
    observer.on_launch_call = _on_launch_call;
    observer.on_bot_state_changed = _on_bot_state_changed;
    observer.on_error = _on_aitalk_error;

    tc_iot_aitalk_init_params_s init_params;
    memset(&init_params, 0, sizeof(init_params));
    if (cfg->bot_id && cfg->bot_id[0] != '\0') {
      strncpy(init_params.bot_id, cfg->bot_id, sizeof(init_params.bot_id) - 1);
    }
    init_params.audio_option.codec = audio_device_default_codec();
    init_params.audio_option.sample_rate = TC_IOT_AUDIO_SAMPLE_RATE_16000;
    init_params.audio_option.frame_duration_ms =
        audio_device_frame_duration_ms(init_params.audio_option.codec);
    init_params.prompt_variables_json = cfg->prompt_variables_json;

    rc = tc_iot_aitalk_init(&init_params, &observer, NULL);
    if (rc != TC_IOT_ERR_SUCCESS) {
      AITALK_LOG("tc_iot_aitalk_init failed: %d", rc);
      goto fail;
    }
    g_state.aitalk_inited = true;
  }

  // 3.5) register contacts (best-effort, non-blocking)
  if (cfg->contacts && cfg->contact_count > 0) {
    rc = tc_iot_aitalk_register_contacts(cfg->contacts, cfg->contact_count, _on_register_contacts);
    if (rc != TC_IOT_ERR_SUCCESS) {
      AITALK_LOG("register_contacts call failed: %d", rc);
    }
  }

  // 4) audio_device_open
  {
    audio_device_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.sample_rate = 16000;
    dev_cfg.channels = 1;
    dev_cfg.bits_per_sample = 16;
    dev_cfg.frame_duration_ms = audio_device_frame_duration_ms(audio_device_default_codec());
    dev_cfg.send_codec = audio_device_default_codec();
    dev_cfg.recv_codec = audio_device_default_codec();

    if (audio_device_open(&dev_cfg) != 0) {
      AITALK_LOG("audio_device_open failed");
      goto fail;
    }
    g_state.device_opened = true;
  }

  // 5) start_speak
  _start_speak_result_init();
  rc = tc_iot_aitalk_start_speak(TC_IOT_AITALK_MODE_CONTINUOUS, _on_start_speak);
  if (rc != TC_IOT_ERR_SUCCESS) {
    AITALK_LOG("start_speak call failed: %d", rc);
    goto fail;
  }
  const int32_t kStartSpeakTimeoutMs = 30000;
  if (!_wait_start_speak_result(kStartSpeakTimeoutMs)) {
    AITALK_LOG("start_speak timeout");
    goto fail;
  }

  // 6) start capture
  audio_device_start_capture(_on_mic_captured, NULL);

  g_state.running = true;
  AITALK_LOG("started");
  return 0;

fail:
  if (g_state.device_opened) {
    audio_device_close();
    g_state.device_opened = false;
  }
  if (g_state.aitalk_inited) {
    tc_iot_aitalk_deinit();
    g_state.aitalk_inited = false;
  }
  if (g_state.sdk_inited) {
    tc_iot_deinit();
    g_state.sdk_inited = false;
  }
  return -1;
}

// ==================== 登录回调链 ====================

static void _login_result_init(void) {
  s_login_result.called = false;
  s_login_result.error_code = TC_IOT_ERR_SUCCESS;
}

static void _on_login(tc_iot_error_e error_code, const char *error_message) {
  AITALK_LOG("login cb: code=%d msg=%s", error_code, error_message ? error_message : "");
  s_login_result.error_code = error_code;
  s_login_result.called = true;
}

static bool _wait_login_result(int32_t timeout_ms) {
  for (int32_t elapsed = 0; !s_login_result.called && elapsed < timeout_ms; elapsed += 100) {
    audio_device_sleep_ms(100);
  }
  return s_login_result.called && s_login_result.error_code == TC_IOT_ERR_SUCCESS;
}

// ==================== Observer 回调 ====================

static void _on_recv_bot_audio(const tc_iot_audio_frame *frame, void *user_data) {
  (void)user_data;
  if (!frame || !frame->data || frame->data_size == 0) {
    return;
  }
  audio_device_write(frame->data, (uint32_t)frame->data_size, frame->codec);
}

static void _on_recv_bot_text(const char *text, void *user_data) {
  (void)user_data;
  AITALK_LOG("bot: %s", text ? text : "");
}

static void _on_recv_asr_text(const char *text, void *user_data) {
  (void)user_data;
  AITALK_LOG("asr: %s", text ? text : "");
}

static void _on_launch_call(const tc_iot_contact_s *contact, void *user_data) {
  (void)user_data;
  if (!contact) {
    return;
  }
  AITALK_LOG("launch_call: user_id=%s name=%s", contact->user_id ? contact->user_id : "",
             contact->user_name ? contact->user_name : "");
}

static void _on_bot_state_changed(tc_iot_aitalk_bot_state_e old_state,
                                  tc_iot_aitalk_bot_state_e new_state, void *user_data) {
  (void)user_data;
  AITALK_LOG("bot_state changed: %d -> %d", old_state, new_state);
}

static void _on_aitalk_error(tc_iot_error_e error_code, const char *error_msg, void *user_data) {
  (void)user_data;
  AITALK_LOG("error: code=%d msg=%s", error_code, error_msg ? error_msg : "");
  g_state.error_signaled = true;
}

static void _on_register_contacts(tc_iot_error_e error_code, const char *error_message) {
  AITALK_LOG("register_contacts cb: code=%d msg=%s", error_code,
             error_message ? error_message : "");
}

// ==================== start_speak 回调链 ====================

static void _start_speak_result_init(void) {
  s_start_speak_result.called = false;
  s_start_speak_result.error_code = TC_IOT_ERR_SUCCESS;
}

static void _on_start_speak(tc_iot_error_e error_code, const char *error_message) {
  AITALK_LOG("start_speak cb: code=%d msg=%s", error_code, error_message ? error_message : "");
  s_start_speak_result.error_code = error_code;
  s_start_speak_result.called = true;
}

static bool _wait_start_speak_result(int32_t timeout_ms) {
  for (int32_t elapsed = 0; !s_start_speak_result.called && elapsed < timeout_ms; elapsed += 100) {
    audio_device_sleep_ms(100);
  }
  return s_start_speak_result.called && s_start_speak_result.error_code == TC_IOT_ERR_SUCCESS;
}

// ==================== MIC 采集回调 ====================

static void _on_mic_captured(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec,
                             uint64_t pts_ms, void *user_data) {
  (void)user_data;
  if (!g_state.running || !data || size == 0) {
    return;
  }

  tc_iot_audio_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.codec = codec;
  frame.sample_rate = TC_IOT_AUDIO_SAMPLE_RATE_16000;
  frame.channels = TC_IOT_AUDIO_CHANNEL_MONO;
  frame.frame_duration_ms = audio_device_frame_duration_ms(codec);
  frame.data = (uint8_t *)data;
  frame.data_size = size;
  frame.pts_ms = pts_ms;

  tc_iot_aitalk_send_audio(&frame);
}

void aitalk_demo_stop(void) {
  if (!g_state.running) {
    return;
  }
  g_state.running = false;

  audio_device_stop_capture();

  if (g_state.device_opened) {
    audio_device_close();
    g_state.device_opened = false;
  }
  if (g_state.aitalk_inited) {
    tc_iot_aitalk_stop_speak();
    tc_iot_aitalk_deinit();
    g_state.aitalk_inited = false;
  }
  if (g_state.sdk_inited) {
    tc_iot_deinit();
    g_state.sdk_inited = false;
  }

  AITALK_LOG("stopped");
}

bool aitalk_demo_has_error(void) {
  return g_state.error_signaled;
}
