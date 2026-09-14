// Copyright (c) 2026 Tencent. All rights reserved.

// BK7258 audio device for iot_aitalk_demo:
// MIC capture (16kHz/mono/20ms) + SPK playout (16kHz/mono/20ms) + hardware AEC.
// Adapted from av_device_beken_smp_ai.c (video_demo) with all video code removed.

#include <components/bk_voice_read_service.h>
#include <components/bk_voice_read_service_types.h>
#include <components/bk_voice_service.h>
#include <components/bk_voice_service_types.h>
#include <components/bk_voice_write_service.h>
#include <components/bk_voice_write_service_types.h>
#include <components/log.h>
#include <components/system.h>
#include <driver/gpio.h>
#include <os/os.h>
#include <string.h>

#include "audio_device.h"

#define TAG "aitalk_audio"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

// ==================== 音频参数 ====================

#define MIC_SAMPLE_RATE 16000
#define MIC_CHANNELS 1
#define MIC_FRAME_MS 20
#define MIC_FRAME_BYTES (MIC_SAMPLE_RATE * 2 * MIC_CHANNELS * MIC_FRAME_MS / 1000)

#define SPK_SAMPLE_RATE 16000
#define SPK_CHANNELS 1
#define SPK_FRAME_MS 20
#define SPK_FRAME_BYTES (SPK_SAMPLE_RATE * 2 * SPK_CHANNELS * SPK_FRAME_MS / 1000)

// ==================== 内部状态 ====================

static voice_handle_t s_voice_handle = NULL;
static voice_read_handle_t s_voice_read_handle = NULL;
static voice_write_handle_t s_voice_write_handle = NULL;

static bool s_voice_service_opened = false;
static bool s_capture_started = false;
static bool s_inited = false;

static audio_device_on_capture_t s_capture_cb = NULL;
static void *s_capture_user_data = NULL;

typedef struct {
  uint32_t count;
  uint32_t fail_count;
  uint32_t last_write_ms;
  uint32_t total_interval_ms;
  uint32_t min_interval_ms;
  uint32_t max_interval_ms;
  uint32_t max_cost_ms;
} playout_diag_t;

static playout_diag_t s_playout_diag;

// ==================== 前向声明 ====================

static int _audio_upward_callback(unsigned char *data, unsigned int len, void *args);
static bk_err_t _voice_service_open(void);
static bk_err_t _voice_service_close(void);

// ==================== 配置查询接口 ====================

tc_iot_audio_codec_e audio_device_default_codec(void) {
  return TC_IOT_AUDIO_CODEC_AAC;
}

bool audio_device_codec_supported(tc_iot_audio_codec_e codec) {
  return codec == TC_IOT_AUDIO_CODEC_AAC;
}

uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec) {
  (void)codec;
  return 64;
}

// ==================== 生命周期接口 ====================

int audio_device_open(const audio_device_config_t *config) {
  (void)config;
  if (s_inited) {
    return 0;
  }

  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  s_capture_started = false;

  memset(&s_playout_diag, 0, sizeof(s_playout_diag));
  s_playout_diag.min_interval_ms = 0xFFFFFFFF;

  bk_err_t ret = _voice_service_open();
  if (ret != BK_OK) {
    LOGE("voice service open failed\n");
    return -1;
  }
  s_inited = true;
  return 0;
}

void audio_device_close(void) {
  if (!s_inited) {
    return;
  }

  if (s_capture_started) {
    s_capture_started = false;
    if (s_voice_read_handle) {
      bk_voice_read_stop(s_voice_read_handle);
    }
  }

  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  _voice_service_close();
  s_inited = false;
}

// ==================== 采集接口 ====================

int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!cb) {
    return -1;
  }
  if (!s_voice_service_opened) {
    LOGE("voice service not opened\n");
    return -1;
  }

  s_capture_cb = cb;
  s_capture_user_data = user_data;

  if (!s_capture_started) {
    bk_err_t ret = bk_voice_read_start(s_voice_read_handle);
    if (ret != BK_OK) {
      LOGE("voice read start failed: %d\n", (int)ret);
      s_capture_cb = NULL;
      s_capture_user_data = NULL;
      return -1;
    }
    s_capture_started = true;
    bk_printf("[aitalk_audio] capture started\n");
  }
  return 0;
}

void audio_device_stop_capture(void) {
  if (!s_capture_started) {
    return;
  }
  s_capture_started = false;
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  if (s_voice_read_handle) {
    bk_voice_read_stop(s_voice_read_handle);
  }
  bk_printf("[aitalk_audio] capture stopped\n");
}

// ==================== 播放接口 ====================

