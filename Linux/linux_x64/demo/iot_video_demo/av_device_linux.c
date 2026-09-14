// Copyright (c) 2026 Tencent. All rights reserved.

// Linux 平台的 av_device 实现：用 file_stream 模拟"已上电"的麦克风 / 摄像头。
//
// 与 bk7258 实现对称（接口签名完全一致），差异：
//   - 数据来源是 PCM / H.264 文件，不是真硬件；
//   - 下行 playout 写入 dump 文件，不是 SPK；
//   - 采集模型：
//       * 音频是全局共享的单一采集源（与硬件平台只有一只麦克风一致）：整个进程
//         只有一路音频采集线程 + 一个文件流，重复 start 视为同一监听者；
//       * 视频每路 start_video 注册一路独立采集（独立文件流 + 独立线程），
//         不同 sender 可以用不同的视频文件并行采集。
//   - 支持 .mp4 文件作为音视频源：自动解封装→音频 AAC 帧透传 / 视频
//     H.264 MP4 格式转 Annex-B 后回调。
// #define _FILE_OFFSET_BITS 64
// #define _LARGEFILE_SOURCE 1
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "av_device.h"
#include "file_stream.h"
#include "minimp4.h"

#define MAX_CAPTURES 8

// ==================== 类型 ====================

// 一路独立的音频采集：自己的文件流 + 线程 + 回调。
// 全局只有一个实例（音频全局共享单一采集源）。
typedef struct {
  bool active;
  audio_device_callback_t cb;
  void *user_data;
  char file_path[256];
  void *stream;
  pthread_t thread;
  volatile int running;

  // MP4 模式专用
  bool is_mp4;               // 是否从 MP4 采集
  unsigned mp4_frame_index;  // 当前帧游标
} audio_capture_t;

// 一路独立的视频采集。
typedef struct {
  bool active;
  video_device_callback_t cb;
  void *user_data;
  char file_path[256];
  void *stream;
  pthread_t thread;
  volatile int running;

  // MP4 模式专用
  bool is_mp4;               // 是否从 MP4 采集
  unsigned mp4_frame_index;  // 当前帧游标（每路独立）

  // file_stream 模式专用：根据文件名后缀识别出的视频 codec（start_video 阶段确定后不再变）
  tc_iot_video_codec_e video_codec;
} video_capture_t;

typedef struct {
  bool inited;

  // 音频：全局共享的单一采集源。
  pthread_mutex_t audio_mutex;
  audio_capture_t audio_cap;

  pthread_mutex_t video_mutex;
  video_capture_t video_caps[MAX_CAPTURES];

  // 下行 dump
  pthread_mutex_t dump_mutex;
  FILE *dump_audio;
  FILE *dump_video;

  // mjpg 帧保存目录：init 时根据 cfg->dump_video_path 是否是目录决定
  char dump_mjpeg_dir[256];

  // === MP4 解封装（全局共享，懒加载）===
  MP4D_demux_t mp4_demux_context;
  int mp4_audio_track;             // 音频 track 索引，-1 表示无
  int mp4_video_track;             // 视频 track 索引，-1 表示无
  unsigned mp4_audio_frame_count;  // 音频总帧数
  unsigned mp4_video_frame_count;  // 视频总帧数
  bool mp4_is_demuxed;             // MP4 是否已成功解析
  int mp4_file_descriptor;         // 供 _read_mp4_frame 用 pread 读取

  unsigned mp4_audio_timescale;  // 音频 track 时间基（MP4 模式帧间隔计算）

  // H.264 Annex-B 格式的 SPS+PPS 前缀
  uint8_t *mp4_sps_pps;
  uint32_t mp4_sps_pps_size;

  // MP4 seek 起始帧索引（在 start 前设置，线程启动后读取一次）
  unsigned mp4_audio_seek_frame;
  unsigned mp4_video_seek_frame;
} av_device_ctx_t;

static av_device_ctx_t g = {
    .audio_mutex = PTHREAD_MUTEX_INITIALIZER,
    .video_mutex = PTHREAD_MUTEX_INITIALIZER,
    .dump_mutex = PTHREAD_MUTEX_INITIALIZER,
    .mp4_audio_track = -1,
    .mp4_video_track = -1,
    .mp4_file_descriptor = -1,
};

// Annex-B 起始码（H.264）
static const uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// ==================== 内部函数前向声明 ====================

static void *_audio_thread_func(void *arg);
static void *_video_thread_func(void *arg);

// MP4 辅助函数
static int _mp4_read_callback(int64_t offset, void *buffer, size_t size, void *token);
static int _mp4_demux(const char *file_path);
static int _read_mp4_frame(int track, unsigned frame_index, uint8_t **out_data, uint32_t *out_size,
                           unsigned *timestamp, unsigned *duration);
