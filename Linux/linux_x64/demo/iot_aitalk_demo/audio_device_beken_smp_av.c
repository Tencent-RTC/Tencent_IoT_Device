// Copyright (c) 2026 Tencent. All rights reserved.
//
// BK7258 SMP + AV board: AITalk audio via dev_audio (PA GPIO_5).
// Do not run concurrently with av_device_beken_smp_av (shared mic/spk).

#include <components/log.h>
#include <os/os.h>
#include <string.h>

#include "audio_device.h"

#if (CONFIG_DEV_AUDIO_EN > 0)
#  include "dev_audio.h"
#endif

#define TAG "aitalk_audio_gen"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

#define MIC_SAMPLE_RATE 16000
#define MIC_FRAME_MS 20

static bool s_inited = false;
static bool s_capture_started = false;
static audio_device_on_capture_t s_capture_cb = NULL;
static void *s_capture_user_data = NULL;

static void _on_mic_data(const uint8_t *data, uint32_t len, void *user_data) {
  (void)user_data;
  if (!s_capture_cb || !s_capture_started || !data || len == 0) {
    return;
  }
  s_capture_cb(data, len, TC_IOT_AUDIO_CODEC_PCM, (uint64_t)rtos_get_time(), s_capture_user_data);
}

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

int audio_device_open(const audio_device_config_t *config) {
  (void)config;
#if !(CONFIG_DEV_AUDIO_EN > 0)
  LOGE("CONFIG_DEV_AUDIO_EN disabled\n");
  return -1;
#else
  if (s_inited) {
    return 0;
  }
  if (dev_audio_is_open()) {
    LOGE("dev_audio already open (AV session active?); close AV first\n");
    return -1;
  }

  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  s_capture_started = false;

  dev_audio_config_t cfg = {0};
  cfg.type = DEV_AUDIO_TYPE_ONBOARD;
  cfg.mic_codec_format = DEV_AUDIO_CODEC_FORMAT_PCM;
  cfg.mic_sample_rate = DEV_AUDIO_SAMPLE_RATE_16000;
  cfg.spk_codec_format = DEV_AUDIO_CODEC_FORMAT_PCM;
  cfg.spk_sample_rate = DEV_AUDIO_SAMPLE_RATE_16000;
  cfg.aec_en = true;
  cfg.aec_mode = DEV_AUDIO_AEC_MODE_HW;
  cfg.read_cb = _on_mic_data;
  cfg.read_cb_args = NULL;

  if (dev_audio_open(&cfg) != BK_OK) {
    LOGE("dev_audio_open fail\n");
    return -1;
  }
  s_inited = true;
  LOGI("opened via dev_audio (PA GPIO_5)\n");
  return 0;
#endif
}

void audio_device_close(void) {
  if (!s_inited) {
    return;
  }
  s_capture_started = false;
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
#if (CONFIG_DEV_AUDIO_EN > 0)
  if (dev_audio_is_open()) {
    dev_audio_close();
  }
#endif
  s_inited = false;
  LOGI("closed\n");
}

int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!cb) {
    return -1;
  }
  if (!s_inited) {
    LOGE("not opened\n");
    return -1;
  }
  s_capture_cb = cb;
  s_capture_user_data = user_data;
  s_capture_started = true;
  LOGI("capture started\n");
  return 0;
}

void audio_device_stop_capture(void) {
  s_capture_started = false;
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  LOGI("capture stopped\n");
}

int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!s_inited || !data || size == 0) {
    return -1;
  }
  if (codec != TC_IOT_AUDIO_CODEC_PCM) {
    return -1;
  }
#if (CONFIG_DEV_AUDIO_EN > 0) && (DEV_AUDIO_VOICE_WRITE_EN > 0)
  if (!dev_audio_is_open()) {
    return -1;
  }
  if (dev_audio_write_spk(data, size) != BK_OK) {
    return -1;
  }
  return 0;
#else
  (void)data;
  (void)size;
  return -1;
#endif
}

void audio_device_sleep_ms(uint32_t ms) {
  rtos_delay_milliseconds(ms);
}
