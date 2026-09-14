// Copyright (c) 2026 Tencent. All rights reserved.
//
// bk7258_amp + AV 开发板：iot_aitalk_demo 音频（aud_intf，PA GPIO_5）。
//
// 与 audio_device_beken_amp_ai.c 同源；PA 脚由 -DAUD_DAC_PA_CTRL_GPIO=GPIO_5 覆盖。
//
// 接口契约见 src/demo/iot_aitalk_demo/audio_device.h：
//   open → 起 aud_intf 语音全双工（mic+spk+AEC）；start_capture → mic PCM 经回调上行；
//   write → 下行 PCM 写 SPK；codec 固定 PCM（aitalk 上行编码由 SDK 内部完成）。

#include <components/log.h>
#include <os/mem.h>
#include <os/os.h>
#include <string.h>

#include "aud_intf.h"
#include "aud_intf_types.h"
#include "audio_device.h"

extern void HAL_Printf(const char *fmt, ...);

#define TAG "aitalk_aud_avdk_av"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

// ==================== 音频参数 ====================
#define MIC_SAMPLE_RATE 16000
#define MIC_CHANNELS 1
#define MIC_FRAME_MS 20
#define MIC_FRAME_BYTES (MIC_SAMPLE_RATE * 2 * MIC_CHANNELS * MIC_FRAME_MS / 1000)  // 640

// ==================== 内部状态 ====================
static aud_intf_drv_setup_t s_aud_drv_setup = DEFAULT_AUD_INTF_DRV_SETUP_CONFIG();
static aud_intf_voc_setup_t s_aud_voc_setup = DEFAULT_AUD_INTF_VOC_SETUP_CONFIG();

static bool s_opened = false;
static bool s_capture_started = false;
static audio_device_on_capture_t s_capture_cb = NULL;
static void *s_capture_user_data = NULL;
static uint32_t s_mic_frame_count = 0;

// 解耦线程：aud_intf mic 回调跑在很小栈的 bk 音频线程上，不能在其上同步调采集回调
// （_on_mic_captured → tc_iot_aitalk_send_audio 可能深栈/网络发送，易栈溢出）。
// 故回调只入队(轻量)，由本 PSRAM 32KB 栈线程调用采集回调，与 av_device_beken_amp_ai.c 同策略。
#define AUD_TX_SLOTS 8
#define AUD_TX_SLOT_SIZE 1024
// ring buffer 放 PSRAM（省 8KB SRAM；本板 SRAM 是最紧资源）。设备生命周期常驻，不释放。
static uint8_t *s_tx_buf = NULL;  // [AUD_TX_SLOTS][AUD_TX_SLOT_SIZE] flattened in PSRAM
static uint16_t s_tx_len[AUD_TX_SLOTS];
static uint64_t s_tx_pts[AUD_TX_SLOTS];
static volatile uint32_t s_tx_wr = 0;
static volatile uint32_t s_tx_rd = 0;
static volatile uint32_t s_tx_drop = 0;
static beken_semaphore_t s_tx_sem = NULL;
static beken_thread_t s_tx_thread = NULL;
static volatile bool s_tx_run = false;

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

void audio_device_sleep_ms(uint32_t ms) {
  rtos_delay_milliseconds(ms);
}

// ==================== 解耦上行线程 ====================
static void _fwd_thread_main(void *arg) {
  (void)arg;
  while (s_tx_run) {
    if (rtos_get_semaphore(&s_tx_sem, BEKEN_WAIT_FOREVER) != 0) {
      continue;
    }
    while (s_tx_run && s_tx_rd != s_tx_wr) {
      uint32_t rd = s_tx_rd;
      audio_device_on_capture_t cb = s_capture_cb;
      if (s_capture_started && cb) {
        cb(&s_tx_buf[rd * AUD_TX_SLOT_SIZE], s_tx_len[rd], TC_IOT_AUDIO_CODEC_PCM, s_tx_pts[rd],
           s_capture_user_data);
      }
      s_tx_rd = (rd + 1) % AUD_TX_SLOTS;
    }
  }
  s_tx_thread = NULL;
  rtos_delete_thread(NULL);
}

