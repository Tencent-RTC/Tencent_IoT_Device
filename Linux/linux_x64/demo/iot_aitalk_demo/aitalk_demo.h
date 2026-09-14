// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_IOT_AITALK_DEMO_AITALK_DEMO_H_
#define SRC_DEMO_IOT_AITALK_DEMO_AITALK_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tc_iot_av.h"

typedef struct {
  const char *product_id;
  const char *device_id;
  const char *device_secret;
  const char *region;                 // NULL defaults to "ap-guangzhou"
  const char *bot_id;                 // empty string = platform default bot
  const char *prompt_variables_json;  // optional JSON string, NULL to skip
  const tc_iot_contact_s *contacts;   // optional contact array, NULL to skip
  uint32_t contact_count;             // number of contacts, 0 to skip
} aitalk_demo_config_t;

int aitalk_demo_start(const aitalk_demo_config_t *cfg);

void aitalk_demo_stop(void);

bool aitalk_demo_has_error(void);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_IOT_AITALK_DEMO_AITALK_DEMO_H_
