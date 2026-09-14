// Copyright (c) 2026 Tencent. All rights reserved.

// bk7258 硬件音视频采集实现
// 使用 bk_voice_service + bk_camera_ctlr API。

#include <components/bk_camera_ctlr.h>
#include <components/bk_voice_read_service.h>
#include <components/bk_voice_read_service_types.h>
#include <components/bk_voice_service.h>
#include <components/bk_voice_service_types.h>
#include <components/bk_voice_write_service.h>
#include <components/bk_voice_write_service_types.h>
#include <components/dvp_camera.h>
#include <components/log.h>
#include <components/media_types.h>
#include <components/system.h>
#include <driver/gpio.h>
#include <driver/h264_types.h>
#include <driver/jpeg_enc.h>
#include <modules/wifi.h>
#include <os/os.h>
#include <string.h>

#include "av_device.h"
#include "components/bk_video_pipeline/bk_video_pipeline.h"
#include "frame_buffer.h"
#include "gpio_driver.h"
#include "modules/jpeg_enc_sw.h"
#include "modules/wifi_types.h"

#if CONFIG_FLASH
#  include <driver/flash.h>
#endif

#define TAG "av_bk"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

// ==================== 配置参数 ====================
//
// 音频 / 视频参数（采样率 / 声道 / 帧长 / 分辨率 / 帧率）集中在 iot_demo_config.h，
// 由 av_device.h 间接 include；这里只放与本 .c 强相关的硬件常量
// （DMA 对齐 / GPIO 编号等）。

#define DMA_GUARD_SIZE 64
#define DMA_ALIGN_BITS 5
#define DMA_ALIGN_MASK ((1 << DMA_ALIGN_BITS) - 1)

#ifndef GPIO_INVALID_ID
#  define GPIO_INVALID_ID (0xFF)
#endif

#ifdef CONFIG_DVP_CTRL_POWER_GPIO_ID
#  define DVP_POWER_PIN CONFIG_DVP_CTRL_POWER_GPIO_ID
#else
#  define DVP_POWER_PIN GPIO_INVALID_ID
#endif
#define DVP_RST_PIN GPIO_28

// ==================== 内部状态 ====================
//
// bk7258 单设备：每路（audio / video）至多 1 个监听者。
// 多路监听如有需求由调用方自己 fan-out（IoT 场景一般只有 1 个 av_sender）。

typedef struct {
  audio_device_callback_t cb;
  void *user_data;
} audio_listener_t;

typedef struct {
  video_device_callback_t cb;
  void *user_data;
} video_listener_t;

static audio_listener_t s_audio_listener = {NULL, NULL};
static video_listener_t s_video_listener = {NULL, NULL};
static bool s_audio_started = false;
static bool s_video_started = false;
static bool s_inited = false;

// bk7258 视频编码支持 h264（IMAGE_H264） 和 mjpeg(IMAGE_MJPEG) 两种格式
static image_format_t s_video_codec = IMAGE_H264;

static voice_handle_t s_voice_handle = NULL;
static voice_read_handle_t s_voice_read_handle = NULL;
static voice_write_handle_t s_voice_write_handle = NULL;

typedef struct {
  bk_camera_ctlr_handle_t handle;
} video_info_t;

static video_info_t *s_video_info = NULL;

// ==================== Frame buffer ring pool（中断安全，仅 H264）====================
//
// 背景：BK DVP 驱动会在硬件中断上下文调用 .malloc / .complete 回调。
//   开启 CONFIG_MEM_DEBUG=y 后，psram_malloc_debug 在中断上下文会 assert
//   （"Error: [_camera_frame_malloc] line(519). malloc_risk." +
//   psram_malloc_debug:254）。即使关掉调试，在中断里调 psram_malloc
//   也本质不安全（内部 vTaskSuspendAll/ taskENTER_CRITICAL 在 ISR
//   里未定义，还会引发之前分析过的 SMP PRIMASK 漂移 bug）。
//
// 方案：启动时在任务上下文预分配 N 个 frame slot（PSRAM），中断里只做 O(1) 原子
//   slot 申领/归还，完全不碰 heap_4。
//
#define YUV_FRAME_SIZE (VIDEO_WIDTH * VIDEO_HEIGHT * 2)

// 视频帧大小：取 H264 与 JPEG 单帧预算的较大者，
#ifndef CONFIG_JPEG_FRAME_SIZE
#  define CONFIG_JPEG_FRAME_SIZE 102400
#endif
#define VIDEO_FRAME_MAX_SIZE                                                      \
  ((CONFIG_H264_FRAME_SIZE) > (CONFIG_JPEG_FRAME_SIZE) ? (CONFIG_H264_FRAME_SIZE) \
                                                       : (CONFIG_JPEG_FRAME_SIZE))

// 视频帧专用 pool
#define FRAME_POOL_SIZE 3
#define FRAME_PAYLOAD_MAX_SIZE VIDEO_FRAME_MAX_SIZE
#define FRAME_SLOT_TOTAL_SIZE \
  (sizeof(frame_buffer_t) + (1 << DMA_ALIGN_BITS) + FRAME_PAYLOAD_MAX_SIZE + DMA_GUARD_SIZE)

typedef struct {
  // 必须首位，供 header 地址 -> slot 地址反查
  frame_buffer_t header;
  uint8_t payload[FRAME_SLOT_TOTAL_SIZE - sizeof(frame_buffer_t)] __attribute__((aligned(32)));
  volatile uint8_t in_use;
} frame_slot_t;