int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!s_voice_service_opened || !data || size == 0 || !s_voice_write_handle) {
    return -1;
  }
  if (codec != TC_IOT_AUDIO_CODEC_PCM) {
    return -1;
  }

  uint32_t now_ms = (uint32_t)rtos_get_time();
  if (s_playout_diag.last_write_ms != 0) {
    uint32_t interval_ms = now_ms - s_playout_diag.last_write_ms;
    s_playout_diag.total_interval_ms += interval_ms;
    if (interval_ms < s_playout_diag.min_interval_ms) {
      s_playout_diag.min_interval_ms = interval_ms;
    }
    if (interval_ms > s_playout_diag.max_interval_ms) {
      s_playout_diag.max_interval_ms = interval_ms;
    }
  }
  s_playout_diag.last_write_ms = now_ms;

  if (s_playout_diag.count == 0) {
    bk_printf("[aitalk_audio] first playout frame: len=%u\n", size);
  }

  uint32_t write_start_ms = now_ms;
  bk_err_t ret = bk_voice_write_frame_data(s_voice_write_handle, (char *)(uintptr_t)data, size);
  uint32_t cost_ms = (uint32_t)rtos_get_time() - write_start_ms;
  s_playout_diag.count++;

  if (cost_ms > s_playout_diag.max_cost_ms) {
    s_playout_diag.max_cost_ms = cost_ms;
  }

  if (ret != (bk_err_t)size) {
    s_playout_diag.fail_count++;
    if (s_playout_diag.fail_count <= 10 || s_playout_diag.fail_count % 100 == 0) {
      bk_printf("[aitalk_audio] write fail #%u: ret=%d expected=%u\n", s_playout_diag.fail_count,
                (int)ret, size);
    }
  }

  if (s_playout_diag.count <= 5 || s_playout_diag.count % 5000 == 0) {
    bk_printf(
        "[aitalk_audio] stat: count=%u fail=%u avg_interval=%u min=%u max=%u "
        "max_cost=%u\n",
        s_playout_diag.count, s_playout_diag.fail_count,
        (s_playout_diag.count > 1) ? (s_playout_diag.total_interval_ms / (s_playout_diag.count - 1))
                                   : 0,
        (s_playout_diag.min_interval_ms == 0xFFFFFFFF) ? 0 : s_playout_diag.min_interval_ms,
        s_playout_diag.max_interval_ms, s_playout_diag.max_cost_ms);
  }

  return 0;
}

// ==================== MIC 回调 ====================

static int _audio_upward_callback(unsigned char *data, unsigned int len, void *args) {
  (void)args;
  if (!s_capture_cb || !s_capture_started) {
    return len;
  }
  uint64_t pts_ms = (uint64_t)rtos_get_time();
  s_capture_cb((const uint8_t *)data, (uint32_t)len, TC_IOT_AUDIO_CODEC_PCM, pts_ms,
               s_capture_user_data);
  return len;
}

// ==================== Voice Service ====================

