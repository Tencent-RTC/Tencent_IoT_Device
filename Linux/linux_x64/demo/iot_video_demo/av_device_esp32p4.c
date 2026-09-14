// Copyright (c) 2026 Tencent. All rights reserved.
//
// ESP32-P4 (WT99P4C5-S1) 音频/视频设备实现：
//   ES8311 音频上下行 + MIPI-CSI 视频上行 + LCD MJPEG 下行显示。
//
// 上行音频：从 I2S RX 读取立体声帧，下混为单声道 MIC 帧。
// 下行音频：远端单声道 PCM 复制为双声道，经 I2S TX 送硬件播放。

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "av_device.h"
#include "iot_demo_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"
#include "esp_afe_aec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "driver/i2s_std.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "driver/ledc.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#define TAG "av_esp"

// ==================== ESP32-P4 (WT99P4C5-S1) 管脚 ====================

#define AV_ESP_I2C_PORT I2C_NUM_0
#define AV_ESP_I2C_SDA_GPIO GPIO_NUM_7
#define AV_ESP_I2C_SCL_GPIO GPIO_NUM_8

#define AV_ESP_I2S_PORT I2S_NUM_0
#define AV_ESP_I2S_MCLK_GPIO GPIO_NUM_13
#define AV_ESP_I2S_BCLK_GPIO GPIO_NUM_12
#define AV_ESP_I2S_WS_GPIO GPIO_NUM_10
#define AV_ESP_I2S_DOUT_GPIO GPIO_NUM_9
#define AV_ESP_I2S_DIN_GPIO GPIO_NUM_11
#define AV_ESP_PA_GPIO GPIO_NUM_53

// ---- 音频：Codec 与播放缓冲 ----
#define AV_ESP_CODEC_CHANNELS 2
#define AV_ESP_ES8311_OUT_VOL 70

#define AV_ESP_CAPTURE_FRAME_BYTES \
  PCM_FRAME_BYTES(MIC_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS, MIC_FRAME_MS)
#define AV_ESP_PLAYOUT_FRAME_BYTES \
  PCM_FRAME_BYTES(SPK_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS, SPK_FRAME_MS)

// ---- 音频采集任务 ----
#define AV_ESP_AUDIO_TASK_STACK (8 * 1024)
#define AV_ESP_AUDIO_TASK_PRIORITY 5
#define AV_ESP_AUDIO_READ_TIMEOUT_MS 100
#define AV_ESP_AUDIO_STOP_WAIT_MS 1000

#ifndef AV_ESP_ES8311_MIC_GAIN_DB
#  define AV_ESP_ES8311_MIC_GAIN_DB 30.0f
#endif

// ---- AEC 回声消除（esp-sr AFE）----
// 采集链路加入声学回声消除：以"播放出去的远端音频"为参考信号，
// 从麦克风信号中消除被采回的扬声器/Codec 回采声，避免机器人听到自己说话
// 而产生回声/自激（即使不接扬声器，DAC 回采/内部串扰也会形成回声）。
// AEC 输入通道格式：M=麦克风，R=播放参考。本实现为 1 麦 + 1 参考。
#define AV_ESP_AEC_INPUT_FORMAT "MR"
// LOW_COST + 较短滤波器：降低 AFE 内部 SRAM/CPU，回声拖尾略短。
#define AV_ESP_AEC_MODE AFE_MODE_LOW_COST
#define AV_ESP_AEC_FILTER_LENGTH 2
// AEC 工作缓冲与参考环放到 PSRAM，避免挤占内部 DMA 堆。
#define AV_ESP_AEC_BUFFER_ALIGN 16
#define AV_ESP_AEC_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
// 参考信号环形缓冲容量（单声道样本数），约 500ms @16kHz，吸收播放突发与时序抖动。
#define AV_ESP_AEC_REFERENCE_RING_SAMPLES (MIC_SAMPLE_RATE / 2)

// ==================== 公共全局变量 ====================

// ---- 生命周期与回调 ----
static bool audio_hardware_initialized = false;
static volatile bool audio_started = false;
static volatile bool audio_task_running = false;
static TaskHandle_t audio_task_handle = NULL;
static audio_device_callback_t audio_callback = NULL;
static void *audio_callback_context = NULL;

// ---- I2C / I2S 句柄 ----
static i2c_master_bus_handle_t audio_i2c_bus = NULL;
static i2s_chan_handle_t i2s_speaker_channel = NULL;
static i2s_chan_handle_t i2s_microphone_channel = NULL;
static const audio_codec_data_if_t *i2s_data_interface = NULL;

// ---- 播放 Codec（ES8311 DAC） ----
static const audio_codec_ctrl_if_t *speaker_control_interface = NULL;
static const audio_codec_gpio_if_t *speaker_gpio_interface = NULL;
static const audio_codec_if_t *speaker_codec_interface = NULL;
static esp_codec_dev_handle_t speaker_codec_device = NULL;

// ---- 录音 Codec（ES8311 BOTH 模式，与播放共用同一芯片） ----
static esp_codec_dev_handle_t microphone_codec_device = NULL;
static const audio_codec_if_t *microphone_codec_interface = NULL;
static const audio_codec_ctrl_if_t *microphone_control_interface = NULL;

// ---- 音频帧缓存 ----
static int16_t capture_stereo_buffer[AV_ESP_CAPTURE_FRAME_BYTES / sizeof(int16_t)];
static int16_t capture_mono_buffer[MIC_FRAME_BYTES / sizeof(int16_t)];
static int16_t playback_stereo_buffer[AV_ESP_PLAYOUT_FRAME_BYTES / sizeof(int16_t)];

// ---- 下行音频播放诊断 ----
static uint32_t audio_playback_received_frame_count = 0;
static uint32_t audio_playback_discard_frame_count = 0;
static uint32_t audio_playback_write_fail_frame_count = 0;

// ---- 声学回声消除（Acoustic Echo Cancellation）状态 ----
static afe_aec_handle_t *aec_handle = NULL;
static int aec_chunk_samples = 0;               // 每次处理的单声道样本数
static int16_t *aec_microphone_buffer = NULL;   // chunk 单声道麦克风信号
static int16_t *aec_reference_buffer = NULL;    // chunk 单声道参考信号
static int16_t *aec_interleaved_buffer = NULL;  // chunk*2 交织 [M,R,...]
static int16_t *aec_output_buffer = NULL;       // chunk 单声道输出
static size_t aec_frame_accumulated = 0;        // 20ms 上抛帧已累计样本数
// 参考信号环形缓冲：播放线程写入，采集线程读取
static int16_t *aec_reference_ring_buffer = NULL;
static volatile uint32_t aec_reference_ring_buffer_head = 0;
static volatile uint32_t aec_reference_ring_buffer_tail = 0;
static portMUX_TYPE aec_reference_ring_buffer_lock = portMUX_INITIALIZER_UNLOCKED;

// ---- ESP32-P4 视频 ----
#define AV_ESP_VIDEO_TASK_STACK (8 * 1024)
#define AV_ESP_VIDEO_TASK_PRIORITY 4
#define AV_ESP_VIDEO_STOP_WAIT_MS 1000
#define AV_ESP_CAM_BUF_NUM 2

#define AV_ESP_CAM_I2C_PORT I2C_NUM_1
#define AV_ESP_CAM_I2C_SCL GPIO_NUM_8
#define AV_ESP_CAM_I2C_SDA GPIO_NUM_7
#define AV_ESP_CAM_I2C_FREQ 100000

// JPEG 编码质量（1~100）。800x640 quality=10 实测约 10KB/帧，输出缓冲按压缩后上限申请。
#define AV_ESP_JPEG_QUALITY 10
#define AV_ESP_JPEG_OUTPUT_CAPACITY (128U * 1024U)

// 摄像头输出 RGB565，直接送入 JPEG 硬件编码器。
#define AV_ESP_CAM_PIXEL_FORMAT V4L2_PIX_FMT_RGB565
#define AV_ESP_CAM_PIXEL_FORMAT_NAME "RGB565"

// ZX7D00C1060M009A LCD显示屏 1024x600。
#define AV_ESP_LCD_H_RES 1024U
#define AV_ESP_LCD_V_RES 600U
#define AV_ESP_LCD_BITS_PER_PIXEL 16U

// 云端下行帧解码后直接居中显示。
#define AV_ESP_PLAYOUT_INPUT_H_RES 320U
#define AV_ESP_PLAYOUT_INPUT_V_RES 480U
#define AV_ESP_LCD_VIDEO_X_START ((AV_ESP_LCD_H_RES - AV_ESP_PLAYOUT_INPUT_H_RES) / 2U)
#define AV_ESP_LCD_VIDEO_Y_START ((AV_ESP_LCD_V_RES - AV_ESP_PLAYOUT_INPUT_V_RES) / 2U)
#define AV_ESP_LCD_VIDEO_X_END (AV_ESP_LCD_VIDEO_X_START + AV_ESP_PLAYOUT_INPUT_H_RES)
#define AV_ESP_LCD_VIDEO_Y_END (AV_ESP_LCD_VIDEO_Y_START + AV_ESP_PLAYOUT_INPUT_V_RES)
#define AV_ESP_PLAYOUT_INPUT_FRAME_BYTES \
  (AV_ESP_PLAYOUT_INPUT_H_RES * AV_ESP_PLAYOUT_INPUT_V_RES * sizeof(uint16_t))

