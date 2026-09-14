// Copyright (c) 2026 Tencent. All rights reserved.

// cloud_storage_demo：云存储 demo
//
// 设计：
//   - cloud_storage_demo_init / cloud_storage_demo_deinit 控制云存储生命周期
//   - 初始化成功后自动为全时套餐通道开启全时录像
//   - cloud_storage_demo_trigger_event(channel_id) 由外部触发（Linux: 命令行;
//   bk7258: 按键）
//     随机选择录像事件或抓图事件执行
//   - 平台差异（图片获取、定时器、线程）封装在 av_device 层
//
// 使用：
//   cloud_storage_demo_init/deinit 在 iot_demo_start/stop
//   中自动调用（全平台）； Linux:  命令行 "cs_event <channel_id>"
//   触发事件；"cs_deinit" / "cs_init" 可手动重启 bk7258: 按键触发
//   cloud_storage_demo_trigger_event(0)

#ifndef CLOUD_STORAGE_DEMO_H_
#define CLOUD_STORAGE_DEMO_H_

#include "av_sender.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============ 事件 ID 定义示例
// event_id 通过 tc_iot_cloud_storage_event_params_t.event_id 传入
//
// 录像事件：
//   1  - 门铃呼叫事件（访客按下门铃）
//   2  - 运动检测事件（画面中检测到运动物体）
//   3  - 人形检测事件（AI 识别到人形轮廓）
//   4  - 区域入侵事件（目标进入预设警戒区域）
//   5  - 区域徘徊事件（目标在预设区域内停留超过阈值）
//   6  - 异常声音事件（如哭声、玻璃破碎等）
//
// 抓图事件（单次上报抓图事件）：
//   100 - 人脸识别抓拍（AI 识别到人脸时抓拍上传）
//   101 - 开门状态抓拍（检测到门被打开时抓拍上传）
// ======================================

// 初始化云存储 demo；audio_file / video_file 为各通道 sender 使用的媒体源（Linux 仿真）；
// audio_mutex 为共享音频互斥锁（由调用方创建，与其它 sender 共用同一把），成功返回 0。
int cloud_storage_demo_init(const char *audio_file, const char *video_file, HAL_Mutex *audio_mutex);

// 反初始化云存储 demo
void cloud_storage_demo_deinit(void);

// 触发一次随机事件（录像或抓图事件）；
int cloud_storage_demo_trigger_event(int channel_id);

#ifdef __cplusplus
}
#endif

#endif  // CLOUD_STORAGE_DEMO_H_