// H264 帧 slot 存储区（PSRAM）。仅在 _video_open 里一次性分配，_video_close 释放。
static frame_slot_t *s_frame_pool = NULL;
// 分配失败计数（中断里 slot 全占时丢帧，中断不阻塞）。
static volatile uint32_t s_frame_pool_alloc_fail = 0;

// ==================== YUV 抓拍 buffer pool（与 H264 pool 隔离）====================
// 多 YUV buffer 轮转避免 DMA busy 丢帧。2 个即满足双缓冲需求，用 3 个留余量。
#define CAPTURE_SLOT_PAYLOAD_SIZE YUV_FRAME_SIZE
#define CAPTURE_SLOT_TOTAL_SIZE \
  (sizeof(frame_buffer_t) + (1 << DMA_ALIGN_BITS) + CAPTURE_SLOT_PAYLOAD_SIZE + DMA_GUARD_SIZE)

// YUV buffer 数量：2 个即可满足 DVP DMA 双缓冲需求（一个在传输，一个空闲可分配）
#define YUV_CAPTURE_POOL_SIZE 3

typedef struct {
  frame_buffer_t header;
  uint8_t payload[CAPTURE_SLOT_TOTAL_SIZE - sizeof(frame_buffer_t)] __attribute__((aligned(32)));
  volatile uint8_t in_use;
} capture_slot_t;

// YUV 帧 buffer pool（PSRAM）。_video_open 分配，_video_close 释放。
static capture_slot_t *s_capture_pool = NULL;

// ==================== YUV 帧抓拍（云存事件图片）====================

#define JPEG_OUTPUT_BUF_SIZE (150 * 1024)
#define JPEG_QUALITY_FACTOR 90

static volatile bool s_capture_request = false;
static uint8_t *s_capture_yuv_buf = NULL;
static uint32_t s_capture_yuv_len = 0;
static beken_semaphore_t s_capture_sem = NULL;
static bool s_jpeg_enc_inited = false;
static uint8_t *s_jpeg_output_buf = NULL;

typedef struct {
  uint32_t count;
  uint32_t fail_count;
  uint32_t last_write_ms;
  uint32_t total_interval_ms;
  uint32_t min_interval_ms;
  uint32_t max_interval_ms;
  uint32_t max_cost_ms;
} playout_diag_t;

typedef struct {
  uint32_t start_ms;
  uint32_t first_frame_ms;
  uint32_t first_key_ms;
  uint32_t frame_count;
  uint32_t key_count;
  uint32_t request_idr_count;
} video_diag_t;

static playout_diag_t s_playout_diag;
static video_diag_t s_video_diag;

static bool s_voice_service_opened = false;

// ==================== 前置声明 ====================

static int _audio_upward_callback(unsigned char *data, unsigned int len, void *args);
static bk_err_t _voice_service_open(void);
static bk_err_t _voice_service_close(void);
static void _dvp_frame_complete(image_format_t format, frame_buffer_t *frame, int result);
static frame_buffer_t *_camera_frame_malloc(image_format_t format, uint32_t size);
static void _camera_frame_free(frame_buffer_t *frame);
static bk_err_t _video_open(void);
static bk_err_t _video_close(void);
static inline frame_fps_t _video_fps_to_frame_fps(int fps);

// ==================== 公开接口 ====================

void av_device_init(const av_device_config_t *cfg) {
  // bk7258 不需要 file_path，硬件参数取自 iot_demo_config.h
  (void)cfg;
  if (s_inited) {
    return;
  }
  s_audio_listener.cb = NULL;
  s_audio_listener.user_data = NULL;
  s_video_listener.cb = NULL;
  s_video_listener.user_data = NULL;
  s_audio_started = false;
  s_video_started = false;
  s_voice_service_opened = false;

  memset(&s_playout_diag, 0, sizeof(s_playout_diag));
  s_playout_diag.min_interval_ms = 0xFFFFFFFF;

  bk_err_t ret = _voice_service_open();
  if (ret != BK_OK) {
    bk_printf("[bk_av] WARNING: voice service init failed\n");
  }
  s_inited = true;
}

