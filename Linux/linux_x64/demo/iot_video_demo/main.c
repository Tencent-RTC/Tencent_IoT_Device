// Copyright (c) 2026 Tencent. All rights reserved.

// iot_video_demo Linux 入口：
//   - argv 解析三元组并启动 iot_demo（云存自动随 iot_demo_start 初始化）；
//   - 注册命令循环（call / accept / reject / hangup / get_contacts /
//     cs_event / seek / stop），
//     循环阻塞到收到 `stop` 后驱动 iot_demo_stop 完成清理。
//
// bk7258 入口在 platform/bk7258/solution/ap/ap_main.c。

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cloud_storage_demo.h"
#include "cmd_exec.h"
#include "iot_demo.h"
#include "tc_iot_av.h"

// ==================== 状态 ====================

// 命令循环退出标志
static int s_cmd_running = 1;

// ==================== 命令实现 ====================

static int _on_cmd_stop(int argc, char **argv) {
  (void)argc;
  (void)argv;
  s_cmd_running = 0;
  return 0;
}

static int _on_cmd_call(int argc, char **argv) {
  const char *user_id = NULL;
  tc_iot_media_content_e media_content = TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
  tc_iot_video_quality_e video_quality = TC_IOT_VIDEO_QUALITY_SD;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
      user_id = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      int val = atoi(argv[i + 1]);
      media_content = (val == 1) ? TC_IOT_MEDIA_CONTENT_AUDIO : TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
      i++;
    }
  }
  if (!user_id || user_id[0] == '\0') {
    printf("must set -u openid/user_id\n");
    return -1;
  }
  return iot_demo_call(user_id, media_content, video_quality);
}

static int _on_cmd_accept(int argc, char **argv) {
  const char *user_id = NULL;
  tc_iot_media_content_e media_content = TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
  tc_iot_video_quality_e video_quality = TC_IOT_VIDEO_QUALITY_SD;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
      user_id = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      int val = atoi(argv[i + 1]);
      media_content = (val == 1) ? TC_IOT_MEDIA_CONTENT_AUDIO : TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
      i++;
    } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
      int val = atoi(argv[i + 1]);
      if (val >= TC_IOT_VIDEO_QUALITY_LD && val <= TC_IOT_VIDEO_QUALITY_FHD) {
        video_quality = (tc_iot_video_quality_e)val;
      }
      i++;
    }
  }
  if (!user_id || user_id[0] == '\0') {
    printf("must set -u openid/user_id\n");
    return -1;
  }
  return iot_demo_accept(user_id, media_content, video_quality);
}

static int _on_cmd_reject(int argc, char **argv) {
  const char *user_id = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
      user_id = argv[i + 1];
      i++;
    }
  }
  return iot_demo_reject(user_id);
}

static int _on_cmd_hangup(int argc, char **argv) {
  const char *user_id = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
      user_id = argv[i + 1];
      i++;
    }
  }
  return iot_demo_hangup(user_id);
}

static void _get_contacts_cb(tc_iot_error_e error_code, const char *error_message,
                             const tc_iot_contact_s *contacts, uint32_t count,
                             const char *next_cursor) {
  printf("[demo] get_contacts: code=%d msg=%s next_cursor=%s count=%u\n", error_code,
         error_message ? error_message : "", next_cursor ? next_cursor : "", count);
  for (uint32_t i = 0; i < count; i++) {
    printf("  contact: %s %s %s\n", contacts[i].user_id, contacts[i].user_name,
           contacts[i].avatar_url);
  }
}

static int _on_cmd_get_contacts(int argc, char **argv) {
  int32_t count = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      count = atoi(argv[i + 1]);
      i++;
    }
  }
  tc_iot_av_get_contacts(NULL, count, _get_contacts_cb);
  return 0;
}

// ==================== 命令: cs_event ====================

static int _on_cmd_cs_event(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: cs_event <channel_id>\n");
    return -1;
  }
  int channel_id = atoi(argv[1]);
  int rc = cloud_storage_demo_trigger_event(channel_id);
  printf("[cmd] cs_event channel_id=%d rc=%d\n", channel_id, rc);
  return rc;
}

