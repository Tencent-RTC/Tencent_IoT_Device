#ifndef __TC_HAL_AUDIO_H__
#define __TC_HAL_AUDIO_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _HALAudioDecodeError {
  // 解码成功
  HAL_DECODE_OK = 0,
  // 解码失败
  HAL_DECODE_FAILED = -1,
  // 解码缓冲区不足
  HAL_DECODE_BUFFER_NOT_ENOUGH = -2
} HALAudioDecodeError;

struct _HALAacDecoder;
typedef struct _HALAacDecoder HALAacDecoder;

/**
 * @brief 创建AAC解码器
 * @param sample_rate 采样率
 * @param channels 声道数
 * @param frame_duration 帧时长，单位 ms
 * @return 解码器句柄
 */
HALAacDecoder *HAL_CreateAacDecoder(uint32_t sample_rate, uint32_t channels,
                                    uint32_t frame_duration);

/*
 * @brief 解码AAC数据
 * @param decoder 解码器句柄
 * @param aac_data AAC数据
 * @param aac_data_len AAC数据长度
 * @param pcm_data 用于存储 PCM 数据缓冲
 * @param pcm_data_len pcm_data 缓冲区的长度，返回实际 PCM 数据长度
 * @note 当 pcm_data_len 长度不足时，返回 HAL_DECODE_BUFFER_NOT_ENOUGH 错误，
 *       pcm_data_len 中会存储实际需要的长度
 */
HALAudioDecodeError HAL_AacDecode(HALAacDecoder *decoder, const uint8_t *aac_data,
                                  size_t aac_data_len, uint8_t *pcm_data, size_t *pcm_data_len);

/**
 * @brief 销毁AAC解码器
 * @param decoder 解码器句柄
 */
void HAL_DestroyAacDecoder(HALAacDecoder *decoder);

struct HALOpusDecoder;
typedef struct HALOpusDecoder HALOpusDecoder;

/**
 * @brief 创建Opus解码器
 * @param sample_rate 采样率
 * @param channels 声道数
 * @param frame_duration 帧时长，单位 ms
 * @return 解码器句柄
 */
HALOpusDecoder *HAL_CreateOpusDecoder(uint32_t sample_rate, uint32_t channels,
                                      uint32_t frame_duration);

/**
 * @brief 解码Opus数据
 * @param decoder 解码器句柄
 * @param opus_data Opus数据
 * @param opus_data_len Opus数据长度
 * @param pcm_data 用于存储 PCM 数据缓冲
 * @param pcm_data_len pcm_data 缓冲区的长度，返回实际 PCM 数据长度
 * @note 当 pcm_data_len 长度不足时，返回 HAL_DECODE_BUFFER_NOT_ENOUGH 错误，
 *       pcm_data_len 中会存储实际需要的长度
 */
HALAudioDecodeError HAL_OpusDecode(HALOpusDecoder *decoder, const uint8_t *opus_data,
                                   size_t opus_data_len, uint8_t *pcm_data, size_t *pcm_data_len);

/**
 * @brief 销毁Opus解码器
 * @param decoder 解码器句柄
 */
void HAL_DestroyOpusDecoder(HALOpusDecoder *decoder);

struct _HALAacEncoder;
typedef struct _HALAacEncoder HALAacEncoder;

/**
 * @brief 创建AAC编码器
 * @param sample_rate 采样率
 * @param channels 声道数
 * @param bitrate_bps 比特率，单位bps
 * @param p_in_size 输入缓冲区大小
 * @param p_out_size 输出缓冲区大小
 * @return 编码器句柄
 */
HALAacEncoder *HAL_CreateAacEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                    uint32_t *p_in_size, uint32_t *p_out_size);

/**
 * @brief 编码AAC数据
 * @param encoder 编码器句柄
 * @param pcm_data PCM数据
 * @param pcm_len PCM数据长度
 * @param aac_data 用于存储AAC数据缓冲
 * @param aac_len aac_data缓冲区的长度，返回实际AAC数据长度
 * @note 当aac_len长度不足时，返回HAL_DECODE_BUFFER_NOT_ENOUGH错误，
 *       aac_len中会存储实际需要的长度
 */