#if (AV_ESP_PLAYOUT_INPUT_H_RES > AV_ESP_LCD_H_RES) || \
    (AV_ESP_PLAYOUT_INPUT_V_RES > AV_ESP_LCD_V_RES)
#  error "Video frame must fit inside the LCD panel"
#endif
#define AV_ESP_LCD_MAX_JPEG_BYTES (2U * 1024U * 1024U)
#define AV_ESP_LCD_REFRESH_TIMEOUT_MS 100U
#define AV_ESP_LCD_MIPI_PHY_LDO_CHAN 3
#define AV_ESP_LCD_MIPI_PHY_LDO_MV 2500
#define AV_ESP_LCD_BACKLIGHT_GPIO GPIO_NUM_20
#define AV_ESP_LCD_BACKLIGHT_MODE LEDC_LOW_SPEED_MODE
#define AV_ESP_LCD_BACKLIGHT_TIMER LEDC_TIMER_1
#define AV_ESP_LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_1
#define AV_ESP_LCD_BACKLIGHT_FREQ_HZ 5000U
#define AV_ESP_LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_10_BIT
#define AV_ESP_LCD_BACKLIGHT_OFF_DUTY 0U
#define AV_ESP_LCD_BACKLIGHT_ON_DUTY ((1U << 10U) - 1U)

static bool camera_initialized = false;
static int camera_fd = -1;
static jpeg_encoder_handle_t jpeg_encoder_handle = NULL;
static jpeg_encode_cfg_t jpeg_encode_config;
static uint32_t jpeg_encode_input_size = 0;
static video_device_callback_t video_callback = NULL;
static void *video_callback_context = NULL;
static volatile bool video_stream_activate = false;
static TaskHandle_t video_task_handle = NULL;
static uint8_t *camera_buffer[AV_ESP_CAM_BUF_NUM];
static uint32_t camera_buffer_count = 0;
static size_t camera_buffer_length = 0;
static uint32_t camera_width = 0;
static uint32_t camera_height = 0;
static uint32_t camera_pixel_format = 0;
static uint32_t camera_bytesperline = 0;
static uint32_t camera_sizeimage = 0;
static uint8_t *jpeg_output_buffer = NULL;
static size_t jpeg_output_buffer_length = 0;

static bool lcd_initialized = false;
static bool lcd_backlight_initialized = false;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_dsi_bus_handle_t lcd_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t lcd_dbi_io = NULL;
static esp_ldo_channel_handle_t lcd_mipi_ldo = NULL;
static bool lcd_video_playout_initialized = false;
static SemaphoreHandle_t lcd_video_refresh_done = NULL;
static SemaphoreHandle_t lcd_video_lock = NULL;
static uint8_t *lcd_video_input_rgb565_buffer = NULL;
static jpeg_decoder_handle_t lcd_video_jpeg_decoder = NULL;
static jpeg_decode_cfg_t lcd_video_jpeg_decode_config;
static uint32_t lcd_video_drop_count = 0;

// ==================== 前置声明 ====================

// 音频内部实现
static esp_err_t _i2c_open(void);
static esp_err_t _i2s_open(void);
static esp_err_t _speaker_open(void);
static esp_err_t _microphone_open(void);
static void _i2c_close(void);
static void _i2s_close(void);
static void _speaker_close(void);
static void _microphone_close(void);
static void _get_microphone_audio_stream(void *arg);

// 声学回声消除内部实现
static esp_err_t _aec_open(void);
static void _aec_close(void);

// 视频内部实现
static int _camera_open(void);
static void _camera_close(void);
static esp_err_t _jpeg_encoder_init(void);
static void _jpeg_encoder_deinit(void);
static esp_err_t _jpeg_decoder_init(void);
static void _jpeg_decoder_deinit(void);
static void _get_camera_video_stream(void *arg);

static esp_err_t _lcd_open(void);
static void _lcd_close(void);
static esp_err_t _lcd_video_playout_open(void);
static void _lcd_video_playout_close(void);
static esp_err_t _lcd_video_playout_render(const uint8_t *jpeg_data, size_t jpeg_data_length);
static bool _lcd_video_refresh_done_callback(esp_lcd_panel_handle_t panel,
                                             esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);

// ==================== 公开接口 ====================

void av_device_init(const av_device_config_t *cfg) {
  (void)cfg;
  esp_err_t err;

  if (!lcd_initialized) {
    err = _lcd_open();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "LCD initialization failed: %s", esp_err_to_name(err));
    }
  }

  if (audio_hardware_initialized) {
    return;
  }

  err = _i2c_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2C init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _i2s_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2S init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _speaker_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "play codec init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _microphone_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "record codec init failed: %s", esp_err_to_name(err));
    return;
  }

  if (_aec_open() != ESP_OK) {
    ESP_LOGW(TAG, "AEC init failed, capture will run WITHOUT echo cancellation");
  }

  audio_hardware_initialized = true;
  ESP_LOGI(TAG,
           "initialized: i2c_sda=%d i2c_scl=%d "
           "mclk=%d bclk=%d ws=%d dout=%d din=%d pa=%d",
           (int)AV_ESP_I2C_SDA_GPIO, (int)AV_ESP_I2C_SCL_GPIO, (int)AV_ESP_I2S_MCLK_GPIO,
           (int)AV_ESP_I2S_BCLK_GPIO, (int)AV_ESP_I2S_WS_GPIO, (int)AV_ESP_I2S_DOUT_GPIO,
           (int)AV_ESP_I2S_DIN_GPIO, (int)AV_ESP_PA_GPIO);

  if (!camera_initialized) {
    camera_fd = _camera_open();
    if (camera_fd < 0) {
      ESP_LOGW(TAG, "camera init failed, video will be unavailable");
    } else if (_jpeg_encoder_init() != ESP_OK) {
      ESP_LOGW(TAG, "JPEG encoder init failed, video will be unavailable");
      _camera_close();
      camera_fd = -1;
    } else {
      camera_initialized = true;
      ESP_LOGI(TAG, "camera + JPEG encoder initialized and kept alive");
    }
  }
}