// ==================== 命令: seek (MP4 解封装测试) ====================

static int _on_cmd_seek(int argc, char **argv) {
  if (argc < 2) {
    printf(
        "usage: seek <target_sec>\n"
        "  seek MP4 file to specified time position (seconds).\n"
        "  Must be called after init, before audio/video capture starts.\n");
    return -1;
  }
  uint64_t target_sec = (uint64_t)atoll(argv[1]);
  uint64_t actual_ms = iot_demo_seek_mp4_second(target_sec);
  if (actual_ms == 0) {
    printf("[cmd] seek failed: target_sec=%llu\n", (unsigned long long)target_sec);
    return -1;
  }
  printf("[cmd] seek done: target_sec=%llu actual_ms=%llu\n", (unsigned long long)target_sec,
         (unsigned long long)actual_ms);
  return 0;
}

// ==================== 命令注册 ====================

static void _register_commands(void) {
  cmd_exec_register("stop", _on_cmd_stop, "");
  cmd_exec_register("call", _on_cmd_call,
                    "-u open_id/user_id -n user_name -c content(1=audio, else=audio+video)");
  cmd_exec_register("accept", _on_cmd_accept,
                    "-u open_id/user_id -c content(1=audio, else=audio+video) -q quality(1=LD, "
                    "2=SD, 3=HD, 4=FHD)");
  cmd_exec_register("reject", _on_cmd_reject, "-u open_id/user_id");
  cmd_exec_register("hangup", _on_cmd_hangup, "-u open_id/user_id");
  cmd_exec_register("get_contacts", _on_cmd_get_contacts, "-n num");
  cmd_exec_register("cs_event", _on_cmd_cs_event, "<channel_id> trigger random event");
  cmd_exec_register("seek", _on_cmd_seek, "<target_sec> seek mp4 to time position (seconds)");
}

// ==================== main ====================

int main(int argc, char **argv) {
  if (argc < 7) {
    printf("Usage: %s -p <product_id> -d <device_id> -s <device_secret>\n", argv[0]);
    return 0;
  }

  iot_demo_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.region = "ap-guangzhou";
#ifdef TC_IOT_DEMO_ROOT
#  define DEMO_MEDIA_DIR TC_IOT_DEMO_ROOT "/iot_video_demo/demo_media/"
#else
#  define DEMO_MEDIA_DIR "./demo/iot_video_demo/demo_media/"
#endif
  // 如果想使用 MP4 作为音视频输入源，请打开注释
  // cfg.mp4_file_path = DEMO_MEDIA_DIR "size640x360_gop50_fps25_sample16000_mono_16bit_le.mp4";
  cfg.audio_file_path = DEMO_MEDIA_DIR "audio_sample16000_mono_16bit_le.pcm";
  cfg.video_file_path = DEMO_MEDIA_DIR "video_size640x360_gop50_fps25.h264";
  // 如果想使用 MJPEG 作为视频输入源，请打开注释
  // cfg.video_file_path = DEMO_MEDIA_DIR "mjpg/mjpg_size224x126_pts001.jpg";

  cfg.dump_audio_path = "./recv_av_data/recv_audio.pcm";
  cfg.dump_video_path = "./recv_av_data/recv_video.h264";
  // 保存收到的 MJPEG 图片的路径
  // cfg.dump_video_path = "./recv_av_data/";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      cfg.product_id = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      cfg.device_id = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      cfg.device_secret = argv[i + 1];
      i++;
    }
  }

  if (!cfg.product_id || !cfg.device_id || !cfg.device_secret) {
    printf("device info error\n");
    return 1;
  }

  if (cfg.mp4_file_path && cfg.mp4_file_path[0] != '\0') {
    cfg.audio_file_path = cfg.mp4_file_path;
    cfg.video_file_path = cfg.mp4_file_path;
  }

  if (iot_demo_start(&cfg) != 0) {
    return 1;
  }

  // 命令循环
  cmd_exec_init();
  _register_commands();

  s_cmd_running = 1;
  while (s_cmd_running != 0) {
    usleep(100 * 1000);
    cmd_exec_process();
  }

  cmd_exec_exit();

  iot_demo_stop();

  printf("main exit\n");
  return 0;
}
