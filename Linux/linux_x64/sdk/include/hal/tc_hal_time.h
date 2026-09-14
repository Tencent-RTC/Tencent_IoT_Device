/**
 * @file tc_hal_time.h
 * @author shushengwu (shushengwu@tencent.com)
 * @brief
 * @version 0.1
 * @date 2025-10-21
 *
 * @copyright
 * Tencent is pleased to support the open source community by making IoT Hub available.
 * Copyright(C) 2021 - 2026 THL A29 Limited, a Tencent company.All rights reserved.
 * Licensed under the MIT License(the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef __TC_HAL_TIME_H__
#define __TC_HAL_TIME_H__

#include "tc_hal_platform_inc.h"
#include "tc_hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 休眠一段时间
 *
 * @param ms 休眠间隔时间，单位为毫秒
 */
void HAL_SleepMs(uint32_t ms);

//////////////////////////////////////////////////////////////////////////
// utc/local timestamp and timer functions
//////////////////////////////////////////////////////////////////////////
#define TIME_FORMAT_STR_LEN (24)

/**
 * @brief 获取本地时间，格式为：%y-%m-%d %H:%M:%S.%ms（例如：2023-01-11 18:01:12.973）
 *
 * 此函数用于获取当前本地时间，并将其格式化为指定的字符串格式。
 * 用户需要提供一个足够大的缓冲区来存储格式化后的时间字符串。
 *
 * @param time_str 用于存储格式化时间字符串的缓冲区，其大小应至少为 TIME_FORMAT_STR_LEN
 * @param time_str_len 缓冲区 time_str 的长度
 * @return 返回格式化后的时间字符串
 */
char *HAL_GetLocalTime(char *time_str, size_t time_str_len);

/**
 * @brief 获取当前时间戳（毫秒）
 *
 * 该函数用于获取当前系统时间的时间戳，单位为毫秒。
 *
 * @return 返回当前时间的时间戳，单位为毫秒
 */
uint64_t HAL_GetTimeMs(void);

/**
 * @brief 获取当前系统的滴答值，一般情况下是从上电开始一直累加，不可突变
 *
 * @return 返回当前系统的滴答值
 */
uint64_t HAL_GetTicksTimeMs(void);

typedef uint64_t HAL_Timer;

/**
 * @brief 设置计时器的倒计时/过期值（毫秒）
 *
 * @param timeout_ms    倒计时/过期值（单位：毫秒）
 * @return              初始化后的定时器
 */
HAL_Timer HAL_Timer_countdown_ms(uint64_t timeout_ms);

/**
 * @brief 设置计时器的倒计时/过期值（秒）
 *
 * @param timeout_s     倒计时/过期值，单位为秒
 * @return              初始化后的定时器
 */
HAL_Timer HAL_Timer_countdown(uint64_t timeout_s);

/**
 * @brief 检查定时器的剩余时间
 *
 * @param timer     定时器
 * @return          如果定时器已到期返回0，否则返回剩余的毫秒数
 */
uint64_t HAL_Timer_remain(HAL_Timer timer);

/**
 * @brief 检查定时器是否到期
 *
 * @param timer     定时器
 * @return          true = 已到期, false = 尚未到期
 */
bool HAL_Timer_expired(HAL_Timer timer);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
