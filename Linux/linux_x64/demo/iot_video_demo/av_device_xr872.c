// Copyright (c) 2026 Tencent. All rights reserved.

// XR872 av_device 实现：真实摄像头/麦克风采集，下行 MJPEG 显示与音频播放。
// 上行视频：BSP 摄像头（BF20A6/SPI 或 GC0328C/CSI）输出 RGB565（240x320
// 竖屏），
//   软 libjpeg-turbo 编码为 MJPEG 回调；JPEG 质量/帧率见 AV_DEVICE_*。
// 上行音频：snd_pcm 采集 16k/16bit/mono 真实麦克风 PCM 直接回调，
//   SDK 内部自动编码 G722 推流。
// 下行视频：MJPEG → libjpeg 解码 → RGB565 直刷 LCD（通话期间暂停 LVGL 渲染）。
// 下行音频：SDK 已解 G722→PCM，直接 snd_pcm 播放。
// 依赖：bsp_i2c_bus_init/bsp_io_exp_init（摄像头 AW9523B 上电）、snd_pcm_init
//   已由 solution main 完成；LCD 由 lvconf 回调初始化，本模块直刷。注意
//   G3609C(AICAM) 背光冷启动默认灭，首帧刷出后须调
//   drv_lcd_iface_backlight_first_frame_notify() 放行背光（见
//   _dl_video_thread）。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/manager/audio_manager.h"
#include "audio/pcm/audio_pcm.h"
#include "av_device.h"
#include "drive/drv_camera/bsp_camera.h"
#include "drive/drv_lcd/dev_lcd.h"
#include "drive/drv_lcd/drv_lcd_iface.h"
#include "driver/chip/hal_snd_card.h"
#include "driver/chip/hal_spi.h"
#include "iot_demo_config.h"
#include "libjpeg/turbojpeg.h"
#include "os/os.h"
#include "sys/dma_heap.h"
#include "sys/psram_heap.h"
#include "tc_hal_audio.h"

// ==================== 平台参数 ====================

// BSP 摄像头输出竖屏 240x320 RGB565（BF20A6 sensor 横屏 320x240，BSP 转置）。
// CAM_FRAME_W/H 必须匹配 preview_buf 实际布局，否则编码跨行错位出横条画面。
#define CAM_FRAME_W 240
#define CAM_FRAME_H 320
#define CAM_FRAME_BYTES (CAM_FRAME_W * CAM_FRAME_H * 2)

// 上行视频帧率（可调低省流量/CPU）
#define AV_DEVICE_VIDEO_FPS 5
// JPEG 编码质量（1-100）
#define AV_DEVICE_JPEG_QUALITY 50

// 下行显示尺寸 = LCD 面板分辨率（AXS15252 240x320），libjpeg 直接缩放到此上屏
#define AV_DL_OUT_W 240
#define AV_DL_OUT_H 320

// 音频队列 8 槽(≈160ms)吸收解码抖动；视频 3 槽(32KB×3 PSRAM)稳态不积满，省内存
#define DL_AUDIO_QUEUE 8
// G722 解码后 PCM 每帧 640B（16000Hz×2B×20ms），原 512 放不下导致静音
#define DL_AUDIO_SLOT_SIZE 640
#define DL_VIDEO_QUEUE 3
#define DL_VIDEO_SLOT_SIZE (32 * 1024)

#define MAX_VIDEO_CAPTURES 4

// ==================== 类型 ====================

// 上行音频采集（全局单一麦克风，采集 PCM 直接回调）
typedef struct {
  bool active;
  volatile bool running;
  OS_Thread_t thread;
  struct pcm_config pcm_cfg;
  audio_device_callback_t cb;
  void *user_data;
} audio_capture_t;

// 上行视频采集（全局单一摄像头，多路监听者共享一路采集线程）
typedef struct {
  bool active;
  volatile bool running;
  OS_Thread_t thread;
  int jpeg_quality;
  video_device_callback_t cbs[MAX_VIDEO_CAPTURES];
  void *user_data[MAX_VIDEO_CAPTURES];
  int cb_count;
  uint8_t *preview_buf; /* dma 池，CSI DMA 写入 */
} video_capture_t;

