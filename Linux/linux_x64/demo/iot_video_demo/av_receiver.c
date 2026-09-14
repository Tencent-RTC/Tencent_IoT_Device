// Copyright (c) 2026 Tencent. All rights reserved.

#include "av_receiver.h"

#include <stdbool.h>
#include <stdlib.h>

#include "av_device.h"

struct av_receiver_t {
  bool inited;
};

av_receiver_t *av_receiver_alloc(void) {
  av_receiver_t *receiver = (av_receiver_t *)calloc(1, sizeof(av_receiver_t));
  if (!receiver) {
    return NULL;
  }
  receiver->inited = true;
  return receiver;
}

void av_receiver_free(av_receiver_t *receiver) {
  if (!receiver) {
    return;
  }
  free(receiver);
}

void av_receiver_on_audio_frame(av_receiver_t *receiver, const tc_iot_audio_frame *frame) {
  if (!receiver || !frame || !frame->data || frame->data_size == 0) {
    return;
  }
  av_device_playout_audio(frame->data, (uint32_t)frame->data_size);
}

void av_receiver_on_video_frame(av_receiver_t *receiver, const tc_iot_video_frame *frame) {
  if (!receiver || !frame || !frame->data || frame->data_size == 0) {
    return;
  }

  if (TC_IOT_VIDEO_CODEC_MJPEG == frame->codec) {
    av_device_playout_mjpg(frame->data, (uint32_t)frame->data_size);
  } else {
    av_device_playout_video(frame->data, (uint32_t)frame->data_size);
  }
}
