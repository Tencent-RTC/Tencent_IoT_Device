// Copyright (c) 2026 Tencent. All rights reserved.

/**
 * @file audio_device_linux.c
 * @brief audio_device Linux 实现：
 *   - 采集：从固定 WAV 文件读取 PCM，pacing 线程按帧率推送，按需 OPUS 编码；
 *   - 播放：接收 PCM/OPUS 帧，解码后写入本地 WAV 文件；
 *   - 通道差异已统一为 TRTC 通道。
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_device.h"
#include "wave_io.h"

#ifdef AITALK_USE_WEBSOCKET
#  include "opus.h"
#endif

#ifndef PROJECT_SOURCE_DIR
#  define PROJECT_SOURCE_DIR "."
#endif

#define LOG_TAG "[audio_device]"

#define SEND_WAV_RELPATH "demo/iot_video_demo/demo_media/aitalk_send_audio.wav"
#define RECV_WAV_PATH "./aitalk_recv_audio.wav"

#define TRTC_AAC_FRAME_DURATION_MS 64
#define MAX_FRAME_DURATION_MS 64
#define MAX_PCM_FRAME_BYTES \
  (16000 * 1 * 2 * MAX_FRAME_DURATION_MS / 1000) /* 16k mono 16bit 64ms = 2048 */

#ifdef AITALK_USE_WEBSOCKET
#  define OPUS_MAX_PACKET_BYTES 4000
#  define TRTC_OPUS_FRAME_DURATION_MS 20
#endif

/* ─── 通道/编码配置查询 ─── */

tc_iot_audio_codec_e audio_device_default_codec(void) {
  return TC_IOT_AUDIO_CODEC_AAC;
}

bool audio_device_codec_supported(tc_iot_audio_codec_e codec) {
  return codec == TC_IOT_AUDIO_CODEC_AAC;
}

uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec) {
  (void)codec;
  return TRTC_AAC_FRAME_DURATION_MS; /* 20ms */
}

static const char *_codec_name(tc_iot_audio_codec_e codec) {
  switch (codec) {
    case TC_IOT_AUDIO_CODEC_PCM:
      return "pcm";
    case TC_IOT_AUDIO_CODEC_AAC:
      return "aac";
    case TC_IOT_AUDIO_CODEC_OPUS:
      return "opus";
    default:
      return "unknown";
  }
}

/* ─── 内部状态 ─── */

typedef struct {
  audio_device_config_t config;

  /* 采集侧 */
  WaveReaderHandle reader;
  WaveInfo wav_info;
#ifdef AITALK_USE_WEBSOCKET
  OpusEncoder *opus_enc;
#endif
  pthread_t capture_tid;
  bool capture_thread_started;
  atomic_int capture_stop;
  audio_device_on_capture_t capture_cb;
  void *capture_user_data;
  uint32_t bytes_per_frame;
  uint32_t samples_per_frame_per_ch;

  /* 播放侧 */
  WaveWriterHandle writer;
#ifdef AITALK_USE_WEBSOCKET
  OpusDecoder *opus_dec;
#endif
  pthread_mutex_t writer_lock;

  bool opened;
} audio_device_ctx_t;

static audio_device_ctx_t g_ctx;

/* ─── 工具函数 ─── */

static const char *_send_wav_path(void) {
  static char s_path[1024] = {0};
  if (s_path[0] == '\0') {
#ifdef TC_IOT_DEMO_ROOT
    snprintf(s_path, sizeof(s_path), "%s/iot_video_demo/demo_media/aitalk_send_audio.wav",
             TC_IOT_DEMO_ROOT);
#else
    snprintf(s_path, sizeof(s_path), "%s/%s", PROJECT_SOURCE_DIR, SEND_WAV_RELPATH);
#endif
  }
  return s_path;
}

static void _sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static uint64_t _now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

#ifdef AITALK_USE_WEBSOCKET
static bool _need_opus_encode(tc_iot_audio_codec_e codec) {
  (void)codec;
  return false;
}

#  define _need_opus_decode(codec) ((codec) == TC_IOT_AUDIO_CODEC_OPUS)
#endif  // AITALK_USE_WEBSOCKET

/* ─── 采集线程 ─── */

