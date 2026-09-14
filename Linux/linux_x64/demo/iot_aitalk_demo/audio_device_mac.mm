// Copyright (c) 2026 Tencent. All rights reserved.

/**
 * @file audio_device_mac.mm
 * @brief audio_device macOS 实现：基于 VoiceProcessingIO AudioUnit
 *        提供真实 mic 采集与 spk 播放，自带 AEC/NS/AGC，纯 PCM 路径。
 */

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_device.h"
#include "tc_hal_audio.h"

#define LOG_TAG "[audio_device]"

// ==================== 上行编码类型选择 ====================
// 可选: MAC_CODEC_OPUS / MAC_CODEC_AAC / MAC_CODEC_G722
#define MAC_CODEC_OPUS 3
#define MAC_CODEC_AAC 2
#define MAC_CODEC_G722 4

#define MAC_SEND_CODEC MAC_CODEC_AAC

// 编码帧长 (ms): AAC=64, OPUS/G722=20
#if MAC_SEND_CODEC == MAC_CODEC_AAC
#  define MAC_CODEC_FRAME_DURATION_MS 64
#else
#  define MAC_CODEC_FRAME_DURATION_MS 20
#endif

// 将整数宏映射回枚举值供 C 代码使用
#if MAC_SEND_CODEC == MAC_CODEC_OPUS
#  define MAC_SEND_CODEC_E TC_IOT_AUDIO_CODEC_OPUS
#elif MAC_SEND_CODEC == MAC_CODEC_AAC
#  define MAC_SEND_CODEC_E TC_IOT_AUDIO_CODEC_AAC
#elif MAC_SEND_CODEC == MAC_CODEC_G722
#  define MAC_SEND_CODEC_E TC_IOT_AUDIO_CODEC_G722
#endif

#define MAC_SAMPLE_RATE 16000
#define MAC_CHANNELS 1
#define MAC_BITS_PER_SAMPLE 16
#define MAC_FRAME_DURATION_MS 20
#define MAC_TARGET_FRAMES ((MAC_SAMPLE_RATE * MAC_FRAME_DURATION_MS) / 1000) /* 320 */

/* 解码输出 PCM 缓冲区上限 (16k * 20ms = 320 samples * 2 bytes = 640 bytes) */
#define MAC_DECODE_PCM_BUF_SIZE 640
typedef struct {
  int16_t *data;
  int buffer_size_frames;
  int write_pos;
  int read_pos;
  int available_frames;
  volatile int is_active;
  pthread_mutex_t mutex;
} mac_play_ring_t;

static OSStatus _recording_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                                    const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                                    UInt32 inNumberFrames, AudioBufferList *ioData);

static OSStatus _playback_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                                   UInt32 inNumberFrames, AudioBufferList *ioData);

@interface AitalkMacAudio : NSObject {
 @public
  AudioUnit audio_unit;
  AudioStreamBasicDescription audio_format;

  audio_device_on_capture_t capture_cb;
  void *capture_user_data;
  volatile BOOL is_recording;
  int16_t *accum_buf;
  int accum_buf_size_fr;
  int accum_count_fr;
  uint64_t pts_ms;

  mac_play_ring_t play_ring;
  volatile BOOL is_playing;

  BOOL torn_down;

  // 静音检测：macOS TCC 未授权时 AudioUnit 不会报错，但回调数据全为 0
  int silence_check_remaining;
  BOOL silence_warned;

  // 惰性解码器（首次遇到非 PCM 编码时按需创建）
  HALAacDecoder *aac_decoder;
  HALOpusDecoder *opus_decoder;
  HALG722Decoder *g722_decoder;
}
- (BOOL)setupAudioUnit;
- (void)teardown;
@end

@implementation AitalkMacAudio

