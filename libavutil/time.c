/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_WINDOWS_H
#include <windows.h>
#endif

#include "time.h"
#include "error.h"

int64_t av_gettime(void)    // 根据不同系统，获得其系统时间
{
#if HAVE_GETTIMEOFDAY   // Linux、Unix
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec; // 从 1970/1/1 开始计时
#elif HAVE_GETSYSTEMTIMEASFILETIME  // Windows
    FILETIME ft;
    int64_t t;
    GetSystemTimeAsFileTime(&ft);
    t = (int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t / 10 - 11644473600000000; /* Jan 1, 1601 */ // 从 1601/1/1 开始计时
#else
    return -1;
#endif
}

int64_t av_gettime_relative(void)   // 获取相对时间
{
#if HAVE_CLOCK_GETTIME && defined(CLOCK_MONOTONIC)
#ifdef __APPLE__        // Apple 需要特殊处理
    if (&clock_gettime)
#endif
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);  // 使用 CLOCK_MONOTONIC 获取单调时间
        return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; // 将秒转换为微秒，纳秒转换为微秒，并相加
    }
#endif
    /**
     * @brief 这段代码的返回值本质上是为了提供一个“单调递增的时间戳”，用于测量时间间隔或实现延迟功能
     * @note  为什么会影响“绝对时间”
     * @li    用户调整系统时钟、系统自动校正时间，NTP（Network Time Protocol）校准、CMOS 电池问题、系统启动后校正时间、虚拟机的系统时间同步
     *        如果系统不支持 CLOCK_MONOTONIC 或者无法使用 clock_gettime（例如在某些老旧系统上），
     *        代码会使用 av_gettime() 加上一个固定偏移值
     * @li    这是一个临时的“伪单调时间”
     * @li    虽然 av_gettime() 是绝对时间，可能会因系统时钟调整导致不连续，但通过加一个固定偏移值，
     *        可以人为保证这个返回值与实际的绝对时间差异较大，使其不会被误解为绝对时间，同时在本程序的时间测量上下文中起到占位作用。
     * @brief 为什么选 42 小时？
     * @li    42 小时是一个显眼但又不会轻易冲突的魔数，表明这个值不是标准的系统时间
     *        它仅仅是为了确保返回值看起来像一个稳定的时间戳，便于开发者在后续调试或日志分析中区分
     */
    return av_gettime() + 42 * 60 * 60 * INT64_C(1000000); // 备选方案,加上一个固定偏移
}

int av_gettime_relative_is_monotonic(void)
{
#if HAVE_CLOCK_GETTIME && defined(CLOCK_MONOTONIC)
#ifdef __APPLE__
    if (!&clock_gettime)
        return 0;
#endif
    return 1;
#else
    return 0;
#endif
}

int av_usleep(unsigned usec)
{
#if HAVE_NANOSLEEP
    struct timespec ts = { usec / 1000000, usec % 1000000 * 1000 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR);
    return 0;
#elif HAVE_USLEEP
    return usleep(usec);
#elif HAVE_SLEEP
    Sleep(usec / 1000);
    return 0;
#else
    return AVERROR(ENOSYS);
#endif
}