void av_device_deinit(void) {
  _lcd_video_playout_close();
  if (lcd_initialized || lcd_panel || lcd_dsi_bus || lcd_mipi_ldo) {
    _lcd_close();
  }

  if (camera_initialized) {
    video_stream_activate = false;
    if (video_task_handle) {
      int wait = AV_ESP_VIDEO_STOP_WAIT_MS / 20;
      while (video_task_handle && wait-- > 0) {
        rtos_delay_milliseconds(20);
      }
      video_task_handle = NULL;
    }
    _jpeg_encoder_deinit();
    _camera_close();
    camera_initialized = false;
  }
  if (audio_started) {
    audio_task_running = false;
    int wait = AV_ESP_AUDIO_STOP_WAIT_MS / 20;
    while (audio_task_handle != NULL && wait-- > 0) {
      rtos_delay_milliseconds(20);
    }
    audio_started = false;
  }
  _aec_close();
  _microphone_close();
  _speaker_close();
  _i2s_close();
  _i2c_close();
  audio_hardware_initialized = false;
  ESP_LOGI(TAG, "deinitialized");
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  (void)audio_file_path;
  if (!audio_hardware_initialized || !callback) {
    return -1;
  }
  if (!microphone_codec_device || !i2s_microphone_channel) {
    ESP_LOGE(TAG, "mic not ready");
    return -1;
  }
  if (audio_callback && audio_callback == callback && audio_callback_context == user_data) {
    return 0;
  }
  if (audio_callback) {
    ESP_LOGE(TAG, "audio listener already occupied");
    return -1;
  }

  audio_callback = callback;
  audio_callback_context = user_data;

  if (!audio_started) {
    audio_task_running = true;
    audio_started = true;
    BaseType_t ok = xTaskCreate(_get_microphone_audio_stream, "av_esp_mic", AV_ESP_AUDIO_TASK_STACK,
                                NULL, AV_ESP_AUDIO_TASK_PRIORITY, &audio_task_handle);
    if (ok != pdPASS) {
      audio_task_running = false;
      audio_started = false;
      audio_task_handle = NULL;
      audio_callback = NULL;
      audio_callback_context = NULL;
      ESP_LOGE(TAG, "failed to create audio task");
      return -1;
    }
    ESP_LOGI(TAG, "audio capture started: raw_ch=%d out_frame=%d bytes", AV_ESP_CODEC_CHANNELS,
             MIC_FRAME_BYTES);
  }

  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  if (!audio_callback || audio_callback != callback || audio_callback_context != user_data) {
    return;
  }

  audio_callback = NULL;
  audio_callback_context = NULL;

  if (audio_started) {
    audio_task_running = false;
    int wait = AV_ESP_AUDIO_STOP_WAIT_MS / 20;
    while (audio_task_handle != NULL && wait-- > 0) {
      rtos_delay_milliseconds(20);
    }
    audio_started = false;
    ESP_LOGI(TAG, "audio capture stopped");
  }
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  static bool s_playout_first_logged = false;

  audio_playback_received_frame_count++;

  if (!audio_hardware_initialized || !speaker_codec_device || !data || len == 0) {
    audio_playback_discard_frame_count++;
    return;
  }

  if (!s_playout_first_logged) {
    ESP_LOGI(TAG, "[PLY] first frame: len=%u", (unsigned)len);
    s_playout_first_logged = true;
  } else if (audio_playback_received_frame_count % 2000 == 0) {
    ESP_LOGI(TAG, "[PLY] count=%u drop=%u fail=%u", (unsigned)audio_playback_received_frame_count,
             (unsigned)audio_playback_discard_frame_count,
             (unsigned)audio_playback_write_fail_frame_count);
  }

  // 远端下行单声道 S16LE → I2S 双声道，复制到 L/R。
  const uint8_t *src = data;
  uint32_t remaining = len;
  while (remaining >= sizeof(int16_t)) {
    uint32_t chunk = remaining;
    if (chunk > SPK_FRAME_BYTES) {
      chunk = SPK_FRAME_BYTES;
    }
    chunk &= ~(uint32_t)(sizeof(int16_t) - 1);
    if (chunk == 0) {
      break;
    }

    size_t samples = chunk / sizeof(int16_t);
    // 单声道 S16LE → 立体声：L/R 复制
    {
      const int16_t *s16 = (const int16_t *)(const void *)src;
      for (size_t i = 0; i < samples; ++i) {
        int16_t s = s16[i];
        playback_stereo_buffer[i * AV_ESP_CODEC_CHANNELS] = s;
        playback_stereo_buffer[i * AV_ESP_CODEC_CHANNELS + 1] = s;
      }
    }

    // 将本帧远端单声道 PCM 推入 AEC 参考环形缓冲，作为回声消除的参考信号。
    // 与 esp_codec_dev_write 紧邻推送，使参考信号尽量贴近实际播放时刻。
    if (aec_reference_ring_buffer) {
      const int16_t *reference_source = (const int16_t *)(const void *)src;
      portENTER_CRITICAL(&aec_reference_ring_buffer_lock);
      for (size_t i = 0; i < samples; ++i) {
        uint32_t next_index =
            (aec_reference_ring_buffer_head + 1) % AV_ESP_AEC_REFERENCE_RING_SAMPLES;
        if (next_index == aec_reference_ring_buffer_tail) {
          aec_reference_ring_buffer_tail =
              (aec_reference_ring_buffer_tail + 1) % AV_ESP_AEC_REFERENCE_RING_SAMPLES;
        }
        aec_reference_ring_buffer[aec_reference_ring_buffer_head] = reference_source[i];
        aec_reference_ring_buffer_head = next_index;
      }
      portEXIT_CRITICAL(&aec_reference_ring_buffer_lock);
    }

    int out_bytes = (int)(samples * AV_ESP_CODEC_CHANNELS * sizeof(int16_t));
    int ret = esp_codec_dev_write(speaker_codec_device, playback_stereo_buffer, out_bytes);
    if (ret != ESP_CODEC_DEV_OK) {
      audio_playback_write_fail_frame_count++;
      ESP_LOGW(TAG, "[PLY] write fail #%u: ret=%d out_bytes=%d",
               (unsigned)audio_playback_write_fail_frame_count, ret, out_bytes);
      break;
    }
    src += chunk;
    remaining -= chunk;
  }
}

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  (void)video_file_path;
  if (!callback) {
    return -1;
  }
  if (!camera_initialized) {
    ESP_LOGE(TAG, "camera not initialized, call av_device_init first");
    return -1;
  }
  if (video_stream_activate) {
    ESP_LOGW(TAG, "video already running");
    return 0;
  }

  video_callback = callback;
  video_callback_context = user_data;
  video_stream_activate = true;

  if (xTaskCreate(_get_camera_video_stream, "av_esp_cam", AV_ESP_VIDEO_TASK_STACK, NULL,
                  AV_ESP_VIDEO_TASK_PRIORITY, &video_task_handle) != pdPASS) {
    video_stream_activate = false;
    video_callback = NULL;
    ESP_LOGE(TAG, "failed to create video task");
    return -1;
  }
  ESP_LOGI(TAG, "video capture started: %dx%d", (int)VIDEO_WIDTH, (int)VIDEO_HEIGHT);
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  (void)callback;
  (void)user_data;
  video_stream_activate = false;
  int wait = AV_ESP_VIDEO_STOP_WAIT_MS / 20;
  while (video_task_handle != NULL && wait-- > 0) {
    rtos_delay_milliseconds(20);
  }
  if (video_task_handle) {
    ESP_LOGW(TAG, "video task did not exit in time");
  }
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  if (data == NULL || len == 0U) {
    return;
  }

  esp_err_t err = _lcd_video_playout_render(data, len);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_SIZE) {
    ESP_LOGW(TAG, "LCD video frame dropped: %s", esp_err_to_name(err));
  }
}

void av_device_request_idr(void) {
}

uint64_t av_device_now_ms(void) {
  return rtos_get_time();
}

// MP4 解封装：硬件平台不支持，返回空桩
uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path) {
  (void)mp4_file_path;
  return 0;
}

uint64_t av_device_seek_mp4_ms(uint64_t target_ms) {
  (void)target_ms;
  return 0;
}

void audio_device_sleep_ms(uint32_t ms) {
  rtos_delay_milliseconds(ms);
}

// ==================== 公共音频实现 ====================
// I2C 总线初始化
static esp_err_t _i2c_open(void) {
  if (audio_i2c_bus) {
    return ESP_OK;
  }

  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = AV_ESP_I2C_PORT,
      .sda_io_num = AV_ESP_I2C_SDA_GPIO,
      .scl_io_num = AV_ESP_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&i2c_cfg, &audio_i2c_bus);
  if (err != ESP_OK) {
    audio_i2c_bus = NULL;
  }
  return err;
}

// 播放 Codec 初始化（ES8311 DAC）
static esp_err_t _speaker_open(void) {
  if (speaker_codec_device) {
    return ESP_OK;
  }
  if (!i2s_data_interface || !audio_i2c_bus) {
    return ESP_ERR_INVALID_STATE;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = AV_ESP_I2C_PORT,
      .addr = ES8311_CODEC_DEFAULT_ADDR,
      .bus_handle = audio_i2c_bus,
  };
  speaker_control_interface = audio_codec_new_i2c_ctrl(&i2c_cfg);
  ESP_RETURN_ON_FALSE(speaker_control_interface, ESP_ERR_NO_MEM, TAG, "play ctrl_if alloc fail");

  speaker_gpio_interface = audio_codec_new_gpio();
  ESP_RETURN_ON_FALSE(speaker_gpio_interface, ESP_ERR_NO_MEM, TAG, "gpio_if alloc fail");

  es8311_codec_cfg_t es8311_cfg = {
      .ctrl_if = speaker_control_interface,
      .gpio_if = speaker_gpio_interface,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
      .master_mode = false,
      .use_mclk = (AV_ESP_I2S_MCLK_GPIO >= 0),
      .pa_pin = AV_ESP_PA_GPIO,
      .pa_reverted = false,
      .no_dac_ref = true,
      .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
  };
  speaker_codec_interface = es8311_codec_new(&es8311_cfg);
  ESP_RETURN_ON_FALSE(speaker_codec_interface, ESP_ERR_NO_MEM, TAG, "play codec_if alloc fail");

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = speaker_codec_interface,
      .data_if = i2s_data_interface,
  };
  speaker_codec_device = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(speaker_codec_device, ESP_ERR_NO_MEM, TAG, "play codec_dev alloc fail");

  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = 16,
      .channel = AV_ESP_CODEC_CHANNELS,
      .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
      .sample_rate = SPK_SAMPLE_RATE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker_codec_device, &sample_cfg) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, TAG, "play codec_dev open fail");
  ESP_RETURN_ON_FALSE(
      esp_codec_dev_set_out_vol(speaker_codec_device, AV_ESP_ES8311_OUT_VOL) == ESP_CODEC_DEV_OK,
      ESP_FAIL, TAG, "set output vol fail");

  ESP_LOGI(TAG, "ES8311 speaker codec opened: sr=%d ch=%d vol=%d", SPK_SAMPLE_RATE,
           AV_ESP_CODEC_CHANNELS, AV_ESP_ES8311_OUT_VOL);
  return ESP_OK;
}

// ---- I2C 总线反初始化 ----
static void _i2c_close(void) {
  if (audio_i2c_bus) {
    i2c_del_master_bus(audio_i2c_bus);
    audio_i2c_bus = NULL;
    ESP_LOGI(TAG, "i2c closed");
  }
}

// ---- 录音 Codec 反初始化 ----
static void _microphone_close(void) {
  if (microphone_codec_device) {
    esp_codec_dev_close(microphone_codec_device);
    esp_codec_dev_delete(microphone_codec_device);
    microphone_codec_device = NULL;
  }
  if (microphone_codec_interface) {
    audio_codec_delete_codec_if(microphone_codec_interface);
    microphone_codec_interface = NULL;
  }
  if (microphone_control_interface) {
    audio_codec_delete_ctrl_if(microphone_control_interface);
    microphone_control_interface = NULL;
  }
  ESP_LOGI(TAG, "record codec closed");
}