- (instancetype)init {
  self = [super init];
  if (!self) {
    return nil;
  }
  audio_unit = NULL;
  capture_cb = NULL;
  capture_user_data = NULL;
  is_recording = NO;
  is_playing = NO;
  pts_ms = 0;
  torn_down = NO;

  accum_buf_size_fr = MAC_TARGET_FRAMES * 2;
  accum_buf = (int16_t *)calloc((size_t)accum_buf_size_fr * MAC_CHANNELS, sizeof(int16_t));
  accum_count_fr = 0;

  silence_check_remaining = 0;
  silence_warned = NO;
  aac_decoder = NULL;
  opus_decoder = NULL;
  g722_decoder = NULL;

  play_ring.buffer_size_frames = MAC_SAMPLE_RATE;
  play_ring.data =
      (int16_t *)calloc((size_t)play_ring.buffer_size_frames * MAC_CHANNELS, sizeof(int16_t));
  play_ring.write_pos = 0;
  play_ring.read_pos = 0;
  play_ring.available_frames = 0;
  play_ring.is_active = 0;
  pthread_mutex_init(&play_ring.mutex, NULL);

  if (!accum_buf || !play_ring.data) {
    printf(LOG_TAG " init: calloc failed\n");
    [self teardown];
    return nil;
  }

  memset(&audio_format, 0, sizeof(audio_format));
  audio_format.mSampleRate = MAC_SAMPLE_RATE;
  audio_format.mFormatID = kAudioFormatLinearPCM;
  audio_format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  audio_format.mChannelsPerFrame = MAC_CHANNELS;
  audio_format.mBitsPerChannel = MAC_BITS_PER_SAMPLE;
  audio_format.mBytesPerFrame = (MAC_BITS_PER_SAMPLE / 8) * MAC_CHANNELS;
  audio_format.mFramesPerPacket = 1;
  audio_format.mBytesPerPacket = audio_format.mBytesPerFrame;

  if (![self setupAudioUnit]) {
    [self teardown];
    return nil;
  }
  return self;
}

- (BOOL)setupAudioUnit {
  AudioComponentDescription desc = {0};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (!comp) {
    printf(LOG_TAG " find VoiceProcessingIO failed\n");
    return NO;
  }
  OSStatus st = AudioComponentInstanceNew(comp, &audio_unit);
  if (st != noErr) {
    printf(LOG_TAG " AudioComponentInstanceNew failed: %d\n", (int)st);
    return NO;
  }

  UInt32 enable = 1;
  st = AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                            &enable, sizeof(enable));
  if (st != noErr) {
    printf(LOG_TAG " enable input failed: %d\n", (int)st);
    return NO;
  }
  st = AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                            0, &enable, sizeof(enable));
  if (st != noErr) {
    printf(LOG_TAG " enable output failed: %d\n", (int)st);
    return NO;
  }

  st = AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                            &audio_format, sizeof(audio_format));
  if (st != noErr) {
    printf(LOG_TAG " set input format failed: %d\n", (int)st);
    return NO;
  }
  st = AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                            &audio_format, sizeof(audio_format));
  if (st != noErr) {
    printf(LOG_TAG " set output format failed: %d\n", (int)st);
    return NO;
  }

  AURenderCallbackStruct in_cb = {.inputProc = _recording_callback,
                                  .inputProcRefCon = (__bridge void *)self};
  st = AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_SetInputCallback,
                            kAudioUnitScope_Global, 0, &in_cb, sizeof(in_cb));
  if (st != noErr) {
    printf(LOG_TAG " set input callback failed: %d\n", (int)st);
    return NO;
  }
  AURenderCallbackStruct out_cb = {.inputProc = _playback_callback,
                                   .inputProcRefCon = (__bridge void *)self};
  st = AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                            0, &out_cb, sizeof(out_cb));
  if (st != noErr) {
    printf(LOG_TAG " set output callback failed: %d\n", (int)st);
    return NO;
  }

  UInt32 agc = 1;
  st = AudioUnitSetProperty(audio_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                            kAudioUnitScope_Global, 0, &agc, sizeof(agc));
  if (st != noErr) {
    printf(LOG_TAG " enable AGC failed: %d\n", (int)st);
  }

  st = AudioUnitInitialize(audio_unit);
  if (st != noErr) {
    printf(LOG_TAG " AudioUnitInitialize failed: %d\n", (int)st);
    return NO;
  }
  printf(LOG_TAG " mac audio unit ready (16k mono 20ms, AEC/NS/AGC on)\n");
  return YES;
}

- (void)teardown {
  if (torn_down) {
    return;
  }
  torn_down = YES;
  if (audio_unit) {
    AudioOutputUnitStop(audio_unit);
    AudioUnitUninitialize(audio_unit);
    AudioComponentInstanceDispose(audio_unit);
    audio_unit = NULL;
  }
  pthread_mutex_destroy(&play_ring.mutex);
  free(play_ring.data);
  play_ring.data = NULL;
  if (aac_decoder) {
    HAL_DestroyAacDecoder(aac_decoder);
    aac_decoder = NULL;
  }
  if (opus_decoder) {
    HAL_DestroyOpusDecoder(opus_decoder);
    opus_decoder = NULL;
  }
  if (g722_decoder) {
    HAL_DestroyG722Decoder(g722_decoder);
    g722_decoder = NULL;
  }
  free(accum_buf);
  accum_buf = NULL;
}

