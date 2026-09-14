// Copyright (c) 2026 Tencent. All rights reserved.
//
// M2 真实 AV 实现（bk7258_amp / AVDK）。
//
// 与 SMP 版 av_device_beken_smp_ai.c 的区别：bk7258_amp 的 avdk 没有 bk_camera_ctlr /
// bk_voice_service 那套 API，改用 AVDK 的 media_app（相机/H264 编码，CP1 干活，
// CPU0 经 mailbox 取编码帧）+ aud_intf（语音全双工，含 AEC）。
// 视频取帧参考 projects/thirdparty/agora/main/agora_rtc_demo.c，
// 音频参考 projects/media/doorbell/main/src/doorbell_devices.c。
//
// 接口契约见 src/demo/iot_video_demo/av_device.h：
//   start_video → 每个 H264 编码帧经 video cb 回调（data/len/is_key）
//   start_audio → 每个 PCM 采集帧经 audio cb 回调；下行 playout_audio 写 SPK
//   request_idr → 让编码器立刻产生一个 I 帧

#include <components/log.h>
#include <driver/dvp_camera_types.h>
#include <driver/h264_types.h>
#include <driver/media_types.h>
#include <os/mem.h>
#include <os/os.h>
#include <string.h>

#include "aud_intf.h"
#include "aud_intf_types.h"
#include "av_device.h"
#include "media_app.h"
#include "tc_iot_def.h"

extern void HAL_Printf(const char *fmt, ...);

#define TAG "av_avdk"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

// ==================== 内部状态 ====================
// 单设备：每路（audio / video）至多 1 个监听者（IoT 场景一般只有 1 个 av_sender）。

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

static camera_handle_t s_camera_handle = NULL;
static camera_type_t s_camera_type = DVP_CAMERA;

static aud_intf_drv_setup_t s_aud_drv_setup = DEFAULT_AUD_INTF_DRV_SETUP_CONFIG();
static aud_intf_voc_setup_t s_aud_voc_setup = DEFAULT_AUD_INTF_VOC_SETUP_CONFIG();

static uint32_t s_video_frame_count = 0;
static uint32_t s_video_key_count = 0;
static uint32_t s_audio_frame_count = 0;

// ==================== 音频上行编码解耦线程 ====================
// mic 回调(_on_mic_data)所在 bk 线程随分核/启动时机变化（CPU0 aud_tras_drv 或 CP1→CPU0 aud_intf），
// 栈都很小；而 av_sender→tc_iot_push_audio_frame 会在调用线程内同步跑 FDK AAC 编码（psy_main 深递归
// ~16KB 栈），在小栈 bk 线程上必栈溢出(UsageFault)。故解耦：回调只入队(轻量)，由本线程(PSRAM 32KB 栈)
// 做编码上行，与具体线程/核无关。等价于 SMP av_device_beken_smp_* 让 voice_read(16KB PSRAM 栈) 承担编码。
#define AUD_TX_SLOTS 8
#define AUD_TX_SLOT_SIZE 1024
// ring buffer 放 PSRAM（省 8KB SRAM；本板 SRAM 是最紧资源）。设备生命周期常驻，不释放。
static uint8_t *s_aud_tx_buf = NULL;  // [AUD_TX_SLOTS][AUD_TX_SLOT_SIZE] flattened in PSRAM
static uint16_t s_aud_tx_len[AUD_TX_SLOTS];
static volatile uint32_t s_aud_tx_wr = 0;
static volatile uint32_t s_aud_tx_rd = 0;
static volatile uint32_t s_aud_tx_drop = 0;
static beken_semaphore_t s_aud_tx_sem = NULL;
static beken_thread_t s_aud_tx_thread = NULL;
static volatile bool s_aud_tx_run = false;

static void _aud_enc_thread_start(void);
static void _aud_enc_thread_stop(void);

