/**
 * @file tc_hal_types.h
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

#ifndef __TC_HAL_TYPES_H__
#define __TC_HAL_TYPES_H__

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* definition of IP addr */
typedef uint32_t HAL_IP_ADDR_T;

#ifndef HAL_FD_MAX_COUNT
#  if defined(_POSIX_VERSION) || defined(__unix__) || defined(__unix) || defined(unix) || \
      (defined(__APPLE__) && defined(__MACH__)) || defined(__linux__)
/* max fd numbers in POSIX-compliant systems (Linux, macOS, Unix, etc.) */
#    define HAL_FD_MAX_COUNT (1024)
#  else
/* max fd numbers in other system */
#    define HAL_FD_MAX_COUNT (64)
#  endif
#endif

/* HAL_FD_SET_T storage budget (in bytes).
 *
 * On OS_XTC the HAL implementation re-uses this buffer as a native
 * `sci_fd_set` (see HAL_Socket_xtc.c). The Spreadtrum sci_fd_set struct is
 * opaque to us and we cannot include socket_types.h here (this header is
 * included by every translation unit), so we reserve a generous fixed buffer
 * to safely cover any reasonable BSD-style fd_set layout. 256 bytes covers
 * fd_count + fd_array[FD_SETSIZE] for FD_SETSIZE up to 60-something on a
 * 32-bit target, which is enough headroom for ms_code's TCPIP stack.
 *
 * On other platforms (lwIP / POSIX) HAL_FD_SET_T is a fixed-size bitmap
 * indexed by fd, sized to (HAL_FD_MAX_COUNT + 7) / 8 bytes.
 */
#define HAL_FD_SET_BITMAP_BYTES ((HAL_FD_MAX_COUNT + 7) / 8)
#define HAL_FD_SET_XTC_RESERVE (256)

#if HAL_FD_SET_BITMAP_BYTES > HAL_FD_SET_XTC_RESERVE
#  define HAL_FD_SET_STORAGE_BYTES HAL_FD_SET_BITMAP_BYTES
#else
#  define HAL_FD_SET_STORAGE_BYTES HAL_FD_SET_XTC_RESERVE
#endif

/* definition of fd operations.
 *
 * Layout note:
 *   - `placeholder` keeps its existing name + uint8_t[] type so that all
 *     existing callers (HAL_Socket_beken.c, generic bitmap helpers, etc.)
 *     compile unchanged.
 *   - On OS_XTC the same buffer is reinterpreted as a native `sci_fd_set`
 *     (see HAL_Socket_xtc.c). Because the buffer is unioned with a
 *     `uint32_t[]`, it satisfies sci_fd_set's alignment.
 */
typedef union {
  uint8_t placeholder[HAL_FD_SET_STORAGE_BYTES];
  /* alignment helper: forces 4-byte alignment for the underlying storage so
   * an overlay sci_fd_set struct (which contains `int` fields) is well
   * aligned regardless of compiler defaults. */
  uint32_t _align[HAL_FD_SET_STORAGE_BYTES / sizeof(uint32_t)];
} HAL_FD_SET_T;

/* definition of socket protocol */
typedef enum {
  PROTOCOL_TCP = 0,
  PROTOCOL_UDP = 1,
  PROTOCOL_RAW = 2,
} HAL_PROTOCOL_TYPE_E;

/* definition of transfer type */
typedef enum {
  TRANS_RECV = 0,
  TRANS_SEND = 1,
} HAL_TRANS_TYPE_E;

/* Platform-neutral flags for HAL_recv / HAL_send (subset of POSIX MSG_*).
 * HAL implementations are responsible for translating these flags into their
 * native counterparts (e.g. lwIP MSG_PEEK, sci_sock MSG_PEEK). Callers must
 * use these HAL_MSG_* constants instead of platform-specific MSG_* to keep
 * upper layers free of platform headers. */
#define HAL_MSG_NONE (0x0000)
#define HAL_MSG_PEEK (0x0001)
#define HAL_MSG_DONTWAIT (0x0002)

// 线程句柄类型定义 - 与pthread_t对齐
typedef void *ThreadHandle_t;

// 线程运行函数 - 与pthread_create函数签名对齐
typedef void *(*ThreadRunFunc)(void *arg);

struct HAL_tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