// 下行播放：音频/视频各一个独立线程 + 独立信号量，互不阻塞。
typedef struct {
  bool active;
  volatile bool running;
  volatile int alive;  /* 两线程各 +1，退出各 -1，deinit 等其归零 */
  OS_Thread_t athread; /* 音频写声卡线程 */
  OS_Thread_t vthread; /* 视频解码上屏线程 */
  struct pcm_config pcm_cfg;

  /* 音频队列（G722 已解 PCM，psram 分配） */
  uint8_t *aqueue[DL_AUDIO_QUEUE];
  uint16_t alen[DL_AUDIO_QUEUE];
  int ahead, atail, acount;

  /* 视频队列（MJPEG 帧拷贝，psram 分配） */
  uint8_t *vslots[DL_VIDEO_QUEUE];
  uint32_t vlen[DL_VIDEO_QUEUE];
  int vhead, vtail, vcount;

  OS_Mutex_t lock;     /* 统一保护两个队列 */
  OS_Semaphore_t asem; /* 音频唤醒（最大计数 = 队列容量） */
  OS_Semaphore_t vsem; /* 视频唤醒 */
  uint8_t *lcd_buf;    /* 上屏缓冲（RGB565） */
} dl_t;

static audio_capture_t s_audio;
static video_capture_t s_video;
static dl_t s_dl;

// ==================== 前置声明 ====================

static void _audio_capture_thread(void *arg);
static void _video_capture_thread(void *arg);
static void _dl_audio_thread(void *arg);
static void _dl_video_thread(void *arg);
static int _camera_start(void);
static void _camera_stop(void);
static int _dl_init(void);
static void _dl_deinit(void);
static void _rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, int n);
static void _rgb888_to_rgb565(const uint8_t *src, uint16_t *dst, int n);

// ==================== 公开接口 ====================

void av_device_init(const av_device_config_t *cfg) {
  (void)cfg;

  memset(&s_audio, 0, sizeof(s_audio));
  memset(&s_video, 0, sizeof(s_video));
  s_video.jpeg_quality = AV_DEVICE_JPEG_QUALITY;

  /* 下行播放/显示与上行同启（_dl_init 约占 1MB PSRAM；若需省内存可改为
   * 条件初始化，playout_* 有 active 保护会直接丢弃） */
  _dl_init();

  printf("[av_device_xr872] init (real camera mic, fps=%d)\n", AV_DEVICE_VIDEO_FPS);
}

