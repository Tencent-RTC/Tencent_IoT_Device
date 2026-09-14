/**
 * @file tc_hal_memory.h
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

#ifndef __TC_HAL_MEMORY_H__
#define __TC_HAL_MEMORY_H__

#include "tc_hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 申请内存，对于资源受限的设备请适配申请PSRAM内存
 *
 * @param size   需要申请的大小，单位：字节 申请完成后需memset为0
 * @return       申请到的内存指针，NULL表示申请失败
 */
void *HAL_Malloc(size_t size);

/**
 * @brief 调整已经申请的内存大小
 *
 * @param ptr   先前分配的内存指针
 * @param size  期望的大小，单位：字节
 * @return      调整后的内存指针，NULL表示申请失败
 */
void *HAL_Realloc(void *ptr, size_t size);

/**
 * @brief 释放内存
 *
 * @param ptr    内存指针
 */
void HAL_Free(void *ptr);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