// ---- 播放 Codec 反初始化 ----
static void _speaker_close(void) {
  if (speaker_codec_device) {
    esp_codec_dev_close(speaker_codec_device);
    esp_codec_dev_delete(speaker_codec_device);
    speaker_codec_device = NULL;
  }
  if (speaker_codec_interface) {
    audio_codec_delete_codec_if(speaker_codec_interface);
    speaker_codec_interface = NULL;
  }
  if (speaker_control_interface) {
    audio_codec_delete_ctrl_if(speaker_control_interface);
    speaker_control_interface = NULL;
  }
  if (speaker_gpio_interface) {
    audio_codec_delete_gpio_if(speaker_gpio_interface);
    speaker_gpio_interface = NULL;
  }
  ESP_LOGI(TAG, "play codec closed");
}

// ---- I2S 通道反初始化 ----
static void _i2s_close(void) {
  if (i2s_data_interface) {
    audio_codec_delete_data_if(i2s_data_interface);
    i2s_data_interface = NULL;
  }
  if (i2s_microphone_channel) {
    i2s_del_channel(i2s_microphone_channel);
    i2s_microphone_channel = NULL;
  }
  if (i2s_speaker_channel) {
    i2s_del_channel(i2s_speaker_channel);
    i2s_speaker_channel = NULL;
  }
  ESP_LOGI(TAG, "i2s closed");
}

// ---- 创建回声消除器实例与相关缓冲 ----
static esp_err_t _aec_open(void) {
  if (aec_handle) {
    return ESP_OK;
  }

  aec_handle = afe_aec_create(AV_ESP_AEC_INPUT_FORMAT, AV_ESP_AEC_FILTER_LENGTH, AFE_TYPE_VC,
                              AV_ESP_AEC_MODE);
  if (!aec_handle) {
    ESP_LOGE(TAG, "afe_aec_create failed");
    return ESP_FAIL;
  }

  aec_chunk_samples = afe_aec_get_chunksize(aec_handle);
  if (aec_chunk_samples <= 0) {
    ESP_LOGE(TAG, "invalid AEC chunk=%d", aec_chunk_samples);
    _aec_close();
    return ESP_FAIL;
  }

  // 采集缓冲按 20ms 立体声分配，AEC chunk 不能超过它，否则单次读取会溢出。
  if ((size_t)aec_chunk_samples * AV_ESP_CODEC_CHANNELS * sizeof(int16_t) >
      sizeof(capture_stereo_buffer)) {
    ESP_LOGE(TAG, "AEC chunk %d too large for capture buffer", aec_chunk_samples);
    _aec_close();
    return ESP_FAIL;
  }

  // AFE 要求 16 字节对齐；工作缓冲与参考环直接从 PSRAM 申请。
  aec_microphone_buffer =
      heap_caps_aligned_calloc(AV_ESP_AEC_BUFFER_ALIGN, (size_t)aec_chunk_samples, sizeof(int16_t),
                               AV_ESP_AEC_BUFFER_CAPS);
  aec_reference_buffer =
      heap_caps_aligned_calloc(AV_ESP_AEC_BUFFER_ALIGN, (size_t)aec_chunk_samples, sizeof(int16_t),
                               AV_ESP_AEC_BUFFER_CAPS);
  aec_interleaved_buffer =
      heap_caps_aligned_calloc(AV_ESP_AEC_BUFFER_ALIGN, (size_t)aec_chunk_samples * 2,
                               sizeof(int16_t), AV_ESP_AEC_BUFFER_CAPS);
  aec_output_buffer =
      heap_caps_aligned_calloc(AV_ESP_AEC_BUFFER_ALIGN, (size_t)aec_chunk_samples, sizeof(int16_t),
                               AV_ESP_AEC_BUFFER_CAPS);
  aec_reference_ring_buffer =
      heap_caps_aligned_calloc(AV_ESP_AEC_BUFFER_ALIGN, AV_ESP_AEC_REFERENCE_RING_SAMPLES,
                               sizeof(int16_t), AV_ESP_AEC_BUFFER_CAPS);
  if (!aec_microphone_buffer || !aec_reference_buffer || !aec_interleaved_buffer ||
      !aec_output_buffer || !aec_reference_ring_buffer) {
    ESP_LOGE(TAG, "AEC buffer alloc failed");
    _aec_close();
    return ESP_ERR_NO_MEM;
  }

  aec_reference_ring_buffer_head = 0;
  aec_reference_ring_buffer_tail = 0;
  aec_frame_accumulated = 0;

  ESP_LOGI(TAG, "AEC ready: fmt=%s mode=%s filter=%d chunk=%d (%d ms) buffers=PSRAM",
           AV_ESP_AEC_INPUT_FORMAT, AV_ESP_AEC_MODE == AFE_MODE_LOW_COST ? "LOW_COST" : "HIGH_PERF",
           AV_ESP_AEC_FILTER_LENGTH, aec_chunk_samples, aec_chunk_samples * 1000 / MIC_SAMPLE_RATE);
  return ESP_OK;
}

// ---- 释放回声消除器实例与相关缓冲 ----
static void _aec_close(void) {
  if (aec_handle) {
    afe_aec_destroy(aec_handle);
    aec_handle = NULL;
  }
  if (aec_microphone_buffer) {
    heap_caps_free(aec_microphone_buffer);
    aec_microphone_buffer = NULL;
  }
  if (aec_reference_buffer) {
    heap_caps_free(aec_reference_buffer);
    aec_reference_buffer = NULL;
  }
  if (aec_interleaved_buffer) {
    heap_caps_free(aec_interleaved_buffer);
    aec_interleaved_buffer = NULL;
  }
  if (aec_output_buffer) {
    heap_caps_free(aec_output_buffer);
    aec_output_buffer = NULL;
  }
  if (aec_reference_ring_buffer) {
    heap_caps_free(aec_reference_ring_buffer);
    aec_reference_ring_buffer = NULL;
  }
  aec_chunk_samples = 0;
  aec_frame_accumulated = 0;
  ESP_LOGI(TAG, "AEC closed");
}

// MIC 采集任务：从 I2S RX 读取立体声帧，下混为单声道；
// 若回声消除器可用则经回声消除后再上抛，否则直接上抛。
static void _get_microphone_audio_stream(void *arg) {
  (void)arg;

  uint32_t frame_count = 0;
  const size_t output_frame_samples = MIC_FRAME_BYTES / sizeof(int16_t);

  if (aec_handle) {
    const int chunk_samples = aec_chunk_samples;
    const size_t read_bytes = (size_t)chunk_samples * AV_ESP_CODEC_CHANNELS * sizeof(int16_t);
    aec_frame_accumulated = 0;

    while (audio_task_running) {
      size_t bytes_read = 0;
      esp_err_t err = i2s_channel_read(i2s_microphone_channel, capture_stereo_buffer, read_bytes,
                                       &bytes_read, AV_ESP_AUDIO_READ_TIMEOUT_MS);
      if (!audio_task_running) {
        break;
      }
      if (err == ESP_ERR_TIMEOUT) {
        continue;
      }
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
        rtos_delay_milliseconds(10);
        continue;
      }
      if (bytes_read != read_bytes) {
        ESP_LOGW(TAG, "drop partial AEC frame: %u/%u", (unsigned)bytes_read, (unsigned)read_bytes);
        continue;
      }

      // 立体声下混单声道，得到近端麦克风信号
      for (int i = 0; i < chunk_samples; ++i) {
        int32_t mixed = (int32_t)capture_stereo_buffer[i * AV_ESP_CODEC_CHANNELS] +
                        (int32_t)capture_stereo_buffer[i * AV_ESP_CODEC_CHANNELS + 1];
        aec_microphone_buffer[i] = (int16_t)(mixed / AV_ESP_CODEC_CHANNELS);
      }
      // 取出与本帧对应的播放参考信号
      portENTER_CRITICAL(&aec_reference_ring_buffer_lock);
      for (int i = 0; i < chunk_samples; ++i) {
        if (aec_reference_ring_buffer_tail == aec_reference_ring_buffer_head) {
          aec_reference_buffer[i] = 0;
        } else {
          aec_reference_buffer[i] = aec_reference_ring_buffer[aec_reference_ring_buffer_tail];
          aec_reference_ring_buffer_tail =
              (aec_reference_ring_buffer_tail + 1) % AV_ESP_AEC_REFERENCE_RING_SAMPLES;
        }
      }
      portEXIT_CRITICAL(&aec_reference_ring_buffer_lock);
      // 交织为 AEC 输入格式 "MR"：[M0,R0,M1,R1,...]
      for (int i = 0; i < chunk_samples; ++i) {
        aec_interleaved_buffer[2 * i] = aec_microphone_buffer[i];
        aec_interleaved_buffer[2 * i + 1] = aec_reference_buffer[i];
      }
      // 回声消除：输出为去回声后的近端单声道
      size_t output_bytes = afe_aec_process(aec_handle, aec_interleaved_buffer, aec_output_buffer);
      int output_samples = (int)(output_bytes / sizeof(int16_t));
      if (output_samples <= 0 || output_samples > chunk_samples) {
        output_samples = chunk_samples;  // 兜底
      }

      // 按 20ms 组装上抛帧，保持与上层 MIC_FRAME_MS 口径一致
      for (int i = 0; i < output_samples; ++i) {
        capture_mono_buffer[aec_frame_accumulated++] = aec_output_buffer[i];
        if (aec_frame_accumulated >= output_frame_samples) {
          audio_device_callback_t cb = audio_callback;
          void *user_data = audio_callback_context;
          if (cb && audio_started) {
            cb((const uint8_t *)capture_mono_buffer, MIC_FRAME_BYTES, user_data);
            frame_count++;
            if (frame_count == 1 || frame_count % 500 == 0) {
              ESP_LOGI(TAG, "captured AEC frames=%u", (unsigned)frame_count);
            }
          }
          aec_frame_accumulated = 0;
        }
      }
    }

    audio_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  // AEC 不可用，沿用原始下混直传 ----
  const size_t samples_per_channel = MIC_FRAME_BYTES / sizeof(int16_t);

  while (audio_task_running) {
    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(i2s_microphone_channel, capture_stereo_buffer, AV_ESP_CAPTURE_FRAME_BYTES,
                         &bytes_read, AV_ESP_AUDIO_READ_TIMEOUT_MS);
    if (!audio_task_running) {
      break;
    }
    if (err == ESP_ERR_TIMEOUT) {
      continue;
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
      rtos_delay_milliseconds(10);
      continue;
    }
    if (bytes_read != AV_ESP_CAPTURE_FRAME_BYTES) {
      ESP_LOGW(TAG, "drop partial audio frame: %u/%u", (unsigned)bytes_read,
               (unsigned)AV_ESP_CAPTURE_FRAME_BYTES);
      continue;
    }

    // 立体声 S16LE (LRLR…) → 单声道：取左右声道平均值
    for (size_t i = 0; i < samples_per_channel; ++i) {
      int32_t mixed = (int32_t)capture_stereo_buffer[i * AV_ESP_CODEC_CHANNELS] +
                      (int32_t)capture_stereo_buffer[i * AV_ESP_CODEC_CHANNELS + 1];
      capture_mono_buffer[i] = (int16_t)(mixed / AV_ESP_CODEC_CHANNELS);
    }

    audio_device_callback_t cb = audio_callback;
    void *user_data = audio_callback_context;
    if (cb && audio_started) {
      cb((const uint8_t *)capture_mono_buffer, MIC_FRAME_BYTES, user_data);
      frame_count++;
      if (frame_count == 1 || frame_count % 500 == 0) {
        ESP_LOGI(TAG, "captured audio frames=%u", (unsigned)frame_count);
      }
    }
  }

  audio_task_handle = NULL;
  vTaskDelete(NULL);
}

