/*
 * Format register and lookup
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#include "config_components.h"

#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avio_internal.h"
#include "avformat.h"
#include "demux.h"
#include "id3v2.h"
#include "internal.h"
#include "url.h"


/**
 * @file
 * Format register and lookup
 */

int av_match_ext(const char *filename, const char *extensions) // 检查文件名的扩展名是否匹配给定的扩展名列表
{
    const char *ext;

    if (!filename)
        return 0;

    ext = strrchr(filename, '.');
    if (ext)
        return av_match_name(ext + 1, extensions); // 拓展名匹配
    return 0;
}

int ff_match_url_ext(const char *url, const char *extensions)
{
    const char *ext;
    URLComponents uc;
    int ret;
    char scratchpad[128];

    if (!url)
        return 0;

    ret = ff_url_decompose(&uc, url, NULL);
    if (ret < 0 || !URL_COMPONENT_HAVE(uc, scheme))
        return ret;
    for (ext = uc.query; *ext != '.' && ext > uc.path; ext--)
        ;

    if (*ext != '.')
        return 0;
    if (uc.query - ext > sizeof(scratchpad))
        return AVERROR(ENOMEM); //not enough memory in our scratchpad
    av_strlcpy(scratchpad, ext + 1, uc.query - ext);

    return av_match_name(scratchpad, extensions);
}

const AVOutputFormat *av_guess_format(const char *short_name, const char *filename,
                                      const char *mime_type)
{
    const AVOutputFormat *fmt = NULL;
    const AVOutputFormat *fmt_found = NULL;
    void *i = 0;
    int score_max, score;

    /* specific test for image sequences */
#if CONFIG_IMAGE2_MUXER
    if (!short_name && filename &&
        av_filename_number_test(filename) &&
        ff_guess_image2_codec(filename) != AV_CODEC_ID_NONE) {
        return av_guess_format("image2", NULL, NULL);
    }
#endif
    /* Find the proper file type. */
    score_max = 0;
    while ((fmt = av_muxer_iterate(&i))) {
        score = 0;
        if (fmt->name && short_name && av_match_name(short_name, fmt->name))
            score += 100;
        if (fmt->mime_type && mime_type && !strcmp(fmt->mime_type, mime_type))
            score += 10;
        if (filename && fmt->extensions &&
            av_match_ext(filename, fmt->extensions)) {
            score += 5;
        }
        if (score > score_max) {
            score_max = score;
            fmt_found = fmt;
        }
    }
    return fmt_found;
}

enum AVCodecID av_guess_codec(const AVOutputFormat *fmt, const char *short_name,
                              const char *filename, const char *mime_type,
                              enum AVMediaType type)
{
    if (av_match_name("segment", fmt->name) || av_match_name("ssegment", fmt->name)) {
        const AVOutputFormat *fmt2 = av_guess_format(NULL, filename, NULL);
        if (fmt2)
            fmt = fmt2;
    }

    if (type == AVMEDIA_TYPE_VIDEO) {
        enum AVCodecID codec_id = AV_CODEC_ID_NONE;

#if CONFIG_IMAGE2_MUXER || CONFIG_IMAGE2PIPE_MUXER
        if (!strcmp(fmt->name, "image2") || !strcmp(fmt->name, "image2pipe")) {
            codec_id = ff_guess_image2_codec(filename);
        }
#endif
        if (codec_id == AV_CODEC_ID_NONE)
            codec_id = fmt->video_codec;
        return codec_id;
    } else if (type == AVMEDIA_TYPE_AUDIO)
        return fmt->audio_codec;
    else if (type == AVMEDIA_TYPE_SUBTITLE)
        return fmt->subtitle_codec;
    else
        return AV_CODEC_ID_NONE;
}

const AVInputFormat *av_find_input_format(const char *short_name)
{
    const AVInputFormat *fmt = NULL;
    void *i = 0;
    while ((fmt = av_demuxer_iterate(&i)))
        if (av_match_name(short_name, fmt->name))
            return fmt;
    return NULL;
}