static void _fwd_thread_start(void) {
  s_tx_wr = s_tx_rd = 0;
  s_tx_drop = 0;
  // ring buffer 放 PSRAM（省 8KB SRAM）；首次分配后常驻、重入复用、不释放（与 s_tx_sem 同策略）。
  if (s_tx_buf == NULL) {
    s_tx_buf = (uint8_t *)psram_malloc(AUD_TX_SLOTS * AUD_TX_SLOT_SIZE);
    if (s_tx_buf == NULL) {
      HAL_Printf("[aitalk_aud] WARNING: psram_malloc tx_buf(%d) failed\n",
                 AUD_TX_SLOTS * AUD_TX_SLOT_SIZE);
      return;  // 不创建转发线程；_on_mic_data 因 s_tx_thread==NULL 不会入队/解引用
    }
  }
  if (s_tx_sem == NULL) {
    rtos_init_semaphore(&s_tx_sem, AUD_TX_SLOTS);
  }
  if (s_tx_thread == NULL) {
    s_tx_run = true;
    int ret = rtos_create_psram_thread(&s_tx_thread, 5, "aitalk_aud_fwd",
                                       (beken_thread_function_t)_fwd_thread_main, 32 * 1024, NULL);
    if (ret != 0) {
      HAL_Printf("[aitalk_aud] WARNING: create fwd thread failed: %d\n", ret);
      s_tx_run = false;
      s_tx_thread = NULL;
    } else {
      HAL_Printf("[aitalk_aud] fwd thread started (PSRAM 32KB stack)\n");
    }
  }
}

static void _fwd_thread_stop(void) {
  if (s_tx_thread && s_tx_run) {
    s_tx_run = false;
    if (s_tx_sem) {
      rtos_set_semaphore(&s_tx_sem);  // 唤醒线程退出
    }
  }
}

// ==================== MIC 回调（bk 音频线程，小栈）====================
static int _on_mic_data(unsigned char *data, unsigned int len) {
  if (!s_capture_started || !s_capture_cb || !data || len == 0) {
    return len;
  }
  s_mic_frame_count++;
  if (s_mic_frame_count <= 3 || (s_mic_frame_count % 500) == 0) {
    HAL_Printf("[aitalk_aud][MIC] frame #%u len=%u (drop=%u)\n", s_mic_frame_count, len, s_tx_drop);
  }
  if (len > AUD_TX_SLOT_SIZE || s_tx_thread == NULL) {
    return len;
  }
  uint32_t wr = s_tx_wr;
  uint32_t next = (wr + 1) % AUD_TX_SLOTS;
  if (next == s_tx_rd) {
    s_tx_drop++;  // 队列满，丢帧（不阻塞 bk 音频线程）
    return len;
  }
  memcpy(&s_tx_buf[wr * AUD_TX_SLOT_SIZE], data, len);
  s_tx_len[wr] = (uint16_t)len;
  s_tx_pts[wr] = (uint64_t)rtos_get_time();
  s_tx_wr = next;
  if (s_tx_sem) {
    rtos_set_semaphore(&s_tx_sem);
  }
  return len;
}

