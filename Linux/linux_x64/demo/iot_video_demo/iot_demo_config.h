// Copyright (c) 2026 Tencent. All rights reserved.
//
// iot_video_demo 集中配置头：
//   - 把上下行音频参数（采样率 / 声道 / 帧长 / 帧字节数）与视频参数
//     （分辨率 / 帧率）集中维护，av_device / playout / 其它 demo 模块
//     共用同一份定义，避免分散导致的隐式不一致。
//   - 仅放编译时常量；运行时配置（如码率、是否启停）继续走 av_config_t。
//
// 命名规则：
//   MIC_*    上行采集端（MIC）参数
//   SPK_*    下行播放端（SPK）参数
//   VIDEO_*  视频采集参数
//   IOT_*    设备身份 / 默认通话目标 / WiFi（仅 bk7258 使用）

#ifndef SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_CONFIG_H_
#define SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_CONFIG_H_

// ==================== 设备身份与默认通话目标 ====================
//
// 仅 bk7258 入口（platform/bk7258/solution/ap/ap_main.c）使用：
//   - 设备三元组 IOT_PRODUCT_ID / IOT_DEVICE_ID / IOT_DEVICE_SECRET：
//     传给 iot_demo_start，登录腾讯云 IoT 平台。
//   - IOT_DEFAULT_CALLEE：`iot_call` 串口命令省略目标参数时使用，
//     格式 "modelId/wxAppId/openId"。
// Linux 入口通过命令行 -p/-d/-s 传入，不读这些宏。

#ifndef IOT_PRODUCT_ID
#  define IOT_PRODUCT_ID "请从控制台获取"
#endif
#ifndef IOT_DEVICE_ID
#  define IOT_DEVICE_ID "请从控制台获取"
#endif
#ifndef IOT_DEVICE_SECRET
#  define IOT_DEVICE_SECRET "请从控制台获取"
#endif

// 微信默认通话目标，格式 "modelId/wxAppId/openId"
#ifndef IOT_DEFAULT_CALLEE
#  define IOT_DEFAULT_CALLEE "请从控制台获取"
#endif

// ==================== WiFi（仅 bk7258 使用）====================
//
// bk7258 启动后通过 `iot_auto_connect_wifi(IOT_WIFI_SSID, IOT_WIFI_PASSWORD)`
// 自动连接此 WiFi。Linux 入口不读这两个宏。

#ifndef IOT_WIFI_SSID
#  define IOT_WIFI_SSID "your-wifi-ssid"
#endif
#ifndef IOT_WIFI_PASSWORD
#  define IOT_WIFI_PASSWORD "your-wifi-password"
#endif

// ==================== 音频参数 ====================
//
// PCM 位深固定为 16-bit（S16LE，2 字节/sample）。下面两个公式宏给出一处口径，
// MIC / SPK / playout / av_sender 等处统一复用，避免分散计算造成的不一致。

// 由 (采样率, 声道数, 帧长 ms) 计算一帧字节数。
#define PCM_FRAME_BYTES(sample_rate, channels, frame_ms) \
  ((sample_rate) * 2 * (channels) * (frame_ms) / 1000)

// 由 (采样率, 声道数, 帧字节数) 反推帧长 ms。
// 整数除法；调用方应保证 frame_bytes 是 (sample_rate × 2 × channels × N / 1000)
// 这种 能整除的"完整帧"，否则会丢掉不足 1ms 的部分。
#define PCM_FRAME_MS(sample_rate, channels, frame_bytes) \
  ((frame_bytes) * 1000 / ((sample_rate) * 2 * (channels)))

#define MIC_SAMPLE_RATE 16000
#define MIC_CHANNELS 1
#define MIC_FRAME_MS 20
#define MIC_FRAME_BYTES PCM_FRAME_BYTES(MIC_SAMPLE_RATE, MIC_CHANNELS, MIC_FRAME_MS)

#define SPK_SAMPLE_RATE 16000
#define SPK_CHANNELS 1
#define SPK_FRAME_MS 20
#define SPK_FRAME_BYTES PCM_FRAME_BYTES(SPK_SAMPLE_RATE, SPK_CHANNELS, SPK_FRAME_MS)

// ==================== 视频参数 ====================

// 视频分辨率与板型/相机路径对齐（av_sender 上报宽高与编码实际尺寸必须一致）：
// - AMP+AV(UVC)：480x320，避免 640x480 MJPEG→H264 打穿 CPU0 PSRAM
// - AMP+AI(DVP/GC2145)：480x480（见 dvp_gc2145.c ppi_cap）
// - SMP+AV(UVC)：640x480
// - XR872 的 BF20A6 sensor: 240x320
// - 其余（含 SMP+AI DVP）：480x320
#if defined(BEKEN_IS_AMP) && defined(BEKEN_BOARD_AV)
#  define VIDEO_WIDTH 480
#  define VIDEO_HEIGHT 320
#elif defined(BEKEN_IS_AMP)
#  define VIDEO_WIDTH 480
#  define VIDEO_HEIGHT 480
#elif defined(BEKEN_BOARD_AV)
#  define VIDEO_WIDTH 640
#  define VIDEO_HEIGHT 480
#elif defined(OS_XR872)
#  define VIDEO_WIDTH 240
#  define VIDEO_HEIGHT 320
#else
#  define VIDEO_WIDTH 480
#  define VIDEO_HEIGHT 320
#endif

// 视频帧率：直接用整数（每秒帧数）。
// 平台层（如 BEKEN DVP）若需要 enum/bitmask 形式的 fps，请在 .c 文件做映射，
// 不要在这里 include 平台头，保持本配置头平台无关。
#define VIDEO_FPS 15

// ==================== AITalk 配置 ====================
//
// iot_aitalk CLI 命令使用：bot_id 可在串口命令行覆盖。
// 空串 = 使用平台默认 bot。

#ifndef IOT_AITALK_BOT_ID
#  define IOT_AITALK_BOT_ID ""
#endif

#endif  // SRC_DEMO_IOT_VIDEO_DEMO_IOT_DEMO_CONFIG_H_
