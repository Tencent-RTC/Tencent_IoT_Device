/**
 * @file tc_hal_mutex.h
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

#ifndef __TC_HAL_MUTEX_H__
#define __TC_HAL_MUTEX_H__

#ifdef __cplusplus
extern "C" {
#endif

struct _HAL_Mutex;
typedef struct _HAL_Mutex HAL_Mutex;

struct _HAL_Condition;
typedef struct _HAL_Condition HAL_Condition;

/**
 * @brief 创建互斥锁
 *
 * 此函数用于创建一个新的互斥锁。互斥锁是一种同步机制，用于防止多个线程同时访问共享资源，
 * 从而避免竞态条件和数据损坏。
 *
 * @return 成功时返回一个有效的互斥锁句柄，失败时返回NULL。
 */
HAL_Mutex *HAL_MutexCreate(void);

/**
 * @brief 销毁互斥锁
 *
 * @param mutex 锁句柄
 */
void HAL_MutexDestroy(HAL_Mutex *mutex);

/**
 * @brief 获取锁，如果无法获取锁，则会阻塞当前线程直到锁可用。
 *
 * 此函数尝试获取一个互斥锁。如果锁已经被其他线程持有，
 * 调用此函数的线程将会进入阻塞状态，直到锁被释放。
 *
 * @param mutex 指向要获取的互斥锁的指针。
 * @return 成功获取锁返回0，失败返回错误码。
 */
int HAL_MutexLock(HAL_Mutex *mutex);

/**
 * @brief 尝试获取锁，如果无法立即获取则立即返回
 *
 * 此函数尝试获取一个互斥锁。如果锁当前可用，则获取锁并返回成功状态码（0）。
 * 如果锁不可用，函数不会阻塞等待，而是立即返回一个错误代码。
 *
 * @param mutex     互斥锁句柄
 * @return 0 表示成功，或错误代码表示失败
 */
int HAL_MutexTryLock(HAL_Mutex *mutex);

/**
 * @brief 解锁
 *
 * 此函数用于解锁一个之前已经被锁定的互斥量（mutex）。
 * 互斥量是一种同步机制，用于防止多个线程同时访问共享资源。
 *
 * @param mutex     mutex handle
 *                  指向要解锁的互斥量的指针。
 */
int HAL_MutexUnlock(HAL_Mutex *mutex);

/**
 * @brief 创建递归互斥锁
 *
 * 此函数用于创建一个递归互斥锁。递归互斥锁允许同一线程多次锁定同一个互斥锁，
 * 而不会导致死锁。当成功创建时，返回一个有效的递归互斥锁句柄；如果创建失败，则返回NULL。
 *
 * @return 成功时返回一个有效的互斥锁句柄，失败时返回NULL。
 */
void *HAL_RecursiveMutexCreate(void);

/**
 * @brief 销毁递归互斥锁
 *
 * @param mutex 指向要销毁的互斥量的指针。
 */
void HAL_RecursiveMutexDestroy(void *mutex);

/**
 * @brief 获取锁，如果无法获取锁，则会阻塞当前线程直到锁可用。
 *
 * 此函数尝试获取一个互斥锁。如果锁已经被其他线程持有，
 * 调用此函数的线程将会进入阻塞状态，直到锁被释放。
 *
 * @param mutex 指向要获取的互斥锁的指针。
 * @return 成功获取锁返回0，失败返回错误码。
 */
int HAL_RecursiveMutexLock(void *mutex);

/**
 * @brief 尝试获取锁，如果无法立即获取则立即返回
 *
 * 此函数尝试获取一个互斥锁。如果锁当前可用，则获取锁并返回成功状态码（0）。
 * 如果锁不可用，函数不会阻塞等待，而是立即返回一个错误代码。
 *
 * @param mutex     互斥锁句柄
 * @return 0 表示成功，或错误代码表示失败
 */
int HAL_RecursiveMutexTryLock(void *mutex);

/**
 * @brief 释放锁
 *
 * @param mutex  指向要解锁的互斥量的指针。
 */
int HAL_RecursiveMutexUnLock(void *mutex);

/**
 * @brief 创建条件变量
 *
 * @return 成功时返回一个有效的条件变量句柄，失败时返回NULL。
 */
HAL_Condition *HAL_CondCreate(void);

/**
 * @brief 销毁条件变量
 *
 * @param cond 指向要释放的条件变量句柄
 */
void HAL_CondFree(HAL_Condition *cond);

/**
 * @brief 通知条件变量
 *
 * 该函数用于通知等待在条件变量上的一个线程。
 *
 * @param cond 指向条件变量的指针。条件变量用于同步线程间的操作。
 * @return 返回值表示操作的成功与否，通常成功返回0，失败返回负数。
 */
int HAL_CondSignal(HAL_Condition *cond);

/**
 * @brief 广播条件变量
 *
 * 该函数用于通知等待在条件变量上的所有线程。
 *
 * @param cond 指向条件变量的指针。条件变量用于同步线程间的操作。
 * @return 返回值表示操作的成功与否，通常成功返回0，失败返回负数。
 */
int HAL_CondBroadcast(HAL_Condition *cond);

/**
 * @brief 等待条件变量触发
 *
 * @param cond    条件变量的句柄
 * @param lock    锁变量的句柄。在调用条件变量之前，线程通常需要先获得锁，以确保对共享资源的访问是互斥的。HAL_CondWait
 * 函数在等待期间会释放这个锁，并在被唤醒后重新获得它。
 */
int HAL_CondWait(HAL_Condition *cond, HAL_Mutex *lock);

/**
 * @brief 带超时等待条件变量触发
 *
 * @param cond    条件变量的句柄
 * @param lock    锁变量的句柄
 * @param timeout_ms 等待的超时时间，以毫秒为单位。
 */
int HAL_CondTimedWait(HAL_Condition *cond, HAL_Mutex *lock, unsigned long timeout_ms);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
