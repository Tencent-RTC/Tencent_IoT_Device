/**
 * @file tc_hal_thread.h
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

#ifndef __TC_HAL_THREAD_H__
#define __TC_HAL_THREAD_H__

#include "tc_hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 线程优先级枚举类型定义
typedef enum {
  THREAD_PRIORITY_LOW = -1,
  THREAD_PRIORITY_NORMAL = 0,
  THREAD_PRIORITY_HIGH = 1,
  THREAD_PRIORITY_HIGHER = 2,
  THREAD_PRIORITY_HIGHEST = 3
} ThreadPriorityLevel;

// 创建线程参数结构体定义
typedef struct ThreadParams {
  char *thread_name;          // 线程名，必要参数，用户需要设置
  ThreadHandle_t thread_id;   // 线程句柄，必要参数
  ThreadRunFunc thread_func;  // 线程运行函数，必要参数，用户需要设置
  void *user_arg;             // 用户自定义参数，可选参数
  size_t stack_size;          // 线程栈大小，单位字节。RTOS上为必要参数，用户需要设置
  void *stack_ptr;            // 线程栈入口指针，某些平台如threadX需要该参数
  uint32_t slice_tick;        // 时间片大小，某些平台如RT-Thread需要指定该参数
  int16_t priority;           // 线程优先级，建议用ThreadPriorityLevel来描述级别
} ThreadParams;

/**
 * @brief 线程创建函数，对于RTOS设备来讲尽可能创建到外部psram中
 *
 * @param params  ThreadParams 线程创建的参数
 *                注意：该变量需要保证生命周期在线程启动后仍然有效，建议用static变量
 *
 * @return 0表示成功，非0表示失败
 */
int HAL_ThreadCreate(ThreadParams *params);

/**
 * @brief 线程销毁函数，SDK创建的线程执行完毕后，会自动调用线程销毁函数，用户不需要重复销毁
 *
 * @param thread_t thread handle 线程句柄
 *
 * @return 0表示成功，非0表示失败
 */
int HAL_ThreadDestroy(ThreadHandle_t thread_t);  // 修改为值传递，与pthread_join对齐

/**
 * @brief 获取当前线程的句柄
 *
 * @return 线程句柄
 */
ThreadHandle_t HAL_GetCurrentThreadHandle(void);  // 返回类型改为ThreadHandle_t

/**
 * @brief 获取平台默认的线程栈大小（HAL_ThreadCreate 的 stack_size 参数推荐值）
 *
 * 该函数返回当前平台推荐的默认线程栈大小，可直接赋值给 ThreadParams::stack_size。
 *
 * @return 平台默认的线程栈大小
 */
size_t HAL_GetDefaultThreadStackSize(void);

typedef void (*TLSFreeHandler)(void *ptr);

/**
 * @brief 分配一个 TLS 槽位，返回 >=0 (槽位索引) 为成功，<0 表示失败
 * @param dtor TLS析构函数，用于在线程退出时清理TLS数据
 * @return 槽位索引，>=0 表示成功，<0 表示失败
 */
int HAL_AllocTLS(TLSFreeHandler dtor);

/**
 * @brief 释放槽位索引
 * @param key 槽位索引
 * @return 0 成功，<0 失败
 */
int HAL_FreeTLS(int key);

/**
 * @brief 为“当前线程”在 key 槽位设置 value；
 * @param key 槽位索引
 * @param value 值
 */
int HAL_SetTLSValue(int key, void *value);

/**
 * @brief 获取“当前线程”在 key 槽位设置的值
 */
void *HAL_GetTLSValue(int key);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