static uint8_t *_mp4_frame_to_annex_b(const uint8_t *data, uint32_t size, const uint8_t *sps_pps,
                                      uint32_t sps_pps_size, uint32_t *out_size, bool *is_keyframe);
static unsigned _seek_mp4_frame(const MP4D_track_t *track, uint64_t target_timestamp);

// ==================== 生命周期 ====================

void av_device_init(const av_device_config_t *cfg) {
  if (g.inited) {
    // 幂等
    return;
  }
  memset(&g.audio_cap, 0, sizeof(g.audio_cap));
  memset(g.video_caps, 0, sizeof(g.video_caps));

  // 初始化 MP4 字段
  g.mp4_is_demuxed = false;
  g.mp4_audio_track = -1;
  g.mp4_video_track = -1;
  g.mp4_audio_seek_frame = 0;
  g.mp4_video_seek_frame = 0;
  g.mp4_sps_pps = NULL;
  g.mp4_sps_pps_size = 0;
  g.mp4_audio_timescale = 0;
  g.mp4_file_descriptor = -1;

  // 打开下行 dump 文件
  pthread_mutex_lock(&g.dump_mutex);
  if (cfg && cfg->dump_audio_path) {
    g.dump_audio = fopen(cfg->dump_audio_path, "wb");
    if (!g.dump_audio) {
      fprintf(stderr, "[av_device_linux] open dump audio failed: %s\n", cfg->dump_audio_path);
    }
  }
  // mjpg 默认目录：仅当 cfg->dump_video_path 不是目录（NULL/空/或一个普通文件路径）时使用
  strncpy(g.dump_mjpeg_dir, "./recv_mjpg", sizeof(g.dump_mjpeg_dir) - 1);
  g.dump_mjpeg_dir[sizeof(g.dump_mjpeg_dir) - 1] = '\0';
  if (cfg && cfg->dump_video_path) {
    struct stat st;
    if (stat(cfg->dump_video_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      // 用户配置的是目录：mjpg 用它；H.264 单文件 dump 在该场景下不适用，保持关闭
      strncpy(g.dump_mjpeg_dir, cfg->dump_video_path, sizeof(g.dump_mjpeg_dir) - 1);
      g.dump_mjpeg_dir[sizeof(g.dump_mjpeg_dir) - 1] = '\0';
    } else {
      // 不是目录：当作 H.264 单文件 dump 处理（保持原有行为）
      g.dump_video = fopen(cfg->dump_video_path, "wb");
      if (!g.dump_video) {
        fprintf(stderr, "[av_device_linux] open dump video failed: %s\n", cfg->dump_video_path);
      }
    }
  }
  pthread_mutex_unlock(&g.dump_mutex);

  // 提前解封装 MP4，使 seek 命令在采集启动前即可用
  if (cfg && cfg->mp4_file_path && cfg->mp4_file_path[0] != '\0') {
    if (_mp4_demux(cfg->mp4_file_path) == 0) {
      printf("[av_device_linux] init: mp4=%s demuxed (audio_track=%d video_track=%d)\n",
             cfg->mp4_file_path, g.mp4_audio_track, g.mp4_video_track);
    } else {
      fprintf(stderr, "[av_device_linux] init: mp4 demux failed: %s\n", cfg->mp4_file_path);
    }
  }

  g.inited = true;
  printf("[av_device_linux] init: dump_audio=%s dump_video=%s mjpg_dir=%s\n",
         (cfg && cfg->dump_audio_path) ? cfg->dump_audio_path : "(none)",
         (cfg && cfg->dump_video_path) ? cfg->dump_video_path : "(none)", g.dump_mjpeg_dir);
}

void av_device_deinit(void) {
  if (!g.inited) {
    return;
  }

  // 停止全局音频采集线程
  audio_capture_t *cap = &g.audio_cap;
  if (cap->active) {
    if (cap->running) {
      cap->running = 0;
      pthread_join(cap->thread, NULL);
    }
    if (cap->stream) {
      file_stream_exit(cap->stream);
      cap->stream = NULL;
    }
    memset(cap, 0, sizeof(*cap));
  }

  // 停止所有视频采集线程
  for (int i = 0; i < MAX_CAPTURES; i++) {
    video_capture_t *vcap = &g.video_caps[i];
    if (vcap->active) {
      if (vcap->running) {
        vcap->running = 0;
        pthread_join(vcap->thread, NULL);
      }
      if (vcap->stream) {
        file_stream_exit(vcap->stream);
        vcap->stream = NULL;
      }
      memset(vcap, 0, sizeof(*vcap));
    }
  }

  // 释放 MP4 资源
  if (g.mp4_is_demuxed) {
    MP4D_close(&g.mp4_demux_context);
    g.mp4_is_demuxed = false;
  }
  if (g.mp4_file_descriptor >= 0) {
    close(g.mp4_file_descriptor);
    g.mp4_file_descriptor = -1;
  }
  free(g.mp4_sps_pps);
  g.mp4_sps_pps = NULL;
  g.mp4_sps_pps_size = 0;

  pthread_mutex_lock(&g.dump_mutex);
  if (g.dump_audio) {
    fclose(g.dump_audio);
    g.dump_audio = NULL;
  }
  if (g.dump_video) {
    fclose(g.dump_video);
    g.dump_video = NULL;
  }
  pthread_mutex_unlock(&g.dump_mutex);

  g.inited = false;
}

// ==================== 音频采集 ====================

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  if (!g.inited || !callback) {
    return -1;
  }
  if (!audio_file_path || audio_file_path[0] == '\0') {
    fprintf(stderr, "[av_device_linux] audio file path not configured\n");
    return -1;
  }

  // 尝试以 MP4 格式解析
  bool is_mp4 = (_mp4_demux(audio_file_path) == 0 && g.mp4_audio_track >= 0);

  pthread_mutex_lock(&g.audio_mutex);
  audio_capture_t *cap = &g.audio_cap;
  // 全局共享单一采集源：同一监听者重复 start 视为幂等；已被占用则拒绝。
  if (cap->active) {
    if (cap->cb == callback && cap->user_data == user_data) {
      pthread_mutex_unlock(&g.audio_mutex);
      return 0;
    }
    pthread_mutex_unlock(&g.audio_mutex);
    fprintf(stderr, "[av_device_linux] audio listener already occupied\n");
    return -1;
  }

  memset(cap, 0, sizeof(*cap));
  cap->cb = callback;
  cap->user_data = user_data;
  strncpy(cap->file_path, audio_file_path, sizeof(cap->file_path) - 1);
  cap->is_mp4 = is_mp4;
  cap->mp4_frame_index = g.mp4_audio_seek_frame;

  if (is_mp4) {
    cap->running = 1;
    cap->active = true;
    if (pthread_create(&cap->thread, NULL, _audio_thread_func, cap) != 0) {
      cap->running = 0;
      cap->active = false;
      pthread_mutex_unlock(&g.audio_mutex);
      fprintf(stderr, "[av_device_linux] audio pthread_create failed\n");
      return -1;
    }
    pthread_mutex_unlock(&g.audio_mutex);
    printf("[av_device_linux] audio capture started (mp4): %s\n", cap->file_path);
    return 0;
  }

  // 原有 file_stream 模式
  cap->stream = file_stream_init(cap->file_path);
  if (!cap->stream) {
    pthread_mutex_unlock(&g.audio_mutex);
    fprintf(stderr, "[av_device_linux] open audio file failed: %s\n", cap->file_path);
    return -1;
  }
  cap->running = 1;
  cap->active = true;
  if (pthread_create(&cap->thread, NULL, _audio_thread_func, cap) != 0) {
    cap->running = 0;
    cap->active = false;
    file_stream_exit(cap->stream);
    cap->stream = NULL;
    pthread_mutex_unlock(&g.audio_mutex);
    fprintf(stderr, "[av_device_linux] audio pthread_create failed\n");
    return -1;
  }
  pthread_mutex_unlock(&g.audio_mutex);

  printf("[av_device_linux] audio capture started: %s\n", cap->file_path);
  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  if (!g.inited) {
    return;
  }
  pthread_mutex_lock(&g.audio_mutex);
  audio_capture_t *cap = &g.audio_cap;
  if (!cap->active || cap->cb != callback || cap->user_data != user_data) {
    pthread_mutex_unlock(&g.audio_mutex);
    return;
  }
  // 标记停止；线程 join 放在锁外，避免与采集线程回调互锁。
  cap->running = 0;
  pthread_t thread = cap->thread;
  void *stream = cap->stream;
  cap->stream = NULL;
  cap->active = false;
  cap->cb = NULL;
  cap->user_data = NULL;
  pthread_mutex_unlock(&g.audio_mutex);

  pthread_join(thread, NULL);
  if (stream) {
    file_stream_exit(stream);
  }
  printf("[av_device_linux] audio capture stopped\n");
}