@end

static AitalkMacAudio *g_audio = nil;

// ==================== 麦克风权限请求（macOS 10.14+） ====================

static int request_microphone_permission(void) {
  if (@available(macOS 10.14, *)) {
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    switch (status) {
      case AVAuthorizationStatusAuthorized:
        printf(LOG_TAG " microphone permission: already authorized\n");
        return 0;

      case AVAuthorizationStatusDenied:
      case AVAuthorizationStatusRestricted:
        printf(LOG_TAG " microphone permission: DENIED — please go to\n" LOG_TAG
                       " System Settings > Privacy & Security > Microphone,\n" LOG_TAG
                       " enable access for your terminal app and restart.\n");
        return -1;

      case AVAuthorizationStatusNotDetermined: {
        printf(LOG_TAG " requesting microphone permission...\n");
        printf(LOG_TAG " NOTE: If no dialog appears, you may need to manually\n" LOG_TAG
                       " grant MICROPHONE access to your terminal app in\n" LOG_TAG
                       " System Settings > Privacy & Security > Microphone\n");
        __block BOOL granted = NO;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL ok) {
                                   granted = ok;
                                   dispatch_semaphore_signal(sem);
                                 }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (!granted) {
          printf(LOG_TAG " microphone permission: user denied\n");
          return -1;
        }
        printf(LOG_TAG " microphone permission: granted\n");
        return 0;
      }

      default:
        return -1;
    }
  }
  // macOS < 10.14，无需权限
  return 0;
}

// ==================== public C API ====================

#ifdef __cplusplus
extern "C" {
#endif

int audio_device_open(const audio_device_config_t *config) {
  if (g_audio) {
    printf(LOG_TAG " open: already opened\n");
    return -1;
  }
  if (!config) {
    return -1;
  }
  if (config->sample_rate != MAC_SAMPLE_RATE || config->channels != MAC_CHANNELS ||
      config->bits_per_sample != MAC_BITS_PER_SAMPLE) {
    printf(LOG_TAG " open: only 16k/mono/16bit supported (got %u/%u/%u)\n", config->sample_rate,
           config->channels, config->bits_per_sample);
    return -1;
  }

  // 主动请求麦克风权限（macOS 10.14+ 会弹出系统授权对话框）
  if (request_microphone_permission() != 0) {
    printf(LOG_TAG " open: microphone permission required\n");
    return -1;
  }

  AitalkMacAudio *obj = [[AitalkMacAudio alloc] init];
  if (!obj) {
    return -1;
  }
  g_audio = obj;
  printf(LOG_TAG " open ok\n");
  return 0;
}

int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!g_audio || !cb) {
    return -1;
  }
  if (g_audio->is_recording) {
    printf(LOG_TAG " start_capture: already running\n");
    return -1;
  }
  g_audio->capture_cb = cb;
  g_audio->capture_user_data = user_data;
  g_audio->accum_count_fr = 0;
  g_audio->pts_ms = 0;
  g_audio->is_recording = YES;

  // 启动静音检测（~2 秒），用于发现 TCC 权限未授权导致的静默失败
  g_audio->silence_check_remaining = 100;
  g_audio->silence_warned = NO;

  if (!g_audio->is_playing) {
    OSStatus st = AudioOutputUnitStart(g_audio->audio_unit);
    if (st != noErr) {
      g_audio->is_recording = NO;
      printf(LOG_TAG " AudioOutputUnitStart failed: %d\n", (int)st);
      return -1;
    }
  }
  printf(LOG_TAG " capture started\n");
  return 0;
}

