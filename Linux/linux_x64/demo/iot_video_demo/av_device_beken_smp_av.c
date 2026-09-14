// Copyright (c) 2026 Tencent. All rights reserved.
//
// BK7258 SMP + AV board AV device: wraps dev_audio / dev_video (UVC-first).
// Implements iot21 av_device.h (including CS get_event_picture / request_idr).

#include <components/log.h>
#include <components/system.h>
#include <os/mem.h>
#include <os/os.h>
#include <string.h>

#include "av_device.h"

#if (CONFIG_DEV_AUDIO_EN > 0)
#  include "dev_audio.h"
#endif
#if (CONFIG_DEV_VIDEO_EN > 0)
#  include "dev_video.h"
#endif

#define TAG "av_device_gen"
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)

#if (CONFIG_DEV_UVC_CAMERA_EN > 0)
#  define AV_DEVICE_CAMERA_TYPE UVC_CAMERA
#elif (CONFIG_DEV_DVP_CAMERA_EN > 0)
#  define AV_DEVICE_CAMERA_TYPE DVP_CAMERA
#else
#  error "Enable CONFIG_DEV_DVP_CAMERA_EN or CONFIG_DEV_UVC_CAMERA_EN"
#endif

#define AV_DEVICE_VIDEO_FORMAT DEV_VIDEO_FORMAT_H264
#define AV_DEVICE_VIDEO_ROTATE ROTATE_NONE

// 分辨率见 iot_demo_config.h（SMP+AV UVC → 640x480）。

typedef struct {
  audio_device_callback_t cb;
  void *user_data;
} audio_listener_t;

typedef struct {
  video_device_callback_t cb;
  void *user_data;
} video_listener_t;

static audio_listener_t g_audio_listener = {NULL, NULL};
static video_listener_t g_video_listener = {NULL, NULL};
static bool g_audio_started = false;
static bool g_video_started = false;
static bool g_inited = false;

static void av_device_audio_read_callback(const uint8_t *data, uint32_t len, void *user_data);
static void av_device_video_frame_callback(const uint8_t *data, uint32_t len, bool is_key_frame,
                                           void *user_data);
static bk_err_t _audio_open(void);
static bk_err_t _audio_close(void);
static bk_err_t _video_open(void);
static bk_err_t _video_close(void);

void av_device_init(const av_device_config_t *cfg) {
  (void)cfg;
  if (g_inited) {
    return;
  }
  g_audio_listener.cb = NULL;
  g_audio_listener.user_data = NULL;
  g_video_listener.cb = NULL;
  g_video_listener.user_data = NULL;
  g_audio_started = false;
  g_video_started = false;
  g_inited = true;
}

void av_device_deinit(void) {
  if (!g_inited) {
    return;
  }
  if (g_audio_started || g_video_started) {
    LOGW("audio/video still started, stop before deinit\n");
  }
  g_audio_listener.cb = NULL;
  g_audio_listener.user_data = NULL;
  g_video_listener.cb = NULL;
  g_video_listener.user_data = NULL;
  g_audio_started = false;
  g_video_started = false;
  g_inited = false;
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  (void)audio_file_path;
  bool need_open = false;

  if (!callback) {
    return -1;
  }
  if (!g_inited) {
    return -1;
  }
  if (g_audio_listener.cb && g_audio_listener.cb == callback &&
      g_audio_listener.user_data == user_data) {
    return 0;
  }
  if (g_audio_listener.cb) {
    LOGW("audio listener already occupied\n");
    return -1;
  }

  need_open = !g_audio_started;
  if (need_open) {
    if (_audio_open() != BK_OK) {
      LOGE("audio open fail\n");
      return -1;
    }
    g_audio_started = true;
    LOGI("audio capture started\n");
  }
  g_audio_listener.cb = callback;
  g_audio_listener.user_data = user_data;
  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  bool do_close = false;
  if (!g_audio_listener.cb || g_audio_listener.cb != callback ||
      g_audio_listener.user_data != user_data) {
    return;
  }
  g_audio_listener.cb = NULL;
  g_audio_listener.user_data = NULL;
  if (g_audio_started) {
    g_audio_started = false;
    do_close = true;
  }
  if (do_close) {
    _audio_close();
    LOGI("audio capture stopped\n");
  }
}

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  (void)video_file_path;
  bool need_open = false;

  if (!callback) {
    return -1;
  }
  if (!g_inited) {
    return -1;
  }
  if (g_video_listener.cb && g_video_listener.cb == callback &&
      g_video_listener.user_data == user_data) {
    return 0;
  }
  if (g_video_listener.cb) {
    LOGW("video listener already occupied\n");
    return -1;
  }

  need_open = !g_video_started;
  if (need_open) {
    if (_video_open() != BK_OK) {
      LOGE("video open fail\n");
      return -1;
    }
    g_video_started = true;
    LOGI("video capture started[%ux%u]\n", VIDEO_WIDTH, VIDEO_HEIGHT);
  }
  g_video_listener.cb = callback;
  g_video_listener.user_data = user_data;
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  bool do_close = false;
  if (!g_video_listener.cb || g_video_listener.cb != callback ||
      g_video_listener.user_data != user_data) {
    return;
  }
  g_video_listener.cb = NULL;
  g_video_listener.user_data = NULL;
  if (g_video_started) {
    g_video_started = false;
    do_close = true;
  }
  if (do_close) {
    _video_close();
    LOGI("video capture stopped\n");
  }
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
#if (CONFIG_DEV_AUDIO_EN > 0)
  if (!data || len == 0) {
    return;
  }
  if (!dev_audio_is_open()) {
    return;
  }
