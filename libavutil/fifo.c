/*
 * a very simple circular buffer FIFO implementation
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 * Copyright (c) 2006 Roman Shaposhnik
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

#include <stdint.h>
#include <string.h>

#include "avassert.h"
#include "error.h"
#include "fifo.h"
#include "macros.h"
#include "mem.h"

// by default the FIFO can be auto-grown to 1MB
#define AUTO_GROW_DEFAULT_BYTES (1024 * 1024)

struct AVFifo {
    uint8_t *buffer;                    // 缓冲区

    size_t elem_size, nb_elems;         // 每个元素的大小、元素数量
    size_t offset_r, offset_w;          // 读偏移、写偏移. 循环缓冲区的双索引，非指针，是 int 偏移量 + buf_size 更容易确定读、写边界
    // distinguishes the ambiguous situation offset_r == offset_w
    int    is_empty;                    // 区分 offset_r == offset_w 的歧义

    unsigned int flags;                 // 标志位
    size_t       auto_grow_limit;       // 自动增长的限制
};

AVFifo *av_fifo_alloc2(size_t nb_elems, size_t elem_size,
                       unsigned int flags) // 分配和初始化一个 AVFifo（先进先出队列）结构
{
    AVFifo *f; // 主要用于数据缓冲和管理
    void *buffer = NULL;

    if (!elem_size)
        return NULL;

    if (nb_elems) {
        buffer = av_realloc_array(NULL, nb_elems, elem_size); // 检测是否溢出,正常则分配内存。给实际数据位置分配内存
        if (!buffer)
            return NULL;
    }
    f = av_mallocz(sizeof(*f)); // 为 fifo 分配内存，*buffer + 其他信息的数据结构, *buffer指向刚分配的内存空间
    if (!f) {
        av_free(buffer); // 释放缓冲区
        return NULL;
    }
    /*  初始化 AVFifo 结构 */
    f->buffer    = buffer;
    f->nb_elems  = nb_elems;
    f->elem_size = elem_size;
    f->is_empty  = 1;
    /**
     * @note FFMAX(AUTO_GROW_DEFAULT_BYTES / elem_size, 1)
     * @li   防止 AUTO_GROW_DEFAULT_BYTES/elem_size 为0
     * @li   确保至少能存储一个元素
     */
    f->flags           = flags;
    f->auto_grow_limit = FFMAX(AUTO_GROW_DEFAULT_BYTES / elem_size, 1); // 支持自动增长功能，通过 auto_grow_limit 控制

    return f;
}

void av_fifo_auto_grow_limit(AVFifo *f, size_t max_elems)
{
    f->auto_grow_limit = max_elems;
}

size_t av_fifo_elem_size(const AVFifo *f)
{
    return f->elem_size;
}

size_t av_fifo_can_read(const AVFifo *f)
{
    if (f->offset_w <= f->offset_r && !f->is_empty)
        return f->nb_elems - f->offset_r + f->offset_w;
    return f->offset_w - f->offset_r;
}

size_t av_fifo_can_write(const AVFifo *f)
{
    return f->nb_elems - av_fifo_can_read(f);
}

int av_fifo_grow2(AVFifo *f, size_t inc)
{
    uint8_t *tmp;

    /**
     * @brief 检查增长大小是否会导致溢出
     * @param SIZE_MAX 是 size_t 类型能表示的最大值
     * @note 如果 inc + f->nb_elems 超过 SIZE_MAX，返回参数无效错误
     */
    if (inc > SIZE_MAX - f->nb_elems)
        return AVERROR(EINVAL);

    /**
     * @brief 重新分配内存
     * @note 新大小 = (现有元素数 + 增长元素数) * 每个元素的大小
     */
    tmp = av_realloc_array(f->buffer, f->nb_elems + inc, f->elem_size);
    if (!tmp)
        return AVERROR(ENOMEM);
    f->buffer = tmp;

    // move the data from the beginning of the ring buffer
    // to the newly allocated space
    /**
     * @note 处理环形缓冲区的数据重排,当写指针在读指针左边且缓冲区非空时，需要重新排列数据
     */
    if (f->offset_w <= f->offset_r && !f->is_empty) {
        // 确定需要复制的数据量，取增长量和写指针位置的较小值
        const size_t copy = FFMIN(inc, f->offset_w);
        memcpy(tmp + f->nb_elems * f->elem_size, tmp, copy * f->elem_size);
        // 如果写指针位置大于实际复制量，需要移动剩余数据
        if (copy < f->offset_w) {
            memmove(tmp, tmp + copy * f->elem_size,
                    (f->offset_w - copy) * f->elem_size);
            f->offset_w -= copy;    // 更新写指针位置
        } else
            // 如果复制量等于增长量，写指针回到开始位置
            // 否则写指针指向新空间中复制数据的末尾
            f->offset_w = copy == inc ? 0 : f->nb_elems + copy;
    }

    f->nb_elems += inc; // 更新缓冲区的总元素数

    return 0;
}