static OSStatus _recording_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                                    const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                                    UInt32 inNumberFrames, AudioBufferList *ioData) {
  (void)ioData;
  AitalkMacAudio *self_ = (__bridge AitalkMacAudio *)inRefCon;
  if (!self_->is_recording) {
    return noErr;
  }

  AudioBufferList list;
  list.mNumberBuffers = 1;
  list.mBuffers[0].mNumberChannels = MAC_CHANNELS;
  list.mBuffers[0].mDataByteSize = (UInt32)(inNumberFrames * MAC_CHANNELS * sizeof(int16_t));
  list.mBuffers[0].mData = malloc(list.mBuffers[0].mDataByteSize);
  if (!list.mBuffers[0].mData) {
    return noErr;
  }
  OSStatus st = AudioUnitRender(self_->audio_unit, ioActionFlags, inTimeStamp, inBusNumber,
                                inNumberFrames, &list);
  if (st != noErr) {
    free(list.mBuffers[0].mData);
    return st;
  }

  const int16_t *src = (const int16_t *)list.mBuffers[0].mData;
  int frames = (int)inNumberFrames;

  // 静音检测：若 TCC 麦克风权限未授权，AudioUnit 回调数据全为 0 但不报错
  if (self_->silence_check_remaining > 0 && !self_->silence_warned) {
    BOOL all_zero = YES;
    int total_samples = (int)(inNumberFrames * MAC_CHANNELS);
    for (int i = 0; i < total_samples; i++) {
      if (src[i] != 0) {
        all_zero = NO;
        break;
      }
    }
    if (!all_zero) {
      self_->silence_check_remaining = 0;  // 有真实音频数据，停止检测
    } else {
      self_->silence_check_remaining--;
      if (self_->silence_check_remaining <= 0) {
        self_->silence_warned = YES;
        printf("\n" LOG_TAG " ============================================\n" LOG_TAG
               "  WARNING: Microphone input is SILENT !\n" LOG_TAG
               "  AudioUnit started successfully but all\n" LOG_TAG
               "  audio data is ZERO — your terminal app\n" LOG_TAG
               "  likely lacks microphone permission.\n" LOG_TAG " \n" LOG_TAG
               "  Please grant MICROPHONE access to your\n" LOG_TAG
               "  terminal (e.g. Terminal.app / iTerm2.app):\n" LOG_TAG
               "  System Settings > Privacy & Security >\n" LOG_TAG
               "  Microphone > enable your terminal app.\n" LOG_TAG
               "  Then restart this demo.\n" LOG_TAG
               " ============================================\n\n");
      }
    }
  }

  if (frames > self_->accum_buf_size_fr) {
    frames = self_->accum_buf_size_fr;
  }

  if (self_->accum_count_fr + frames > self_->accum_buf_size_fr) {
    self_->accum_count_fr = 0;
  }
  memcpy(self_->accum_buf + (size_t)self_->accum_count_fr * MAC_CHANNELS, src,
         (size_t)frames * MAC_CHANNELS * sizeof(int16_t));
  self_->accum_count_fr += frames;

  while (self_->accum_count_fr >= MAC_TARGET_FRAMES) {
    if (self_->capture_cb) {
      uint32_t bytes = (uint32_t)(MAC_TARGET_FRAMES * MAC_CHANNELS * sizeof(int16_t));
      self_->capture_cb((const uint8_t *)self_->accum_buf, bytes, TC_IOT_AUDIO_CODEC_PCM,
                        self_->pts_ms, self_->capture_user_data);
      self_->pts_ms += MAC_FRAME_DURATION_MS;
    }
    int rem = self_->accum_count_fr - MAC_TARGET_FRAMES;
    if (rem > 0) {
      memmove(self_->accum_buf, self_->accum_buf + (size_t)MAC_TARGET_FRAMES * MAC_CHANNELS,
              (size_t)rem * MAC_CHANNELS * sizeof(int16_t));
    }
    self_->accum_count_fr = rem;
  }

  free(list.mBuffers[0].mData);
  return noErr;
}

void audio_device_stop_capture(void) {
  if (!g_audio || !g_audio->is_recording) {
    return;
  }
  g_audio->is_recording = NO;
  g_audio->capture_cb = NULL;
  g_audio->capture_user_data = NULL;
  g_audio->accum_count_fr = 0;
  if (!g_audio->is_playing && g_audio->audio_unit) {
    AudioOutputUnitStop(g_audio->audio_unit);
  }
  printf(LOG_TAG " capture stopped\n");
}

extern uint64_t xc_get_timestamp_ms();