// ==================== I2S 初始化（STD） ====================

static esp_err_t _i2s_open(void) {
  if (i2s_speaker_channel || i2s_microphone_channel) {
    return ESP_OK;
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AV_ESP_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = SPK_SAMPLE_RATE * SPK_FRAME_MS / 1000;
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_speaker_channel, &i2s_microphone_channel);
  if (err != ESP_OK) {
    i2s_speaker_channel = NULL;
    i2s_microphone_channel = NULL;
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = AV_ESP_I2S_MCLK_GPIO,
              .bclk = AV_ESP_I2S_BCLK_GPIO,
              .ws = AV_ESP_I2S_WS_GPIO,
              .dout = AV_ESP_I2S_DOUT_GPIO,
              .din = AV_ESP_I2S_DIN_GPIO,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };

  if (i2s_speaker_channel) {
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s_speaker_channel, &std_cfg), TAG,
                        "tx std init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_speaker_channel), TAG, "tx enable");
  }
  if (i2s_microphone_channel) {
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s_microphone_channel, &std_cfg), TAG,
                        "rx std init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_microphone_channel), TAG, "rx enable");
  }

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = AV_ESP_I2S_PORT,
      .rx_handle = i2s_microphone_channel,
      .tx_handle = i2s_speaker_channel,
  };
  i2s_data_interface = audio_codec_new_i2s_data(&i2s_cfg);
  ESP_RETURN_ON_FALSE(i2s_data_interface, ESP_ERR_NO_MEM, TAG, "i2s data_if alloc");

  return ESP_OK;
}

// ==================== 录音 Codec（ES8311 BOTH） ====================

static esp_err_t _microphone_open(void) {
  if (microphone_codec_device) {
    return ESP_OK;
  }
  if (!i2s_data_interface || !audio_i2c_bus) {
    return ESP_ERR_INVALID_STATE;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = AV_ESP_I2C_PORT,
      .addr = ES8311_CODEC_DEFAULT_ADDR,
      .bus_handle = audio_i2c_bus,
  };
  microphone_control_interface = audio_codec_new_i2c_ctrl(&i2c_cfg);
  ESP_RETURN_ON_FALSE(microphone_control_interface, ESP_ERR_NO_MEM, TAG,
                      "record ctrl_if alloc fail");

  es8311_codec_cfg_t es8311_cfg = {
      .ctrl_if = microphone_control_interface,
      .gpio_if = speaker_gpio_interface,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
      .master_mode = false,
      .use_mclk = true,
      .pa_pin = AV_ESP_PA_GPIO,
      .pa_reverted = false,
      .no_dac_ref = true,
      .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
  };
  microphone_codec_interface = es8311_codec_new(&es8311_cfg);
  ESP_RETURN_ON_FALSE(microphone_codec_interface, ESP_ERR_NO_MEM, TAG,
                      "record codec_if alloc fail");

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = microphone_codec_interface,
      .data_if = i2s_data_interface,
  };
  microphone_codec_device = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(microphone_codec_device, ESP_ERR_NO_MEM, TAG, "record codec_dev alloc fail");

  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = 16,
      .channel = AV_ESP_CODEC_CHANNELS,
      .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
      .sample_rate = MIC_SAMPLE_RATE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  ESP_RETURN_ON_FALSE(esp_codec_dev_open(microphone_codec_device, &sample_cfg) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, TAG, "record codec_dev open fail");

  esp_err_t gain_err =
      esp_codec_dev_set_in_gain(microphone_codec_device, AV_ESP_ES8311_MIC_GAIN_DB);
  if (gain_err != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "set mic gain to %.1f dB failed (err=%d), using codec default",
             (double)AV_ESP_ES8311_MIC_GAIN_DB, gain_err);
  } else {
    ESP_LOGI(TAG, "ES8311 mic gain set to %.1f dB", (double)AV_ESP_ES8311_MIC_GAIN_DB);
  }

  ESP_LOGI(TAG, "ES8311 mic codec opened: sr=%d ch=%d", MIC_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS);
  return ESP_OK;
}

// ==================== 视频实现 ====================