void av_device_deinit(void) {
  if (s_audio.active) {
    av_device_stop_audio(NULL, NULL);
  }
  if (s_video.cb_count > 0) {
    /* 注销所有监听者并停止摄像头 */
    while (s_video.cb_count > 0) {
      av_device_stop_video(s_video.cbs[0], s_video.user_data[0]);
    }
  }
  _dl_deinit();
  printf("[av_device_xr872] deinit\n");
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  (void)audio_file_path; /* xr872 用真实麦克风，忽略路径 */
  if (!callback) {
    return -1;
  }

  // 单一麦克风：同监听者重复 start 幂等，已被占用则拒绝
  if (s_audio.active) {
    if (s_audio.cb == callback && s_audio.user_data == user_data) {
      return 0;
    }
    printf("[av_device_xr872] audio listener already occupied\n");
    return -1;
  }

  memset(&s_audio, 0, sizeof(s_audio));
  s_audio.cb = callback;
  s_audio.user_data = user_data;

  memset(&s_audio.pcm_cfg, 0, sizeof(s_audio.pcm_cfg));
  s_audio.pcm_cfg.rate = MIC_SAMPLE_RATE;
  s_audio.pcm_cfg.channels = MIC_CHANNELS;
  s_audio.pcm_cfg.period_size = MIC_FRAME_BYTES * 2; /* 40ms */
  s_audio.pcm_cfg.period_count = 2;
  s_audio.pcm_cfg.format = PCM_FORMAT_S16_LE;

  if (snd_pcm_open(AUDIO_SND_CARD_DEFAULT, PCM_IN, &s_audio.pcm_cfg) != 0) {
    printf("[av_device_xr872] snd_pcm_open(PCM_IN) failed\n");
    return -1;
  }

  // AMIC 增益：codec 默认 Level0(0dB) 电平太低，按官方 record_wrapper 设 27dB
  HAL_SndCard_SetVolume(AUDIO_SND_CARD_DEFAULT, AUDIO_IN_DEV_AMIC,
                        VOLUME_GAIN_39dB | VOLUME_SET_GAIN);

  s_audio.running = 1;
  s_audio.active = true;
  /* 栈 24KB：SDK 在本线程回调内做 FAAC 编码，8KB 栈会溢出 */
  if (OS_ThreadCreatePSRAM(&s_audio.thread, "iot_mic", _audio_capture_thread, NULL,
                           OS_PRIORITY_NORMAL, 24 * 1024) != OS_OK) {
    s_audio.running = 0;
    s_audio.active = false;
    snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_IN);
    printf("[av_device_xr872] audio thread create failed\n");
    return -1;
  }

  printf(
      "[av_device_xr872] audio capture started (mic %dHz/%dch, PCM->SDK "
      "internal-encode)\n",
      MIC_SAMPLE_RATE, MIC_CHANNELS);
  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  if (!s_audio.active) {
    return;
  }
  if (callback && (s_audio.cb != callback || s_audio.user_data != user_data)) {
    return;
  }
  s_audio.running = 0;
  while (s_audio.active) {
    OS_MSleep(2);
  }
  snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_IN);
  memset(&s_audio, 0, sizeof(s_audio));
  printf("[av_device_xr872] audio capture stopped\n");
}

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  (void)video_file_path; /* xr872 用真实摄像头，忽略路径 */
  if (!callback) {
    return -1;
  }

  // 同监听者重复 start 幂等
  for (int i = 0; i < s_video.cb_count; i++) {
    if (s_video.cbs[i] == callback && s_video.user_data[i] == user_data) {
      return 0;
    }
  }
  if (s_video.cb_count >= MAX_VIDEO_CAPTURES) {
    printf("[av_device_xr872] video captures full\n");
    return -1;
  }

  s_video.cbs[s_video.cb_count] = callback;
  s_video.user_data[s_video.cb_count] = user_data;
  s_video.cb_count++;

  // 第一路监听者注册时启动摄像头
  if (s_video.cb_count == 1) {
    if (_camera_start() != 0) {
      s_video.cb_count--;
      s_video.cbs[0] = NULL;
      s_video.user_data[0] = NULL;
      printf("[av_device_xr872] camera start failed\n");
      return -1;
    }
  }

  printf("[av_device_xr872] video capture started (real camera, %dx%d SW-JPEG)\n", CAM_FRAME_W,
         CAM_FRAME_H);
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  int found = -1;
  for (int i = 0; i < s_video.cb_count; i++) {
    if (s_video.cbs[i] == callback && s_video.user_data[i] == user_data) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    return;
  }
  // 移除该监听者
  for (int i = found; i < s_video.cb_count - 1; i++) {
    s_video.cbs[i] = s_video.cbs[i + 1];
    s_video.user_data[i] = s_video.user_data[i + 1];
  }
  s_video.cb_count--;
  s_video.cbs[s_video.cb_count] = NULL;
  s_video.user_data[s_video.cb_count] = NULL;

  // 最后一个监听者注销时停止摄像头
  if (s_video.cb_count == 0) {
    _camera_stop();
  }
  printf("[av_device_xr872] video capture stopped\n");
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  if (!data || len == 0 || len > DL_AUDIO_SLOT_SIZE || !s_dl.active) {
    printf("[av_device_xr872] playout_audio dropped: data=%p len=%u active=%d\n", data, len,
           s_dl.active);
    return;
  }
  OS_MutexLock(&s_dl.lock, OS_WAIT_FOREVER);
  if (s_dl.acount == DL_AUDIO_QUEUE) {
    /* 队列满：丢最旧一帧 */
    s_dl.atail = (s_dl.atail + 1) % DL_AUDIO_QUEUE;
    s_dl.acount--;
  }
  memcpy(s_dl.aqueue[s_dl.ahead], data, len);
  s_dl.alen[s_dl.ahead] = (uint16_t)len;
  s_dl.ahead = (s_dl.ahead + 1) % DL_AUDIO_QUEUE;
  s_dl.acount++;
  OS_MutexUnlock(&s_dl.lock);
  OS_SemaphoreRelease(&s_dl.asem);
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  if (!data || len == 0 || len > DL_VIDEO_SLOT_SIZE || !s_dl.active) {
    return;
  }
  OS_MutexLock(&s_dl.lock, OS_WAIT_FOREVER);
  if (s_dl.vcount == DL_VIDEO_QUEUE) {
    /* 队列满：丢最旧一帧 */
    s_dl.vtail = (s_dl.vtail + 1) % DL_VIDEO_QUEUE;
    s_dl.vcount--;
  }
  memcpy(s_dl.vslots[s_dl.vhead], data, len);
  s_dl.vlen[s_dl.vhead] = len;
  s_dl.vhead = (s_dl.vhead + 1) % DL_VIDEO_QUEUE;
  s_dl.vcount++;
  OS_MutexUnlock(&s_dl.lock);
  OS_SemaphoreRelease(&s_dl.vsem);
}

void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  // 下行视频就是 MJPEG，与 playout_video 同一条解码上屏路径
  av_device_playout_video(data, len);
}

void av_device_request_idr(void) { /* MJPEG 每帧都是完整图像，无 IDR 概念 */ }

uint64_t av_device_now_ms(void) {
  return (uint64_t)OS_GetTime(); /* 单调递增 ms */
}

uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path) {
  (void)mp4_file_path;
  return 0; /* xr872 走 MJPEG/PCM 实时采集，无 MP4 */
}

uint64_t av_device_seek_mp4_ms(uint64_t target_ms) {
  (void)target_ms;
  return 0;
}