int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!g_audio || !data || size == 0) {
    return -1;
  }

  const int16_t *pcm = NULL;
  int frames = 0;
  uint8_t decode_buf[MAC_DECODE_PCM_BUF_SIZE];
  size_t decode_pcm_len = sizeof(decode_buf);

  if (codec == TC_IOT_AUDIO_CODEC_PCM) {
    uint32_t bytes_per_frame = MAC_CHANNELS * (MAC_BITS_PER_SAMPLE / 8);
    if ((size % bytes_per_frame) != 0) {
      printf(LOG_TAG " write: size %u not aligned to frame %u\n", size, bytes_per_frame);
      return -1;
    }
    frames = (int)(size / bytes_per_frame);
    pcm = (const int16_t *)data;
  } else if (codec == TC_IOT_AUDIO_CODEC_OPUS) {
    if (!g_audio->opus_decoder) {
      g_audio->opus_decoder =
          HAL_CreateOpusDecoder(MAC_SAMPLE_RATE, MAC_CHANNELS, MAC_CODEC_FRAME_DURATION_MS);
      if (!g_audio->opus_decoder) {
        printf(LOG_TAG " write: HAL_CreateOpusDecoder failed\n");
        return -1;
      }
      printf(LOG_TAG " opus decoder created (lazy) input audio size: %u\n", size);
    }
    HALAudioDecodeError err =
        HAL_OpusDecode(g_audio->opus_decoder, data, (size_t)size, decode_buf, &decode_pcm_len);
    if (err != HAL_DECODE_OK) {
      printf(LOG_TAG " write: opus decode failed (err=%d)\n", (int)err);
      return 0;
    }
    printf(LOG_TAG " opus %d decoded to %d\n", (int)size, (int)decode_pcm_len);
    frames = (int)(decode_pcm_len / (MAC_CHANNELS * (MAC_BITS_PER_SAMPLE / 8)));
    pcm = (const int16_t *)decode_buf;
  } else if (codec == TC_IOT_AUDIO_CODEC_AAC) {
    if (!g_audio->aac_decoder) {
      g_audio->aac_decoder =
          HAL_CreateAacDecoder(MAC_SAMPLE_RATE, MAC_CHANNELS, MAC_CODEC_FRAME_DURATION_MS);
      if (!g_audio->aac_decoder) {
        printf(LOG_TAG " write: HAL_CreateAacDecoder failed\n");
        return -1;
      }
      printf(LOG_TAG " aac decoder created (lazy) input audio size: %u\n", size);
    }
    HALAudioDecodeError err =
        HAL_AacDecode(g_audio->aac_decoder, data, (size_t)size, decode_buf, &decode_pcm_len);
    if (err != HAL_DECODE_OK) {
      printf(LOG_TAG " write: aac decode failed (err=%d)\n", (int)err);
      return 0;
    }
    frames = (int)(decode_pcm_len / (MAC_CHANNELS * (MAC_BITS_PER_SAMPLE / 8)));
    pcm = (const int16_t *)decode_buf;
  } else if (codec == TC_IOT_AUDIO_CODEC_G722) {
    if (!g_audio->g722_decoder) {
      g_audio->g722_decoder =
          HAL_CreateG722Decoder(MAC_SAMPLE_RATE, MAC_CHANNELS, MAC_CODEC_FRAME_DURATION_MS);
      if (!g_audio->g722_decoder) {
        printf(LOG_TAG " write: HAL_CreateG722Decoder failed\n");
        return -1;
      }
      printf(LOG_TAG " g722 decoder created (lazy) input audio size: %u\n", size);
    }
    HALAudioDecodeError err =
        HAL_G722Decode(g_audio->g722_decoder, data, (size_t)size, decode_buf, &decode_pcm_len);
    if (err != HAL_DECODE_OK) {
      printf(LOG_TAG " write: g722 decode failed (err=%d)\n", (int)err);
      return 0;
    }
    frames = (int)(decode_pcm_len / (MAC_CHANNELS * (MAC_BITS_PER_SAMPLE / 8)));
    pcm = (const int16_t *)decode_buf;
  } else {
    printf(LOG_TAG " write: unsupported codec %d\n", (int)codec);
    return -1;
  }

  if (frames <= 0) {
    printf(LOG_TAG " write: decoded 0 frames\n");
    return 0;
  }

  bool need_start = !g_audio->is_recording && !g_audio->is_playing;
  if (need_start) {
    OSStatus st = AudioOutputUnitStart(g_audio->audio_unit);
    if (st != noErr) {
      printf(LOG_TAG " write: AudioOutputUnitStart failed: %d\n", (int)st);
      return -1;
    }
    g_audio->is_playing = YES;
  }

  pthread_mutex_lock(&g_audio->play_ring.mutex);

  if (frames > g_audio->play_ring.buffer_size_frames) {
    int skip = frames - g_audio->play_ring.buffer_size_frames;
    pcm += (size_t)skip * MAC_CHANNELS;
    frames = g_audio->play_ring.buffer_size_frames;
  }

  for (int f = 0; f < frames; f++) {
    int idx = (g_audio->play_ring.write_pos + f) % g_audio->play_ring.buffer_size_frames;
    for (int c = 0; c < MAC_CHANNELS; c++) {
      g_audio->play_ring.data[idx * MAC_CHANNELS + c] = pcm[f * MAC_CHANNELS + c];
    }
  }
  g_audio->play_ring.write_pos =
      (g_audio->play_ring.write_pos + frames) % g_audio->play_ring.buffer_size_frames;
  g_audio->play_ring.available_frames += frames;
  g_audio->play_ring.is_active = 1;
  pthread_mutex_unlock(&g_audio->play_ring.mutex);

  g_audio->is_playing = YES;
  return 0;
}