static int fifo_check_space(AVFifo *f, size_t to_write)
{
    const size_t can_write = av_fifo_can_write(f);
    const size_t need_grow = to_write > can_write ? to_write - can_write : 0;
    size_t can_grow;

    if (!need_grow)
        return 0;

    can_grow = f->auto_grow_limit > f->nb_elems ?
               f->auto_grow_limit - f->nb_elems : 0;
    if ((f->flags & AV_FIFO_FLAG_AUTO_GROW) && need_grow <= can_grow) {
        // allocate a bit more than necessary, if we can
        const size_t inc = (need_grow < can_grow / 2 ) ? need_grow * 2 : can_grow;
        return av_fifo_grow2(f, inc);
    }

    return AVERROR(ENOSPC);
}

static int fifo_write_common(AVFifo *f, const uint8_t *buf, size_t *nb_elems,
                             AVFifoCB read_cb, void *opaque)
{
    size_t to_write = *nb_elems;
    size_t offset_w;
    int         ret = 0;

    ret = fifo_check_space(f, to_write);
    if (ret < 0)
        return ret;

    offset_w = f->offset_w;

    while (to_write > 0) {
        size_t    len = FFMIN(f->nb_elems - offset_w, to_write);
        uint8_t *wptr = f->buffer + offset_w * f->elem_size;

        if (read_cb) {
            ret = read_cb(opaque, wptr, &len);
            if (ret < 0 || len == 0)
                break;
        } else {
            memcpy(wptr, buf, len * f->elem_size);
            buf += len * f->elem_size;
        }
        offset_w += len;
        if (offset_w >= f->nb_elems)
            offset_w = 0;
        to_write -= len;
    }
    f->offset_w = offset_w;

    if (*nb_elems != to_write)
        f->is_empty = 0;
    *nb_elems -= to_write;

    return ret;
}

int av_fifo_write(AVFifo *f, const void *buf, size_t nb_elems)
{
    return fifo_write_common(f, buf, &nb_elems, NULL, NULL);
}

int av_fifo_write_from_cb(AVFifo *f, AVFifoCB read_cb,
                          void *opaque, size_t *nb_elems)
{
    return fifo_write_common(f, NULL, nb_elems, read_cb, opaque);
}

static int fifo_peek_common(const AVFifo *f, uint8_t *buf, size_t *nb_elems,
                            size_t offset, AVFifoCB write_cb, void *opaque)
{
    size_t  to_read = *nb_elems;
    size_t offset_r = f->offset_r;
    size_t can_read = av_fifo_can_read(f);
    int         ret = 0;

    if (offset > can_read || to_read > can_read - offset) {
        *nb_elems = 0;
        return AVERROR(EINVAL);
    }

    if (offset_r >= f->nb_elems - offset)
        offset_r -= f->nb_elems - offset;
    else
        offset_r += offset;

    while (to_read > 0) {
        size_t    len = FFMIN(f->nb_elems - offset_r, to_read);
        uint8_t *rptr = f->buffer + offset_r * f->elem_size;

        if (write_cb) {
            ret = write_cb(opaque, rptr, &len);
            if (ret < 0 || len == 0)
                break;
        } else {
            memcpy(buf, rptr, len * f->elem_size);
            buf += len * f->elem_size;
        }
        offset_r += len;
        if (offset_r >= f->nb_elems)
            offset_r = 0;
        to_read -= len;
    }

    *nb_elems -= to_read;

    return ret;
}

int av_fifo_read(AVFifo *f, void *buf, size_t nb_elems)
{
    int ret = fifo_peek_common(f, buf, &nb_elems, 0, NULL, NULL);
    av_fifo_drain2(f, nb_elems);
    return ret;
}

int av_fifo_read_to_cb(AVFifo *f, AVFifoCB write_cb,
                       void *opaque, size_t *nb_elems)
{
    int ret = fifo_peek_common(f, NULL, nb_elems, 0, write_cb, opaque);
    av_fifo_drain2(f, *nb_elems);
    return ret;
}

int av_fifo_peek(const AVFifo *f, void *buf, size_t nb_elems, size_t offset)
{
    return fifo_peek_common(f, buf, &nb_elems, offset, NULL, NULL);
}

int av_fifo_peek_to_cb(const AVFifo *f, AVFifoCB write_cb, void *opaque,
                       size_t *nb_elems, size_t offset)
{
    return fifo_peek_common(f, NULL, nb_elems, offset, write_cb, opaque);
}

void av_fifo_drain2(AVFifo *f, size_t size)
{
    const size_t cur_size = av_fifo_can_read(f);

    av_assert0(cur_size >= size);
    if (cur_size == size)
        f->is_empty = 1;

    if (f->offset_r >= f->nb_elems - size)
        f->offset_r -= f->nb_elems - size;
    else
        f->offset_r += size;
}

void av_fifo_reset2(AVFifo *f)
{
    f->offset_r = f->offset_w = 0;
    f->is_empty = 1;
}

void av_fifo_freep2(AVFifo **f)
{
    if (*f) {
        av_freep(&(*f)->buffer);
        av_freep(f);
    }
}
