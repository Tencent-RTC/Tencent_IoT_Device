// Copyright (c) 2026 Tencent. All rights reserved.

/**
 * @file wave_io.c
 * @brief WAV 文件读写实现（仅 Desktop 平台，使用标准 C 文件 I/O）
 */

#include "wave_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVE_LOGE(fmt, ...) fprintf(stderr, "[wave_io] " fmt "\n", ##__VA_ARGS__)

/* ─── WAV 文件头（44 字节） ─── */
typedef struct {
  char riff_header[4];     /* "RIFF" */
  uint32_t file_size;      /* 文件大小 - 8 */
  char wave_header[4];     /* "WAVE" */
  char fmt_header[4];      /* "fmt " */
  uint32_t fmt_chunk_size; /* PCM = 16 */
  uint16_t audio_format;   /* PCM = 1 */
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t block_align;
  uint16_t bits_per_sample;
  char data_header[4]; /* "data" */
  uint32_t data_chunk_size;
} WavHeader;

/* ─── Writer 内部结构 ─── */
typedef struct {
  FILE *fp;
  WavHeader header;
  uint32_t data_size;
  int header_written;
} WaveWriter;

/* ─── Reader 内部结构 ─── */
typedef struct {
  FILE *fp;
  WavHeader header;
  uint32_t data_size;
  uint32_t data_position;
  uint32_t data_offset;
} WaveReader;

/* ─── 静态函数声明（顺序 = 定义顺序，集中放在文件末尾） ─── */
static int _write_wav_header(FILE *fp, WavHeader *header);
static int _update_wav_header(FILE *fp, WavHeader *header, uint32_t data_size);
static int _read_wav_header(FILE *fp, WavHeader *header);

/* ─── Writer ─── */

WaveWriterHandle wave_writer_open(const char *file_path, uint32_t sample_rate,
                                  uint16_t num_channels, uint16_t bits_per_sample) {
  if (!file_path) {
    WAVE_LOGE("invalid file path");
    return NULL;
  }

  WaveWriter *writer = (WaveWriter *)malloc(sizeof(WaveWriter));
  if (!writer) {
    WAVE_LOGE("malloc WaveWriter failed");
    return NULL;
  }
  memset(writer, 0, sizeof(WaveWriter));

  writer->fp = fopen(file_path, "wb+");
  if (!writer->fp) {
    WAVE_LOGE("failed to open file: %s", file_path);
    free(writer);
    return NULL;
  }

  memcpy(writer->header.riff_header, "RIFF", 4);
  memcpy(writer->header.wave_header, "WAVE", 4);
  memcpy(writer->header.fmt_header, "fmt ", 4);
  writer->header.fmt_chunk_size = 16;
  writer->header.audio_format = 1;
  writer->header.num_channels = num_channels;
  writer->header.sample_rate = sample_rate;
  writer->header.bits_per_sample = bits_per_sample;
  writer->header.byte_rate = sample_rate * num_channels * bits_per_sample / 8;
  writer->header.block_align = num_channels * bits_per_sample / 8;
  memcpy(writer->header.data_header, "data", 4);
  writer->header.data_chunk_size = 0;

  if (_write_wav_header(writer->fp, &writer->header) != 0) {
    WAVE_LOGE("failed to write WAV header");
    fclose(writer->fp);
    free(writer);
    return NULL;
  }
  writer->header_written = 1;
  return (WaveWriterHandle)writer;
}

int wave_writer_write(WaveWriterHandle handle, const void *data, uint32_t length) {
  if (!handle || !data || length == 0) {
    return -1;
  }
  WaveWriter *writer = (WaveWriter *)handle;
  if (fwrite(data, 1, length, writer->fp) != length) {
    WAVE_LOGE("failed to write audio data");
    return -1;
  }
  writer->data_size += length;
  return 0;
}

int wave_writer_close(WaveWriterHandle handle) {
  if (!handle) {
    return -1;
  }
  WaveWriter *writer = (WaveWriter *)handle;
  if (writer->header_written && writer->fp) {
    _update_wav_header(writer->fp, &writer->header, writer->data_size);
  }
  if (writer->fp) {
    fclose(writer->fp);
  }
  free(writer);
  return 0;
}

uint32_t wave_writer_get_data_size(WaveWriterHandle handle) {
  if (!handle) {
    return 0;
  }
  return ((WaveWriter *)handle)->data_size;
}

/* ─── Reader ─── */