const AVInputFormat *av_probe_input_format3(const AVProbeData *pd,
                                            int is_opened, int *score_ret)
{
    AVProbeData lpd = *pd; // 创建探测数据的本地副本
    const AVInputFormat *fmt1 = NULL;
    const AVInputFormat *fmt = NULL;
    int score, score_max = 0;
    void *i = 0;
    const static uint8_t zerobuffer[AVPROBE_PADDING_SIZE];
    enum nodat {
        NO_ID3,
        ID3_ALMOST_GREATER_PROBE,
        ID3_GREATER_PROBE,
        ID3_GREATER_MAX_PROBE,
    } nodat = NO_ID3; // 帮助识别和处理不同大小的 ID3 标签，这些标签通常出现在 MP3 等音频文件的开头。

    /*  如果没有提供缓冲区，使用零缓冲区 */
    if (!lpd.buf)
        lpd.buf = (unsigned char *) zerobuffer;

    /* 检查并处理ID3v2标签  */
    if (lpd.buf_size > 10 && ff_id3v2_match(lpd.buf, ID3v2_DEFAULT_MAGIC)) { // 用于检测 ID3v2 标签的。ID3v2 是一种常用于 MP3 文件中的元数据格式
        int id3len = ff_id3v2_tag_len(lpd.buf);  // 计算 ID3v2 标签的总长度,包括标签头和标签内容
        if (lpd.buf_size > id3len + 16) {
            if (lpd.buf_size < 2LL*id3len + 16)
                nodat = ID3_ALMOST_GREATER_PROBE;
            lpd.buf      += id3len; // 跳过ID3标签
            lpd.buf_size -= id3len;
        } else if (id3len >= PROBE_BUF_MAX) {
            nodat = ID3_GREATER_MAX_PROBE;
        } else
            nodat = ID3_GREATER_PROBE;
    }

    while ((fmt1 = av_demuxer_iterate(&i))) { // 用于遍历所有可用解复用器（demuxer）的函数,遍历所有可用的输入格式
        /*  跳过实验性质的格式  */
        if (fmt1->flags & AVFMT_EXPERIMENTAL)
            continue;
        /*  根据是否已打开文件和格式特性进行筛选    */
        if (!is_opened == !(fmt1->flags & AVFMT_NOFILE) && strcmp(fmt1->name, "image2"))
            continue;
        score = 0;
        /*  如果格式有探测函数，调用它  */
        if (ffifmt(fmt1)->read_probe) { // 格式探测函数 read_probe.
            score = ffifmt(fmt1)->read_probe(&lpd); // 每种文件格式（如MP3, AVI, MKV等）都有自己的 read_probe 函数.这些函数知道如何识别它们各自格式的特征
            if (score)
                av_log(NULL, AV_LOG_TRACE, "Probing %s score:%d size:%d\n", fmt1->name, score, lpd.buf_size);
            /* 根据文件扩展名和ID3标签状态调整分数  */
            if (fmt1->extensions && av_match_ext(lpd.filename, fmt1->extensions)) { // 拓展名匹配
                switch (nodat) {
                case NO_ID3:
                    score = FFMAX(score, 1);
                    break;
                case ID3_GREATER_PROBE:
                case ID3_ALMOST_GREATER_PROBE:
                    score = FFMAX(score, AVPROBE_SCORE_EXTENSION / 2 - 1);
                    break;
                case ID3_GREATER_MAX_PROBE:
                    score = FFMAX(score, AVPROBE_SCORE_EXTENSION);
                    break;
                }
            }
        } else if (fmt1->extensions) {
            /*  如果没有探测函数但有扩展名匹配，给予扩展名分数  */
            if (av_match_ext(lpd.filename, fmt1->extensions))
                score = AVPROBE_SCORE_EXTENSION;
        }

        /*  根据MIME类型调整分数    */
        if (av_match_name(lpd.mime_type, fmt1->mime_type)) {
            if (AVPROBE_SCORE_MIME > score) {
                av_log(NULL, AV_LOG_DEBUG, "Probing %s score:%d increased to %d due to MIME type\n", fmt1->name, score, AVPROBE_SCORE_MIME);
                score = AVPROBE_SCORE_MIME;
            }
        }
        /* 更新最高分和对应的格式   */
        if (score > score_max) {
            score_max = score;
            fmt       = fmt1;
        } else if (score == score_max)
            fmt = NULL; // 如果有相同的最高分，结果不确定
    }
    /* 对于某些ID3标签情况，限制最高分 */
    if (nodat == ID3_GREATER_PROBE)
        score_max = FFMIN(AVPROBE_SCORE_EXTENSION / 2 - 1, score_max);
    *score_ret = score_max; // 设置返回的分数

    return fmt; // 返回最可能的格式
}

const AVInputFormat *av_probe_input_format2(const AVProbeData *pd,
                                            int is_opened, int *score_max) // 通过综合分析文件名、内容和已知格式的特征来确定输入数据的最可能格式
{
    int score_ret;
    const AVInputFormat *fmt = av_probe_input_format3(pd, is_opened, &score_ret);
    if (score_ret > *score_max) {
        *score_max = score_ret;
        return fmt;
    } else
        return NULL;
}