// ==================== 视频回调（CPU0，media_app 取帧线程上下文）====================
//
// frame 由 media 框架持有，回调返回后框架自行回收，这里只做同步消费，不可缓存指针。
static void _on_h264_frame(frame_buffer_t *frame) {
  if (!s_video_listener.cb || !s_video_started || !frame || !frame->frame) {
    return;
  }
  uint32_t len = frame->length;
  if (len == 0 || len > 200 * 1024) {
    return;
  }
  bool is_key_frame = (frame->h264_type & (1 << H264_NAL_I_FRAME)) != 0;

  s_video_frame_count++;
  if (is_key_frame) {
    s_video_key_count++;
  }
  if (s_video_frame_count <= 3 || (s_video_frame_count % 300) == 0) {
    HAL_Printf("[av_avdk][VID] frame #%u len=%u key=%d (keys=%u)\n", s_video_frame_count, len,
               is_key_frame, s_video_key_count);
  }

  s_video_listener.cb((const uint8_t *)frame->frame, len, is_key_frame, TC_IOT_VIDEO_CODEC_H264,
                      s_video_listener.user_data);
}

// 编码线程：从环形队列取 PCM，在本 32KB PSRAM 栈上调 av_sender 回调（内部跑 FDK AAC 编码 + TRTC 上行）。
static void _aud_enc_thread_main(void *arg) {
  (void)arg;
  while (s_aud_tx_run) {
    if (rtos_get_semaphore(&s_aud_tx_sem, BEKEN_WAIT_FOREVER) != 0) {
      continue;
    }
    while (s_aud_tx_run && s_aud_tx_rd != s_aud_tx_wr) {
      uint32_t rd = s_aud_tx_rd;
      audio_device_callback_t cb = s_audio_listener.cb;
      if (s_audio_started && cb) {
        cb(&s_aud_tx_buf[rd * AUD_TX_SLOT_SIZE], s_aud_tx_len[rd], s_audio_listener.user_data);
      }
      s_aud_tx_rd = (rd + 1) % AUD_TX_SLOTS;
    }
  }
  s_aud_tx_thread = NULL;
  rtos_delete_thread(NULL);
}

static void _aud_enc_thread_start(void) {
  s_aud_tx_wr = s_aud_tx_rd = 0;
  s_aud_tx_drop = 0;
  // ring buffer 放 PSRAM（省 8KB SRAM）；首次分配后常驻、重入复用、不释放（与 s_aud_tx_sem 同策略）。
  if (s_aud_tx_buf == NULL) {
    s_aud_tx_buf = (uint8_t *)psram_malloc(AUD_TX_SLOTS * AUD_TX_SLOT_SIZE);
    if (s_aud_tx_buf == NULL) {
      HAL_Printf("[av_avdk] WARNING: psram_malloc aud_tx_buf(%d) failed\n",
                 AUD_TX_SLOTS * AUD_TX_SLOT_SIZE);
      return;  // 不创建编码线程；_on_mic_data 因 s_aud_tx_thread==NULL 不会入队/解引用
    }
  }
  if (s_aud_tx_sem == NULL) {
    rtos_init_semaphore(&s_aud_tx_sem, AUD_TX_SLOTS);
  }
  if (s_aud_tx_thread == NULL) {
    s_aud_tx_run = true;
    // priority 5：worker 档（低于网络 8 / 网络 worker 6 避免抢占；高于应用 2 以跟上音频帧节奏）。
    int ret =
        rtos_create_psram_thread(&s_aud_tx_thread, 5, "av_aud_enc",
                                 (beken_thread_function_t)_aud_enc_thread_main, 32 * 1024, NULL);
    if (ret != 0) {
      HAL_Printf("[av_avdk] WARNING: create av_aud_enc thread failed: %d\n", ret);
      s_aud_tx_run = false;
      s_aud_tx_thread = NULL;
    } else {
      HAL_Printf("[av_avdk] av_aud_enc thread started (PSRAM 32KB stack)\n");
    }
  }
}

static void _aud_enc_thread_stop(void) {
  if (s_aud_tx_thread && s_aud_tx_run) {
    s_aud_tx_run = false;
    if (s_aud_tx_sem) {
      rtos_set_semaphore(&s_aud_tx_sem);  // 唤醒线程退出
    }
  }
}

