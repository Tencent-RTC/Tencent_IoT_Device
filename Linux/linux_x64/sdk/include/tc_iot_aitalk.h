// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef __TC_IOT_AITALK_H__
#define __TC_IOT_AITALK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tc_iot_av.h"
#include "tc_iot_def.h"
#include "tc_iot_err.h"

#define TC_IOT_AITALK_BOT_ID_MAX_LEN 16
#define TC_IOT_AITALK_MAX_CONTACT_COUNT 10

typedef enum {
  TC_IOT_AITALK_MODE_CONTINUOUS,
  TC_IOT_AITALK_MODE_PUSH_TO_TALK,
} tc_iot_aitalk_mode_e;

typedef enum {
  TC_IOT_AITALK_BOT_STATE_LISTENING = 1,
  TC_IOT_AITALK_BOT_STATE_THINKING = 2,
  TC_IOT_AITALK_BOT_STATE_SPEAKING = 3,
  TC_IOT_AITALK_BOT_STATE_INTERRUPTED = 4,
  TC_IOT_AITALK_BOT_STATE_FINISHED = 5,
} tc_iot_aitalk_bot_state_e;

typedef struct {
  tc_iot_audio_codec_e codec;
  tc_iot_audio_sample_rate_e sample_rate;
  uint32_t frame_duration_ms;
} tc_iot_aitalk_audio_option_s;

typedef struct {
  void (*on_receive_bot_audio)(const tc_iot_audio_frame *frame, void *user_data);

  void (*on_receive_bot_text)(const char *text, void *user_data);
  void (*on_receive_asr_text)(const char *text, void *user_data);
  void (*on_bot_state_changed)(tc_iot_aitalk_bot_state_e old_state, tc_iot_aitalk_bot_state_e new_state,
                               void *user_data);

  void (*on_launch_call)(const tc_iot_contact_s *contact, void *user_data);

  void (*on_error)(tc_iot_error_e error_code, const char *error_msg, void *user_data);
} tc_iot_aitalk_observer_s;

typedef struct {
  char bot_id[TC_IOT_AITALK_BOT_ID_MAX_LEN + 1];
  tc_iot_aitalk_audio_option_s audio_option;
  const char *prompt_variables_json;
} tc_iot_aitalk_init_params_s;

typedef void (*tc_iot_aitalk_operation_cb)(tc_iot_error_e error_code, const char *error_message);

tc_iot_error_e tc_iot_aitalk_init(const tc_iot_aitalk_init_params_s *init_params,
                                  const tc_iot_aitalk_observer_s *observer, void *user_data);
tc_iot_error_e tc_iot_aitalk_deinit(void);

tc_iot_error_e tc_iot_aitalk_start_speak(tc_iot_aitalk_mode_e mode, tc_iot_aitalk_operation_cb callback);
tc_iot_error_e tc_iot_aitalk_stop_speak(void);

tc_iot_error_e tc_iot_aitalk_send_audio(const tc_iot_audio_frame *frame);
tc_iot_error_e tc_iot_aitalk_send_text(const char *text);

tc_iot_error_e tc_iot_aitalk_register_contacts(const tc_iot_contact_s *contacts, uint32_t contact_count,
                                               tc_iot_aitalk_operation_cb callback);

tc_iot_error_e tc_iot_aitalk_interrupt(void);

#ifdef __cplusplus
}
#endif

#endif /* __TC_IOT_AITALK_H__ */