// 音频采集线程：约 MIC_FRAME_MS / 帧
static void *_audio_thread_func(void *arg) {
  audio_capture_t *cap = (audio_capture_t *)arg;

  // === MP4 模式：直接发送原始 AAC 帧，不进行解码 ===
  if (cap->is_mp4) {
    unsigned frame_index = cap->mp4_frame_index;
    uint8_t *frame_buffer = NULL;

    while (cap->running) {
      if (frame_index >= g.mp4_audio_frame_count) {
        frame_index = 0;  // 循环播放
      }

      uint32_t frame_size;
      unsigned timestamp, duration;
      if (_read_mp4_frame(g.mp4_audio_track, frame_index, &frame_buffer, &frame_size, &timestamp,
                          &duration) == 0) {
        if (cap->running && cap->cb) {
          cap->cb(frame_buffer, frame_size, cap->user_data);
        }
        free(frame_buffer);
        frame_buffer = NULL;
      }
      frame_index++;

      // 按帧实际时长等待（duration 和 timescale 均为 track 时间基单位）
      unsigned sleep_us = g.mp4_audio_timescale > 0
                              ? (unsigned)((uint64_t)duration * 1000000 / g.mp4_audio_timescale)
                              : (unsigned)(MIC_FRAME_MS * 1000);
      usleep(sleep_us);
    }
    free(frame_buffer);
    return NULL;
  }

  // === 原有 file_stream 模式 ===
  frame_data_s frame;

  while (cap->running) {
    if (cap->stream && file_stream_get_frame(cap->stream, &frame) == 0) {
      if (cap->running && cap->cb) {
        cap->cb(frame.data, frame.data_size, cap->user_data);
      }
    }
    usleep(MIC_FRAME_MS * 1000);
  }
  return NULL;
}