void av_device_deinit(void) {
  if (!s_inited) {
    return;
  }

  if (s_audio_started) {
    s_audio_started = false;
    if (s_voice_read_handle) {
      bk_voice_read_stop(s_voice_read_handle);
    }
  }

  if (s_video_started) {
    s_video_started = false;
    _video_close();
  }

  s_audio_listener.cb = NULL;
  s_audio_listener.user_data = NULL;
  s_video_listener.cb = NULL;
  s_video_listener.user_data = NULL;
  _voice_service_close();
  s_inited = false;
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  if (!callback) {
    return -1;
  }

  if (s_audio_listener.cb && s_audio_listener.cb == callback &&
      s_audio_listener.user_data == user_data) {
    return 0;
  }

  if (s_audio_listener.cb) {
    bk_printf("[bk_av] audio listener already occupied\n");
    return -1;
  }

  if (!s_voice_service_opened) {
    bk_printf("[bk_av] voice service not opened, cannot start audio\n");
    return -1;
  }

  s_audio_listener.cb = callback;
  s_audio_listener.user_data = user_data;

  if (!s_audio_started) {
    bk_err_t ret = bk_voice_read_start(s_voice_read_handle);
    if (ret != BK_OK) {
      bk_printf("[bk_av] voice read start failed: %d\n", (int)ret);
      s_audio_listener.cb = NULL;
      s_audio_listener.user_data = NULL;
      return -1;
    }

    s_audio_started = true;
    bk_printf("[bk_av] audio capture started\n");
  }
  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  if (!s_audio_listener.cb || s_audio_listener.cb != callback ||
      s_audio_listener.user_data != user_data) {
    return;
  }
  s_audio_listener.cb = NULL;
  s_audio_listener.user_data = NULL;
  if (s_audio_started) {
    s_audio_started = false;
    if (s_voice_read_handle) {
      bk_voice_read_stop(s_voice_read_handle);
    }
    bk_printf("[bk_av] audio capture stopped\n");
  }
}

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  if (!callback) {
    return -1;
  }
  if (s_video_listener.cb && s_video_listener.cb == callback &&
      s_video_listener.user_data == user_data) {
    return 0;
  }
  if (s_video_listener.cb) {
    bk_printf("[bk_av] video listener already occupied\n");
    return -1;
  }

  memset(&s_video_diag, 0, sizeof(s_video_diag));
  s_video_listener.cb = callback;
  s_video_listener.user_data = user_data;

  if (!s_video_started) {
    bk_err_t ret = _video_open();
    if (ret != BK_OK) {
      s_video_listener.cb = NULL;
      s_video_listener.user_data = NULL;
      return -1;
    }
    s_video_started = true;
    s_video_diag.start_ms = (uint32_t)rtos_get_time();
    bk_printf("[bk_av][VID] video capture started: %ux%u\n", VIDEO_WIDTH, VIDEO_HEIGHT);
  }
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  if (!s_video_listener.cb || s_video_listener.cb != callback ||
      s_video_listener.user_data != user_data) {
    return;
  }
  s_video_listener.cb = NULL;
  s_video_listener.user_data = NULL;
  if (s_video_started) {
    s_video_started = false;
    bk_printf(
        "[bk_av][VID] stop summary: frames=%u keys=%u first_frame_delay=%u first_key_delay=%u "
        "request_idr=%u\n",
        s_video_diag.frame_count, s_video_diag.key_count,
        (s_video_diag.first_frame_ms > 0 && s_video_diag.start_ms > 0)
            ? (s_video_diag.first_frame_ms - s_video_diag.start_ms)
            : 0,
        (s_video_diag.first_key_ms > 0 && s_video_diag.start_ms > 0)
            ? (s_video_diag.first_key_ms - s_video_diag.start_ms)
            : 0,
        s_video_diag.request_idr_count);
    _video_close();
    bk_printf("[bk_av] video capture stopped\n");
  }
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  if (!s_voice_service_opened || !data || len == 0 || !s_voice_write_handle) {
    return;
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
    bk_printf("[bk_av][SPK] first playout frame: len=%u handle=%p\n", len, s_voice_write_handle);
  }

  uint32_t write_start_ms = now_ms;
  bk_err_t ret = bk_voice_write_frame_data(s_voice_write_handle, (char *)(uintptr_t)data, len);
  uint32_t cost_ms = (uint32_t)rtos_get_time() - write_start_ms;
  s_playout_diag.count++;

  if (cost_ms > s_playout_diag.max_cost_ms) {
    s_playout_diag.max_cost_ms = cost_ms;
  }

  if (ret != (bk_err_t)len) {
    s_playout_diag.fail_count++;
    if (s_playout_diag.fail_count <= 10 || s_playout_diag.fail_count % 100 == 0) {
      bk_printf("[bk_av][SPK] write fail #%u: ret=%d expected=%u cost=%u\n",
                s_playout_diag.fail_count, (int)ret, len, cost_ms);
    }
  }

  if (cost_ms >= 5 && (s_playout_diag.count <= 10 || s_playout_diag.count % 100 == 0)) {
    bk_printf("[bk_av][SPK] slow write #%u: len=%u cost=%u ret=%d\n", s_playout_diag.count, len,
              cost_ms, (int)ret);
  }

  if (s_playout_diag.count <= 5 || s_playout_diag.count % 5000 == 0) {
    bk_printf(
        "[bk_av][SPK] stat: count=%u fail=%u avg_interval=%u min=%u max=%u max_cost=%u "
        "last_ret=%d\n",
        s_playout_diag.count, s_playout_diag.fail_count,
        (s_playout_diag.count > 1) ? (s_playout_diag.total_interval_ms / (s_playout_diag.count - 1))
                                   : 0,
        (s_playout_diag.min_interval_ms == 0xFFFFFFFF) ? 0 : s_playout_diag.min_interval_ms,
        s_playout_diag.max_interval_ms, s_playout_diag.max_cost_ms, (int)ret);
  }
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  // bk7258 当前不渲染远端视频；保留 stub 以对齐 Linux 接口。
  (void)data;
  (void)len;
}

void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  // bk7258 当前不渲染远端视频；保留 stub 以对齐 Linux 接口。
  (void)data;
  (void)len;
}

