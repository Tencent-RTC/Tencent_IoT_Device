#include "file_stream.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PCM_AUDIO_FRAME_LENGTH_MS 40
#define MAX_FILE_NAME_LEN 256
#define MAX_AUDIO_BUF_SIZE (1024 * 32)
#define MAX_VIDEO_BUF_SIZE (1024 * 256)

#define INDEX_SUFFIX "_index.txt"

typedef struct {
  // Numerator
  int32_t num;
  // Denominator
  int32_t den;
} fraction;

typedef struct {
  uint32_t num;
  uint32_t frame_len;
  uint32_t offset;
  uint32_t timestamp;
  char type;
} media_index_info;

typedef struct {
  int wait_for_i_frame;
  FILE *fp_data;
  FILE *fp_index;
  fraction fps;
  media_index_info index_info;
  uint64_t seq;
  // 总 pts，当一个文件播放完以后，这个值会加上这个文件的时长，以便后续计算 pts 使用
  uint64_t pts_total;
  // 每个视频文件或音频文件读完后，重新获取当前时间作为基准时间
  uint64_t base_time_ms;
  stream_format_s format;
  // 每个流实例独占的帧缓冲区，避免多路采集线程并发读帧时共用全局 buffer 造成
  // 数据竞争（一路 fread 覆盖另一路尚未推送的帧）从而导致下游花屏。
  uint8_t *frame_buf;
  uint32_t frame_buf_size;
  // MJPG 专用：每一帧是独立的 .jpg 文件，这里保存其所在目录(含末尾 '/')与
  // 文件名前缀(如 "mjpg_size224x126_pts")，_read_mjpg_frame 据此拼出
  // "目录+前缀+NNN.jpg" 逐帧打开读取。
  char mjpg_dir[MAX_FILE_NAME_LEN];
  char mjpg_prefix[MAX_FILE_NAME_LEN];
} media_file_s;

// ==================== 内部函数前向声明 ====================

static int _parse_file_name(media_file_s *ctx, const char *file_name);
static int _read_audio_frame(media_file_s *ctx, frame_data_s *frame);
static int _read_video_frame(media_file_s *ctx, frame_data_s *frame);
static int _read_mjpg_frame(media_file_s *ctx, frame_data_s *frame);
static int _parse_index(FILE *fp, media_index_info *index);
static int32_t _is_empty_line(const char *line);
static uint64_t _get_time_ms(void);

// ==================== 公开接口 ====================