static void *_capture_thread(void *arg) {
  audio_device_ctx_t *ctx = (audio_device_ctx_t *)arg;
  uint8_t pcm_buf[MAX_PCM_FRAME_BYTES * 2];
#ifdef AITALK_USE_WEBSOCKET
  uint8_t opus_buf[OPUS_MAX_PACKET_BYTES];
#endif
  uint64_t pts_ms = 0;
  uint64_t base_ms = _now_ms();
  uint64_t frame_idx = 0;

  while (!atomic_load(&ctx->capture_stop)) {
    int n = wave_reader_read(ctx->reader, pcm_buf, ctx->bytes_per_frame);
    if (n <= 0) {
      printf(LOG_TAG " capture EOF\n");
      break;
    }
    if ((uint32_t)n < ctx->bytes_per_frame) {
      memset(pcm_buf + n, 0, ctx->bytes_per_frame - (uint32_t)n);
    }

#ifdef AITALK_USE_WEBSOCKET
    if (_need_opus_encode(ctx->config.send_codec)) {
      int encoded =
          opus_encode(ctx->opus_enc, (const opus_int16 *)pcm_buf,
                      (int)ctx->samples_per_frame_per_ch, opus_buf, (opus_int32)sizeof(opus_buf));
      if (encoded <= 0) {
        printf(LOG_TAG " opus_encode failed: %d\n", encoded);
        break;
      }
      ctx->capture_cb(opus_buf, (uint32_t)encoded, TC_IOT_AUDIO_CODEC_OPUS, pts_ms,
                      ctx->capture_user_data);
    } else
#endif
    {
      ctx->capture_cb(pcm_buf, ctx->bytes_per_frame, TC_IOT_AUDIO_CODEC_PCM, pts_ms,
                      ctx->capture_user_data);
    }

    pts_ms += ctx->config.frame_duration_ms;
    frame_idx++;

    uint64_t target = base_ms + frame_idx * ctx->config.frame_duration_ms;
    uint64_t now = _now_ms();
    if (target > now) {
      uint64_t wait = target - now;
      while (wait > 0 && !atomic_load(&ctx->capture_stop)) {
        uint32_t step = wait > 20 ? 20 : (uint32_t)wait;
        _sleep_ms(step);
        wait -= step;
      }
    }
  }

  return NULL;
}

/* ─── 公开 API ─── */

int audio_device_open(const audio_device_config_t *config) {
  if (!config || g_ctx.opened) {
    return -1;
  }
  memset(&g_ctx, 0, sizeof(g_ctx));
  g_ctx.config = *config;
  pthread_mutex_init(&g_ctx.writer_lock, NULL);

  g_ctx.reader = wave_reader_open(_send_wav_path());
  if (!g_ctx.reader) {
    printf(LOG_TAG " open send wav failed: %s\n", _send_wav_path());
    return -1;
  }
  if (wave_reader_get_info(g_ctx.reader, &g_ctx.wav_info) != 0) {
    printf(LOG_TAG " read wav info failed\n");
    wave_reader_close(g_ctx.reader);
    g_ctx.reader = NULL;
    return -1;
  }

  printf(LOG_TAG " send wav: rate=%u ch=%u bits=%u size=%u path=%s\n", g_ctx.wav_info.sample_rate,
         g_ctx.wav_info.num_channels, g_ctx.wav_info.bits_per_sample, g_ctx.wav_info.data_size,
         _send_wav_path());

  g_ctx.bytes_per_frame = (config->sample_rate * config->channels * (config->bits_per_sample / 8) *
                           config->frame_duration_ms) /
                          1000;
  g_ctx.samples_per_frame_per_ch = (config->sample_rate * config->frame_duration_ms) / 1000;

#ifdef AITALK_USE_WEBSOCKET
  if (_need_opus_encode(config->send_codec)) {
    int err = 0;
    g_ctx.opus_enc = opus_encoder_create((opus_int32)config->sample_rate, config->channels,
                                         OPUS_APPLICATION_VOIP, &err);
    if (!g_ctx.opus_enc || err != OPUS_OK) {
      printf(LOG_TAG " opus_encoder_create failed: %d\n", err);
      wave_reader_close(g_ctx.reader);
      g_ctx.reader = NULL;
      return -1;
    }
  }

  if (_need_opus_decode(config->recv_codec)) {
    int err = 0;
    g_ctx.opus_dec = opus_decoder_create((opus_int32)config->sample_rate, config->channels, &err);
    if (!g_ctx.opus_dec || err != OPUS_OK) {
      printf(LOG_TAG " opus_decoder_create failed: %d\n", err);
      if (g_ctx.opus_enc) {
        opus_encoder_destroy(g_ctx.opus_enc);
        g_ctx.opus_enc = NULL;
      }
      wave_reader_close(g_ctx.reader);
      g_ctx.reader = NULL;
      return -1;
    }
  }
#endif

  printf(LOG_TAG " opened: send_codec=%s recv_codec=%s frame_ms=%u\n",
         _codec_name(config->send_codec), _codec_name(config->recv_codec),
         config->frame_duration_ms);

  g_ctx.opened = true;
  return 0;
}