int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  /* 已移除事件抓拍缓存（省 128KB PSRAM），云存抓拍暂不支持 */
  (void)event_id;
  (void)buf;
  (void)buf_size;
  if (out_len) {
    *out_len = 0;
  }
  return -1;
}

// ==================== 音频内部实现 ====================

// 采集线程：读 40ms PCM（1280B）切成 20ms 帧（640B）直接回调
static void _audio_capture_thread(void *arg) {
  (void)arg;
  uint8_t pcm[MIC_FRAME_BYTES * 2];

  while (s_audio.running) {
    int n = snd_pcm_read(AUDIO_SND_CARD_DEFAULT, pcm, sizeof(pcm));
    if (n <= 0) {
      OS_MSleep(2);
      continue;
    }
    for (int off = 0; off + MIC_FRAME_BYTES <= n; off += MIC_FRAME_BYTES) {
      if (!s_audio.running || !s_audio.cb) {
        break;
      }
      s_audio.cb(pcm + off, MIC_FRAME_BYTES, s_audio.user_data);
    }
  }

  s_audio.active = false;
  OS_ThreadDelete(&s_audio.thread);
}

// ==================== 视频内部实现 ====================

// 初始化 BSP 摄像头并启动 RGB565 流。注意：不能 memset 整个 s_video！
// start_video 刚把本路监听者注册进 cbs[]，memset 会清掉导致视频零推流。
static int _camera_start(void) {
  s_video.preview_buf = (uint8_t *)dma_malloc(CAM_FRAME_BYTES, DMAHEAP_PSRAM);
  if (!s_video.preview_buf) {
    printf("[av_device_xr872] alloc preview buf failed\n");
    return -1;
  }

  bsp_cam_cfg_t cam_cfg;
  memset(&cam_cfg, 0, sizeof(cam_cfg));
  cam_cfg.resolution = BSP_CAM_RES_QVGA; /* BSP 转置输出 240x320 竖屏 */
  cam_cfg.format = BSP_CAM_FMT_RGB565;
  cam_cfg.jpeg_quality = (uint8_t)s_video.jpeg_quality;
  cam_cfg.preview_buf = s_video.preview_buf;
  cam_cfg.preview_buf_size = CAM_FRAME_BYTES;

  if (g_bsp_camera_ops.init(&cam_cfg) != HAL_OK) {
    printf("[av_device_xr872] camera init failed (I2C/sensor?)\n");
    dma_free(s_video.preview_buf, DMAHEAP_PSRAM);
    s_video.preview_buf = NULL;
    return -1;
  }

  // 统一走 RGB565 流 + 软件编码（BF20A6/GC0328C 均支持）
  if (g_bsp_camera_ops.start_stream(BSP_CAM_FMT_RGB565) != HAL_OK) {
    printf("[av_device_xr872] camera start_stream(RGB565) failed\n");
    g_bsp_camera_ops.deinit();
    dma_free(s_video.preview_buf, DMAHEAP_PSRAM);
    s_video.preview_buf = NULL;
    return -1;
  }

  s_video.jpeg_quality = AV_DEVICE_JPEG_QUALITY;
  s_video.running = 1;
  s_video.active = true;
  /* 软 JPEG 编码在采集线程内执行，栈给足 24KB */
  if (OS_ThreadCreatePSRAM(&s_video.thread, "iot_cam", _video_capture_thread, NULL,
                           OS_PRIORITY_NORMAL, 24 * 1024) != OS_OK) {
    s_video.running = 0;
    s_video.active = false;
    g_bsp_camera_ops.stop_stream();
    g_bsp_camera_ops.deinit();
    dma_free(s_video.preview_buf, DMAHEAP_PSRAM);
    s_video.preview_buf = NULL;
    printf("[av_device_xr872] camera thread create failed\n");
    return -1;
  }

  printf("[av_device_xr872] camera streaming (RGB565+SW-JPEG %dx%d)\n", CAM_FRAME_W, CAM_FRAME_H);
  return 0;
}

static void _camera_stop(void) {
  if (!s_video.active) {
    return;
  }
  s_video.running = 0;
  while (s_video.active) {
    OS_MSleep(2);
  }
  g_bsp_camera_ops.stop_stream();
  g_bsp_camera_ops.deinit();
  if (s_video.preview_buf) {
    dma_free(s_video.preview_buf, DMAHEAP_PSRAM);
  }
  memset(&s_video, 0, sizeof(s_video));
  printf("[av_device_xr872] camera stopped\n");
}

