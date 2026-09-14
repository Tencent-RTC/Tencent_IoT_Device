/*****************************************************************************
 * Copyright (C) 2022 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the MIT License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://opensource.org/licenses/MIT
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#ifndef __TC_HAL_PLATFORM_INC_H__
#define __TC_HAL_PLATFORM_INC_H__

// OS dependant
// enable the specific macro in different platform

#if defined(__linux__)
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pthread.h>
#  include <semaphore.h>
#  include <sys/prctl.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#ifdef BEKEN_PLATFORM
#  include "FreeRTOS.h"
#  include "lwip/inet.h"
#  include "lwip/netdb.h"
#  include "lwip/sockets.h"
#  include "os/mem.h"
#  include "semphr.h"
#  include "task.h"
#endif

#ifdef EC718_PLATFORM
#  include "FreeRTOS.h"
#  include "lwip/inet.h"
#  include "lwip/netdb.h"
#  include "lwip/sockets.h"
#  include "semphr.h"
#  include "task.h"
#endif

#ifdef ML307N_PLATFORM
// ML307N (ict2110, OneOS + CMSIS-RTOS2 + lwIP + mbedtls 2.28)
// 宏 ML307N_PLATFORM 由 ml307n 固件 xmake 构建传入(add_defines)
#  include "lwip/inet.h"
#  include "lwip/netdb.h"
#  include "lwip/sockets.h"
#  include "os.h"
#endif

#ifdef ESP_PLATFORM
#  include <arpa/inet.h>
#  include <errno.h>
#  include <esp_log.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include "freertos/FreeRTOS.h"
#  include "freertos/semphr.h"
#  include "freertos/task.h"
#endif

#ifdef JIELI_RTOS
#  include "lwip/inet.h"
#  include "lwip/netdb.h"
#  include "lwip/sockets.h"
#  include "os_api.h"
#endif

#ifdef PLATFORM_RTTHREAD
#  include <dfs_posix.h>
#  include <rtconfig.h>
#  include <rtthread.h>

#endif

#ifdef PLATFORM_ALLWINNER
#  include <fcntl.h>
#  include <lwip/inet.h>
#  include <lwip/netdb.h>
#  include <lwip/sockets.h>
#  include <rtconfig.h>
#  include <rtthread.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#ifdef PLAT_USE_THREADX
// HAL for ASR3603 platform which is based on ThreadX
#  include "lwip/netdb.h"
#  include "lwip/netif.h"
#  include "lwip/sockets.h"
#  include "osa.h"
#  include "osa_mem.h"
#  include "tx_api.h"

typedef int ssize_t;
#endif

#ifdef XRADIO_PLATFORM
#  include "FreeRTOS.h"
#  include "lwip/inet.h"
#  include "lwip/netdb.h"
#  include "lwip/netif.h"
#  include "lwip/sockets.h"
#  include "semphr.h"
#  include "task.h"
#endif

#ifdef PLATFORM_HAS_CMSIS
#  include "cmsis_os.h"
#endif

#ifdef OS_ANDROID
#  include <android/log.h>
#endif

#ifdef WIN32
#  include <Windows.h>
#  include <direct.h>
#  include <limits.h>
#  define getcwd(buffer, len) _getcwd(buffer, len)
typedef unsigned long ssize_t;
#endif

#ifdef __APPLE__
#  include <netinet/in.h>
#  include <sys/time.h>
#endif

#if defined(OS_XTC)
#  include <stdint.h>

static inline uint16_t _xtc_hal_swap16(uint16_t v) {
  return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}
static inline uint32_t _xtc_hal_swap32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
         ((v & 0xFF000000u) >> 24);
}
static inline uint16_t _xtc_hal_htons(uint16_t v) {
  uint16_t one = 1;
  return (*(const uint8_t *)&one == 0) ? v : _xtc_hal_swap16(v);
}
static inline uint32_t _xtc_hal_htonl(uint32_t v) {
  uint16_t one = 1;
  return (*(const uint8_t *)&one == 0) ? v : _xtc_hal_swap32(v);
}

#  undef htons
#  undef ntohs
#  undef htonl
#  undef ntohl
#  define htons(v) _xtc_hal_htons((uint16_t)(v))
#  define ntohs(v) _xtc_hal_htons((uint16_t)(v))
#  define htonl(v) _xtc_hal_htonl((uint32_t)(v))
#  define ntohl(v) _xtc_hal_htonl((uint32_t)(v))
#endif /* OS_XTC */

#endif /* __TC_HAL_PLATFORM_INC_H__ */