void audio_device_close(void) {
  if (!g_ctx.opened) {
    return;
  }
  audio_device_stop_capture();

#ifdef AITALK_USE_WEBSOCKET
  if (g_ctx.opus_enc) {
    opus_encoder_destroy(g_ctx.opus_enc);
    g_ctx.opus_enc = NULL;
  }
  if (g_ctx.opus_dec) {
    opus_decoder_destroy(g_ctx.opus_dec);
    g_ctx.opus_dec = NULL;
  }
#endif

  if (g_ctx.reader) {
    wave_reader_close(g_ctx.reader);
    g_ctx.reader = NULL;
  }

  pthread_mutex_lock(&g_ctx.writer_lock);
  if (g_ctx.writer) {
    wave_writer_close(g_ctx.writer);
    g_ctx.writer = NULL;
  }
  pthread_mutex_unlock(&g_ctx.writer_lock);
  pthread_mutex_destroy(&g_ctx.writer_lock);

  g_ctx.opened = false;
  printf(LOG_TAG " closed\n");
}

int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!g_ctx.opened || !cb) {
    return -1;
  }
  if (g_ctx.capture_thread_started) {
    return -1;
  }

  g_ctx.capture_cb = cb;
  g_ctx.capture_user_data = user_data;
  atomic_init(&g_ctx.capture_stop, 0);

  int rc = pthread_create(&g_ctx.capture_tid, NULL, _capture_thread, &g_ctx);
  if (rc != 0) {
    printf(LOG_TAG " pthread_create failed: %d\n", rc);
    return -1;
  }
  g_ctx.capture_thread_started = true;
  return 0;
}

void audio_device_stop_capture(void) {
  if (!g_ctx.capture_thread_started) {
    return;
  }
  atomic_store(&g_ctx.capture_stop, 1);
  pthread_join(g_ctx.capture_tid, NULL);
  g_ctx.capture_thread_started = false;
}

int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!g_ctx.opened || !data || size == 0) {
    return -1;
  }

  const uint8_t *pcm_data = data;
  uint32_t pcm_size = size;
#ifdef AITALK_USE_WEBSOCKET
  int16_t decode_buf[16000 * MAX_FRAME_DURATION_MS / 1000];

  if (codec != TC_IOT_AUDIO_CODEC_PCM && g_ctx.opus_dec) {
    int samples = opus_decode(g_ctx.opus_dec, data, (opus_int32)size, decode_buf,
                              (int)(sizeof(decode_buf) / sizeof(decode_buf[0])), 0);
    if (samples <= 0) {
      printf(LOG_TAG " opus_decode failed: %d\n", samples);
      return -1;
    }
    pcm_data = (const uint8_t *)decode_buf;
    pcm_size = (uint32_t)(samples * g_ctx.config.channels * (g_ctx.config.bits_per_sample / 8));
  }
#else
  if (codec != TC_IOT_AUDIO_CODEC_PCM) {
    printf(LOG_TAG " blocked: non-PCM codec on TRTC path, codec=%s\n", _codec_name(codec));
    return -1;
  }
#endif

  pthread_mutex_lock(&g_ctx.writer_lock);
  if (!g_ctx.writer) {
    g_ctx.writer = wave_writer_open(RECV_WAV_PATH, g_ctx.config.sample_rate, g_ctx.config.channels,
                                    g_ctx.config.bits_per_sample);
    if (!g_ctx.writer) {
      printf(LOG_TAG " create recv wav failed: %s\n", RECV_WAV_PATH);
      pthread_mutex_unlock(&g_ctx.writer_lock);
      return -1;
    }
    printf(LOG_TAG " recv wav created: %s\n", RECV_WAV_PATH);
  }
  wave_writer_write(g_ctx.writer, pcm_data, pcm_size);
  pthread_mutex_unlock(&g_ctx.writer_lock);
  return 0;
}

void audio_device_sleep_ms(uint32_t ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}