void *file_stream_init(const char *file_path) {
  media_file_s *ctx = malloc(sizeof(media_file_s));
  if (!ctx) {
    printf("malloc memory %ld failed\n", sizeof(media_file_s));
    return NULL;
  }
  memset(ctx, 0, sizeof(media_file_s));

  // 用 do {...} while (0) 串起"解析文件名 → 打开数据文件 → 打开索引文件"
  // 三个可能失败的步骤；任一失败 break 出循环，统一由循环外的清理段
  // 关闭已经打开的 fp 并 free ctx。
  bool ok = false;
  do {
    // 提取并解析文件名
    const char *file_name = strrchr(file_path, '/');
    if (!file_name) {
      file_name = file_path;
    } else {
      // 跳过 '/'
      file_name++;
    }
    if (_parse_file_name(ctx, file_name) != 0) {
      break;
    }

    // 解析出 codec 后，按音/视频分别分配本实例独占的帧缓冲区。
    ctx->frame_buf_size =
        (ctx->format.codec_type >= CODEC_PCM && ctx->format.codec_type <= CODEC_AAC)
            ? MAX_AUDIO_BUF_SIZE
            : MAX_VIDEO_BUF_SIZE;
    ctx->frame_buf = malloc(ctx->frame_buf_size);
    if (!ctx->frame_buf) {
      printf("malloc frame buffer %u failed\n", ctx->frame_buf_size);
      break;
    }

    ctx->fp_data = fopen(file_path, "rb");
    if (!ctx->fp_data) {
      printf("open file %s failed\n", file_path);
      break;
    }

    // PCM 文件不需要索引文件；MJPG 每帧是独立 .jpg 文件，同样不需要索引。
    if (ctx->format.codec_type != CODEC_PCM && ctx->format.codec_type != CODEC_MJPG) {
      char index_name[MAX_FILE_NAME_LEN];
      strcpy(index_name, file_path);
      char *dot_char_ptr = strrchr(index_name, '.');
      *dot_char_ptr = '_';
      strcat(index_name, INDEX_SUFFIX);
      ctx->fp_index = fopen(index_name, "r");
      if (!ctx->fp_index) {
        printf("open file %s failed\n", index_name);
        break;
      }
    }

    // MJPG：从 file_path 中拆出目录与文件名前缀，_read_mjpg_frame 据此
    // 拼出 "目录+前缀+NNN.jpg" 逐帧打开。file_path 形如
    // .../demo_media/mjpg/mjpg_size224x126_pts001.jpg
    if (ctx->format.codec_type == CODEC_MJPG) {
      const char *slash = strrchr(file_path, '/');
      if (slash) {
        uint32_t dir_len = (uint32_t)(slash - file_path) + 1;  // 含末尾 '/'
        if (dir_len >= sizeof(ctx->mjpg_dir)) {
          dir_len = sizeof(ctx->mjpg_dir) - 1;
        }
        memcpy(ctx->mjpg_dir, file_path, dir_len);
        ctx->mjpg_dir[dir_len] = '\0';
      } else {
        ctx->mjpg_dir[0] = '\0';
      }

      const char *fname = slash ? slash + 1 : file_path;
      // 前缀 = 文件名中 "_pts" 之前(含 "_pts")的部分，后面接 3 位序号 + ".jpg"
      const char *pts_tag = strstr(fname, "_pts");
      if (pts_tag) {
        uint32_t prefix_len = (uint32_t)(pts_tag - fname) + 4;  // 4 == strlen("_pts")
        if (prefix_len >= sizeof(ctx->mjpg_prefix)) {
          prefix_len = sizeof(ctx->mjpg_prefix) - 1;
        }
        memcpy(ctx->mjpg_prefix, fname, prefix_len);
        ctx->mjpg_prefix[prefix_len] = '\0';
      } else {
        // 回退：去掉 ".jpg" 后缀作为前缀
        strncpy(ctx->mjpg_prefix, fname, sizeof(ctx->mjpg_prefix) - 1);
        ctx->mjpg_prefix[sizeof(ctx->mjpg_prefix) - 1] = '\0';
        char *dot = strrchr(ctx->mjpg_prefix, '.');
        if (dot) {
          *dot = '\0';
        }
      }
    }
    printf("dir:%s   mjpg_prefix:%s\n", ctx->mjpg_dir, ctx->mjpg_prefix);

    ok = true;
  } while (0);

  if (ok) {
    return ctx;
  }

  if (ctx->fp_data) {
    fclose(ctx->fp_data);
  }
  if (ctx->fp_index) {
    fclose(ctx->fp_index);
  }
  if (ctx->frame_buf) {
    free(ctx->frame_buf);
  }
  free(ctx);
  return NULL;
}

void file_stream_exit(void *handle) {
  media_file_s *ctx = (media_file_s *)handle;
  if (ctx) {
    if (ctx->fp_data) {
      fclose(ctx->fp_data);
    }
    if (ctx->fp_index) {
      fclose(ctx->fp_index);
    }
    if (ctx->frame_buf) {
      free(ctx->frame_buf);
    }
    free(ctx);
    printf("file stream close!\n");
  }
}

int32_t file_stream_get_format(void *handle, stream_format_s *format) {
  media_file_s *ctx = (media_file_s *)handle;
  if (!ctx) {
    return -1;
  }
  memcpy(format, &ctx->format, sizeof(stream_format_s));
  return 0;
}

int file_stream_get_frame(void *handle, frame_data_s *frame) {
  media_file_s *ctx = (media_file_s *)handle;
  if (!ctx) {
    return -1;
  }

  if (ctx->format.codec_type >= CODEC_PCM && ctx->format.codec_type <= CODEC_AAC) {
    return _read_audio_frame(ctx, frame);
  } else if (ctx->format.codec_type >= CODEC_H264 && ctx->format.codec_type <= CODEC_H265) {
    return _read_video_frame(ctx, frame);
  } else if (ctx->format.codec_type == CODEC_MJPG) {
    return _read_mjpg_frame(ctx, frame);
  } else {
    return -1;
  }
}

// ==================== 内部实现 ====================

