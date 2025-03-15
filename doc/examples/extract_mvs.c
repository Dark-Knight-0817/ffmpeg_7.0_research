/*
 * Copyright (c) 2012 Stefano Sabatini
 * Copyright (c) 2014 Clément Bœsch
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
 * @file libavcodec motion vectors extraction API usage example
 * @example extract_mvs.c
 *
 * Read from input file, decode video stream and print a motion vectors
 * representation to stdout.
 */

#include <libavutil/motion_vector.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL;
static AVStream *video_stream = NULL;
static const char *src_filename = NULL;

static int video_stream_idx = -1;
static AVFrame *frame = NULL;
static int video_frame_count = 0;

/**
 * 解码数据包并提取运动向量
 */
static int decode_packet(const AVPacket *pkt)
{
    // 1. 发送包到解码器
    int ret = avcodec_send_packet(video_dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error while sending a packet to the decoder: %s\n", av_err2str(ret));
        return ret;
    }
    // 2. 接收解码后的帧
    while (ret >= 0)  {
        ret = avcodec_receive_frame(video_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error while receiving a frame from the decoder: %s\n", av_err2str(ret));
            return ret;
        }

        if (ret >= 0) {
            int i;
            AVFrameSideData *sd;

            video_frame_count++;
            // 3. 获取运动向量侧数据
            sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
            if (sd) {
                 // 4. 遍历所有运动向量
                const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
                for (i = 0; i < sd->size / sizeof(*mvs); i++) {
                    const AVMotionVector *mv = &mvs[i];
                    // 打印运动向量信息：
                    // 帧号,源块,宽度,高度,源坐标,目标坐标,标志,运动向量,比例
                    printf("%d,%2d,%2d,%2d,%4d,%4d,%4d,%4d,0x%"PRIx64",%4d,%4d,%4d\n",
                        video_frame_count, mv->source,          // 帧号,源块类型
                        mv->w, mv->h, mv->src_x, mv->src_y,     // 块大小、源坐标
                        mv->dst_x, mv->dst_y, mv->flags,        // 目标坐标、标志
                        mv->motion_x, mv->motion_y, mv->motion_scale); // x方向运动、y方向运动、运动比例
                }
            }
            av_frame_unref(frame);
        }
    }

    return 0;
}

/**
 * 打开解码器并配置运动向量导出
 */
static int open_codec_context(AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret;
    AVStream *st;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    // 1. 查找最佳流
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, &dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        int stream_idx = ret;
        st = fmt_ctx->streams[stream_idx];
        // 2. 分配解码器上下文
        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            fprintf(stderr, "Failed to allocate codec\n");
            return AVERROR(EINVAL);
        }
        // 3. 复制编解码参数
        ret = avcodec_parameters_to_context(dec_ctx, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters to codec context\n");
            return ret;
        }

        /* Init the video decoder */
        // 4. 设置运动向量导出标志
        av_dict_set(&opts, "flags2", "+export_mvs", 0);
        // 5. 打开解码器
        ret = avcodec_open2(dec_ctx, dec, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }

        video_stream_idx = stream_idx;
        video_stream = fmt_ctx->streams[video_stream_idx];
        video_dec_ctx = dec_ctx;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;
    AVPacket *pkt = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <video>\n", argv[0]);
        exit(1);
    }
    src_filename = argv[1];
    // 1. 打开输入文件
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }
    // 2. 获取流信息
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }
    // 3. 打开视频解码器
    open_codec_context(fmt_ctx, AVMEDIA_TYPE_VIDEO);

    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!video_stream) {
        fprintf(stderr, "Could not find video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }
    // 4. 分配帧和包缓冲
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    // 5. 打印CSV表头
    printf("framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale\n");

    /* read frames from the file */
    // 6. 读取和解码视频帧
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* flush cached frames */
    // 7. 刷新解码器
    decode_packet(NULL);

end:
    avcodec_free_context(&video_dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0;
}
