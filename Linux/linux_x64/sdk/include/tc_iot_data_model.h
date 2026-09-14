// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef __TC_IOT_DATA_MODEL_H__
#define __TC_IOT_DATA_MODEL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tc_iot_err.h"

typedef bool data_model_bool_t;
typedef int32_t data_model_int_t;
typedef double data_model_float_t;
typedef char *data_model_string_t;
typedef uint32_t data_model_enum_t;
typedef uint32_t data_model_time_t;

typedef union {
  data_model_bool_t value_bool;
  data_model_int_t value_int;
  data_model_float_t value_float;
  data_model_string_t value_string;
  data_model_enum_t value_enum;
  data_model_time_t value_time;
} data_model_data_value_t;

typedef enum {
  DATA_MODEL_DATA_TYPE_BOOL,
  DATA_MODEL_DATA_TYPE_INT,
  DATA_MODEL_DATA_TYPE_FLOAT,
  DATA_MODEL_DATA_TYPE_STRING,
  DATA_MODEL_DATA_TYPE_ENUM,
  DATA_MODEL_DATA_TYPE_TIME,
  DATA_MODEL_DATA_TYPE_OBJECT,
  DATA_MODEL_DATA_TYPE_ARRAY
} data_model_data_type_t;

typedef struct {
  char *data_id;
  data_model_data_type_t data_type;
  data_model_data_value_t data_value;
} data_model_data_item_t;

typedef struct {
  data_model_data_item_t property;
} tc_iot_data_model_property_t;

typedef enum {
  DATA_MODEL_EVENT_TYPE_INFO,
  DATA_MODEL_EVENT_TYPE_ALERT,
  DATA_MODEL_EVENT_TYPE_FAULT,
} data_model_event_type_t;

typedef struct {
  char *event_id;
  data_model_event_type_t event_type;

  int event_data_num;
  data_model_data_item_t *event_data_list;
} tc_iot_data_model_event_t;

typedef struct {
  char *action_id;
  char *token;
  uint32_t timestamp;

  int action_input_data_num;
  data_model_data_item_t *action_input_data_list;

  int action_output_data_num;
  data_model_data_item_t *action_output_data_list;
} tc_iot_data_model_action_t;

typedef enum {
  DATA_MODEL_REPORT_SUCCESS = 0,
  DATA_MODEL_REPORT_REJECTED = 1,
  DATA_MODEL_REPORT_NO_RESPONSE = 2,
  DATA_MODEL_REPORT_LOCAL_TIMEOUT = 3,
} data_model_report_result_t;

typedef struct tc_iot_data_model_callback {
  void (*on_report_property_result_cb)(char *property_id, void *user_data, data_model_report_result_t result_code);
  void (*on_receive_property_changed_cb)(char *property_id, tc_iot_data_model_property_t *data_model_property);

  void (*on_report_event_result_cb)(char *event_id, void *user_data, data_model_report_result_t result_code);

  int (*on_receive_new_action_cb)(char *action_id, tc_iot_data_model_action_t *data_model_action);
} tc_iot_data_model_callback_t;

tc_iot_error_e tc_iot_data_model_init(tc_iot_data_model_callback_t data_model_callback);
tc_iot_error_e tc_iot_data_model_report_property(tc_iot_data_model_property_t *data_model_property, int property_num,
                                                 void *report_cb_user_data);
tc_iot_error_e tc_iot_data_model_report_event(tc_iot_data_model_event_t *data_model_event, int event_num,
                                              void *event_cb_user_data);
tc_iot_error_e tc_iot_data_model_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __TC_IOT_DATA_MODEL_H__ */