static int _camera_open(void) {
  esp_video_init_csi_config_t csi_config[] = {
      {
          .sccb_config =
              {
                  .init_sccb = true,
                  .i2c_config =
                      {
                          .port = AV_ESP_CAM_I2C_PORT,
                          .scl_pin = AV_ESP_CAM_I2C_SCL,
                          .sda_pin = AV_ESP_CAM_I2C_SDA,
                      },
                  .freq = AV_ESP_CAM_I2C_FREQ,
              },
          .reset_pin = -1,
          .pwdn_pin = -1,
      },
  };

  if (audio_i2c_bus != NULL) {
    csi_config[0].sccb_config.init_sccb = false;
    csi_config[0].sccb_config.i2c_handle = audio_i2c_bus;
  }

  esp_video_init_config_t cam_cfg = {
      .csi = csi_config,
  };
  esp_video_init(&cam_cfg);

  int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    return -1;
  }

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
    ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
    goto fail;
  }
  ESP_LOGI(TAG, "camera default: %ux%u pixelformat=0x%x", fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat);

  fmt.fmt.pix.pixelformat = AV_ESP_CAM_PIXEL_FORMAT;
  fmt.fmt.pix.width = VIDEO_WIDTH;
  fmt.fmt.pix.height = VIDEO_HEIGHT;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_FMT %s %dx%d failed", AV_ESP_CAM_PIXEL_FORMAT_NAME,
             (int)VIDEO_WIDTH, (int)VIDEO_HEIGHT);
    goto fail;
  }
  ESP_LOGI(TAG,
           "camera set: %ux%u fourcc=%c%c%c%c bytesperline=%u sizeimage=%u requested=%s",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           (char)(fmt.fmt.pix.pixelformat & 0xffU),
           (char)((fmt.fmt.pix.pixelformat >> 8) & 0xffU),
           (char)((fmt.fmt.pix.pixelformat >> 16) & 0xffU),
           (char)((fmt.fmt.pix.pixelformat >> 24) & 0xffU),
           fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage, AV_ESP_CAM_PIXEL_FORMAT_NAME);
           
  camera_width = fmt.fmt.pix.width;
  camera_height = fmt.fmt.pix.height;
  camera_pixel_format = fmt.fmt.pix.pixelformat;
  camera_bytesperline = fmt.fmt.pix.bytesperline;
  camera_sizeimage = fmt.fmt.pix.sizeimage;

  // esp_video CSI may leave these optional V4L2 metadata fields at zero even though it allocates
  // tightly packed RGB565 buffers internally. Derive them only for the requested fixed format.
  if (camera_width == 0 || camera_height == 0 || camera_pixel_format != AV_ESP_CAM_PIXEL_FORMAT ||
      camera_width > UINT32_MAX / 2U) {
    ESP_LOGE(TAG, "invalid camera format: %ux%u fourcc=0x%08x", (unsigned)camera_width,
             (unsigned)camera_height, (unsigned)camera_pixel_format);
    goto fail;
  }
  const uint32_t packed_stride = camera_width * 2U;
  if (camera_height > UINT32_MAX / packed_stride) {
    ESP_LOGE(TAG, "camera frame size overflows: %ux%u", (unsigned)camera_width,
             (unsigned)camera_height);
    goto fail;
  }
  const uint32_t packed_sizeimage = packed_stride * camera_height;
  if (camera_bytesperline == 0) {
    camera_bytesperline = packed_stride;
    ESP_LOGW(TAG, "camera omitted bytesperline; using RGB565 packed stride=%u",
             (unsigned)camera_bytesperline);
  }
  if (camera_sizeimage == 0) {
    camera_sizeimage = packed_sizeimage;
    ESP_LOGW(TAG, "camera omitted sizeimage; using RGB565 packed size=%u",
             (unsigned)camera_sizeimage);
  }
  if (camera_bytesperline != packed_stride || camera_sizeimage < packed_sizeimage) {
    ESP_LOGE(TAG, "unsupported camera layout: stride=%u sizeimage=%u expected>=%u",
             (unsigned)camera_bytesperline, (unsigned)camera_sizeimage,
             (unsigned)packed_sizeimage);
    goto fail;
  }

  struct v4l2_streamparm stream_parm = {0};
  stream_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  stream_parm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
  stream_parm.parm.capture.timeperframe.numerator = 1;
  stream_parm.parm.capture.timeperframe.denominator = VIDEO_FPS;
  if (ioctl(fd, VIDIOC_S_PARM, &stream_parm) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_PARM request %u fps failed", (unsigned)VIDEO_FPS);
    goto fail;
  }

  memset(&stream_parm, 0, sizeof(stream_parm));
  stream_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_G_PARM, &stream_parm) != 0) {
    ESP_LOGE(TAG, "VIDIOC_G_PARM failed");
    goto fail;
  }

  const struct v4l2_fract *time_per_frame = &stream_parm.parm.capture.timeperframe;
  if (time_per_frame->numerator == 0U || time_per_frame->denominator == 0U ||
      time_per_frame->denominator != (uint32_t)VIDEO_FPS * time_per_frame->numerator) {
    ESP_LOGE(TAG, "camera rejected requested frame rate: requested=%u actual=%u/%u fps",
             (unsigned)VIDEO_FPS, (unsigned)time_per_frame->denominator,
             (unsigned)time_per_frame->numerator);
    goto fail;
  }
  ESP_LOGI(TAG, "camera frame rate set: %u/%u fps", (unsigned)time_per_frame->denominator,
           (unsigned)time_per_frame->numerator);

  struct v4l2_requestbuffers req = {0};
  req.count = AV_ESP_CAM_BUF_NUM;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
    goto fail;
  }
  if (req.count == 0 || req.count > AV_ESP_CAM_BUF_NUM) {
    ESP_LOGE(TAG, "unexpected camera buffer count: %u (requested %u)",
             (unsigned)req.count, (unsigned)AV_ESP_CAM_BUF_NUM);
    goto fail;
  }
  camera_buffer_count = req.count;

  for (uint32_t i = 0; i < camera_buffer_count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%lu] failed", (unsigned long)i);
      goto fail;
    }
    camera_buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (camera_buffer[i] == MAP_FAILED) {
      ESP_LOGE(TAG, "mmap[%lu] failed", (unsigned long)i);
      goto fail;
    }
    if (buf.length < camera_sizeimage) {
      ESP_LOGE(TAG, "camera buffer[%lu] too small: length=%u required=%u",
               (unsigned long)i, (unsigned)buf.length, (unsigned)camera_sizeimage);
      goto fail;
    }
    camera_buffer_length = buf.length;

    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF[%lu] failed", (unsigned long)i);
      goto fail;
    }
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
    goto fail;
  }
  ESP_LOGI(TAG,
           "camera opened: fd=%d buf_num=%u buf_len=%u format=%ux%u fourcc=0x%08x "
           "stride=%u sizeimage=%u",
           fd, (unsigned)camera_buffer_count, (unsigned)camera_buffer_length,
           (unsigned)camera_width, (unsigned)camera_height, (unsigned)camera_pixel_format,
           (unsigned)camera_bytesperline, (unsigned)camera_sizeimage);
  return fd;

fail:
  for (uint32_t i = 0; i < camera_buffer_count; i++) {
    if (camera_buffer[i] && camera_buffer[i] != MAP_FAILED) {
      munmap(camera_buffer[i], camera_buffer_length);
      camera_buffer[i] = NULL;
    }
  }
  camera_buffer_count = 0;
  camera_buffer_length = 0;
  camera_width = 0;
  camera_height = 0;
  camera_pixel_format = 0;
  camera_bytesperline = 0;
  camera_sizeimage = 0;
  close(fd);
  esp_video_deinit();
  return -1;
}