#  if (DEV_AUDIO_VOICE_WRITE_EN > 0)
  dev_audio_write_spk(data, len);
#  endif
#else
  (void)data;
  (void)len;
#endif
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  (void)data;
  (void)len;
}

void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  // bk7258 当前不渲染远端视频；保留 stub 以对齐 Linux 接口。
  (void)data;
  (void)len;
}

void av_device_request_idr(void) {
  // UVC H264 path has no public IDR force API in dev_video; log and rely on
  // encoder periodic IDR / next key frame.
  LOGW("request_idr: no-op on AV UVC path (no public IDR API)\n");
}

uint64_t av_device_now_ms(void) {
  return rtos_get_time();
}

uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path) {
  (void)mp4_file_path;
  return 0;
}

uint64_t av_device_seek_mp4_ms(uint64_t target_ms) {
  (void)target_ms;
  return 0;
}

int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  (void)event_id;
  if (!buf || !out_len || buf_size == 0) {
    return -1;
  }
  *out_len = 0;
#if (CONFIG_DEV_VIDEO_EN > 0)
  if (!g_video_started || !dev_video_is_open()) {
    LOGE("get_event_picture: video not started\n");
    return -1;
  }
  if (dev_video_copy_last_mjpeg(buf, buf_size, out_len) != BK_OK || *out_len == 0) {
    LOGE("get_event_picture: no MJPEG snap yet\n");
    return -1;
  }
  return 0;
#else
  LOGE("get_event_picture: video disabled\n");
  return -1;
#endif
}

static void av_device_audio_read_callback(const uint8_t *data, uint32_t len, void *user_data) {
  (void)user_data;
  if (!data || len == 0 || !g_audio_started || !g_audio_listener.cb) {
    return;
  }
  g_audio_listener.cb(data, len, g_audio_listener.user_data);
}

static void av_device_video_frame_callback(const uint8_t *data, uint32_t len, bool is_key_frame,
                                           void *user_data) {
  (void)user_data;
  if (!data || len == 0 || !g_video_started || !g_video_listener.cb) {
    return;
  }
  g_video_listener.cb(data, len, is_key_frame, TC_IOT_VIDEO_CODEC_H264, g_video_listener.user_data);
}

static bk_err_t _audio_open(void) {
#if (CONFIG_DEV_AUDIO_EN > 0)
  if (dev_audio_is_open()) {
    return BK_OK;
  }
  dev_audio_config_t config = {0};
  config.type = DEV_AUDIO_TYPE_ONBOARD;
  config.mic_codec_format = DEV_AUDIO_CODEC_FORMAT_PCM;
  config.mic_sample_rate = (dev_audio_sample_rate_t)MIC_SAMPLE_RATE;
  config.spk_codec_format = DEV_AUDIO_CODEC_FORMAT_PCM;
  config.spk_sample_rate = (dev_audio_sample_rate_t)SPK_SAMPLE_RATE;
  config.aec_en = true;
  config.aec_mode = DEV_AUDIO_AEC_MODE_HW;
  config.read_cb = av_device_audio_read_callback;
  return dev_audio_open(&config);
#else
  LOGE("CONFIG_DEV_AUDIO_EN disabled\n");
  return BK_FAIL;
#endif
}

static bk_err_t _audio_close(void) {
#if (CONFIG_DEV_AUDIO_EN > 0)
  if (!dev_audio_is_open()) {
    return BK_OK;
  }
  return dev_audio_close();
#else
  return BK_OK;
#endif
}

static bk_err_t _video_open(void) {
#if (CONFIG_DEV_VIDEO_EN > 0)
  if (dev_video_is_open()) {
    return BK_OK;
  }
  dev_video_config_t config = {0};
  config.camera_type = AV_DEVICE_CAMERA_TYPE;
  config.format = AV_DEVICE_VIDEO_FORMAT;
  config.width = VIDEO_WIDTH;
  config.height = VIDEO_HEIGHT;
  config.fps = VIDEO_FPS;
  config.rotate = AV_DEVICE_VIDEO_ROTATE;
  config.frame_cb = av_device_video_frame_callback;
  config.frame_cb_args = NULL;
  return dev_video_open(&config);
#else
  LOGE("CONFIG_DEV_VIDEO_EN disabled\n");
  return BK_FAIL;
#endif
}

static bk_err_t _video_close(void) {
#if (CONFIG_DEV_VIDEO_EN > 0)
  if (!dev_video_is_open()) {
    return BK_OK;
  }
  return dev_video_close();
#else
  return BK_OK;
#endif
}