// 采集线程：拉 RGB565 帧 → 软编码 JPEG → 回调全部监听者
static void _video_capture_thread(void *arg) {
  (void)arg;
  int fps = AV_DEVICE_VIDEO_FPS;
  if (fps <= 0) {
    fps = 10;
  }
  const uint32_t frame_ms = 1000 / (uint32_t)fps;

  // JPEG 输出缓冲固定 48KB：实测 QVGA Q60 每帧仅 10-30KB，留足余量
  unsigned long jpeg_cap = 48 * 1024;
  uint8_t *jpeg_buf = (uint8_t *)psram_malloc(jpeg_cap);
  uint8_t *rgb888 = (uint8_t *)psram_malloc((size_t)CAM_FRAME_W * CAM_FRAME_H * 3);
  tjhandle cj = tjInitCompress();
  if (!jpeg_buf || !rgb888 || !cj) {
    printf("[av_device_xr872] jpeg enc alloc failed\n");
    if (jpeg_buf) {
      psram_free(jpeg_buf);
    }
    if (rgb888) {
      psram_free(rgb888);
    }
    if (cj) {
      tjDestroy(cj);
    }
    s_video.active = false;
    OS_ThreadDelete(&s_video.thread);
    return;
  }

  while (s_video.running) {
    uint8_t *frame = NULL;
    uint32_t frame_size = 0;
    if (g_bsp_camera_ops.get_next_frame(&frame, &frame_size, 1000) != HAL_OK) {
      OS_MSleep(2);
      continue;
    }
    if (!frame || frame_size < CAM_FRAME_BYTES) {
      continue;
    }

    _rgb565_to_rgb888((const uint16_t *)frame, rgb888, CAM_FRAME_W * CAM_FRAME_H);
    unsigned long jpeg_size = jpeg_cap;
    if (tjCompress2(cj, rgb888, CAM_FRAME_W, CAM_FRAME_W * 3, CAM_FRAME_H, TJPF_RGB, &jpeg_buf,
                    &jpeg_size, TJSAMP_420, s_video.jpeg_quality, 0) != 0) {
      continue;
    }
    uint8_t *out = jpeg_buf;
    uint32_t out_len = (uint32_t)jpeg_size;

    // MJPEG 每帧都是完整图像，回调全部监听者并标记关键帧
    for (int i = 0; i < s_video.cb_count; i++) {
      if (s_video.running && s_video.cbs[i]) {
        s_video.cbs[i](out, out_len, true /* is_key_frame */, TC_IOT_VIDEO_CODEC_MJPEG,
                       s_video.user_data[i]);
      }
    }

    OS_MSleep(frame_ms); /* 帧率节流 */
  }

  if (cj) {
    tjDestroy(cj);
  }
  if (rgb888) {
    psram_free(rgb888);
  }
  if (jpeg_buf) {
    psram_free(jpeg_buf);
  }
  s_video.active = false;
  OS_ThreadDelete(&s_video.thread);
}

// 像素格式转换（24-bit 中转）
static void _rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t p = src[i];
    dst[0] = (uint8_t)(((p >> 11) & 0x1F) << 3);
    dst[1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
    dst[2] = (uint8_t)((p & 0x1F) << 3);
    dst += 3;
  }
}

static void _rgb888_to_rgb565(const uint8_t *src, uint16_t *dst, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t r = (uint16_t)((src[0] >> 3) & 0x1F);
    uint16_t g = (uint16_t)((src[1] >> 2) & 0x3F);
    uint16_t b = (uint16_t)((src[2] >> 3) & 0x1F);
    dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
    src += 3;
  }
}

// ==================== 下行播放内部实现 ====================