// ==================== 视频采集 ====================

int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  if (!g.inited || !callback) {
    return -1;
  }
  if (!video_file_path || video_file_path[0] == '\0') {
    fprintf(stderr, "[av_device_linux] video file path not configured\n");
    return -1;
  }

  // 尝试以 MP4 格式解析
  bool is_mp4 = (_mp4_demux(video_file_path) == 0 && g.mp4_video_track >= 0);

  pthread_mutex_lock(&g.video_mutex);
  for (int i = 0; i < MAX_CAPTURES; i++) {
    if (g.video_caps[i].active && g.video_caps[i].cb == callback &&
        g.video_caps[i].user_data == user_data) {
      pthread_mutex_unlock(&g.video_mutex);
      return 0;
    }
  }
  int slot = -1;
  for (int i = 0; i < MAX_CAPTURES; i++) {
    if (!g.video_caps[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&g.video_mutex);
    fprintf(stderr, "[av_device_linux] video captures full\n");
    return -1;
  }

  video_capture_t *cap = &g.video_caps[slot];
  memset(cap, 0, sizeof(*cap));
  cap->cb = callback;
  cap->user_data = user_data;
  strncpy(cap->file_path, video_file_path, sizeof(cap->file_path) - 1);
  cap->is_mp4 = is_mp4;
  cap->mp4_frame_index = g.mp4_video_seek_frame;

  if (is_mp4) {
    cap->running = 1;
    cap->active = true;
    if (pthread_create(&cap->thread, NULL, _video_thread_func, cap) != 0) {
      cap->running = 0;
      cap->active = false;
      pthread_mutex_unlock(&g.video_mutex);
      fprintf(stderr, "[av_device_linux] video pthread_create failed\n");
      return -1;
    }
    pthread_mutex_unlock(&g.video_mutex);
    printf("[av_device_linux] video capture started (mp4): %s\n", cap->file_path);
    return 0;
  }

  // 原有 file_stream 模式
  cap->stream = file_stream_init(cap->file_path);
  if (!cap->stream) {
    pthread_mutex_unlock(&g.video_mutex);
    fprintf(stderr, "[av_device_linux] open video file failed: %s\n", cap->file_path);
    return -1;
  }

  cap->video_codec = TC_IOT_VIDEO_CODEC_H264;
  stream_format_s fmt;
  if (file_stream_get_format(cap->stream, &fmt) == 0) {
    switch (fmt.codec_type) {
      case CODEC_H264:
        cap->video_codec = TC_IOT_VIDEO_CODEC_H264;
        break;
      case CODEC_H265:
        cap->video_codec = TC_IOT_VIDEO_CODEC_H265;
        break;
      case CODEC_MJPG:
        cap->video_codec = TC_IOT_VIDEO_CODEC_MJPEG;
        break;
      default:
        break;
    }
  }
  cap->running = 1;
  cap->active = true;
  if (pthread_create(&cap->thread, NULL, _video_thread_func, cap) != 0) {
    cap->running = 0;
    cap->active = false;
    file_stream_exit(cap->stream);
    cap->stream = NULL;
    pthread_mutex_unlock(&g.video_mutex);
    fprintf(stderr, "[av_device_linux] video pthread_create failed\n");
    return -1;
  }
  pthread_mutex_unlock(&g.video_mutex);

  printf("[av_device_linux] video capture started: %s (video_codec=%d)\n", cap->file_path,
         cap->video_codec);
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  if (!g.inited) {
    return;
  }
  pthread_mutex_lock(&g.video_mutex);
  video_capture_t *cap = NULL;
  for (int i = 0; i < MAX_CAPTURES; i++) {
    if (g.video_caps[i].active && g.video_caps[i].cb == callback &&
        g.video_caps[i].user_data == user_data) {
      cap = &g.video_caps[i];
      break;
    }
  }
  if (!cap) {
    pthread_mutex_unlock(&g.video_mutex);
    return;
  }
  cap->running = 0;
  pthread_t thread = cap->thread;
  void *stream = cap->stream;
  cap->stream = NULL;
  cap->active = false;
  pthread_mutex_unlock(&g.video_mutex);

  pthread_join(thread, NULL);
  if (stream) {
    file_stream_exit(stream);
  }
  printf("[av_device_linux] video capture stopped\n");
}

