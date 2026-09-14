// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_IOT_VIDEO_DEMO_AV_RECEIVER_H_
#define SRC_DEMO_IOT_VIDEO_DEMO_AV_RECEIVER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "tc_iot_def.h"

typedef struct av_receiver_t av_receiver_t;
av_receiver_t *av_receiver_alloc(void);
void av_receiver_free(av_receiver_t *receiver);

void av_receiver_on_audio_frame(av_receiver_t *receiver, const tc_iot_audio_frame *frame);
void av_receiver_on_video_frame(av_receiver_t *receiver, const tc_iot_video_frame *frame);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_VIDEO_DEMO_AV_RECEIVER_H_