static int _parse_file_name(media_file_s *ctx, const char *file_name) {
  // 解析文件名
  if (strstr(file_name, "video") != NULL) {
    // 解析宽高
    const char *size_start = strstr(file_name, "video_size");
    if (size_start) {
      size_start += strlen("video_size");
      sscanf(size_start, "%dx%d", &ctx->format.width, &ctx->format.height);
    } else {
      printf("unsupported video size: %s\n", file_name);
      return 1;
    }

    // 解析FPS
    const char *fps_start = strstr(file_name, "_fps");
    if (fps_start) {
      int fps_num;
      sscanf(fps_start, "_fps%d", &fps_num);
      ctx->fps.num = fps_num;
      ctx->fps.den = 1;
    } else {
      printf("unsupported video fps: %s\n", file_name);
      return 1;
    }

    // 解析格式
    if (strstr(file_name, ".h264") != NULL) {
      ctx->format.codec_type = CODEC_H264;
    } else if (strstr(file_name, ".h265") != NULL) {
      ctx->format.codec_type = CODEC_H265;
    } else {
      printf("unsupported video format: %s\n", file_name);
      return 1;
    }
  } else if (strstr(file_name, "audio") != NULL) {
    // 解析采样率
    const char *sample_start = strstr(file_name, "audio_sample");
    if (sample_start) {
      sample_start += strlen("audio_sample");
      sscanf(sample_start, "%d", (int32_t *)&ctx->format.sample_rate);
    } else {
      printf("unsupported audio sample rate: %s\n", file_name);
      return 1;
    }

    // 解析声道数
    if (strstr(file_name, "_mono_") != NULL) {
      ctx->format.channel = 1;
    } else if (strstr(file_name, "_stereo_") != NULL) {
      ctx->format.channel = 2;
    } else {
      printf("unsupported audio channel: %s\n", file_name);
      return 1;
    }

    // 根据格式进一步解析
    if (strstr(file_name, ".pcm") != NULL) {
      ctx->format.codec_type = CODEC_PCM;

      // 解析位深
      const char *bit_start = strstr(file_name, "_16bit_");
      if (bit_start) {
        ctx->format.bit_width = 16;
      } else if (strstr(file_name, "_24bit_") != NULL) {
        ctx->format.bit_width = 24;
      } else if (strstr(file_name, "_32bit_") != NULL) {
        ctx->format.bit_width = 32;
      } else if (strstr(file_name, "_8bit_") != NULL) {
        ctx->format.bit_width = 8;
      } else {
        printf("unsupported audio bit depth: %s\n", file_name);
        return 1;
      }

      // PCM的fps设为1000/每帧时长
      ctx->fps.num = 1000 / PCM_AUDIO_FRAME_LENGTH_MS;
      ctx->fps.den = 1;
    } else if (strstr(file_name, ".aac") != NULL) {
      ctx->format.codec_type = CODEC_AAC;
      // aac的fps设为采样率/1024（aac每帧固定都是1024采样点）
      ctx->fps.num = ctx->format.sample_rate;
      ctx->fps.den = 1024;
    } else {
      printf("unsupported audio format: %s\n", file_name);
      return 1;
    }
  } else if (strstr(file_name, "mjpg") != NULL) {
    // MJPG：每一帧是独立的 .jpg 文件，文件名形如 mjpg_size224x126_pts001.jpg
    // 解析宽高
    const char *size_start = strstr(file_name, "size");
    if (size_start) {
      size_start += strlen("size");
      sscanf(size_start, "%dx%d", &ctx->format.width, &ctx->format.height);
    } else {
      printf("unsupported mjpg size: %s\n", file_name);
      return 1;
    }
    // 文件名未携带 fps，默认 25fps
    ctx->fps.num = 25;
    ctx->fps.den = 1;
    ctx->format.codec_type = CODEC_MJPG;
  } else {
    printf("unsupported file format: %s\n", file_name);
    return 1;
  }
  return 0;
}