// 视频采集线程：约 1000 / VIDEO_FPS 毫秒一帧
static void *_video_thread_func(void *arg) {
  video_capture_t *cap = (video_capture_t *)arg;

  // === MP4 模式：从 MP4 解封装读取 H.264 帧，转换为 Annex-B 格式 ===
  if (cap->is_mp4) {
    unsigned frame_index = cap->mp4_frame_index;
    uint8_t *raw_buffer = NULL;
    const useconds_t period_us = (useconds_t)(1000000 / (VIDEO_FPS > 0 ? VIDEO_FPS : 25));

    while (cap->running) {
      if (frame_index >= g.mp4_video_frame_count) {
        frame_index = 0;  // 循环播放
      }

      uint32_t raw_size;
      unsigned timestamp, duration;
      if (_read_mp4_frame(g.mp4_video_track, frame_index, &raw_buffer, &raw_size, &timestamp,
                          &duration) == 0) {
        // H.264 MP4 格式 -> Annex-B 格式，检测关键帧并注入 SPS/PPS
        uint32_t annexb_size;
        bool is_keyframe;
        uint8_t *annexb = _mp4_frame_to_annex_b(raw_buffer, raw_size, g.mp4_sps_pps,
                                                g.mp4_sps_pps_size, &annexb_size, &is_keyframe);
        if (annexb && annexb_size > 0) {
          if (cap->running && cap->cb) {
            cap->cb(annexb, annexb_size, is_keyframe, TC_IOT_VIDEO_CODEC_H264, cap->user_data);
          }
          free(annexb);
        }
        free(raw_buffer);
        raw_buffer = NULL;
      }
      frame_index++;
      usleep(period_us);
    }
    free(raw_buffer);
    return NULL;
  }

  // === 原有 file_stream 模式 ===
  frame_data_s frame;
  const useconds_t period_us = (useconds_t)(1000000 / (VIDEO_FPS > 0 ? VIDEO_FPS : 25));

  while (cap->running) {
    if (cap->stream && file_stream_get_frame(cap->stream, &frame) == 0) {
      if (cap->running && cap->cb) {
        cap->cb(frame.data, frame.data_size, frame.is_key_frame ? true : false, cap->video_codec,
                cap->user_data);
      }
    }
    usleep(period_us);
  }
  return NULL;
}

// ==================== MP4 解封装辅助函数 ====================

// minimp4 文件读取回调
static int _mp4_read_callback(int64_t offset, void *buffer, size_t size, void *token) {
  FILE *file = (FILE *)token;
  if (fseeko(file, offset, SEEK_SET) != 0) {
    return -1;
  }
  size_t bytes_read = fread(buffer, 1, size, file);
  return (bytes_read == size) ? 0 : -1;
}

