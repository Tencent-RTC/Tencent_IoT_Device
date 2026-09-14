// Copyright (c) 2026 Tencent. All rights reserved.

/**
 * @file wave_io.h
 * @brief WAV 文件读写接口
 */

#ifndef SRC_DEMO_IOT_AITALK_DEMO_WAVE_IO_H_
#define SRC_DEMO_IOT_AITALK_DEMO_WAVE_IO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *WaveWriterHandle;
typedef void *WaveReaderHandle;

/**
 * @brief WAV 文件信息
 */
typedef struct {
  uint32_t sample_rate;
  uint16_t num_channels;
  uint16_t bits_per_sample;
  uint32_t data_size;
  uint32_t total_size;
} WaveInfo;

/* ─── Writer ─── */

WaveWriterHandle wave_writer_open(const char *file_path, uint32_t sample_rate,
                                  uint16_t num_channels, uint16_t bits_per_sample);
int wave_writer_write(WaveWriterHandle handle, const void *data, uint32_t length);
int wave_writer_close(WaveWriterHandle handle);
uint32_t wave_writer_get_data_size(WaveWriterHandle handle);

/* ─── Reader ─── */

WaveReaderHandle wave_reader_open(const char *file_path);
int wave_reader_get_info(WaveReaderHandle handle, WaveInfo *info);
int wave_reader_read(WaveReaderHandle handle, void *data, uint32_t length);
int wave_reader_seek(WaveReaderHandle handle, uint32_t offset);
int wave_reader_close(WaveReaderHandle handle);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_AITALK_DEMO_WAVE_IO_H_