static int _read_audio_frame(media_file_s *ctx, frame_data_s *frame) {
  media_index_info *index_info = &ctx->index_info;

  if (ctx->format.codec_type == CODEC_PCM) {
    uint64_t cur_time = _get_time_ms();
    if (ctx->base_time_ms) {
      // 根据索引中的时间戳控制帧率
      if (ctx->base_time_ms + index_info->timestamp > cur_time) {
        return 1;
      }
      index_info->num += 1;
      index_info->frame_len = ctx->format.sample_rate * ctx->format.channel *
                              ctx->format.bit_width / 8 / (1000 / PCM_AUDIO_FRAME_LENGTH_MS);
      index_info->offset += index_info->frame_len;
      index_info->timestamp += PCM_AUDIO_FRAME_LENGTH_MS;
      index_info->type = 'A';
    } else {
      memset(index_info, 0, sizeof(media_index_info));
      ctx->base_time_ms = cur_time;
      index_info->num = 0;
      index_info->frame_len = ctx->format.sample_rate * ctx->format.channel *
                              ctx->format.bit_width / 8 / (1000 / PCM_AUDIO_FRAME_LENGTH_MS);
      index_info->offset = 0;
      index_info->timestamp = 0;
      index_info->type = 'A';
    }

    uint32_t read_len = 0;
    read_len =
        (uint32_t)fread(ctx->frame_buf, sizeof(uint8_t), index_info->frame_len, ctx->fp_data);
    if (read_len == 0 || feof(ctx->fp_data)) {
      printf("read audio frame end of file\n");
      fseek(ctx->fp_data, 0, SEEK_SET);
      ctx->base_time_ms = cur_time;
      ctx->pts_total += index_info->timestamp;
      memset(index_info, 0, sizeof(media_index_info));
    }

    frame->data = ctx->frame_buf;
    frame->data_size = read_len;
    frame->pts_ms = ctx->pts_total + index_info->timestamp;
    frame->seq = ctx->seq;
    frame->is_key_frame = 0;
    ctx->seq += 1;

    return 0;
  } else if (ctx->format.codec_type == CODEC_AAC) {
    uint64_t cur_time = _get_time_ms();
    if (ctx->base_time_ms) {
      // 根据索引中的时间戳控制帧率
      if (ctx->base_time_ms + index_info->timestamp > cur_time) {
        return 1;
      }
    } else {
      ctx->base_time_ms = cur_time;
      memset(index_info, 0, sizeof(media_index_info));
      fseek(ctx->fp_index, 0, SEEK_SET);
      _parse_index(ctx->fp_index, index_info);
    }

    // 大小检查必须在 fread 之前无条件执行，否则顺序读路径
    // (current_offset == offset) 会跳过检查导致写穿 ctx->frame_buf。
    if (index_info->frame_len > MAX_AUDIO_BUF_SIZE) {
      printf("audio frame too large (%d)\n", index_info->frame_len);
      return -1;
    }

    uint64_t current_offset = ftell(ctx->fp_data);
    if (current_offset != index_info->offset) {
      fseek(ctx->fp_data, index_info->offset, SEEK_SET);
    }

    uint32_t read_len = 0;
    read_len =
        (uint32_t)fread(ctx->frame_buf, sizeof(uint8_t), index_info->frame_len, ctx->fp_data);
    if (read_len != index_info->frame_len) {
      printf("read video frame error\n");
      return -1;
    }

    frame->data = ctx->frame_buf;
    frame->data_size = index_info->frame_len;
    frame->pts_ms = ctx->pts_total + index_info->timestamp;
    frame->seq = ctx->seq;
    frame->is_key_frame = 0;
    ctx->seq += 1;

    uint64_t last_pts = index_info->timestamp;
    memset(index_info, 0, sizeof(media_index_info));
    if (_parse_index(ctx->fp_index, index_info) || feof(ctx->fp_index)) {
      printf("read audio frame end of file\n");
      fseek(ctx->fp_index, 0, SEEK_SET);
      memset(index_info, 0, sizeof(media_index_info));
      _parse_index(ctx->fp_index, index_info);
      ctx->base_time_ms = cur_time;
      ctx->pts_total += last_pts;
    }
    return 0;
  } else {
    printf("unsupported audio codec type: %d\n", ctx->format.codec_type);
    return -1;
  }
}

static int _read_video_frame(media_file_s *ctx, frame_data_s *frame) {
  media_index_info *index_info = &ctx->index_info;

  uint64_t cur_time = _get_time_ms();
  if (ctx->base_time_ms) {
    // 根据索引中的时间戳控制帧率
    if (ctx->base_time_ms + index_info->timestamp > cur_time) {
      return 1;
    }
  } else {
    ctx->base_time_ms = cur_time;
    memset(index_info, 0, sizeof(media_index_info));
    fseek(ctx->fp_index, 0, SEEK_SET);
    _parse_index(ctx->fp_index, index_info);
  }

  // 大小检查必须在 fread 之前无条件执行，否则顺序读路径
  // (current_offset == offset) 会跳过检查导致写穿 ctx->frame_buf。
  if (index_info->frame_len > MAX_VIDEO_BUF_SIZE) {
    printf("video frame too large (%d)\n", index_info->frame_len);
    return -1;
  }

  uint64_t current_offset = ftell(ctx->fp_data);
  if (current_offset != index_info->offset) {
    fseek(ctx->fp_data, index_info->offset, SEEK_SET);
  }

  uint32_t read_len = 0;
  read_len = (uint32_t)fread(ctx->frame_buf, sizeof(uint8_t), index_info->frame_len, ctx->fp_data);
  if (read_len != index_info->frame_len) {
    printf("read video frame error\n");
    return -1;
  }

  frame->data = ctx->frame_buf;
  frame->data_size = index_info->frame_len;
  frame->pts_ms = ctx->pts_total + index_info->timestamp;
  frame->seq = ctx->seq;
  if (index_info->type == 'I') {
    frame->is_key_frame = 1;
  } else {
    frame->is_key_frame = 0;
  }
  ctx->seq += 1;

  uint64_t last_pts = index_info->timestamp;
  memset(index_info, 0, sizeof(media_index_info));
  if (_parse_index(ctx->fp_index, index_info) || feof(ctx->fp_index)) {
    printf("read video frame end of file\n");
    fseek(ctx->fp_index, 0, SEEK_SET);
    memset(index_info, 0, sizeof(media_index_info));
    _parse_index(ctx->fp_index, index_info);
    ctx->base_time_ms = cur_time;
    ctx->pts_total += last_pts;
  }

  return 0;
}