// 对每一帧 H.264 数据，将 MP4 风格转换为 Annex-B 格式
static uint8_t *_mp4_frame_to_annex_b(const uint8_t *data, uint32_t size, const uint8_t *sps_pps,
                                      uint32_t sps_pps_size, uint32_t *out_size,
                                      bool *is_keyframe) {
  *is_keyframe = false;

  // 扫描所有 NAL，计算总输出大小并检测关键帧
  uint32_t position = 0, total_nal_size = 0;
  while (position + 4 <= size) {
    uint32_t nal_length = ((uint32_t)data[position] << 24) | ((uint32_t)data[position + 1] << 16) |
                          ((uint32_t)data[position + 2] << 8) | (uint32_t)data[position + 3];
    position += 4;
    if (position + nal_length > size) {
      break;
    }

    if (nal_length > 0 && (data[position] & 0x1F) == 5) {
      *is_keyframe = true;
    }
    total_nal_size += 4 + nal_length;  // 起始码 + NAL 数据
    position += nal_length;
  }

  // 关键帧时在前面追加 SPS/PPS
  uint32_t prefix_size = (*is_keyframe && sps_pps) ? sps_pps_size : 0;
  uint32_t total = prefix_size + total_nal_size;
  if (total == 0) {
    *out_size = 0;
    return NULL;
  }

  uint8_t *output = (uint8_t *)malloc(total);
  if (!output) {
    *out_size = 0;
    *is_keyframe = false;
    return NULL;
  }

  // 写入 SPS/PPS 前缀
  uint32_t output_position = 0;
  if (prefix_size > 0) {
    memcpy(output, sps_pps, prefix_size);
    output_position = prefix_size;
  }

  // 逐 NAL 写入 Annex-B 起始码 + NAL 数据
  position = 0;
  while (position + 4 <= size) {
    uint32_t nal_length = ((uint32_t)data[position] << 24) | ((uint32_t)data[position + 1] << 16) |
                          ((uint32_t)data[position + 2] << 8) | (uint32_t)data[position + 3];
    position += 4;
    if (position + nal_length > size) {
      break;
    }

    memcpy(output + output_position, kAnnexBStartCode, 4);
    output_position += 4;
    memcpy(output + output_position, data + position, nal_length);
    output_position += nal_length;
    position += nal_length;
  }

  *out_size = output_position;
  return output;
}

// MP4 解封装
static int _mp4_demux(const char *file_path) {
  if (g.mp4_is_demuxed) {
    return 0;
  }
  if (!file_path || file_path[0] == '\0') {
    return -1;
  }

  FILE *file = fopen(file_path, "rb");
  if (!file) {
    fprintf(stderr, "[av_device_linux] open mp4 file failed: %s\n", file_path);
    return -1;
  }

  // 获取文件大小
  fseeko(file, 0, SEEK_END);
  int64_t file_size = ftello(file);
  fseeko(file, 0, SEEK_SET);

  memset(&g.mp4_demux_context, 0, sizeof(g.mp4_demux_context));
  int result = MP4D_open(&g.mp4_demux_context, _mp4_read_callback, file, file_size);

  // 保存 fd 供 _read_mp4_frame 使用 pread（线程安全），然后关闭 FILE*
  g.mp4_file_descriptor = dup(fileno(file));
  fclose(file);

  if (!result) {
    fprintf(stderr, "[av_device_linux] MP4D_open failed: %s\n", file_path);
    close(g.mp4_file_descriptor);
    g.mp4_file_descriptor = -1;
    return -1;
  }

  g.mp4_audio_track = -1;
  g.mp4_video_track = -1;
  g.mp4_audio_frame_count = 0;
  g.mp4_video_frame_count = 0;

  for (unsigned i = 0; i < g.mp4_demux_context.track_count; i++) {
    MP4D_track_t *track = &g.mp4_demux_context.track[i];
#if MP4D_INFO_SUPPORTED
    if (track->handler_type == MP4D_HANDLER_TYPE_SOUN) {
      g.mp4_audio_track = (int)i;
      g.mp4_audio_frame_count = track->sample_count;
      g.mp4_audio_timescale = track->timescale;
    } else if (track->handler_type == MP4D_HANDLER_TYPE_VIDE) {
      g.mp4_video_track = i;
      g.mp4_video_frame_count = track->sample_count;
      printf("[av_device_linux] mp4 video track %u: %ux%u samples=%u video_codec=0x%x\n", i,
             track->SampleDescription.video.width, track->SampleDescription.video.height,
             track->sample_count, track->object_type_indication);
    }
#else
    fprintf(stderr,
            "[av_device_linux] ERROR: MP4D_INFO_SUPPORTED is disabled, "
            "cannot distinguish audio/video track type, abort\n");
    MP4D_close(&g.mp4_demux_context);
    close(g.mp4_file_descriptor);
    g.mp4_file_descriptor = -1;
    return -1;
#endif
  }

  // 提取 H.264 SPS/PPS（从 MP4 DSI 拼接为 Annex-B 前缀块，用于注入到关键帧前）
  if (g.mp4_video_track >= 0) {
    free(g.mp4_sps_pps);
    g.mp4_sps_pps = NULL;
    g.mp4_sps_pps_size = 0;

    typedef struct {
      const void *data;
      int bytes;
    } sps_pps_entry_t;

    sps_pps_entry_t sps_list[MINIMP4_MAX_SPS], pps_list[MINIMP4_MAX_PPS];
    int sps_count = 0, pps_count = 0;
    int total_size = 0;

    // 收集 SPS
    while (sps_count < MINIMP4_MAX_SPS) {
      int bytes;
      const void *sps =
          MP4D_read_sps(&g.mp4_demux_context, (unsigned)g.mp4_video_track, sps_count, &bytes);
      if (!sps) {
        break;
      }
      sps_list[sps_count].data = sps;
      sps_list[sps_count].bytes = bytes;
      total_size += 4 + bytes;
      sps_count++;
    }

    // 收集 PPS
    while (pps_count < MINIMP4_MAX_PPS) {
      int bytes;
      const void *pps =
          MP4D_read_pps(&g.mp4_demux_context, (unsigned)g.mp4_video_track, pps_count, &bytes);
      if (!pps) {
        break;
      }
      pps_list[pps_count].data = pps;
      pps_list[pps_count].bytes = bytes;
      total_size += 4 + bytes;
      pps_count++;
    }

    // 拼接 Annex-B 格式：SPS 在前，PPS 在后
    if (total_size > 0) {
      g.mp4_sps_pps = (uint8_t *)malloc((size_t)total_size);
      if (g.mp4_sps_pps) {
        uint8_t *p = g.mp4_sps_pps;
        for (int i = 0; i < sps_count; i++) {
          memcpy(p, kAnnexBStartCode, 4);
          memcpy(p + 4, sps_list[i].data, (size_t)sps_list[i].bytes);
          p += 4 + sps_list[i].bytes;
        }
        for (int i = 0; i < pps_count; i++) {
          memcpy(p, kAnnexBStartCode, 4);
          memcpy(p + 4, pps_list[i].data, (size_t)pps_list[i].bytes);
          p += 4 + pps_list[i].bytes;
        }
        g.mp4_sps_pps_size = (uint32_t)total_size;
        printf("[av_device_linux] mp4 sps/pps extracted: %d sps, %d pps, %u bytes total\n",
               sps_count, pps_count, g.mp4_sps_pps_size);
      }
    }
  }

  g.mp4_is_demuxed = true;

  printf("[av_device_linux] mp4 parsed: %s, audio_track=%d video_track=%d\n", file_path,
         g.mp4_audio_track, g.mp4_video_track);
  return 0;
}