void av_device_request_idr(void) {
  if (!s_video_started || !s_video_info || !s_video_info->handle) {
    bk_printf("[bk_av][VID] request_idr ignored: started=%d info=%p handle=%p\n", s_video_started,
              s_video_info, s_video_info ? s_video_info->handle : NULL);
    return;
  }

  if (s_video_codec == IMAGE_H264) {
    s_video_diag.request_idr_count++;
    bk_printf("[bk_av][VID] request_idr #%u: since_start=%u ms frames=%u keys=%u handle=%p\n",
              s_video_diag.request_idr_count,
              (s_video_diag.start_ms > 0) ? ((uint32_t)rtos_get_time() - s_video_diag.start_ms) : 0,
              s_video_diag.frame_count, s_video_diag.key_count, s_video_info->handle);
    bk_dvp_h264_idr_reset(s_video_info->handle);
  } else {
    // MJPEG 每帧都是独立 JPEG（关键帧），无需 IDR 重置。
    bk_printf("[bk_av][VID] request_idr ignored: MJPEG has no inter-frame dependency\n");
  }
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

// ==================== 音频内部实现 ====================

static bk_err_t _voice_service_open(void) {
  if (s_voice_service_opened) {
    return BK_OK;
  }

  voice_cfg_t voice_cfg = VOICE_BY_ONBOARD_MIC_SPK_CFG_DEFAULT();

  // MIC 采集（硬件 AEC 模式下使用双通道 ADC：L=MIC 信号，R=DAC 回环参考）
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.sample_rate = MIC_SAMPLE_RATE;
  // 双通道 ADC：L=MIC, R=DAC loopback
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.chl_num = 2;
  voice_cfg.mic_cfg.onboard_mic_cfg.frame_size = MIC_FRAME_BYTES;
  voice_cfg.mic_cfg.onboard_mic_cfg.out_block_size = MIC_FRAME_BYTES;
  voice_cfg.mic_cfg.onboard_mic_cfg.out_block_num = 1;
  // MIC 多路输出给 voice_read
  voice_cfg.mic_cfg.onboard_mic_cfg.multi_out_port_num = 1;
  // 硬件 AEC 回声消除更强，MIC 增益保守些减少耦合
  // 0 dB
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.dig_gain = 0x2D;
  voice_cfg.mic_cfg.onboard_mic_cfg.adc_cfg.ana_gain = 0x08;
  voice_cfg.mic_cfg.onboard_mic_cfg.task_stack = 4096;

  // SPK 播放：48kHz / 单声道 / 40ms（3840 字节/帧）。
  // 硬件 AEC 模式下 SPK 不需要再多路输出（参考信号走 ADC 回环），multi_out_port_num=0 即可。
  voice_cfg.spk_cfg.onboard_spk_cfg.sample_rate = SPK_SAMPLE_RATE;
  voice_cfg.spk_cfg.onboard_spk_cfg.chl_num = SPK_CHANNELS;
  voice_cfg.spk_cfg.onboard_spk_cfg.frame_size = SPK_FRAME_BYTES;
  // pool 按 8 帧 / 3 帧 / 0.5 帧的水位关系（容量 / 起播阈值 / 暂停阈值）。
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_length = SPK_FRAME_BYTES * 8;
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_play_thold = SPK_FRAME_BYTES * 3;
  voice_cfg.spk_cfg.onboard_spk_cfg.pool_pause_thold = SPK_FRAME_BYTES / 2;
  // 硬件 AEC 不需要 SPK 多路输出
  voice_cfg.spk_cfg.onboard_spk_cfg.multi_out_port_num = 0;
  voice_cfg.spk_cfg.onboard_spk_cfg.dig_gain = 0x1F;
  voice_cfg.spk_cfg.onboard_spk_cfg.ana_gain = 0x07;
  voice_cfg.spk_cfg.onboard_spk_cfg.task_stack = 8192;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_ctrl_en = true;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_ctrl_gpio = GPIO_50;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_on_level = 1;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_on_delay = 10;
  voice_cfg.spk_cfg.onboard_spk_cfg.pa_off_delay = 30;

  // AEC 回声消除：硬件回采模式
  // 相比软件回采（SPK 多路输出作为参考信号），硬件回采通过 ADC 双通道直接采 DAC 回环，
  // 多 ~10 dB 回声消除深度、路径更短更稳定，且不额外消耗 CPU。
  voice_cfg.aec_en = true;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.mode = AEC_MODE_HARDWARE;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_only_output = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.fs = MIC_SAMPLE_RATE;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.init_flags = 0x1f;
  voice_cfg.aec_cfg.aec_alg_cfg.dual_ch = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.task_stack = 4096;
  voice_cfg.aec_cfg.aec_alg_cfg.out_block_size = MIC_FRAME_BYTES;

  // 硬件回采路径短，ec_depth / delay_points 可以用保守值省 CPU
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_depth = 0x1A;
  // 两级滤波
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ec_filter = 0x3;
  // 硬件回采无需延迟补偿
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.delay_points = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ref_scale = 0;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.TxRxThr = 30;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.TxRxFlr = 6;

  // 降噪：传统模式（AI 模式更耗 CPU，嵌入式优先 NS_TRADITION）
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_type = NS_TRADITION;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_level = 5;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_para = 1;
  voice_cfg.aec_cfg.aec_alg_cfg.aec_cfg.ns_filter = 0x3;

  // DRC / 音量
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

  // 用 do {...} while (0) 把所有"按顺序初始化、任一失败即整体回滚"的步骤
  // 串成一段线性流程；任何一步失败就 break 出循环，统一在循环外按 opened 标志
  // 决定是返回 BK_OK 还是调 _voice_service_close 兜底回收。
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
    // 16KB 留足 FDK AAC psy_main 深栈调用 margin，否则 HardFault。
    voice_read_cfg.task_stack = 1024 * 16;
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

  bk_printf("[bk_av] voice service opened (MIC ready, SPK active)\n");
  return BK_OK;
}

static bk_err_t _voice_service_close(void) {
  if (!s_voice_service_opened) {
    return BK_OK;
  }
  s_voice_service_opened = false;
  s_audio_started = false;

  bk_printf(
      "[bk_av][SPK] stop summary: count=%u fail=%u avg_interval=%u min=%u max=%u "
      "max_cost=%u\n",
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
  bk_printf("[bk_av] voice service closed\n");
  return BK_OK;
}

// MIC 上行数据回调：在 voice_read 工作线程上下文，单帧 MIC_FRAME_BYTES。
static int _audio_upward_callback(unsigned char *data, unsigned int len, void *args) {
  if (!s_audio_listener.cb || !s_audio_started) {
    return len;
  }
  s_audio_listener.cb((const uint8_t *)data, len, s_audio_listener.user_data);
  return len;
}

// ==================== 视频内部实现 ====================

// YUV 抓拍：在 DVP 回调中仅 memcpy，JPEG 编码移到业务线程避免 DMA 冲突。
static void _do_yuv_capture(uint8_t *yuv_data, uint32_t yuv_len) {
  if (!s_capture_yuv_buf) {
    s_capture_yuv_len = 0;
    s_capture_request = false;
    rtos_set_semaphore(&s_capture_sem);
    return;
  }

  if (yuv_len > YUV_FRAME_SIZE) {
    yuv_len = YUV_FRAME_SIZE;
  }

  // memcpy YUV 数据（~1-2ms，中断可接受）
  os_memcpy(s_capture_yuv_buf, yuv_data, yuv_len);
  s_capture_yuv_len = yuv_len;

  s_capture_request = false;
  rtos_set_semaphore(&s_capture_sem);
}

static void _dvp_frame_complete(image_format_t format, frame_buffer_t *frame, int result) {
  if (result != BK_OK || !frame) {
    if (frame) {
      _camera_frame_free(frame);
    }
    return;
  }

  // YUV：抓拍时截获，不编码
  if (format == IMAGE_YUV) {
    if (s_capture_request && frame->frame && frame->length > 0) {
      _do_yuv_capture((uint8_t *)frame->frame, frame->length);
    }
    _camera_frame_free(frame);
    return;
  }

  if (format != IMAGE_H264 && format != IMAGE_MJPEG) {
    _camera_frame_free(frame);
    return;
  }
  if (!s_video_listener.cb || !s_video_started || !frame->frame) {
    _camera_frame_free(frame);
    return;
  }

  uint32_t data_len = frame->length;
  if (data_len == 0 || data_len > 200 * 1024 || data_len > frame->size) {
    _camera_frame_free(frame);
    return;
  }

  // MJPEG 每帧都是独立 JPEG，等价于关键帧；
  bool is_key_frame = true;
  if (format == IMAGE_H264) {
    is_key_frame = (frame->h264_type & (1 << H264_NAL_I_FRAME)) != 0;
  }
  s_video_listener.cb(
      (const uint8_t *)frame->frame, data_len, is_key_frame,
      s_video_codec == IMAGE_MJPEG ? TC_IOT_VIDEO_CODEC_MJPEG : TC_IOT_VIDEO_CODEC_H264,
      s_video_listener.user_data);
  _camera_frame_free(frame);
}

// 中断安全的 frame buffer 申领：O(1) 原子占位，不调 heap_4。H264 从 s_frame_pool，YUV 从 s_capture_pool 轮转。
static frame_buffer_t *_camera_frame_malloc(image_format_t format, uint32_t size) {
  // YUV 帧仅在抓拍窗口分配，平时直接丢弃以降低 DMA 负载。
  // 注意：DVP 初始化时（s_video_started==false）会调一次 malloc(IMAGE_YUV) 配置 DMA，必须放行。
  if (format == IMAGE_YUV) {
    if (s_video_started && !s_capture_request) {
      return NULL;
    }

    if (s_capture_pool == NULL) {
      return NULL;
    }
    if (size > CAPTURE_SLOT_PAYLOAD_SIZE) {
      s_frame_pool_alloc_fail++;
      return NULL;
    }
    // 遍历 pool 找一个空闲 slot（O(N)，N=2，在中断中可接受）
    for (int i = 0; i < YUV_CAPTURE_POOL_SIZE; i++) {
      uint8_t expected = 0;
      if (__atomic_compare_exchange_n(&s_capture_pool[i].in_use, &expected, 1, false /* strong */,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        frame_buffer_t *frame = &s_capture_pool[i].header;
        os_memset(frame, 0, sizeof(*frame));
        uint32_t raw_data_addr = (uint32_t)s_capture_pool[i].payload;
        uint32_t aligned_addr = (raw_data_addr + DMA_ALIGN_MASK) & ~DMA_ALIGN_MASK;
        frame->frame = (uint8_t *)aligned_addr;
        frame->size = size;
        frame->fmt = PIXEL_FMT_YUYV;
        return frame;
      }
    }
    // 抓拍窗口内但所有 YUV slot 都被占用（DMA 双缓冲都在传输中）
    s_frame_pool_alloc_fail++;
    return NULL;
  }

  // H264 帧：从 s_frame_pool 分配（原有逻辑不变）
  if (s_frame_pool == NULL) {
    return NULL;
  }
  if (size > FRAME_PAYLOAD_MAX_SIZE) {
    // 超出预分配容量：和配置项 CONFIG_H264_FRAME_SIZE 不符，理论上不应达到这里。
    s_frame_pool_alloc_fail++;
    return NULL;
  }

  for (int i = 0; i < FRAME_POOL_SIZE; i++) {
    // 原子 CAS-like：in_use 从 0 交换到 1；返回旧值为 0 表示抢到了这个 slot。
    uint8_t expected = 0;
    if (__atomic_compare_exchange_n(&s_frame_pool[i].in_use, &expected, 1, false /* strong */,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      frame_slot_t *slot = &s_frame_pool[i];
      frame_buffer_t *frame = &slot->header;
      // 清零 header（保留 in_use，它不在 header 里）。
      os_memset(frame, 0, sizeof(*frame));
      // 对齐 payload 起始地址到 32 字节边界（满足 DMA 要求）。
      uint32_t raw_data_addr = (uint32_t)slot->payload;
      uint32_t aligned_addr = (raw_data_addr + DMA_ALIGN_MASK) & ~DMA_ALIGN_MASK;
      frame->frame = (uint8_t *)aligned_addr;
      frame->size = size;
      // 按当前编码格式设置像素格式：MJPEG 用 PIXEL_FMT_JPEG，H264 用 PIXEL_FMT_H264
      frame->fmt = (s_video_codec == IMAGE_MJPEG) ? PIXEL_FMT_JPEG : PIXEL_FMT_H264;
      return frame;
    }
  }

  // 所有 slot 都被占用：丢一帧（DVP 驱动能容忍 malloc 返回 NULL，会跳过这次采集）。
  s_frame_pool_alloc_fail++;
  return NULL;
}

// 中断安全的 frame buffer 归还：通过 header 地址判断属于 H264 pool 还是 YUV capture pool。
// 可在中断或任务上下文调用。
static void _camera_frame_free(frame_buffer_t *frame) {
  if (!frame) {
    return;
  }

  // 检查是否在 YUV capture pool 中
  if (s_capture_pool) {
    uintptr_t cap_base = (uintptr_t)s_capture_pool;
    uintptr_t cap_addr = (uintptr_t)frame;
    uintptr_t cap_end = cap_base + sizeof(capture_slot_t) * YUV_CAPTURE_POOL_SIZE;
    if (cap_addr >= cap_base && cap_addr < cap_end &&
        ((cap_addr - cap_base) % sizeof(capture_slot_t)) == 0) {
      capture_slot_t *slot = (capture_slot_t *)frame;
      __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);
      return;
    }
  }

  // 检查是否在 H264 frame pool 中
  if (!s_frame_pool) {
    return;
  }

  // header 是 slot 首位，地址 == slot 地址。
  frame_slot_t *slot = (frame_slot_t *)frame;
  // 校验地址落在 pool 数组内。
  uintptr_t base = (uintptr_t)s_frame_pool;
  uintptr_t addr = (uintptr_t)slot;
  uintptr_t end = base + sizeof(frame_slot_t) * FRAME_POOL_SIZE;
  if (addr < base || addr >= end || ((addr - base) % sizeof(frame_slot_t)) != 0) {
    // 地址不在 pool 内：兼容迁移前遗留的 os_free/psram_malloc 分配路径，保底回退。
    // 正常情况下不应走到这里。
    os_free(frame);
    return;
  }

  __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);
}

static const bk_dvp_callback_t s_dvp_callbacks = {
    .malloc = _camera_frame_malloc,
    .complete = _dvp_frame_complete,
};

#if CONFIG_FLASH
static void _flash_op_camera_callback(uint32_t state) {
  if (!s_video_info || !s_video_info->handle) {
    return;
  }
  if (state) {
    bk_camera_suspend(s_video_info->handle);
  } else {
    bk_camera_resume(s_video_info->handle);
  }
}
#endif

static bk_err_t _video_open(void) {
  bk_err_t ret = BK_OK;
  bool success = false;

  do {
    // 先分配 H264 frame buffer pool（任务上下文，可以安全调 psram_malloc）。
    // DVP 回调注册前必须就绪，否则首帧中断里 _camera_frame_malloc 会返回 NULL。
    if (s_frame_pool == NULL) {
      uint32_t pool_bytes = sizeof(frame_slot_t) * FRAME_POOL_SIZE;
      s_frame_pool = (frame_slot_t *)psram_malloc(pool_bytes);
      if (s_frame_pool == NULL) {
        bk_printf("[bk_av][VID] frame pool alloc failed: need %u bytes\n", pool_bytes);
        ret = BK_FAIL;
        break;
      }
      os_memset(s_frame_pool, 0, pool_bytes);
      s_frame_pool_alloc_fail = 0;
      bk_printf("[bk_av][VID] H264 frame pool ready: %d slots x %u bytes = %u bytes\n",
                FRAME_POOL_SIZE, (unsigned)sizeof(frame_slot_t), pool_bytes);
    }

    // 分配抓拍 YUV capture pool（独立于 H264 pool）
    if (s_capture_pool == NULL) {
      uint32_t cap_pool_bytes = sizeof(capture_slot_t) * YUV_CAPTURE_POOL_SIZE;
      s_capture_pool = (capture_slot_t *)psram_malloc(cap_pool_bytes);
      if (s_capture_pool == NULL) {
        bk_printf("[bk_av][CAP] capture pool alloc failed: need %u bytes\n",
                  (unsigned)cap_pool_bytes);
      } else {
        os_memset(s_capture_pool, 0, cap_pool_bytes);
        bk_printf("[bk_av][CAP] capture pool ready: %d slots x %u bytes = %u bytes\n",
                  YUV_CAPTURE_POOL_SIZE, (unsigned)sizeof(capture_slot_t), cap_pool_bytes);
      }
    }

    s_video_info = (video_info_t *)os_malloc(sizeof(video_info_t));
    if (!s_video_info) {
      ret = BK_FAIL;
      break;
    }
    os_memset(s_video_info, 0, sizeof(video_info_t));

    // 模拟 I2C GPIO 解映射
    gpio_dev_unmap(GPIO_42);
    gpio_dev_unmap(GPIO_43);
    bk_gpio_pull_up(GPIO_42);
    bk_gpio_pull_up(GPIO_43);

    // DVP 上电
    if (DVP_POWER_PIN != GPIO_INVALID_ID) {
      GPIO_UP(DVP_POWER_PIN);
      rtos_delay_milliseconds(100);
    }

    bk_dvp_config_t dvp_config = BK_DVP_864X480_30FPS_MJPEG_CONFIG();
    dvp_config.i2c_config.id = CONFIG_DVP_CAMERA_I2C_ID;
    dvp_config.reset_pin = DVP_RST_PIN;
    dvp_config.fps = _video_fps_to_frame_fps(VIDEO_FPS);
    dvp_config.img_format = IMAGE_YUV | s_video_codec;
    dvp_config.width = VIDEO_WIDTH;
    dvp_config.height = VIDEO_HEIGHT;

    bk_dvp_ctlr_config_t dvp_ctlr_config = {
        .config = dvp_config,
        .cbs = &s_dvp_callbacks,
    };

    ret = bk_camera_dvp_ctlr_new(&s_video_info->handle, &dvp_ctlr_config);
    if (ret != BK_OK) {
      break;
    }

    ret = bk_camera_open(s_video_info->handle);
    if (ret != BK_OK) {
      bk_camera_delete(s_video_info->handle);
      s_video_info->handle = NULL;
      break;
    }

    if (s_video_codec == IMAGE_MJPEG) {
      // MJPEG 码率控制：bk7258 硬件 JPEG 编码器按"每帧字节数上下限"做 CBR。
      // 把目标码率折算成每帧目标字节，写入编码器的目标大小带。该配置在下一个
      // JPEG_EOF 中断里由驱动自动生效（开启硬件码率控制并重写目标大小），
      uint32_t video_bitrate_mjpeg = 1200 * 1024;
      uint32_t fps = (VIDEO_FPS > 0) ? (uint32_t)VIDEO_FPS : 10;
      // 每帧目标字节 = 码率(bps) / 8 / fps
      uint32_t per_frame = video_bitrate_mjpeg / 8 / fps;
      if (per_frame > 1) {
        uint32_t up_size = per_frame + per_frame / 4;  // 上限 +25%
        uint32_t low_size =
            (per_frame > per_frame / 4) ? (per_frame - per_frame / 4) : 1;  // 下限 -25%
        // 安全夹取：不超过帧 pool 单帧上限，避免超预算丢帧。
        if (up_size > VIDEO_FRAME_MAX_SIZE) {
          up_size = VIDEO_FRAME_MAX_SIZE;
        }
        if (low_size >= up_size) {
          low_size = (up_size > 1) ? (up_size - 1) : 1;
        }
        bk_err_t bitrate_ret = bk_jpeg_enc_encode_config(1, up_size, low_size);
        bk_printf("[bk_av][VID] mjpeg target bitrate=%u bps -> per_frame=%uB band[%u,%u]B ret=%d\n",
                  (unsigned)video_bitrate_mjpeg, (unsigned)per_frame, (unsigned)low_size,
                  (unsigned)up_size, bitrate_ret);
      } else {
        bk_printf("[bk_av][VID] video_bitrate_bps too small (%u), ignored\n",
                  (unsigned)video_bitrate_mjpeg);
      }
    }

#if CONFIG_FLASH
    mb_flash_register_op_camera_notify(_flash_op_camera_callback);
#endif

    bk_wifi_set_wifi_media_mode(true);
    bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_SD);

    // 分配 JPEG 编码和 YUV 抓拍 buffer（云存抓拍用），编码实例每次 open/enc/deinit。
    if (!s_jpeg_enc_inited) {
      s_jpeg_output_buf = (uint8_t *)psram_malloc(JPEG_OUTPUT_BUF_SIZE);
      s_capture_yuv_buf = (uint8_t *)psram_malloc(YUV_FRAME_SIZE);
      if (!s_capture_pool || !s_jpeg_output_buf || !s_capture_yuv_buf) {
        bk_printf("[bk_av][CAP] jpeg buf alloc fail: capture_pool=%p jpeg_out=%p yuv_buf=%p\n",
                  s_capture_pool, s_jpeg_output_buf, s_capture_yuv_buf);
      } else {
        s_jpeg_enc_inited = true;
        bk_printf(
            "[bk_av][CAP] jpeg capture ready: %ux%u quality=%d output_buf=%uKB yuv_buf=%uKB\n",
            VIDEO_WIDTH, VIDEO_HEIGHT, JPEG_QUALITY_FACTOR, (unsigned)(JPEG_OUTPUT_BUF_SIZE / 1024),
            (unsigned)(YUV_FRAME_SIZE / 1024));
      }
    }

    // 创建抓拍信号量
    if (s_capture_sem == NULL) {
      rtos_init_semaphore(&s_capture_sem, 1);
    }

    success = true;
  } while (false);

  if (success) {
    return BK_OK;
  }

  // 失败清理
  if (DVP_POWER_PIN != GPIO_INVALID_ID) {
    GPIO_DOWN(DVP_POWER_PIN);
  }
  if (s_video_info) {
    os_free(s_video_info);
    s_video_info = NULL;
  }
  if (s_frame_pool) {
    psram_free(s_frame_pool);
    s_frame_pool = NULL;
  }
  if (s_capture_pool) {
    psram_free(s_capture_pool);
    s_capture_pool = NULL;
  }
  return BK_FAIL;
}

static inline frame_fps_t _video_fps_to_frame_fps(int fps) {
  // 把 iot_demo_config.h 里的整数 fps（每秒帧数）映射到 BEKEN frame_fps_t enum。
  // frame_fps_t 是位掩码（FPS5 / FPS10 / ... / FPS30），不能直接用整数赋值，
  // 因此保留这层平台适配宏；编译期 _Static_assert 会在选了不支持的 fps 时报错。
  switch (fps) {
    case 5:
      return FPS5;
    case 10:
      return FPS10;
    case 15:
      return FPS15;
    case 20:
      return FPS20;
    case 25:
      return FPS25;
    case 30:
      return FPS30;
    default:
      // 兜底，避免传 0 让 DVP 不出帧
      return FPS15;
  }
}

static bk_err_t _video_close(void) {
  bk_wifi_set_wifi_media_mode(false);
  bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_HD);

  if (!s_video_info) {
    return BK_OK;
  }

  if (s_video_info->handle) {
    bk_camera_close(s_video_info->handle);
    bk_camera_delete(s_video_info->handle);
#if CONFIG_FLASH
    mb_flash_unregister_op_camera_notify();
#endif
    s_video_info->handle = NULL;
  }

  if (DVP_POWER_PIN != GPIO_INVALID_ID) {
    GPIO_DOWN(DVP_POWER_PIN);
  }

  os_free(s_video_info);
  s_video_info = NULL;

  // 清理 JPEG 编码资源
  s_jpeg_enc_inited = false;
  if (s_jpeg_output_buf) {
    psram_free(s_jpeg_output_buf);
    s_jpeg_output_buf = NULL;
  }
  if (s_capture_sem) {
    rtos_deinit_semaphore(&s_capture_sem);
    s_capture_sem = NULL;
  }
  if (s_capture_yuv_buf) {
    psram_free(s_capture_yuv_buf);
    s_capture_yuv_buf = NULL;
    s_capture_yuv_len = 0;
  }
  s_capture_request = false;

  // 释放抓拍专用 YUV capture pool（必须在 DVP 关闭后，此时不再有中断访问）
  if (s_capture_pool) {
    bk_printf("[bk_av][CAP] capture pool free\n");
    psram_free(s_capture_pool);
    s_capture_pool = NULL;
  }

  if (s_frame_pool) {
    bk_printf("[bk_av][VID] frame pool free (alloc_fail_count=%u)\n",
              (unsigned)s_frame_pool_alloc_fail);
    psram_free(s_frame_pool);
    s_frame_pool = NULL;
  }
  return BK_OK;
}

