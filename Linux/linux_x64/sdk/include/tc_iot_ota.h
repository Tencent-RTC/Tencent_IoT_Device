// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_API_IOT_TC_IOT_OTA_H_
#define SRC_API_IOT_TC_IOT_OTA_H_

#include <stddef.h>
#include <stdint.h>

#include "tc_iot.h"
#include "tc_iot_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_iot_ota_firmware_info_s {
  char module_name[64];
  char module_version[32];
} tc_iot_ota_firmware_info_t;

typedef struct tc_iot_ota_config_s {
  tc_iot_ota_firmware_info_t *firmware_info;
  uint32_t firmware_info_count;
} tc_iot_ota_config_t;

typedef struct tc_iot_ota_observer_s {
  void (*on_receive_firmware_upgrade_notify)(const tc_iot_ota_firmware_info_t *firmware_info, void *user_data);
  void (*on_download_firmware_progress)(const tc_iot_ota_firmware_info_t *firmware_info, uint32_t current_size,
                                        uint32_t total_size, void *user_data);
  void (*on_download_firmware_result)(const tc_iot_ota_firmware_info_t *firmware_info, tc_iot_error_e error_code,
                                      const char *error_message, const char *file_path, void *user_data);
} tc_iot_ota_observer_t;

tc_iot_error_e tc_iot_ota_init(const tc_iot_ota_config_t *config, const tc_iot_ota_observer_t *observer,
                               void *user_data);

tc_iot_error_e tc_iot_ota_deinit(void);

tc_iot_error_e tc_iot_ota_download_firmware(const tc_iot_ota_firmware_info_t *firmware_info,
                                            const char *download_directory);

tc_iot_error_e tc_iot_ota_report_upgrade_progress(const tc_iot_ota_firmware_info_t *firmware_info,
                                                  uint32_t upgrade_progress);

tc_iot_error_e tc_iot_ota_report_upgrade_result(const tc_iot_ota_firmware_info_t *firmware_info, int32_t result_code,
                                                const char *description);

#ifdef __cplusplus
}
#endif

#endif  // SRC_API_IOT_TC_IOT_OTA_H_