// ==================== 音频回调（mic 上行，bk 音频线程上下文）====================
//
// 上行为 PCM（见 voc_setup.data_type），单帧 MIC_FRAME_BYTES。本回调只入队（轻量，绝不在此编码，
// 否则会在小栈 bk 线程上栈溢出），由 _aud_enc_thread_main 在大 PSRAM 栈上做编码。返回已消费长度。
static int _on_mic_data(unsigned char *data, unsigned int len) {
  if (!s_audio_listener.cb || !s_audio_started || !data || len == 0) {
    return len;
  }
  s_audio_frame_count++;
  if (s_audio_frame_count <= 3 || (s_audio_frame_count % 500) == 0) {
    HAL_Printf("[av_avdk][MIC] frame #%u len=%u (drop=%u)\n", s_audio_frame_count, len,
               s_aud_tx_drop);
  }
  if (len > AUD_TX_SLOT_SIZE || s_aud_tx_thread == NULL) {
    return len;
  }
  uint32_t wr = s_aud_tx_wr;
  uint32_t next = (wr + 1) % AUD_TX_SLOTS;
  if (next == s_aud_tx_rd) {
    s_aud_tx_drop++;  // 队列满，丢帧（不阻塞 bk 音频线程）
    return len;
  }
  memcpy(&s_aud_tx_buf[wr * AUD_TX_SLOT_SIZE], data, len);
  s_aud_tx_len[wr] = (uint16_t)len;
  s_aud_tx_wr = next;
  if (s_aud_tx_sem) {
    rtos_set_semaphore(&s_aud_tx_sem);
  }
  return len;
}

// ==================== 公开接口 ====================

void av_device_init(const av_device_config_t *cfg) {
  // amp 不需要 file_path，硬件参数取自 iot_demo_config.h。
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
  s_camera_handle = NULL;
  _aud_enc_thread_start();  // 音频编码解耦线程常驻设备生命周期；无音频时阻塞在信号量
  s_inited = true;
  HAL_Printf("[av_avdk] init (media_app + aud_intf)\n");
}

void av_device_deinit(void) {
  if (!s_inited) {
    return;
  }
  if (s_video_started) {
    av_device_stop_video(s_video_listener.cb, s_video_listener.user_data);
  }
  if (s_audio_started) {
    av_device_stop_audio(s_audio_listener.cb, s_audio_listener.user_data);
  }
  _aud_enc_thread_stop();
  s_inited = false;
}

// -------------------- 视频 --------------------

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  if (!callback) {
    return -1;
  }
  if (s_video_listener.cb == callback && s_video_listener.user_data == user_data) {
    return 0;
  }
  if (s_video_listener.cb) {
    LOGW("video listener already occupied\n");
    return -1;
  }
  if (s_video_started) {
    return -1;
  }

  // DVP 相机：传感器出 YUV，硬件编码 H264。分辨率/帧率取自 iot_demo_config.h。
  media_camera_device_t device = {0};
  device.type = DVP_CAMERA;
  device.port = 0;
  device.format = IMAGE_YUV | IMAGE_H264;
  device.width = VIDEO_WIDTH;
  device.height = VIDEO_HEIGHT;
  // tc_iot: 640x480@15fps。注意 gc2145 在 640x480 下只实现了 FPS30/25/20/15 的寄存器表，
  // 没有 FPS10——设 FPS10 会退回原生 ~27fps(~2000Kbps)直接打爆 WiFi/PSRAM。
  // 降码率改在 H264 编码器(bk_idk h264_driver.c 的 MIDDLE 档调低 imb/pmb_bits)，与帧率解耦。
  device.fps = FPS15;
  device.rotate = ROTATE_NONE;
  s_camera_type = device.type;

  s_video_listener.cb = callback;
  s_video_listener.user_data = user_data;
  s_video_frame_count = 0;
  s_video_key_count = 0;

  // 相机上电由 cp1 上的 bk_dvp 经 CONFIG_DVP_CTRL_POWER_GPIO_ID（AI 板 =GPIO_49，
  // 相机 2.8V_EN）在探测时序里完成。注意：不能在 CPU0 旁路拉高该脚——cp1 启动会按
  // 自己的 GPIO map 重置而失效。
  bk_err_t ret = media_app_camera_open(&s_camera_handle, &device);
  if (ret != BK_OK) {
    LOGE("media_app_camera_open failed: %d\n", (int)ret);
    s_camera_handle = NULL;
    s_video_listener.cb = NULL;
    s_video_listener.user_data = NULL;
    return -1;
  }

  // 注册编码帧读取回调：只取 H264 编码输出。
  ret = media_app_register_read_frame_callback(IMAGE_H264, _on_h264_frame);
  if (ret != BK_OK) {
    LOGE("register read_frame_callback failed: %d\n", (int)ret);
    media_app_camera_close(&s_camera_handle);
    s_camera_handle = NULL;
    s_video_listener.cb = NULL;
    s_video_listener.user_data = NULL;
    return -1;
  }

  s_video_started = true;
  HAL_Printf("[av_avdk][VID] video capture started: %ux%u@%dfps\n", VIDEO_WIDTH, VIDEO_HEIGHT,
             VIDEO_FPS);
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
    media_app_unregister_read_frame_callback();
    if (s_camera_handle) {
      media_app_camera_close(&s_camera_handle);
      s_camera_handle = NULL;
    }
    HAL_Printf("[av_avdk][VID] stopped: frames=%u keys=%u\n", s_video_frame_count,
               s_video_key_count);
  }
}