static void _camera_close(void) {
  if (camera_fd < 0) {
    return;
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(camera_fd, VIDIOC_STREAMOFF, &type);
  for (uint32_t i = 0; i < camera_buffer_count; i++) {
    if (camera_buffer[i]) {
      munmap(camera_buffer[i], camera_buffer_length);
      camera_buffer[i] = NULL;
    }
  }
  close(camera_fd);
  camera_fd = -1;
  camera_buffer_count = 0;
  camera_buffer_length = 0;
  camera_width = 0;
  camera_height = 0;
  camera_pixel_format = 0;
  camera_bytesperline = 0;
  camera_sizeimage = 0;
  esp_video_deinit();
  ESP_LOGI(TAG, "camera closed");
}

static esp_err_t _jpeg_encoder_init(void) {
  if (camera_width == 0 || camera_height == 0 || camera_pixel_format != AV_ESP_CAM_PIXEL_FORMAT ||
      camera_bytesperline != camera_width * 2U ||
      camera_sizeimage < camera_bytesperline * camera_height) {
    ESP_LOGE(TAG,
             "JPEG input format unavailable or invalid: %ux%u fourcc=0x%08x stride=%u "
             "sizeimage=%u",
             (unsigned)camera_width, (unsigned)camera_height, (unsigned)camera_pixel_format,
             (unsigned)camera_bytesperline, (unsigned)camera_sizeimage);
    return ESP_ERR_INVALID_STATE;
  }

  const jpeg_encode_engine_cfg_t engine_config = {
      .intr_priority = 0,
      .timeout_ms = 100,
      .flags = {.allow_pd = 0},
  };
  if (jpeg_new_encoder_engine(&engine_config, &jpeg_encoder_handle) != ESP_OK ||
      !jpeg_encoder_handle) {
    ESP_LOGE(TAG, "Failed to create hardware JPEG encoder");
    _jpeg_encoder_deinit();
    return ESP_FAIL;
  }

  jpeg_encode_config = (jpeg_encode_cfg_t){
      .height = camera_height,
      .width = camera_width,
      .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
      .image_quality = AV_ESP_JPEG_QUALITY,
      .pixel_reverse = false,
  };
  jpeg_encode_input_size = camera_bytesperline * camera_height;

  jpeg_encode_memory_alloc_cfg_t output_mem_config = {
      .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  jpeg_output_buffer = jpeg_alloc_encoder_mem(AV_ESP_JPEG_OUTPUT_CAPACITY, &output_mem_config,
                                              &jpeg_output_buffer_length);
  if (!jpeg_output_buffer || jpeg_output_buffer_length == 0 ||
      jpeg_output_buffer_length > UINT32_MAX) {
    ESP_LOGE(TAG,
             "JPEG encoder output buffer alloc failed: request=%u actual=%u "
             "internal_dma_free=%u internal_dma_largest=%u spiram_free=%u",
             (unsigned)AV_ESP_JPEG_OUTPUT_CAPACITY, (unsigned)jpeg_output_buffer_length,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _jpeg_encoder_deinit();
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG,
           "JPEG HW encoder ready: %ux%u RGB565 quality=%u input_stride=%u input_size=%u "
           "output_capacity=%u",
           (unsigned)camera_width, (unsigned)camera_height, (unsigned)AV_ESP_JPEG_QUALITY,
           (unsigned)camera_bytesperline, (unsigned)jpeg_encode_input_size,
           (unsigned)jpeg_output_buffer_length);
  return ESP_OK;
}

static void _jpeg_encoder_deinit(void) {
  if (jpeg_encoder_handle) {
    jpeg_del_encoder_engine(jpeg_encoder_handle);
    jpeg_encoder_handle = NULL;
  }
  if (jpeg_output_buffer) {
    free(jpeg_output_buffer);
    jpeg_output_buffer = NULL;
    jpeg_output_buffer_length = 0;
  }
  jpeg_encode_input_size = 0;
  ESP_LOGI(TAG, "JPEG encoder deinit done");
}

static esp_err_t _jpeg_decoder_init(void) {
  if (lcd_video_jpeg_decoder) {
    return ESP_OK;
  }

  const jpeg_decode_engine_cfg_t engine_config = {
      .timeout_ms = 100,
  };
  if (jpeg_new_decoder_engine(&engine_config, &lcd_video_jpeg_decoder) != ESP_OK ||
      !lcd_video_jpeg_decoder) {
    ESP_LOGE(TAG, "Failed to create hardware JPEG decoder");
    return ESP_FAIL;
  }

  lcd_video_jpeg_decode_config = (jpeg_decode_cfg_t){
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
  };
  return ESP_OK;
}

static void _jpeg_decoder_deinit(void) {
  if (lcd_video_jpeg_decoder) {
    jpeg_del_decoder_engine(lcd_video_jpeg_decoder);
    lcd_video_jpeg_decoder = NULL;
  }
}

static void _get_camera_video_stream(void *arg) {
  (void)arg;
  uint32_t encode_fail_count = 0;
  uint32_t frame_count = 0;          // 摄像头实际出帧数（DQBUF 成功且缓冲有效）
  uint32_t encoded_frame_count = 0;  // MJPEG 编码成功的帧数

  // 周期性帧率统计基准：仿照 app_main 的 [STAT] 打印，对计数器做差值换算实测帧率。
  uint64_t stats_last_ms = rtos_get_time();
  uint32_t stats_last_camera = 0;
  uint32_t stats_last_encoded = 0;

  while (video_stream_activate) {
    // --- 1. 从摄像头驱动出队一帧数据 ---
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(camera_fd, VIDIOC_DQBUF, &buf) != 0) {
      if (video_stream_activate) {
        ESP_LOGW(TAG, "VIDIOC_DQBUF failed");
        rtos_delay_milliseconds(10);
      }
      continue;
    }

    if (buf.index >= camera_buffer_count || !camera_buffer[buf.index]) {
      ESP_LOGW(TAG, "invalid camera buffer index=%u count=%u", (unsigned)buf.index,
               (unsigned)camera_buffer_count);
      continue;
    }

    uint8_t *cam_data = camera_buffer[buf.index];
    frame_count++;
    const uint32_t camera_frame_size = camera_bytesperline * camera_height;
    if (buf.bytesused < camera_frame_size || buf.bytesused > camera_buffer_length) {
      ESP_LOGW(TAG,
               "camera frame invalid: frame=%u index=%u bytesused=%u expected>=%u buffer=%u",
               (unsigned)frame_count, (unsigned)buf.index, (unsigned)buf.bytesused,
               (unsigned)camera_frame_size, (unsigned)camera_buffer_length);
    } else {
      // --- 2. JPEG 编码：CSI/ISP 已输出 RGB565，直接送入硬件 JPEG 编码器 ---
      uint32_t jpeg_len = 0;
      const esp_err_t jpeg_ret = jpeg_encoder_process(
          jpeg_encoder_handle, &jpeg_encode_config, cam_data, jpeg_encode_input_size,
          jpeg_output_buffer, (uint32_t)jpeg_output_buffer_length, &jpeg_len);
      if (jpeg_ret == ESP_OK && jpeg_len > 0) {
        encoded_frame_count++;
        // MJPEG 每一帧都是独立关键帧，无需 IDR 对齐
        if (video_callback && video_stream_activate) {
          video_callback(jpeg_output_buffer, jpeg_len, true, TC_IOT_VIDEO_CODEC_MJPEG,
                         video_callback_context);
        }
        if (frame_count == 1) {
          ESP_LOGI(TAG, "first JPEG frame encoded: input=%u output=%u",
                   (unsigned)jpeg_encode_input_size, (unsigned)jpeg_len);
        }
      } else {
        encode_fail_count++;
        if (encode_fail_count <= 5 || (encode_fail_count % 30U) == 0) {
          ESP_LOGW(TAG,
                   "JPEG encode failed: %s(%d) frame=%u fail=%u index=%u input=%u "
                   "output_capacity=%u encoded=%u",
                   esp_err_to_name(jpeg_ret), (int)jpeg_ret, (unsigned)frame_count,
                   (unsigned)encode_fail_count, (unsigned)buf.index,
                   (unsigned)jpeg_encode_input_size, (unsigned)jpeg_output_buffer_length,
                   (unsigned)jpeg_len);
        }
      }
    }

    // --- 3. 将缓冲区归还摄像头驱动，形成循环缓冲 ---
    if (ioctl(camera_fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGW(TAG, "VIDIOC_QBUF failed");
    }
  }

  // 循环退出后不清理硬件资源，相机和编码器已由 av_device_init() 统一初始化，
  // 由 av_device_deinit() 统一回收。手机退房后再次进房可直接复用。
  video_task_handle = NULL;
  vTaskDelete(NULL);
}

static esp_err_t _lcd_open(void) {
  if (lcd_initialized) {
    return ESP_OK;
  }

  esp_err_t err = ESP_OK;
  const char *stage = "backlight PWM configuration";
  const ledc_timer_config_t backlight_timer_config = {
      .speed_mode = AV_ESP_LCD_BACKLIGHT_MODE,
      .duty_resolution = AV_ESP_LCD_BACKLIGHT_DUTY_RES,
      .timer_num = AV_ESP_LCD_BACKLIGHT_TIMER,
      .freq_hz = AV_ESP_LCD_BACKLIGHT_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  err = ledc_timer_config(&backlight_timer_config);
  if (err != ESP_OK) {
    goto fail;
  }

  const ledc_channel_config_t backlight_channel_config = {
      .gpio_num = AV_ESP_LCD_BACKLIGHT_GPIO,
      .speed_mode = AV_ESP_LCD_BACKLIGHT_MODE,
      .channel = AV_ESP_LCD_BACKLIGHT_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = AV_ESP_LCD_BACKLIGHT_TIMER,
      .duty = AV_ESP_LCD_BACKLIGHT_OFF_DUTY,
      .hpoint = 0,
  };
  err = ledc_channel_config(&backlight_channel_config);
  if (err != ESP_OK) {
    goto fail;
  }
  lcd_backlight_initialized = true;

  stage = "MIPI D-PHY LDO acquisition";
  esp_ldo_channel_config_t ldo_config = {
      .chan_id = AV_ESP_LCD_MIPI_PHY_LDO_CHAN,
      .voltage_mv = AV_ESP_LCD_MIPI_PHY_LDO_MV,
  };
  err = esp_ldo_acquire_channel(&ldo_config, &lcd_mipi_ldo);
  if (err != ESP_OK) {
    goto fail;
  }
  stage = "MIPI-DSI bus creation";
  esp_lcd_dsi_bus_config_t dsi_bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
  err = esp_lcd_new_dsi_bus(&dsi_bus_config, &lcd_dsi_bus);
  if (err != ESP_OK) {
    goto fail;
  }
  ESP_LOGI(TAG, "LCD: MIPI-DSI bus ready (lanes=%d, bitrate=%.1f Mbps)",
           dsi_bus_config.num_data_lanes, (double)dsi_bus_config.lane_bit_rate_mbps);

  stage = "MIPI DBI command channel creation";
  esp_lcd_dbi_io_config_t dbi_io_config = EK79007_PANEL_IO_DBI_CONFIG();
  err = esp_lcd_new_panel_io_dbi(lcd_dsi_bus, &dbi_io_config, &lcd_dbi_io);
  if (err != ESP_OK) {
    goto fail;
  }
  ESP_LOGI(TAG, "LCD: MIPI DBI command channel ready");

  stage = "EK79007 panel driver creation";
  esp_lcd_dpi_panel_config_t dpi_config =
      EK79007_1024_600_PANEL_60HZ_CONFIG_CF(LCD_COLOR_FMT_RGB565);
  ek79007_vendor_config_t vendor_config = {
      .mipi_config =
          {
              .dsi_bus = lcd_dsi_bus,
              .dpi_config = &dpi_config,
              .lane_num = 2,
          },
  };
  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = -1,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = AV_ESP_LCD_BITS_PER_PIXEL,
      .vendor_config = &vendor_config,
  };
  err = esp_lcd_new_panel_ek79007(lcd_dbi_io, &panel_config, &lcd_panel);
  if (err != ESP_OK) {
    goto fail;
  }

  stage = "EK79007 software reset";
  err = esp_lcd_panel_reset(lcd_panel);
  if (err != ESP_OK) {
    goto fail;
  }

  stage = "EK79007 initialization command sequence";
  err = esp_lcd_panel_init(lcd_panel);
  if (err != ESP_OK) {
    goto fail;
  }
  ESP_LOGI(TAG, "LCD: EK79007 initialization sequence completed");

  stage = "DCS Display ON command (0x29)";
  ESP_LOGI(TAG, "LCD: sending DCS Display ON (0x%02X)", LCD_CMD_DISPON);
  err = esp_lcd_panel_io_tx_param(lcd_dbi_io, LCD_CMD_DISPON, NULL, 0);
  if (err != ESP_OK) {
    goto fail;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_LOGI(TAG, "LCD: DCS Display ON accepted; waited 20 ms before video output");

  stage = "backlight PWM enable";
  err = ledc_set_duty(AV_ESP_LCD_BACKLIGHT_MODE, AV_ESP_LCD_BACKLIGHT_CHANNEL,
                      AV_ESP_LCD_BACKLIGHT_ON_DUTY);
  if (err != ESP_OK) {
    goto fail;
  }
  err = ledc_update_duty(AV_ESP_LCD_BACKLIGHT_MODE, AV_ESP_LCD_BACKLIGHT_CHANNEL);
  if (err != ESP_OK) {
    goto fail;
  }

  lcd_initialized = true;
  ESP_LOGI(TAG, "LCD: backlight PWM enabled (duty=%u/%u)", AV_ESP_LCD_BACKLIGHT_ON_DUTY,
           AV_ESP_LCD_BACKLIGHT_ON_DUTY);
  ESP_LOGI(TAG,
           "LCD initialized: backlight is on; incoming MJPEG frames will be decoded and displayed");
  return ESP_OK;

fail:
  ESP_LOGE(TAG, "LCD failed at %s: %s", stage, esp_err_to_name(err));
  _lcd_close();
  return err;
}

static void _lcd_close(void) {
  esp_err_t err;

  if (lcd_backlight_initialized) {
    err = ledc_stop(AV_ESP_LCD_BACKLIGHT_MODE, AV_ESP_LCD_BACKLIGHT_CHANNEL,
                    AV_ESP_LCD_BACKLIGHT_OFF_DUTY);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: backlight PWM stop failed: %s", esp_err_to_name(err));
    }
  }

  if (lcd_panel) {
    err = esp_lcd_panel_del(lcd_panel);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: panel deletion failed: %s", esp_err_to_name(err));
    }
    lcd_panel = NULL;
  }
  if (lcd_dbi_io) {
    err = esp_lcd_panel_io_del(lcd_dbi_io);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: DBI IO deletion failed: %s", esp_err_to_name(err));
    }
    lcd_dbi_io = NULL;
  }
  if (lcd_dsi_bus) {
    err = esp_lcd_del_dsi_bus(lcd_dsi_bus);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: DSI bus deletion failed: %s", esp_err_to_name(err));
    }
    lcd_dsi_bus = NULL;
  }
  if (lcd_mipi_ldo) {
    err = esp_ldo_release_channel(lcd_mipi_ldo);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: MIPI D-PHY LDO release failed: %s", esp_err_to_name(err));
    }
    lcd_mipi_ldo = NULL;
  }
  if (lcd_backlight_initialized) {
    err = gpio_reset_pin(AV_ESP_LCD_BACKLIGHT_GPIO);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "LCD: backlight GPIO reset failed: %s", esp_err_to_name(err));
    }
    lcd_backlight_initialized = false;
  }
  lcd_initialized = false;
  ESP_LOGI(TAG, "LCD resources released");
}