static bk_err_t _voice_service_open(void) {
  if (s_voice_service_opened) {
    return BK_OK;
  }

  voice_cfg_t voice_cfg = VOICE_BY_ONBOARD_MIC_SPK_CFG_DEFAULT();

  // MIC: 16kHz / 双通道 ADC（L=MIC, R=DAC 回环用于硬件 AEC）
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.sample_rate = MIC_SAMPLE_RATE;
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.chl_num = 2;
  voice_cfg.mic_cfg.onboard_mic_cfg.frame_size = MIC_FRAME_BYTES;
  voice_cfg.mic_cfg.onboard_mic_cfg.out_block_size = MIC_FRAME_BYTES;
  voice_cfg.mic_cfg.onboard_mic_cfg.out_block_num = 1;
  voice_cfg.mic_cfg.onboard_mic_cfg.multi_out_port_num = 1;
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.dig_gain = 0x2D;
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.ana_gain = 0x08;
  voice_cfg.mic_cfg.onboard_mic_cfg.task_stack = 4096;

  // SPK: 16kHz / 单声道 / 20ms
  voice_cfg.spk_cfg.onboard_spk_cfg.sample_rate = SPK_SAMPLE_RATE;
  voice_cfg.spk_cfg.onboard_spk_cfg.chl_num = SPK_CHANNELS;
  voice_cfg.spk_cfg.onboard_spk_cfg.frame_size = SPK_FRAME_BYTES;
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_length = SPK_FRAME_BYTES * 8;
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_play_thold = SPK_FRAME_BYTES * 3;
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_pause_thold = SPK_FRAME_BYTES / 2;
  voice_cfg.spk_cfg.onboard_spk_cfg.multi_out_port_num = 0;
  voice_cfg.spk_cfg.onboard_spk_cfg.dig_gain = 0x0D;
  voice_cfg.spk_cfg.onboard_spk_cfg.ana_gain = 0x07;
  voice_cfg.spk_cfg.onboard_spk_cfg.task_stack = 8192;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_ctrl_en = true;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_ctrl_gpio = GPIO_50;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_on_level = 1;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_on_delay = 10;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_off_delay = 30;

  // AEC: 硬件回采模式
  voice_cfg.aec_en = true;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.mode = AEC_MODE_HARDWARE;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_only_output = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.fs = MIC_SAMPLE_RATE;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.init_flags = 0x1f;
  voice_cfg.aec_cfg.aec_alg_cfg.dual_ch = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.task_stack = 4096;
  voice_cfg.aec_cfg.aec_alg_cfg.out_block_size = MIC_FRAME_BYTES;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_depth = 0x1A;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_filter = 0x3;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.delay_points = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ref_scale = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.TxRxThr = 30;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.TxRxFlr = 6;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_type = NS_TRADITION;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_level = 5;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_para = 1;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_filter = 0x3;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.drc = 0x10;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.voice_vol = 0x0e;

  voice_cfg.enc_type = AUDIO_ENC_TYPE_PCM;
  voice_cfg.enc_cfg.pcm_enc_cfg = 0;
  voice_cfg.dec_type = AUDIO_DEC_TYPE_PCM;
  voice_cfg.dec_cfg.pcm_dec_cfg = 0;
  voice_cfg.read_pool_size = MIC_FRAME_BYTES;
  voice_cfg.write_pool_size = SPK_FRAME_BYTES;
  voice_cfg.event_handle = NULL;
  voice_cfg.args = NULL;

  do {
    s_voice_handle = bk_voice_init(&voice_cfg);
    if (!s_voice_handle) {
      LOGE("bk_voice_init fail\n");
      break;
    }

    voice_read_cfg_t voice_read_cfg = VOICE_READ_CFG_DEFAULT();
    voice_read_cfg.voice_handle = s_voice_handle;
    voice_read_cfg.max_read_size = 2048;
    voice_read_cfg.voice_read_callback = _audio_upward_callback;
    voice_read_cfg.args = NULL;
    voice_read_cfg.task_stack = 1024 * 4;
    voice_read_cfg.mem_type = AUDIO_MEM_TYPE_PSRAM;
    s_voice_read_handle = bk_voice_read_init(&voice_read_cfg);
    if (!s_voice_read_handle) {
      LOGE("bk_voice_read_init fail\n");
      break;
    }

    voice_write_cfg_t voice_write_cfg = VOICE_WRITE_CFG_DEFAULT();
    voice_write_cfg.voice_handle = s_voice_handle;
    voice_write_cfg.node_size = 20480;
    voice_write_cfg.mem_type = AUDIO_MEM_TYPE_PSRAM;
    s_voice_write_handle = bk_voice_write_init(&voice_write_cfg);
    if (!s_voice_write_handle) {
      LOGE("bk_voice_write_init fail\n");
      break;
    }

    if (bk_voice_start(s_voice_handle) != BK_OK) {
      break;
    }
    if (bk_voice_write_start(s_voice_write_handle) != BK_OK) {
      break;
    }

    s_voice_service_opened = true;
  } while (0);

  if (!s_voice_service_opened) {
    _voice_service_close();
    return BK_FAIL;
  }

  bk_printf("[aitalk_audio] voice service opened\n");
  return BK_OK;
}

static bk_err_t _voice_service_close(void) {
  if (!s_voice_service_opened) {
    return BK_OK;
  }
  s_voice_service_opened = false;
  s_capture_started = false;

  bk_printf(
      "[aitalk_audio] stop summary: count=%u fail=%u avg_interval=%u min=%u "
      "max=%u max_cost=%u\n",
      s_playout_diag.count, s_playout_diag.fail_count,
      (s_playout_diag.count > 1) ? (s_playout_diag.total_interval_ms / (s_playout_diag.count - 1))
                                 : 0,
      (s_playout_diag.min_interval_ms == 0xFFFFFFFF) ? 0 : s_playout_diag.min_interval_ms,
      s_playout_diag.max_interval_ms, s_playout_diag.max_cost_ms);

  if (s_voice_read_handle) {
    bk_voice_read_stop(s_voice_read_handle);
  }
  if (s_voice_write_handle) {
    bk_voice_write_stop(s_voice_write_handle);
  }
  if (s_voice_handle) {
    bk_voice_stop(s_voice_handle);
  }
  if (s_voice_read_handle) {
    bk_voice_read_deinit(s_voice_read_handle);
  }
  if (s_voice_write_handle) {
    bk_voice_write_deinit(s_voice_write_handle);
  }
  if (s_voice_handle) {
    bk_voice_deinit(s_voice_handle);
  }

  s_voice_read_handle = NULL;
  s_voice_write_handle = NULL;
  s_voice_handle = NULL;
  bk_printf("[aitalk_audio] voice service closed\n");
  return BK_OK;
}

void audio_device_sleep_ms(uint32_t ms) {
  rtos_delay_milliseconds(ms);
}