void av_device_request_idr(void) {
  if (!s_video_started) {
    return;
  }
  HAL_Printf("[av_avdk][VID] request_idr (frames=%u keys=%u)\n", s_video_frame_count,
             s_video_key_count);
  media_app_h264_regenerate_idr(s_camera_type);
}

// 单调递增的毫秒时间戳，用于音视频帧 PTS（合入 origin/main 后 av_sender 新增依赖该接口）。
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

// -------------------- 音频 --------------------

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  if (!callback) {
    return -1;
  }
  if (s_audio_listener.cb == callback && s_audio_listener.user_data == user_data) {
    return 0;
  }
  if (s_audio_listener.cb) {
    LOGW("audio listener already occupied\n");
    return -1;
  }
  if (s_audio_started) {
    return -1;
  }

  s_audio_listener.cb = callback;
  s_audio_listener.user_data = user_data;
  s_audio_frame_count = 0;

  // 语音全双工：mic 采集（PCM）经 _on_mic_data 上行；spk 经 write_spk_data 下行。
  // s_aud_drv_setup / s_aud_voc_setup 已用 DEFAULT_*_CONFIG() 静态初始化，这里只覆盖需要的字段。
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
  // tc_iot/AMP: 默认 spk_gain=0x2d(71%) + 本板 GPIO_50 功放 → 喇叭过响，被 mic 拾取形成正反馈啸叫，
  // 对端也听到回声(SMP/iot12 无此问题=增益更低)。先压到 0x18(38%) 破环，但通话播放偏小，
  // 回调到 0x20(≈51%)：仍明显低于默认 0x2d、配合 ec_depth=30 不复啸叫；运行时可用 CLI `vol <0-100>` 微调。
  // ec_depth 默认 20→30，针对本板较强声学耦合加大回声消除深度。
  s_aud_voc_setup.spk_gain = 0x20;
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

  s_audio_started = true;
  HAL_Printf("[av_avdk][AUD] voice started: %dHz PCM (AEC on)\n", MIC_SAMPLE_RATE);
  return 0;

fail_voc:
  bk_aud_intf_voc_deinit();
fail_mode:
  bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
fail_drv:
  bk_aud_intf_drv_deinit();
fail:
  s_audio_listener.cb = NULL;
  s_audio_listener.user_data = NULL;
  return -1;
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
    bk_aud_intf_voc_stop();
    bk_aud_intf_voc_deinit();
    bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
    bk_aud_intf_drv_deinit();
    HAL_Printf("[av_avdk][AUD] voice stopped: frames=%u\n", s_audio_frame_count);
  }
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  if (!s_audio_started || !data || len == 0) {
    return;
  }
  bk_aud_intf_write_spk_data((uint8_t *)(uintptr_t)data, len);
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  // amp 当前不渲染远端视频（无 LCD 显示需求时为空操作），与 Linux/bk7258 接口对齐。
  (void)data;
  (void)len;
}

void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  // amp 当前不渲染远端视频（无 LCD 显示需求时为空操作），与 Linux/bk7258 接口对齐。
  (void)data;
  (void)len;
}

// ==================== 事件抓拍图（云存图片）====================
// TODO: AMP 平台事件抓拍暂未实现（mailbox 竞态问题待解决），返回空图片。
int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  (void)event_id;
  (void)buf;
  (void)buf_size;
  if (out_len) {
    *out_len = 0;
  }
  LOGE("get_event_picture: not supported on AMP AI/DVP path yet\n");
  return -1;
}