static int _dl_init(void) {
  memset(&s_dl, 0, sizeof(s_dl));

  for (int i = 0; i < DL_AUDIO_QUEUE; i++) {
    s_dl.aqueue[i] = (uint8_t *)psram_malloc(DL_AUDIO_SLOT_SIZE);
    if (!s_dl.aqueue[i]) {
      for (int j = 0; j < i; j++) {
        psram_free(s_dl.aqueue[j]);
        s_dl.aqueue[j] = NULL;
      }
      printf("[av_device_xr872] dl audio queue alloc failed\n");
      return -1;
    }
  }

  for (int i = 0; i < DL_VIDEO_QUEUE; i++) {
    s_dl.vslots[i] = (uint8_t *)psram_malloc(DL_VIDEO_SLOT_SIZE);
    if (!s_dl.vslots[i]) {
      for (int j = 0; j < i; j++) {
        psram_free(s_dl.vslots[j]);
        s_dl.vslots[j] = NULL;
      }
      printf("[av_device_xr872] dl video slot alloc failed\n");
      goto fail_audio;
    }
  }

  s_dl.lcd_buf = (uint8_t *)psram_malloc(AV_DL_OUT_W * AV_DL_OUT_H * 2);
  if (!s_dl.lcd_buf) {
    printf("[av_device_xr872] dl lcd_buf alloc failed\n");
    goto fail_video;
  }

  memset(&s_dl.pcm_cfg, 0, sizeof(s_dl.pcm_cfg));
  s_dl.pcm_cfg.rate = SPK_SAMPLE_RATE;
  s_dl.pcm_cfg.channels = SPK_CHANNELS;
  s_dl.pcm_cfg.period_size = SPK_FRAME_BYTES; /* 20ms */
  s_dl.pcm_cfg.period_count = 4;
  s_dl.pcm_cfg.format = PCM_FORMAT_S16_LE;
  if (snd_pcm_open(AUDIO_SND_CARD_DEFAULT, PCM_OUT, &s_dl.pcm_cfg) != 0) {
    printf("[av_device_xr872] snd_pcm_open(PCM_OUT) failed\n");
    goto fail_lcd;
  }

  /* 下行播放必须设非 0 音量：demo 只设过 AMIC 采集增益，播放音量默认落到
   * LINEOUT volume 0 → DAC 增益 0 → 物理无声。SPK/LINEOUT 共用 DAC 寄存器。 */
  HAL_SndCard_SetVolume(AUDIO_SND_CARD_DEFAULT, AUDIO_OUT_DEV_SPK, VOLUME_LEVEL31);

  if (OS_MutexCreate(&s_dl.lock) != OS_OK ||
      OS_SemaphoreCreate(&s_dl.asem, 0, DL_AUDIO_QUEUE) != OS_OK ||
      OS_SemaphoreCreate(&s_dl.vsem, 0, DL_VIDEO_QUEUE) != OS_OK) {
    snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
    printf("[av_device_xr872] dl sync obj create failed\n");
    goto fail_lcd;
  }

  s_dl.running = 1;
  s_dl.active = true;
  s_dl.alive = 0;
  /* 音频线程：仅 snd_pcm_write，栈 6KB；OS_PRIORITY_HIGH 防被视频软解抢占
   * 导致 Tx underrun */
  if (OS_ThreadCreatePSRAM(&s_dl.athread, "iot_dl_a", _dl_audio_thread, NULL, OS_PRIORITY_HIGH,
                           6 * 1024) != OS_OK) {
    s_dl.running = 0;
    s_dl.active = false;
    OS_SemaphoreDelete(&s_dl.asem);
    OS_SemaphoreDelete(&s_dl.vsem);
    OS_MutexDelete(&s_dl.lock);
    snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
    printf("[av_device_xr872] dl audio thread create failed\n");
    goto fail_lcd;
  }
  /* 视频线程：libjpeg 解码 + LCD 上屏，栈给足 24KB */
  if (OS_ThreadCreatePSRAM(&s_dl.vthread, "iot_dl_v", _dl_video_thread, NULL, OS_PRIORITY_NORMAL,
                           24 * 1024) != OS_OK) {
    s_dl.running = 0;
    OS_SemaphoreRelease(&s_dl.asem); /* 唤醒音频线程退出 */
    while (s_dl.alive > 0) {
      OS_MSleep(2);
    }
    s_dl.active = false;
    OS_SemaphoreDelete(&s_dl.asem);
    OS_SemaphoreDelete(&s_dl.vsem);
    OS_MutexDelete(&s_dl.lock);
    snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
    printf("[av_device_xr872] dl video thread create failed\n");
    goto fail_lcd;
  }
  return 0;

fail_lcd:
  psram_free(s_dl.lcd_buf);
  s_dl.lcd_buf = NULL;
fail_video:
  for (int i = 0; i < DL_VIDEO_QUEUE; i++) {
    if (s_dl.vslots[i]) {
      psram_free(s_dl.vslots[i]);
      s_dl.vslots[i] = NULL;
    }
  }
fail_audio:
  for (int i = 0; i < DL_AUDIO_QUEUE; i++) {
    if (s_dl.aqueue[i]) {
      psram_free(s_dl.aqueue[i]);
      s_dl.aqueue[i] = NULL;
    }
  }
  return -1;
}

static void _dl_deinit(void) {
  if (!s_dl.active) {
    return;
  }
  s_dl.running = 0;
  OS_SemaphoreRelease(&s_dl.asem);
  OS_SemaphoreRelease(&s_dl.vsem);
  while (s_dl.alive > 0) {
    OS_MSleep(2);
  }
  s_dl.active = false;
  OS_SemaphoreDelete(&s_dl.asem);
  OS_SemaphoreDelete(&s_dl.vsem);
  OS_MutexDelete(&s_dl.lock);
  snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
  if (s_dl.lcd_buf) {
    psram_free(s_dl.lcd_buf);
    s_dl.lcd_buf = NULL;
  }
  for (int i = 0; i < DL_AUDIO_QUEUE; i++) {
    if (s_dl.aqueue[i]) {
      psram_free(s_dl.aqueue[i]);
      s_dl.aqueue[i] = NULL;
    }
  }
  for (int i = 0; i < DL_VIDEO_QUEUE; i++) {
    if (s_dl.vslots[i]) {
      psram_free(s_dl.vslots[i]);
      s_dl.vslots[i] = NULL;
    }
  }
  memset(&s_dl, 0, sizeof(s_dl));
  printf("[av_device_xr872] dl stopped\n");
}