int32_t HAL_AacEncode(HALAacEncoder *encoder, const uint8_t *pcm_data, size_t pcm_len,
                      uint8_t *aac_data, size_t aac_len);

/**
 * @brief 设置AAC编码器比特率
 * @param encoder 编码器句柄
 * @param bitrate_bps 比特率，单位bps
 */
int32_t HAL_AacEncoderSetBitrate(HALAacEncoder *encoder, uint32_t bitrate_bps);

/**
 * @brief 销毁AAC编码器
 * @param encoder 编码器句柄
 */
void HAL_DestroyAacEncoder(HALAacEncoder *encoder);

/**
 * @brief 创建Opus编码器
 * @param sample_rate 采样率
 * @param channels 声道数
 * @param bitrate 比特率，单位bps
 * @return 编码器句柄
 */
void *HAL_CreateOpusEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate);

/**
 * @brief 编码Opus数据
 * @param encoder 编码器句柄
 * @param pcm PCM数据
 * @param frame_size PCM采样数
 * @param data 用于存储Opus数据缓冲
 * @param max_data_bytes data缓冲区的最大长度
 * @return 编码后的字节数，失败返回负数
 */
int32_t HAL_OpusEncode(void *encoder, const short *pcm, uint32_t frame_size, unsigned char *data,
                       uint32_t max_data_bytes);

/**
 * @brief 销毁Opus编码器
 * @param encoder 编码器句柄
 */
void HAL_DestroyOpusEncoder(void *encoder);

/* ---------------------------------------------------------------------- */
/* G722 Codec HAL Interface                                               */
/* ---------------------------------------------------------------------- */

struct _HALG722Decoder;
typedef struct _HALG722Decoder HALG722Decoder;

/**
 * @brief Create G722 decoder
 * @param sample_rate Sample rate (G.722 operates at 16000Hz, output can be 16000 or 8000)
 * @param channels Number of channels (G.722 supports mono only)
 * @param frame_duration Frame duration in ms
 * @return Decoder handle
 */
HALG722Decoder *HAL_CreateG722Decoder(uint32_t sample_rate, uint32_t channels,
                                      uint32_t frame_duration);

/**
 * @brief Decode G722 data
 * @param decoder Decoder handle
 * @param g722_data G722 encoded data
 * @param g722_data_len G722 data length in bytes
 * @param pcm_data Buffer for decoded PCM data
 * @param pcm_data_len Buffer size on input, actual PCM data length on output
 * @note Returns HAL_DECODE_BUFFER_NOT_ENOUGH if pcm_data_len is insufficient,
 *       pcm_data_len will contain the required size
 */
HALAudioDecodeError HAL_G722Decode(HALG722Decoder *decoder, const uint8_t *g722_data,
                                   size_t g722_data_len, uint8_t *pcm_data, size_t *pcm_data_len);

/**
 * @brief Destroy G722 decoder
 * @param decoder Decoder handle
 */
void HAL_DestroyG722Decoder(HALG722Decoder *decoder);

struct _HALG722Encoder;
typedef struct _HALG722Encoder HALG722Encoder;

/**
 * @brief Create G722 encoder
 * @param sample_rate Sample rate (G.722 operates at 16000Hz input)
 * @param channels Number of channels (mono only)
 * @param bitrate Bitrate in bps (64000, 56000, or 48000)
 * @return Encoder handle
 */
HALG722Encoder *HAL_CreateG722Encoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate);

/**
 * @brief Encode PCM data to G722
 * @param encoder Encoder handle
 * @param pcm PCM data (16-bit signed samples)
 * @param frame_size Number of PCM samples to encode
 * @param data Buffer for encoded G722 data
 * @param max_data_bytes Maximum size of output buffer
 * @return Number of encoded bytes, negative on failure
 */
int32_t HAL_G722Encode(HALG722Encoder *encoder, const int16_t *pcm, uint32_t frame_size,
                       uint8_t *data, uint32_t max_data_bytes);

/**
 * @brief Destroy G722 encoder
 * @param encoder Encoder handle
 */
void HAL_DestroyG722Encoder(HALG722Encoder *encoder);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