static int _read_mjpg_frame(media_file_s *ctx, frame_data_s *frame) {
  media_index_info *index_info = &ctx->index_info;

  uint64_t cur_time = _get_time_ms();
  if (ctx->base_time_ms) {
    // 根据合成时间戳控制帧率：下一帧的 pts 还没到就返回 1（未就绪）
    if (ctx->base_time_ms + index_info->timestamp > cur_time) {
      return 1;
    }
  } else {
    // 首帧：以当前时间为基准，timestamp = 0
    ctx->base_time_ms = cur_time;
    memset(index_info, 0, sizeof(media_index_info));
    index_info->type = 'I';
  }

  // 文件名从 001 开始，按帧序号递增；拼出 "目录+前缀+NNN.jpg" 逐帧打开
  uint32_t file_num = index_info->num + 1;
  char path[MAX_FILE_NAME_LEN];
  snprintf(path, sizeof(path), "%s%s%03d.jpg", ctx->mjpg_dir, ctx->mjpg_prefix, (int)file_num);
  // printf("mjpg filename:%s\n", path);

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    // 当前序号文件不存在：一轮播放结束（num != 0 时），回到第 1 帧循环播放
    if (index_info->num != 0) {
      uint64_t cycle_ms = (uint64_t)index_info->num * 1000 * ctx->fps.den / ctx->fps.num;
      ctx->pts_total += cycle_ms;
      index_info->num = 0;
      index_info->timestamp = 0;
      ctx->base_time_ms = cur_time;
      file_num = 1;
      snprintf(path, sizeof(path), "%s%s%03d.jpg", ctx->mjpg_dir, ctx->mjpg_prefix, (int)file_num);
      fp = fopen(path, "rb");
    }
    if (!fp) {
      printf("open mjpg file %s failed\n", path);
      return -1;
    }
  }

  uint32_t read_len = (uint32_t)fread(ctx->frame_buf, 1, ctx->frame_buf_size, fp);
  fclose(fp);
  if (read_len == 0) {
    printf("read mjpg frame error: %s\n", path);
    return -1;
  }

  frame->data = ctx->frame_buf;
  frame->data_size = read_len;
  frame->pts_ms = ctx->pts_total + index_info->timestamp;
  frame->seq = ctx->seq;
  // MJPG 每帧均为独立可解码帧，全部按关键帧处理
  frame->is_key_frame = 1;
  ctx->seq += 1;

  // 推进到下一帧：序号 +1，时间戳按帧率递增
  index_info->num += 1;
  index_info->timestamp =
      (uint32_t)((uint64_t)index_info->num * 1000 * ctx->fps.den / ctx->fps.num);

  return 0;
}

static int _parse_index(FILE *fp, media_index_info *index) {
  char line[128] = "";
  char *tmp = NULL;

  while (1) {
    memset(line, 0, sizeof(line));
    tmp = fgets(line, sizeof(line), fp);
    if (tmp == NULL) {
      return 1;
    }
    if (!_is_empty_line(line)) {
      sscanf(line, "%d,%c,%d,%d,%d,", &index->num, &index->type, &index->offset, &index->timestamp,
             &index->frame_len);
      // printf(line);
      return 0;
    }
  }
  return 1;
}

static int32_t _is_empty_line(const char *line) {
  int i;
  for (i = 0; line[i] != '\0'; i++) {
    if (!isspace((unsigned char)line[i])) {
      return 0;
    }
  }
  return 1;
}

static uint64_t _get_time_ms(void) {
  uint64_t time = 0;
  struct timespec on;
  if (clock_gettime(CLOCK_MONOTONIC, &on) == 0) {
    time = on.tv_sec * 1000;
    time += on.tv_nsec / 1000000;
  }

  return time;
}