// ==================== 生命周期接口 ====================
int audio_device_open(const audio_device_config_t *config) {
  (void)config;  // amp 固定 16kHz/mono/20ms PCM 全双工，参数取自本文件宏。
  if (s_opened) {
    return 0;
  }
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  s_capture_started = false;
  s_mic_frame_count = 0;
  _fwd_thread_start();

  // 语音全双工：mic 采集（PCM）经 _on_mic_data 上行；spk 经 write_spk_data 下行。
  // voc_setup 字段与 av_device_beken_amp_av.c 同源（本板 PA=GPIO_5，由 CMake
  // -DAUD_DAC_PA_CTRL_GPIO 覆盖；单端 DAC + 调低增益破啸叫）。
  s_aud_drv_setup.aud_intf_tx_mic_data = _on_mic_data;

  bk_err_t ret = bk_aud_intf_drv_init(&s_aud_drv_setup);
  if (ret != BK_ERR_AUD_INTF_OK) {
    LOGE("bk_aud_intf_drv_init failed: %d\n", (int)ret);
    goto fail;
  }

  ret = bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_VOICE);
  if (ret != BK_ERR_AUD_INTF_OK) {
    LOGE("bk_aud_intf_set_mode failed: %d\n", (int)ret);
    goto fail_drv;
  }

  s_aud_voc_setup.aec_enable = true;
  s_aud_voc_setup.samp_rate = MIC_SAMPLE_RATE;
  s_aud_voc_setup.data_type = AUD_INTF_VOC_DATA_TYPE_PCM;
  s_aud_voc_setup.mic_type = AUD_INTF_MIC_TYPE_BOARD;
  s_aud_voc_setup.spk_type = AUD_INTF_SPK_TYPE_BOARD;
  s_aud_voc_setup.spk_mode = AUD_DAC_WORK_MODE_SIGNAL_END;
  // aitalk 的 TTS 内容振幅偏满，比通话人声更响；spk_gain 在视频路径(0x18)基础上降到 0x0A，
  // 避免本板 PA(GPIO_5) 功放下过响。运行时仍可用 CLI `vol <0-100>` 微调(gain=0x3F*百分比/100)。
  s_aud_voc_setup.spk_gain = 0x0A;
  s_aud_voc_setup.aec_cfg.ec_depth = 30;

  ret = bk_aud_intf_voc_init(s_aud_voc_setup);
  if (ret != BK_ERR_AUD_INTF_OK) {
    LOGE("bk_aud_intf_voc_init failed: %d\n", (int)ret);
    goto fail_mode;
  }

  ret = bk_aud_intf_voc_start();
  if (ret != BK_ERR_AUD_INTF_OK) {
    LOGE("bk_aud_intf_voc_start failed: %d\n", (int)ret);
    goto fail_voc;
  }

  s_opened = true;
  HAL_Printf("[aitalk_aud] opened: %dHz PCM full-duplex (AEC on)\n", MIC_SAMPLE_RATE);
  return 0;

fail_voc:
  bk_aud_intf_voc_deinit();
fail_mode:
  bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
fail_drv:
  bk_aud_intf_drv_deinit();
fail:
  _fwd_thread_stop();
  return -1;
}

void audio_device_close(void) {
  if (!s_opened) {
    return;
  }
  s_capture_started = false;
  s_capture_cb = NULL;
  s_capture_user_data = NULL;

  bk_aud_intf_voc_stop();
  bk_aud_intf_voc_deinit();
  bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
  bk_aud_intf_drv_deinit();

  _fwd_thread_stop();
  s_opened = false;
  HAL_Printf("[aitalk_aud] closed (mic frames=%u)\n", s_mic_frame_count);
}

// ==================== 采集接口 ====================
int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!cb) {
    return -1;
  }
  if (!s_opened) {
    LOGE("audio device not opened\n");
    return -1;
  }
  s_capture_cb = cb;
  s_capture_user_data = user_data;
  s_capture_started = true;
  HAL_Printf("[aitalk_aud] capture started\n");
  return 0;
}

void audio_device_stop_capture(void) {
  s_capture_started = false;
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  HAL_Printf("[aitalk_aud] capture stopped\n");
}

// ==================== 播放接口 ====================
int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!s_opened || !data || size == 0) {
    return -1;
  }
  if (codec != TC_IOT_AUDIO_CODEC_PCM) {
    return -1;
  }
  bk_aud_intf_write_spk_data((uint8_t *)(uintptr_t)data, size);
  return 0;
}