// ==================== 事件抓拍图（云存图片）====================
// s_capture_request → 等待下一帧 YUV → memcpy → 业务线程 JPEG 编码 → 返回。超时 1000ms。
#define CAPTURE_TIMEOUT_MS 1000

int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  (void)event_id;
  if (out_len) {
    *out_len = 0;
  }

  if (!buf || buf_size == 0 || !out_len) {
    return -1;
  }

  if (!s_video_started || !s_jpeg_enc_inited || !s_capture_sem) {
    bk_printf("[bk_av][CAP] not ready: started=%d enc=%d sem=%p\n", s_video_started,
              s_jpeg_enc_inited, s_capture_sem);
    return -1;
  }

  s_capture_yuv_len = 0;
  s_capture_request = true;

  bk_err_t ret = rtos_get_semaphore(&s_capture_sem, CAPTURE_TIMEOUT_MS);
  if (ret != BK_OK) {
    s_capture_request = false;
    bk_printf("[bk_av][CAP] timeout waiting for YUV frame\n");
    return -1;
  }

  if (!s_capture_yuv_buf || s_capture_yuv_len == 0) {
    bk_printf("[bk_av][CAP] no YUV data captured\n");
    return -1;
  }

  // DVP 输出 VYUY，转换为 YUYV（JPEG 编码器期望的格式）
  {
    uint32_t pixel_count = s_capture_yuv_len / 4;
    uint8_t *yuv_data = s_capture_yuv_buf;
    for (uint32_t i = 0; i < pixel_count; i++) {
      uint8_t v = yuv_data[i * 4 + 0];
      uint8_t y1 = yuv_data[i * 4 + 1];
      uint8_t u = yuv_data[i * 4 + 2];
      uint8_t y2 = yuv_data[i * 4 + 3];
      yuv_data[i * 4 + 0] = y1;
      yuv_data[i * 4 + 1] = v;
      yuv_data[i * 4 + 2] = y2;
      yuv_data[i * 4 + 3] = u;
    }
  }

  jpeg_sw_encoder_init();

  uint16_t header_len = 0;
  int enc_ret =
      jpeg_sw_encoder.open(&jpeg_sw_encoder.codec, VIDEO_WIDTH, VIDEO_HEIGHT, s_capture_yuv_buf,
                           s_jpeg_output_buf, &header_len, JPEG_QUALITY_FACTOR);
  if (enc_ret != 0 || !jpeg_sw_encoder.codec) {
    bk_printf("[bk_av][CAP] jpeg open fail: ret=%d\n", enc_ret);
    return -1;
  }

  int enc_size = 0;
  enc_ret = jpeg_sw_encoder.enc(jpeg_sw_encoder.codec, s_jpeg_output_buf + header_len,
                                JPEG_OUTPUT_BUF_SIZE - header_len, &enc_size);

  jpeg_sw_encoder.deinit(&jpeg_sw_encoder.codec);

  if (enc_ret != 0 || enc_size <= 0) {
    bk_printf("[bk_av][CAP] jpeg enc fail: ret=%d size=%d\n", enc_ret, enc_size);
    return -1;
  }

  uint32_t total_size = (uint32_t)header_len + (uint32_t)enc_size;

  if (total_size > buf_size) {
    bk_printf("[bk_av][CAP] buf too small: need=%u have=%u\n", total_size, buf_size);
    return -1;
  }

  os_memcpy(buf, s_jpeg_output_buf, total_size);
  *out_len = total_size;

  bk_printf("[bk_av][CAP] captured JPEG: %u bytes (event_id=%u)\n", *out_len, event_id);
  return 0;
}