/* 音频下行线程：取 PCM 写声卡，独立唤醒，不受视频解码/上屏阻塞（防 underrun）
 */
static void _dl_audio_thread(void *arg) {
  (void)arg;
  s_dl.alive++;
  uint8_t pcm[DL_AUDIO_SLOT_SIZE];

  while (s_dl.running) {
    if (OS_SemaphoreWait(&s_dl.asem, 100) != OS_OK) {
      continue;
    }
    /* 一次唤醒排空音频队列，保证 PCM 连续写入声卡 */
    for (;;) {
      uint16_t alen = 0;
      OS_MutexLock(&s_dl.lock, OS_WAIT_FOREVER);
      if (s_dl.acount > 0) {
        alen = s_dl.alen[s_dl.atail];
        memcpy(pcm, s_dl.aqueue[s_dl.atail], alen);
        s_dl.atail = (s_dl.atail + 1) % DL_AUDIO_QUEUE;
        s_dl.acount--;
      }
      OS_MutexUnlock(&s_dl.lock);
      if (alen == 0) {
        break;
      }
      int wrc = snd_pcm_write(AUDIO_SND_CARD_DEFAULT, pcm, alen);
      /* 成功时返回 count(==alen)，仅 <0 才是真失败 */
      if (wrc < 0) {
        printf("[av_device_xr872] snd_pcm_write fail rc=%d len=%u\n", wrc, alen);
      }
    }
  }

  s_dl.alive--;
  OS_ThreadDelete(&s_dl.athread);
}

