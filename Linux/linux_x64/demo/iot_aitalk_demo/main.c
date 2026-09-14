// Copyright (c) 2026 Tencent. All rights reserved.

// Linux entry for iot_aitalk_demo:
// Parses command-line args, calls aitalk_demo_start(), waits for Ctrl+C,
// then calls aitalk_demo_stop().

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aitalk_demo.h"
#include "tc_iot_aitalk.h"

static volatile sig_atomic_t g_exit_flag = 0;

typedef struct {
  const char *product_id;
  const char *device_id;
  const char *device_secret;
  const char *bot_id;
  const char *prompt_variables_json;
  const char *contacts_json;
} demo_args_t;

static void _signal_handler(int sig);
static bool _parse_args(int argc, char **argv, demo_args_t *out);
static void _print_usage(const char *app);
static bool _parse_contacts_json(char *json_str, tc_iot_contact_s *out_contacts,
                                 uint32_t *out_count);

int main(int argc, char **argv) {
  signal(SIGINT, _signal_handler);
  signal(SIGTERM, _signal_handler);

  demo_args_t args;
  if (!_parse_args(argc, argv, &args)) {
    return 1;
  }

  printf("[main] aitalk demo start\n");

  aitalk_demo_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.product_id = args.product_id;
  cfg.device_id = args.device_id;
  cfg.device_secret = args.device_secret;
  cfg.region = "ap-guangzhou";
  cfg.bot_id = args.bot_id;
  cfg.prompt_variables_json = args.prompt_variables_json;

  // parse contacts JSON if provided
  tc_iot_contact_s contacts_arr[TC_IOT_AITALK_MAX_CONTACT_COUNT];
  memset(contacts_arr, 0, sizeof(contacts_arr));
  uint32_t contact_count = 0;
  char *json_buf = NULL;

  if (args.contacts_json && args.contacts_json[0] != '\0') {
    json_buf = strdup(args.contacts_json);
    if (!json_buf) {
      printf("[main] failed to allocate memory for contacts JSON\n");
      return 1;
    }
    if (!_parse_contacts_json(json_buf, contacts_arr, &contact_count)) {
      printf("[main] failed to parse contacts JSON, exiting\n");
      free(json_buf);
      return 1;
    }
    cfg.contacts = contacts_arr;
    cfg.contact_count = contact_count;
  }

  int rc = aitalk_demo_start(&cfg);
  if (rc != 0) {
    printf("[main] aitalk_demo_start failed\n");
    free(json_buf);
    return 1;
  }

  while (!g_exit_flag && !aitalk_demo_has_error()) {
    usleep(100000);
  }

  aitalk_demo_stop();
  free(json_buf);
  printf("[main] exit\n");
  return 0;
}

static void _signal_handler(int sig) {
  (void)sig;
  g_exit_flag = 1;
}

static bool _parse_contacts_json(char *json_str, tc_iot_contact_s *out_contacts,
                                 uint32_t *out_count) {
  *out_count = 0;
  if (!json_str || json_str[0] == '\0') {
    return true;
  }

  char *p = json_str;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
    p++;
  }
  if (*p != '[') {
    printf("[main] contacts JSON: expected '[' at start\n");
    return false;
  }
  p++;

  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (*p == ']') {
      return true;
    }
    if (*p == ',') {
      p++;
      continue;
    }

    if (*p != '{') {
      printf("[main] contacts JSON: expected '{'\n");
      return false;
    }
    p++;

    if (*out_count >= TC_IOT_AITALK_MAX_CONTACT_COUNT) {
      printf("[main] contacts JSON: max %d contacts reached\n", TC_IOT_AITALK_MAX_CONTACT_COUNT);
      return true;
    }

    tc_iot_contact_s *c = &out_contacts[*out_count];
    memset(c, 0, sizeof(*c));

    while (*p) {
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
      }
      if (*p == '}') {
        p++;
        break;
      }
      if (*p == ',') {
        p++;
        continue;
      }

      // key: "fieldname"
      if (*p != '"') {
        printf("[main] contacts JSON: expected string key\n");
        return false;
      }
      p++;
      char *key = p;
      while (*p && *p != '"') {
        p++;
      }
      if (*p != '"') {
        printf("[main] contacts JSON: unclosed key\n");
        return false;
      }
      *p++ = '\0';

      // ':'
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
      }
      if (*p != ':') {
        printf("[main] contacts JSON: expected ':'\n");
        return false;
      }
      p++;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
      }

      // value: "string"
      if (*p != '"') {
        printf("[main] contacts JSON: expected string value\n");
        return false;
      }
      p++;
      char *val = p;
      while (*p && *p != '"') {
        p++;
      }
      if (*p != '"') {
        printf("[main] contacts JSON: unclosed value\n");
        return false;
      }
      *p++ = '\0';

      if (strcmp(key, "user_id") == 0) {
        c->user_id = val;
      } else if (strcmp(key, "user_name") == 0) {
        c->user_name = val;
      }
    }

    if (!c->user_id || !c->user_name) {
      printf("[main] contacts JSON: contact %u missing user_id or user_name\n", *out_count);
      return false;
    }

    printf("[main] contact %u: user_id=%s user_name=%s\n", *out_count, c->user_id, c->user_name);
    (*out_count)++;
  }

  return true;
}

static bool _parse_args(int argc, char **argv, demo_args_t *out) {
  memset(out, 0, sizeof(*out));
  out->bot_id = "";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      out->product_id = argv[++i];
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      out->device_id = argv[++i];
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      out->device_secret = argv[++i];
    } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      out->bot_id = argv[++i];
    } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
      out->prompt_variables_json = argv[++i];
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      out->contacts_json = argv[++i];
    } else {
      _print_usage(argv[0]);
      return false;
    }
  }

  if (!out->product_id || !out->device_id || !out->device_secret || !*out->product_id ||
      !*out->device_id || !*out->device_secret) {
    _print_usage(argv[0]);
    return false;
  }
  return true;
}

static void _print_usage(const char *app) {
  printf("Usage: %s [options]\n", app);
  printf("  -p <product_id>\n");
  printf("  -d <device_id>\n");
  printf("  -s <device_secret>\n");
  printf("  -b <bot_id>\n");
  printf("  -v <prompt_variables_json>   (optional, JSON object string)\n");
  printf("  -c <contacts_json>           (optional, JSON array, max %d items)\n",
         TC_IOT_AITALK_MAX_CONTACT_COUNT);
  printf("\n");
  printf("  -v example: -v '{\"key\":\"value\"}'\n");
  printf("  -c example: -c '[{\"user_id\":\"xxx\",\"user_name\":\"Alice\"}]'\n");
}