WaveReaderHandle wave_reader_open(const char *file_path) {
  if (!file_path) {
    WAVE_LOGE("invalid file path");
    return NULL;
  }

  WaveReader *reader = (WaveReader *)malloc(sizeof(WaveReader));
  if (!reader) {
    WAVE_LOGE("malloc WaveReader failed");
    return NULL;
  }
  memset(reader, 0, sizeof(WaveReader));

  reader->fp = fopen(file_path, "rb");
  if (!reader->fp) {
    WAVE_LOGE("failed to open file: %s", file_path);
    free(reader);
    return NULL;
  }

  if (_read_wav_header(reader->fp, &reader->header) != 0) {
    WAVE_LOGE("failed to read WAV header");
    fclose(reader->fp);
    free(reader);
    return NULL;
  }

  reader->data_size = reader->header.file_size - sizeof(WavHeader) + 8;
  reader->data_offset = sizeof(WavHeader);
  reader->data_position = 0;
  return (WaveReaderHandle)reader;
}

int wave_reader_get_info(WaveReaderHandle handle, WaveInfo *info) {
  if (!handle || !info) {
    return -1;
  }
  WaveReader *reader = (WaveReader *)handle;
  info->sample_rate = reader->header.sample_rate;
  info->num_channels = reader->header.num_channels;
  info->bits_per_sample = reader->header.bits_per_sample;
  info->data_size = reader->data_size;
  info->total_size = reader->header.file_size + 8;
  /* 定位到音频数据起始位置 */
  fseek(reader->fp, (long)reader->data_offset, SEEK_SET);
  reader->data_position = 0;
  return 0;
}

int wave_reader_read(WaveReaderHandle handle, void *data, uint32_t length) {
  if (!handle || !data) {
    return -1;
  }
  WaveReader *reader = (WaveReader *)handle;
  if (reader->data_position >= reader->data_size) {
    return 0; /* EOF */
  }
  uint32_t remaining = reader->data_size - reader->data_position;
  if (length > remaining) {
    length = remaining;
  }
  size_t bytes_read = fread(data, 1, length, reader->fp);
  if (bytes_read == 0) {
    return -1;
  }
  reader->data_position += (uint32_t)bytes_read;
  return (int)bytes_read;
}

int wave_reader_seek(WaveReaderHandle handle, uint32_t offset) {
  if (!handle) {
    return -1;
  }
  WaveReader *reader = (WaveReader *)handle;
  if (offset > reader->data_size) {
    WAVE_LOGE("seek offset out of bounds");
    return -1;
  }
  if (fseek(reader->fp, (long)(reader->data_offset + offset), SEEK_SET) != 0) {
    WAVE_LOGE("fseek failed");
    return -1;
  }
  reader->data_position = offset;
  return 0;
}

int wave_reader_close(WaveReaderHandle handle) {
  if (!handle) {
    return -1;
  }
  WaveReader *reader = (WaveReader *)handle;
  if (reader->fp) {
    fclose(reader->fp);
  }
  free(reader);
  return 0;
}

/* ─── 共享辅助：WAV 文件头底层读写 ─── */

static int _write_wav_header(FILE *fp, WavHeader *header) {
  return fwrite(header, 1, sizeof(WavHeader), fp) == sizeof(WavHeader) ? 0 : -1;
}

static int _update_wav_header(FILE *fp, WavHeader *header, uint32_t data_size) {
  header->file_size = data_size + sizeof(WavHeader) - 8;
  header->data_chunk_size = data_size;

  if (fseek(fp, 4, SEEK_SET) != 0) {
    return -1;
  }
  if (fwrite(&header->file_size, 1, sizeof(header->file_size), fp) != sizeof(header->file_size)) {
    return -1;
  }
  if (fseek(fp, 40, SEEK_SET) != 0) {
    return -1;
  }
  if (fwrite(&header->data_chunk_size, 1, sizeof(header->data_chunk_size), fp) !=
      sizeof(header->data_chunk_size)) {
    return -1;
  }
  fseek(fp, 0, SEEK_END);
  return 0;
}

static int _read_wav_header(FILE *fp, WavHeader *header) {
  if (fread(header, 1, sizeof(WavHeader), fp) != sizeof(WavHeader)) {
    return -1;
  }
  if (memcmp(header->riff_header, "RIFF", 4) != 0) {
    WAVE_LOGE("invalid RIFF header");
    return -1;
  }
  if (memcmp(header->wave_header, "WAVE", 4) != 0) {
    WAVE_LOGE("invalid WAVE header");
    return -1;
  }
  if (memcmp(header->fmt_header, "fmt ", 4) != 0) {
    WAVE_LOGE("invalid fmt header");
    return -1;
  }
  if (header->audio_format != 1) {
    WAVE_LOGE("unsupported audio format: %u (only PCM)", header->audio_format);
    return -1;
  }
  return 0;
}