/* 视频下行线程：MJPEG 解码 + 缩放 + LCD 上屏 */
static void _dl_video_thread(void *arg) {
  (void)arg;
  s_dl.alive++;
  tjhandle dj = tjInitDecompress();
  if (!dj) {
    printf("[av_device_xr872] jpeg dec init failed\n");
    s_dl.alive--;
    OS_ThreadDelete(&s_dl.vthread);
    return;
  }

  dev_lcd_init_info_t *li = dev_lcd_get_info();
  const uint16_t lcd_w = li ? li->lcd_w : AV_DL_OUT_W;
  const uint16_t lcd_h = li ? li->lcd_h : AV_DL_OUT_H;

  // 本平台 LCD 240×320 竖屏直写，无软件旋转。之前误用 320×240 窗口 + 270°
  // 旋转超出面板列地址范围，导致全黑。
  uint8_t *dec_rgb888 = (uint8_t *)psram_malloc((size_t)lcd_w * lcd_h * 3);
  if (!dec_rgb888) {
    tjDestroy(dj);
    s_dl.alive--;
    OS_ThreadDelete(&s_dl.vthread);
    return;
  }
  uint16_t *lcd_buf = (uint16_t *)s_dl.lcd_buf;
  memset(lcd_buf, 0, (size_t)lcd_w * lcd_h * 2); /* 纯黑底，缩放未覆盖区为黑边 */

  /* 屏模式：AXS15252 默认 0x36=0x60（横屏），须 dev_lcd_set_hw_rotate(270)
   * 修正为竖屏地址模式，否则画面横竖错位只占上部。 */
  dev_lcd_set_hw_rotate(LCD_ROTATION_270);

  int last_jw = 0, last_jh = 0; /* 分辨率缓存：变化才重算缩放/清黑边 */
  uint32_t last_dec_ticks = 0;  /* 上次解码时刻 */

  while (s_dl.running) {
    if (OS_SemaphoreWait(&s_dl.vsem, 100) != OS_OK) {
      continue;
    }

    /* 取视频队头解码。对端若推 15~30fps，按时间间隔丢帧压到 ~5fps，
     * 避免软解 + 上屏耗尽 CPU（曾触发音频 DMA overrun + 看门狗复位） */
    uint8_t *slot = NULL;
    uint32_t len = 0;
    OS_MutexLock(&s_dl.lock, OS_WAIT_FOREVER);
    if (s_dl.vcount > 0) {
      slot = s_dl.vslots[s_dl.vtail];
      len = s_dl.vlen[s_dl.vtail];
      s_dl.vtail = (s_dl.vtail + 1) % DL_VIDEO_QUEUE;
      s_dl.vcount--;
    }
    OS_MutexUnlock(&s_dl.lock);
    if (!slot || len == 0) {
      continue;
    }
    uint32_t now_ticks = OS_GetTicks();
    if (now_ticks - last_dec_ticks < OS_HZ / 5) {
      continue; /* >5fps 隔帧丢 */
    }
    last_dec_ticks = now_ticks;

    // 取 JPEG 原始尺寸（宽高由对端分辨率决定，非本设备上行的 240×320）
    int jw = 0, jh = 0, jsub = 0;
    if (tjDecompressHeader2(dj, slot, len, &jw, &jh, &jsub) != 0 || jw <= 0 || jh <= 0) {
      continue;
    }
    // 等比缩放 + 黑边 + 居中，纯整数运算避免软浮点开销
    int scale_w, scale_h;
    if ((int)lcd_w * jh <= (int)lcd_h * jw) {
      scale_w = lcd_w;
      scale_h = (lcd_w * jh) / jw;
    } else {
      scale_h = lcd_h;
      scale_w = (lcd_h * jw) / jh;
    }
    if (scale_w <= 0 || scale_h <= 0) {
      continue;
    }
    int off_x = ((int)lcd_w - scale_w) / 2;
    int off_y = ((int)lcd_h - scale_h) / 2;

    // 分辨率变化才重清黑边 + 重算居中（避免每帧 memset）
    if (jw != last_jw || jh != last_jh) {
      memset(dec_rgb888, 0, (size_t)lcd_w * lcd_h * 3);
      memset(lcd_buf, 0, (size_t)lcd_w * lcd_h * 2);
      last_jw = jw;
      last_jh = jh;
    }

    // 等比缩放解码到居中子区域：pitch 传完整行宽 lcd_w*3，指针偏移到目标区
    if (tjDecompress2(dj, slot, len, dec_rgb888 + ((size_t)off_y * lcd_w + off_x) * 3, scale_w,
                      lcd_w * 3, scale_h, TJPF_RGB, 0) != 0) {
      continue;
    }

    // RGB888 → RGB565 竖屏逐行直写（无旋转），只覆盖有效区域
    for (int sy = 0; sy < scale_h; sy++) {
      const uint8_t *src_row = dec_rgb888 + ((size_t)(off_y + sy) * lcd_w + off_x) * 3;
      uint16_t *dst_row = lcd_buf + (size_t)(off_y + sy) * lcd_w + off_x;
      for (int sx = 0; sx < scale_w; sx++) {
        const uint8_t *p = src_row + (size_t)sx * 3;
        uint16_t r = (uint16_t)((p[0] >> 3) & 0x1F);
        uint16_t g = (uint16_t)((p[1] >> 2) & 0x3F);
        uint16_t b = (uint16_t)((p[2] >> 3) & 0x1F);
        dst_row[sx] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    }

    /* 字节序：LVGL 编译时 LV_COLOR_16_SWAP=1，全管线 RGB565 为高低字节交换
     * 格式，直接上屏会整屏偏绿/紫，须对整帧做 32-bit 批量 REV16。 */
    {
      uint32_t *p = (uint32_t *)lcd_buf;
      uint32_t n = ((uint32_t)lcd_w * lcd_h * 2) / 4U;
      while (n--) {
        uint32_t v = *p;
        *p++ = ((v & 0xFF00FF00U) >> 8) | ((v & 0x00FF00FFU) << 8);
      }
    }

    /* 上屏：LCD 与 flash 共享 SPI0（CFG_LCD_SPI_RECONFIG_ON_TRANSFER=1），
     * 写像素前后须显式切换 SPI 目标，否则面板收不到数据全黑。
     * TE 同步须在 bus_lock 之外：先 put_flush_sem 门控，再 get_te_sem 等边沿，
     * 否则 te_sem 永不 release，每帧超时空等 30ms。 */
    dev_lcd_put_flush_sem();
    dev_lcd_get_te_sem();
    dev_lcd_bus_lock();
    dev_lcd_set_spi_clk(48 * 1000 * 1000, SPI_DEVICE_LCD);
    dev_lcd_set_box(0, 0, lcd_w, lcd_h);
    dev_lcd_send_data((uint8_t *)lcd_buf, (size_t)lcd_w * lcd_h * 2);
    dev_lcd_set_spi_clk(48 * 1000 * 1000, SPI_DEVICE_FLASH);
    dev_lcd_bus_unlock();

    /* G3609C(AICAM) 背光门控：冷启动背光默认灭，须同时满足 bl_duty>0
     * （dev_lcd_set_bright 设置，本模块直刷 LCD 绕过了 LVGL 路径必须补设）和
     * first_frame 标志（notify 置位），否则 GRAM 有数据但屏全黑。二者幂等。 */
#if defined(CFG_BOARD_AICAM)
    dev_lcd_set_bright(100);
    drv_lcd_iface_backlight_first_frame_notify();
#endif
  }

  psram_free(dec_rgb888);
  tjDestroy(dj);
  s_dl.alive--;
  OS_ThreadDelete(&s_dl.vthread);
}
