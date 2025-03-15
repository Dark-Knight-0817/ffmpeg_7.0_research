/*
 * Copyright (c) 2001 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file libavcodec encoding audio API usage examples
 * @example encode_audio.c
 *
 * Generate a synthetic audio signal and encode it to an output MP2 file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>

#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

/* check that a given sample format is supported by the encoder */
/**
 * 检查指定的采样格式是否被编码器支持
 */
static int check_sample_fmt(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* just pick the highest supported samplerate */
/**
 * 检查指定的采样格式是否被编码器支持
 */
static int select_sample_rate(const AVCodec *codec)
{
    const int *p;
    int best_samplerate = 0;

    if (!codec->supported_samplerates)
        return 44100;

    p = codec->supported_samplerates;
    // 找到最接近44100的采样率
    while (*p) {
        if (!best_samplerate || abs(44100 - *p) < abs(44100 - best_samplerate))
            best_samplerate = *p;
        p++;
    }
    return best_samplerate;
}

/* select layout with the highest channel count */
/**
 * 选择通道布局
 * 选择支持最多通道数的布局
 */
static int select_channel_layout(const AVCodec *codec, AVChannelLayout *dst)
{
    const AVChannelLayout *p, *best_ch_layout;
    int best_nb_channels   = 0;
    // 默认使用立体声
    if (!codec->ch_layouts)
        return av_channel_layout_copy(dst, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

    p = codec->ch_layouts;
    // 选择通道数最多的布局
    while (p->nb_channels) {
        int nb_channels = p->nb_channels;

        if (nb_channels > best_nb_channels) {
            best_ch_layout   = p;
            best_nb_channels = nb_channels;
        }
        p++;
    }
    return av_channel_layout_copy(dst, best_ch_layout);
}

/**
 * 编码并写入文件
 */
static void encode(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *output)
{
    int ret;

    /* send the frame for encoding */
    // 1. 发送帧到编码器
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending the frame to the encoder\n");
        exit(1);
    }

    /* read all the available output packets (in general there may be any
     * number of them */
    // 2. 获取所有编码后的包
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding audio frame\n");
            exit(1);
        }
        // 3. 写入文件
        fwrite(pkt->data, 1, pkt->size, output);
        av_packet_unref(pkt);
    }
}

int main(int argc, char **argv)
{
    const char *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVFrame *frame;
    AVPacket *pkt;
    int i, j, k, ret;
    FILE *f;
    uint16_t *samples;
    float t, tincr;

    if (argc <= 1) {
        fprintf(stderr, "Usage: %s <output file>\n", argv[0]);
        return 0;
    }
    filename = argv[1];

    /* find the MP2 encoder */
    // 1. 查找MP2编码器
    codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    // 2. 创建编码器上下文
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

    /* put sample parameters */
    // 3. 设置编码参数
    c->bit_rate = 64000;                                    // 码率

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;                      // 采样格式
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        fprintf(stderr, "Encoder does not support sample format %s",
                av_get_sample_fmt_name(c->sample_fmt));
        exit(1);
    }

    /* select other audio parameters supported by the encoder */
    c->sample_rate    = select_sample_rate(codec);          // 采样率
    ret = select_channel_layout(codec, &c->ch_layout);      // 通道布局
    if (ret < 0)
        exit(1);

    /* open it */
    // 4. 打开编码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    /* packet for holding encoded output */
    // 5. 创建帧和包
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "could not allocate the packet\n");
        exit(1);
    }

    /* frame containing input raw audio */
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate audio frame\n");
        exit(1);
    }
    // 6. 设置帧参数
    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);
    if (ret < 0)
        exit(1);

    /* allocate the data buffers */
    // 7. 分配帧缓冲区
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate audio data buffers\n");
        exit(1);
    }

    /* encode a single tone sound */
    // 8. 生成正弦波并编码
    t = 0;
    tincr = 2 * M_PI * 440.0 / c->sample_rate;  // 440Hz正弦波
    for (i = 0; i < 200; i++) {
        /* make sure the frame is writable -- makes a copy if the encoder
         * kept a reference internally */
        // 确保帧可写
        ret = av_frame_make_writable(frame);
        if (ret < 0)
            exit(1);
        samples = (uint16_t*)frame->data[0];
        // 生成正弦波
        for (j = 0; j < c->frame_size; j++) {
            samples[2*j] = (int)(sin(t) * 10000);
            // 复制到其他通道
            for (k = 1; k < c->ch_layout.nb_channels; k++)
                samples[2*j + k] = samples[2*j];
            t += tincr;
        }
        // 编码帧
        encode(c, frame, pkt, f);
    }

    /* flush the encoder */
    // 9. 冲洗编码器
    encode(c, NULL, pkt, f);

    fclose(f);

    // 10. 清理资源
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);

    return 0;
}
