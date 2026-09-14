#ifndef _FILE_STREAM_H_
#define _FILE_STREAM_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>

typedef enum {
  CODEC_UNKNOWN = 0,

  CODEC_PCM = 100,
  CODEC_OPUS = 101,
  CODEC_AAC = 102,

  CODEC_H264 = 200,
  CODEC_H265 = 201,
  CODEC_MJPG = 202,
} codec_type_e;

typedef enum {
  PICTURE_TYPE_UNKNOWN = 0,
  PICTURE_TYPE_I = 1,
  PICTURE_TYPE_P = 2,
  PICTURE_TYPE_B = 3,
} picture_type_e;

typedef enum {
  SAMPLE_RATE_UNKNOWN = 0,
  SAMPLE_RATE_8000 = 8000,
  SAMPLE_RATE_11025 = 11025,
  SAMPLE_RATE_12000 = 12000,
  SAMPLE_RATE_16000 = 16000,
  SAMPLE_RATE_22050 = 22050,
  SAMPLE_RATE_24000 = 24000,
  SAMPLE_RATE_32000 = 32000,
  SAMPLE_RATE_44100 = 44100,
  SAMPLE_RATE_48000 = 48000,
} sample_rate_e;

typedef enum {
  BIT_WIDTH_UNKNOWN = 0,
  BIT_WIDTH_8 = 8,
  BIT_WIDTH_16 = 16,
  BIT_WIDTH_24 = 24,
} audio_bit_width_e;

typedef enum {
  AUDIO_CHANNEL_UNKNOWN = 0,
  AUDIO_CHANNEL_MONO = 1,
  AUDIO_CHANNEL_STEREO = 2,
} audio_channel_e;

typedef struct {
  codec_type_e codec_type;
  sample_rate_e sample_rate;
  audio_bit_width_e bit_width;
  audio_channel_e channel;
  uint32_t width;
  uint32_t height;
} stream_format_s;

typedef struct {
  uint8_t *data;
  uint32_t data_size;
  uint32_t seq;
  uint32_t is_key_frame;
  uint64_t pts_ms;
} frame_data_s;

void *file_stream_init(const char *file_path);

void file_stream_exit(void *handle);

int32_t file_stream_get_format(void *handle, stream_format_s *format);

int32_t file_stream_get_frame(void *handle, frame_data_s *frame);

// int32_t file_stream_get_progress(void *handle);

#if defined(__cplusplus)
}
#endif

#endif
