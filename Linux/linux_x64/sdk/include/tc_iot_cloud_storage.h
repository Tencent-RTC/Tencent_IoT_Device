#ifndef __TC_IOT_CLOUD_STORAGE_H__
#define __TC_IOT_CLOUD_STORAGE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tc_iot_err.h"

typedef enum {
  TC_IOT_CLOUD_STORAGE_PLAN_TYPE_UNKNOWN = 0,
  TC_IOT_CLOUD_STORAGE_PLAN_TYPE_NONE = 1,
  TC_IOT_CLOUD_STORAGE_PLAN_TYPE_FULL_TIME = 2,
  TC_IOT_CLOUD_STORAGE_PLAN_TYPE_EVENT = 3,
  TC_IOT_CLOUD_STORAGE_PLAN_TYPE_IMAGE = 4,
} tc_iot_cloud_storage_plan_type_t;

typedef struct {
  uint32_t event_id;

  // 调用方须保证 picture_data 在 on_event_result 回调触发前始终有效
  const uint8_t *picture_data;
  uint32_t picture_size;
  uint32_t pre_record_seconds;
  const char *extra_info;
} tc_iot_cloud_storage_event_params_t;

typedef struct {
  uint32_t event_id;
  tc_iot_error_e event_report_result;
  tc_iot_error_e picture_upload_result;
} tc_iot_cloud_storage_event_result_t;

typedef struct {
  void (*on_init_result)(tc_iot_error_e error_code, const char *error_message);
  void (*on_event_result)(uint8_t channel_id, const tc_iot_cloud_storage_event_result_t *event_result);
  void (*on_plan_type_changed)(uint8_t channel_id, tc_iot_cloud_storage_plan_type_t plan_type);
} tc_iot_cloud_storage_callback_t;

tc_iot_error_e tc_iot_cloud_storage_init(const tc_iot_cloud_storage_callback_t *cloud_storage_callback);

tc_iot_error_e tc_iot_cloud_storage_deinit(void);

tc_iot_cloud_storage_plan_type_t tc_iot_cloud_storage_get_plan_type(uint8_t channel_id);

tc_iot_error_e tc_iot_cloud_storage_start_continuous_recording(uint8_t channel_id);

tc_iot_error_e tc_iot_cloud_storage_stop_continuous_recording(uint8_t channel_id);

tc_iot_error_e tc_iot_cloud_storage_start_event_recording(uint8_t channel_id,
                                                          const tc_iot_cloud_storage_event_params_t *event_params);

tc_iot_error_e tc_iot_cloud_storage_stop_event_recording(uint8_t channel_id, uint32_t event_id);

tc_iot_error_e tc_iot_cloud_storage_report_event_snapshot(uint8_t channel_id,
                                                          const tc_iot_cloud_storage_event_params_t *event_params);

#ifdef __cplusplus
}
#endif

#endif /* __TC_IOT_CLOUD_STORAGE_H__ */