static esp_err_t _lcd_video_playout_open(void) {
  if (lcd_video_playout_initialized) {
    return ESP_OK;
  }
  if (!lcd_initialized || lcd_panel == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = ESP_OK;
  lcd_video_refresh_done = xSemaphoreCreateBinary();
  lcd_video_lock = xSemaphoreCreateMutex();
  if (lcd_video_refresh_done == NULL || lcd_video_lock == NULL) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }

  lcd_video_input_rgb565_buffer = heap_caps_aligned_alloc(
      64, AV_ESP_PLAYOUT_INPUT_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (lcd_video_input_rgb565_buffer == NULL) {
    lcd_video_input_rgb565_buffer = heap_caps_aligned_alloc(
        64, AV_ESP_PLAYOUT_INPUT_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (lcd_video_input_rgb565_buffer == NULL) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }

  if (_jpeg_decoder_init() != ESP_OK) {
    err = ESP_FAIL;
    goto fail;
  }

  const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
      .on_color_trans_done = _lcd_video_refresh_done_callback,
  };
  err = esp_lcd_dpi_panel_register_event_callbacks(lcd_panel, &callbacks,
                                                   lcd_video_refresh_done);
  if (err != ESP_OK) {
    goto fail;
  }

  lcd_video_playout_initialized = true;
  ESP_LOGI(TAG, "LCD video ready: JPEG %ux%u at [%u,%u)-[%u,%u)",
           (unsigned)AV_ESP_PLAYOUT_INPUT_H_RES, (unsigned)AV_ESP_PLAYOUT_INPUT_V_RES,
           (unsigned)AV_ESP_LCD_VIDEO_X_START, (unsigned)AV_ESP_LCD_VIDEO_Y_START,
           (unsigned)AV_ESP_LCD_VIDEO_X_END, (unsigned)AV_ESP_LCD_VIDEO_Y_END);
  return ESP_OK;

fail:
  _lcd_video_playout_close();
  return err;
}

static void _lcd_video_playout_close(void) {
  _jpeg_decoder_deinit();
  if (lcd_video_input_rgb565_buffer != NULL) {
    heap_caps_free(lcd_video_input_rgb565_buffer);
    lcd_video_input_rgb565_buffer = NULL;
  }
  if (lcd_video_lock != NULL) {
    vSemaphoreDelete(lcd_video_lock);
    lcd_video_lock = NULL;
  }
  if (lcd_video_refresh_done != NULL) {
    vSemaphoreDelete(lcd_video_refresh_done);
    lcd_video_refresh_done = NULL;
  }
  lcd_video_playout_initialized = false;
}

static esp_err_t _lcd_video_playout_render(const uint8_t *jpeg_data, size_t jpeg_data_length) {
  if (jpeg_data == NULL || jpeg_data_length < 4U || jpeg_data_length > AV_ESP_LCD_MAX_JPEG_BYTES ||
      jpeg_data[0] != 0xffU || jpeg_data[1] != 0xd8U) {
    return ESP_ERR_INVALID_SIZE;
  }

  esp_err_t err = _lcd_video_playout_open();
  if (err != ESP_OK) {
    return err;
  }
  if (xSemaphoreTake(lcd_video_lock, 0) != pdTRUE) {
    ++lcd_video_drop_count;
    return ESP_ERR_TIMEOUT;
  }

  jpeg_decode_picture_info_t picture_info = {0};
  uint32_t decoded_size = 0;
  const esp_err_t info_ret =
      jpeg_decoder_get_info(jpeg_data, (uint32_t)jpeg_data_length, &picture_info);
  const esp_err_t decode_result =
      (info_ret == ESP_OK && picture_info.width == AV_ESP_PLAYOUT_INPUT_H_RES &&
       picture_info.height == AV_ESP_PLAYOUT_INPUT_V_RES)
          ? jpeg_decoder_process(lcd_video_jpeg_decoder, &lcd_video_jpeg_decode_config, jpeg_data,
                                 (uint32_t)jpeg_data_length, lcd_video_input_rgb565_buffer,
                                 AV_ESP_PLAYOUT_INPUT_FRAME_BYTES, &decoded_size)
          : ESP_FAIL;
  if (decode_result != ESP_OK || decoded_size != AV_ESP_PLAYOUT_INPUT_FRAME_BYTES) {
    ++lcd_video_drop_count;
    if (lcd_video_drop_count == 1U || lcd_video_drop_count % 50U == 0U) {
      ESP_LOGW(TAG, "drop MJPEG frame: result=%d size=%ux%u decoded=%u drops=%u", decode_result,
               (unsigned)picture_info.width, (unsigned)picture_info.height, (unsigned)decoded_size,
               (unsigned)lcd_video_drop_count);
    }
    err = ESP_ERR_INVALID_SIZE;
    goto done;
  }

  (void)xSemaphoreTake(lcd_video_refresh_done, 0);
  err = esp_lcd_panel_draw_bitmap(lcd_panel, AV_ESP_LCD_VIDEO_X_START,
                                  AV_ESP_LCD_VIDEO_Y_START, AV_ESP_LCD_VIDEO_X_END,
                                  AV_ESP_LCD_VIDEO_Y_END, lcd_video_input_rgb565_buffer);
  if (err == ESP_OK &&
      xSemaphoreTake(lcd_video_refresh_done, pdMS_TO_TICKS(AV_ESP_LCD_REFRESH_TIMEOUT_MS)) !=
          pdTRUE) {
    err = ESP_ERR_TIMEOUT;
  }
  if (err != ESP_OK) {
    ++lcd_video_drop_count;
    ESP_LOGW(TAG, "LCD refresh failed: %s", esp_err_to_name(err));
  }

done:
  xSemaphoreGive(lcd_video_lock);
  return err;
}

static bool IRAM_ATTR _lcd_video_refresh_done_callback(esp_lcd_panel_handle_t panel,
                                                       esp_lcd_dpi_panel_event_data_t *edata,
                                                       void *user_ctx) {
  (void)panel;
  (void)edata;

  BaseType_t higher_priority_task_woken = pdFALSE;
  if (user_ctx != NULL) {
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &higher_priority_task_woken);
  }
  return higher_priority_task_woken == pdTRUE;
}