static OSStatus _playback_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                                   UInt32 inNumberFrames, AudioBufferList *ioData) {
  (void)ioActionFlags;
  (void)inTimeStamp;
  (void)inBusNumber;
  AitalkMacAudio *self_ = (__bridge AitalkMacAudio *)inRefCon;

  for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
    int16_t *out = (int16_t *)ioData->mBuffers[i].mData;
    if (!out) {
      continue;
    }

    pthread_mutex_lock(&self_->play_ring.mutex);
    // 有一帧播一帧，没有则填静音，SDK 内部做 jitter 抗抖动
    if (self_->play_ring.is_active && self_->play_ring.available_frames > 0) {
      UInt32 take = inNumberFrames;
      if ((int)take > self_->play_ring.available_frames) {
        take = (UInt32)self_->play_ring.available_frames;
      }
      for (UInt32 f = 0; f < take; f++) {
        int idx = (self_->play_ring.read_pos + (int)f) % self_->play_ring.buffer_size_frames;
        for (int c = 0; c < MAC_CHANNELS; c++) {
          out[f * MAC_CHANNELS + c] = self_->play_ring.data[idx * MAC_CHANNELS + c];
        }
      }
      self_->play_ring.read_pos =
          (self_->play_ring.read_pos + (int)take) % self_->play_ring.buffer_size_frames;
      self_->play_ring.available_frames -= (int)take;

      if (take < inNumberFrames) {
        memset(out + (size_t)take * MAC_CHANNELS, 0,
               (size_t)(inNumberFrames - take) * MAC_CHANNELS * sizeof(int16_t));
      }
    } else {
      memset(out, 0, ioData->mBuffers[i].mDataByteSize);
    }
    pthread_mutex_unlock(&self_->play_ring.mutex);
  }
  return noErr;
}

void audio_device_close(void) {
  if (!g_audio) {
    return;
  }
  AitalkMacAudio *obj = g_audio;
  g_audio = nil;
  obj->is_recording = NO;
  obj->is_playing = NO;
  pthread_mutex_lock(&obj->play_ring.mutex);
  obj->play_ring.is_active = 0;
  obj->play_ring.read_pos = 0;
  obj->play_ring.write_pos = 0;
  obj->play_ring.available_frames = 0;
  pthread_mutex_unlock(&obj->play_ring.mutex);
  [obj teardown];
  obj = nil; /* ARC 释放对象 */
  printf(LOG_TAG " closed\n");
}

tc_iot_audio_codec_e audio_device_default_codec(void) {
  return MAC_SEND_CODEC_E;
}

bool audio_device_codec_supported(tc_iot_audio_codec_e codec) {
  return codec == TC_IOT_AUDIO_CODEC_PCM || codec == MAC_SEND_CODEC_E ||
         codec == TC_IOT_AUDIO_CODEC_OPUS || codec == TC_IOT_AUDIO_CODEC_AAC ||
         codec == TC_IOT_AUDIO_CODEC_G722;
}

uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec) {
  (void)codec;
  return MAC_CODEC_FRAME_DURATION_MS;
}

void audio_device_sleep_ms(uint32_t ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

#ifdef __cplusplus
}
#endif