const AVInputFormat *av_probe_input_format(const AVProbeData *pd, int is_opened)
{
    int score = 0;
    return av_probe_input_format2(pd, is_opened, &score);
}

int av_probe_input_buffer2(AVIOContext *pb, const AVInputFormat **fmt,
                           const char *filename, void *logctx,
                           unsigned int offset, unsigned int max_probe_size) // 探测输入数据的格式（文件类型），并且在这个过程中分配和管理缓冲区
{
    AVProbeData pd = { filename ? filename : "" }; // 检测文件格式
    uint8_t *buf = NULL;
    int ret = 0, probe_size, buf_offset = 0;
    int score = 0;
    int ret2;
    int eof = 0;

    if (!max_probe_size)
        max_probe_size = PROBE_BUF_MAX;
    else if (max_probe_size < PROBE_BUF_MIN) {
        av_log(logctx, AV_LOG_ERROR,
               "Specified probe size value %u cannot be < %u\n", max_probe_size, PROBE_BUF_MIN);
        return AVERROR(EINVAL);
    }

    if (offset >= max_probe_size)
        return AVERROR(EINVAL);

    if (pb->av_class) {
        uint8_t *mime_type_opt = NULL;
        char *semi;
        av_opt_get(pb, "mime_type", AV_OPT_SEARCH_CHILDREN, &mime_type_opt); // 于获取选项值的函数
        pd.mime_type = (const char *)mime_type_opt;
        semi = pd.mime_type ? strchr(pd.mime_type, ';') : NULL;
        if (semi) {
            *semi = '\0';
        }
    }

    for (probe_size = PROBE_BUF_MIN; probe_size <= max_probe_size && !*fmt && !eof;
         probe_size = FFMIN(probe_size << 1,
                            FFMAX(max_probe_size, probe_size + 1))) {
        score = probe_size < max_probe_size ? AVPROBE_SCORE_RETRY : 0;

        /* Read probe data. */
        if ((ret = av_reallocp(&buf, probe_size + AVPROBE_PADDING_SIZE)) < 0) // 安全地重新分配内存，并更新指向这块内存的指针
            goto fail;
        if ((ret = avio_read(pb, buf + buf_offset,
                             probe_size - buf_offset)) < 0) { // 从 AVIOContext 读取数据的函数
            /* Fail if error was not end of file, otherwise, lower score. */
            if (ret != AVERROR_EOF)
                goto fail;

            score = 0;
            ret   = 0;          /* error was end of file, nothing read */
            eof   = 1;
        }
        buf_offset += ret;
        if (buf_offset < offset)
            continue;
        pd.buf_size = buf_offset - offset;
        pd.buf = &buf[offset];

        memset(pd.buf + pd.buf_size, 0, AVPROBE_PADDING_SIZE);

        /* Guess file format. */
        *fmt = av_probe_input_format2(&pd, 1, &score); // 通过综合分析文件名、内容和已知格式的特征来确定输入数据的最可能格式
        if (*fmt) {
            /* This can only be true in the last iteration. */
            if (score <= AVPROBE_SCORE_RETRY) {
                av_log(logctx, AV_LOG_WARNING,
                       "Format %s detected only with low score of %d, "
                       "misdetection possible!\n", (*fmt)->name, score);
            } else
                av_log(logctx, AV_LOG_DEBUG,
                       "Format %s probed with size=%d and score=%d\n",
                       (*fmt)->name, probe_size, score);
#if 0
            FILE *f = fopen("probestat.tmp", "ab");
            fprintf(f, "probe_size:%d format:%s score:%d filename:%s\n", probe_size, (*fmt)->name, score, filename);
            fclose(f);
#endif
        }
    }

    if (!*fmt)
        ret = AVERROR_INVALIDDATA;

fail:
    /* Rewind. Reuse probe buffer to avoid seeking. */
    ret2 = ffio_rewind_with_probe_data(pb, &buf, buf_offset);  // FFmpeg 中用于重新定位 I/O 上下文并整合探测数据的函数
    if (ret >= 0)
        ret = ret2;

    av_freep(&pd.mime_type);
    return ret < 0 ? ret : score;
}

int av_probe_input_buffer(AVIOContext *pb, const AVInputFormat **fmt,
                          const char *filename, void *logctx,
                          unsigned int offset, unsigned int max_probe_size)
{
    int ret = av_probe_input_buffer2(pb, fmt, filename, logctx, offset, max_probe_size);
    return ret < 0 ? ret : 0;
}