// 从已解析的 MP4 中读取单帧数据
static int _read_mp4_frame(int track, unsigned frame_index, uint8_t **out_data, uint32_t *out_size,
                           unsigned *timestamp, unsigned *duration) {
  unsigned frame_bytes;
  MP4D_file_offset_t offset = MP4D_frame_offset(&g.mp4_demux_context, (unsigned)track, frame_index,
                                                &frame_bytes, timestamp, duration);
  if (frame_bytes == 0 || g.mp4_file_descriptor < 0) {
    *out_data = NULL;
    *out_size = 0;
    return -1;
  }

  uint8_t *buffer = (uint8_t *)malloc(frame_bytes);
  if (!buffer) {
    *out_data = NULL;
    *out_size = 0;
    return -1;
  }

  ssize_t bytes_read = pread(g.mp4_file_descriptor, buffer, frame_bytes, (off_t)offset);
  if (bytes_read != (ssize_t)frame_bytes) {
    free(buffer);
    *out_data = NULL;
    *out_size = 0;
    return -1;
  }

  *out_data = buffer;
  *out_size = frame_bytes;
  return 0;
}

// 二分查找 ≤ target_timestamp 的最大 frame 索引
static unsigned _seek_mp4_frame(const MP4D_track_t *track, uint64_t target_timestamp) {
  unsigned lo = 0, hi = track->sample_count - 1;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo + 1) / 2;
    if (track->timestamp[mid] <= target_timestamp) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

// ==================== 下行 ====================

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  if (!data || len == 0) {
    return;
  }
  pthread_mutex_lock(&g.dump_mutex);
  if (g.dump_audio) {
    fwrite(data, 1, len, g.dump_audio);
    fflush(g.dump_audio);
  }
  pthread_mutex_unlock(&g.dump_mutex);
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  if (!data || len == 0) {
    return;
  }
  pthread_mutex_lock(&g.dump_mutex);
  if (g.dump_video) {
    fwrite(data, 1, len, g.dump_video);
    fflush(g.dump_video);
  }
  pthread_mutex_unlock(&g.dump_mutex);
}

void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  if (!data || len == 0) {
    return;
  }
  // 保存目录：init 时已根据 cfg->dump_video_path 是否为目录确定；非目录则默认 ./recv_mjpg/
  static int count = 0;

  mkdir(g.dump_mjpeg_dir, 0755);  // 目录已存在时忽略错误

  char path[320];
  snprintf(path, sizeof(path), "%s/recv_%d.jpg", g.dump_mjpeg_dir, count++);

  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "[av_device_linux] playout_mjpg: open file failed: %s\n", path);
    return;
  }
  fwrite(data, 1, len, fp);
  fclose(fp);
}

void av_device_request_idr(void) {
  // Linux 仿真：no-op（文件 GOP 已确定，无法重产 IDR）
}

// ==================== MP4 公开接口 ====================

uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path) {
  if (!mp4_file_path || mp4_file_path[0] == '\0') {
    return 0;
  }

  FILE *file = fopen(mp4_file_path, "rb");
  if (!file) {
    fprintf(stderr, "[av_device_linux] get_mp4_duration: open file failed: %s\n", mp4_file_path);
    return 0;
  }

  fseeko(file, 0, SEEK_END);
  int64_t file_size = ftello(file);
  fseeko(file, 0, SEEK_SET);

  MP4D_demux_t demux;
  memset(&demux, 0, sizeof(demux));
  int result = MP4D_open(&demux, _mp4_read_callback, file, file_size);
  fclose(file);

  if (!result) {
    fprintf(stderr, "[av_device_linux] get_mp4_duration: MP4D_open failed: %s\n", mp4_file_path);
    return 0;
  }

#if MP4D_INFO_SUPPORTED
  uint64_t duration = ((uint64_t)demux.duration_hi << 32) | demux.duration_lo;
  if (demux.timescale == 0) {
    MP4D_close(&demux);
    return 0;
  }
  uint64_t duration_ms = duration * 1000 / demux.timescale;
  MP4D_close(&demux);
  return duration_ms;
#else
  MP4D_close(&demux);
  return 0;
#endif
}

uint64_t av_device_seek_mp4_ms(uint64_t target_ms) {
  if (!g.inited) {
    fprintf(stderr, "[av_device_linux] seek_mp4: not inited\n");
    return 0;
  }
  if (!g.mp4_is_demuxed) {
    fprintf(stderr, "[av_device_linux] seek_mp4: no mp4 file demuxed\n");
    return 0;
  }

  uint64_t result_ms = 0;

  // seek 视频 track
  if (g.mp4_video_track >= 0) {
    MP4D_track_t *video_track = &g.mp4_demux_context.track[g.mp4_video_track];
    if (video_track->timescale > 0 && video_track->sample_count > 0) {
      uint64_t target_timestamp = target_ms * (uint64_t)video_track->timescale / 1000;
      g.mp4_video_seek_frame = _seek_mp4_frame(video_track, target_timestamp);
      result_ms =
          (uint64_t)video_track->timestamp[g.mp4_video_seek_frame] * 1000 / video_track->timescale;
      printf("[av_device_linux] seek_mp4 video: target=%llums frame=%u/%u actual=%llums\n",
             (unsigned long long)target_ms, g.mp4_video_seek_frame, video_track->sample_count,
             (unsigned long long)result_ms);
    }
  }

  // seek 音频 track
  if (g.mp4_audio_track >= 0) {
    MP4D_track_t *audio_track = &g.mp4_demux_context.track[g.mp4_audio_track];
    if (audio_track->timescale > 0 && audio_track->sample_count > 0) {
      uint64_t target_timestamp = target_ms * (uint64_t)audio_track->timescale / 1000;
      g.mp4_audio_seek_frame = _seek_mp4_frame(audio_track, target_timestamp);

      // 如果视频 track 不存在，以音频 track 的结果为准
      if (g.mp4_video_track < 0) {
        result_ms = (uint64_t)audio_track->timestamp[g.mp4_audio_seek_frame] * 1000 /
                    audio_track->timescale;
      }
      printf("[av_device_linux] seek_mp4 audio: target=%lums frame=%u/%u actual=%lums\n",
             (unsigned long)target_ms, g.mp4_audio_seek_frame, audio_track->sample_count,
             (unsigned long)((uint64_t)audio_track->timestamp[g.mp4_audio_seek_frame] * 1000 /
                             audio_track->timescale));
    }
  }

  return result_ms;
}

uint64_t av_device_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  if (!buf || !out_len || buf_size == 0) {
    return -1;
  }
  *out_len = 0;

  char path[256];
  if (event_id < 100) {
    snprintf(path, sizeof(path), "./src/demo/iot_video_demo/demo_media/event_pic/%02u.jpg",
             (unsigned)event_id);
  } else {
    snprintf(path, sizeof(path), "./src/demo/iot_video_demo/demo_media/event_pic/%u.jpg",
             (unsigned)event_id);
  }

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "[av_device_linux] open event pic failed: %s\n", path);
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long size = ftell(fp);
  if (size <= 0 || (uint32_t)size > buf_size) {
    fprintf(stderr, "[av_device_linux] event pic too large or invalid: %s (%ld, buf=%u)\n", path,
            size, buf_size);
    fclose(fp);
    return -1;
  }
  fseek(fp, 0, SEEK_SET);
  size_t n = fread(buf, 1, (size_t)size, fp);
  fclose(fp);
  if (n == 0) {
    return -1;
  }
  *out_len = (uint32_t)n;
  return 0;
}
