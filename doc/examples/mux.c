/*
 * Copyright (c) 2003 Fabrice Bellard
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
 * @file libavformat muxing API usage example
 * FFmpeg 复用(muxing)示例
 * @example mux.c
 *
 * Generate a synthetic audio and video signal and mux them to a media file in
 * any supported libavformat format. The default codecs are used.
 * 主要功能：生成合成的音频和视频流并将它们复用到一个媒体文件中
 */

// 1. 基本参数定义
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define STREAM_DURATION   10.0               // 流的持续时间(秒)
#define STREAM_FRAME_RATE 25                 /* 25 images/s 视频帧率(fps) */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt 像素格式 */

#define SCALE_FLAGS SWS_BICUBIC              // 图像缩放算法

// a wrapper around a single output AVStream
/**
 * 输出流包装结构体
 * 用于管理单个输出流(音频或视频)的所有相关组件
 */
typedef struct OutputStream {
    AVStream *st;           // 流对象
    AVCodecContext *enc;    // 编码器上下文

    /* pts of the next frame that will be generated */
    int64_t next_pts;       // 下一个帧的pts
    int samples_count;      // 音频采样帧计数

    AVFrame *frame;         // 编码帧
    AVFrame *tmp_frame;     // 临时帧(用于像素格式转换)

    AVPacket *tmp_pkt;      // 临时包(用于存储编码后的数据

    float t, tincr, tincr2; // 用于生成音频样本

    struct SwsContext *sws_ctx; // 图像格式转换
    struct SwrContext *swr_ctx; // 音频重采样
} OutputStream;

/**
 * 记录包信息的函数
 * 打印出 pts, dts 等包相关的时间戳信息
 */
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

/**
 * 写入一帧数据的核心函数
 * 完成从原始帧到编码包的转换和写入
 */
static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt)
{
    int ret;

    // send the frame to the encoder
    // 1. 发送帧到编码器
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame to the encoder: %s\n",
                av_err2str(ret));
        exit(1);
    }

    // 2. 循环从编码器接收包
    while (ret >= 0) {
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        // 3. 时间基转换：从编码器时间基转到流时间基
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        /* Write the compressed frame to the media file. */
        // 4. 写入复用器
        log_packet(fmt_ctx, pkt);
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    return ret == AVERROR_EOF ? 1 : 0;
}

/* Add an output stream. */
/**
 * 添加输出流
 * 配置编码器和流参数
 */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec,
                       enum AVCodecID codec_id)
{
    AVCodecContext *c;
    int i;

    /* find the encoder */
    // 1. 查找编码器
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        exit(1);
    }

    // 2. 分配包和流对象
    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL); // 创建流
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1; // 设置流ID.  nb_streams 是在调用 avformat_new_stream() 时自动增加的

    // 3. 分配编码器上下文
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;

    // 4. 根据媒体类型设置参数
    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:    
        // 设置音频参数
        c->sample_fmt  = (*codec)->sample_fmts ?
            (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP; // 采样格式
        c->bit_rate    = 64000; // 比特率——单位时间内传输或处理的比特数
        c->sample_rate = 44100; // 采样率——每秒对模拟信号采样的次数
        /**
         * 优先选择 44100Hz 采样率（CD音质的标准采样率）
         * 如果不支持 44100Hz，就使用编码器支持的第一个采样率
         */
        if ((*codec)->supported_samplerates) { // 首先检查编码器是否有支持的采样率列表
            c->sample_rate = (*codec)->supported_samplerates[0]; // 默认设置为列表中的第一个采样率
            for (i = 0; (*codec)->supported_samplerates[i]; i++) { // 然后遍历所有支持的采样率
                if ((*codec)->supported_samplerates[i] == 44100) // 如果找到44100Hz，就使用它
                    c->sample_rate = 44100;
            }
        }
        av_channel_layout_copy(&c->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO); // 双声道（立体声）
        ost->st->time_base = (AVRational){ 1, c->sample_rate }; // AVRational 是一个（分数）结构体，用于表示有理数，这里表示1秒有 c->sample_rate 个时间单位，即采样率
        break;

    case AVMEDIA_TYPE_VIDEO:
        // 设置视频参数
        c->codec_id = codec_id;

        c->bit_rate = 400000;   // 比特率
        /* Resolution must be a multiple of two. */
        c->width    = 352;       // 宽度
        c->height   = 288;       // 高度    
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };  // 时间基
        c->time_base       = ost->st->time_base;                    

        c->gop_size      = 12; /* emit one intra frame every twelve frames at most . 每12帧会有一个 I 帧*/
        c->pix_fmt       = STREAM_PIX_FMT;              // 像素格式. 这里是 YUV420P
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {    // 如果是 MPEG-2 视频编码
            /* just for testing, we also add B-frames */
            /**
             * @param max_b_frames 控制了连续 B 帧的最大数量
             * @note 编码延迟、压缩效率、内存使用、解码复杂度
             */
            c->max_b_frames = 2;    
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {    // 如果是 MPEG-1 视频编码
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            /**
             * c->mb_decision = 2 设置为率失真优化模式,因为:
             * MPEG-1 的色度平面运动与亮度平面不匹配
             * 可能导致某些宏块的系数溢出
             * RDO模式可以更好地处理这种情况
             * @param mb_decision 宏块(macroblock)决策模式的参数，控制编码器如何选择宏块的编码方式
             */
            c->mb_decision = 2;
        }
        break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    // 5. 处理全局头部标志
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* audio output */
/**
 * 分配音频帧
 * @param sample_fmt      采样格式
 * @param channel_layout  通道布局
 * @param sample_rate    采样率
 * @param nb_samples     采样数
 */
static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  const AVChannelLayout *channel_layout,
                                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    // 设置音频帧的基本参数
    frame->format = sample_fmt;                                 // 采样格式
    av_channel_layout_copy(&frame->ch_layout, channel_layout);  // 通道布局
    frame->sample_rate = sample_rate;                           // 采样率
    frame->nb_samples = nb_samples;                             // 每帧的采样数

    // 如果指定了采样数，为音频数据分配实际的缓冲区
    if (nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

/**
 * 打开音频编码器并初始化音频相关的结构
 */
static void open_audio(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    // 1. 打开音频编码器
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* init signal generator */
    // 2. 初始化音频信号生成器参数
    ost->t     = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

    // 3. 确定每帧的采样数
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;         // 可变帧大小
    else
        nb_samples = c->frame_size; // 固定帧大小

    // 4. 分配音频帧
    ost->frame     = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
                                       c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    // 5. 复制编码器参数到流
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    // 6. 初始化重采样器
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        exit(1);
    }

    /* set options */
    // 设置重采样器参数
    av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout",       &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
    av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout",      &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

    /* initialize the resampling context */
    // 初始化重采样器
    if ((ret = swr_init(ost->swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(1);
    }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
/**
 * 获取音频帧
 * 生成一个正弦波形的合成音频
 */
static AVFrame *get_audio_frame(OutputStream *ost)
{
    AVFrame *frame = ost->tmp_frame;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];

    /* check if we want to generate more frames */
    // 检查是否需要生成更多帧
    if (av_compare_ts(ost->next_pts, ost->enc->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    // 生成正弦波形的音频样本
    for (j = 0; j <frame->nb_samples; j++) {
        v = (int)(sin(ost->t) * 10000);     // 生成正弦波
        for (i = 0; i < ost->enc->ch_layout.nb_channels; i++)
            *q++ = v;                       // 填充所有通道
        ost->t     += ost->tincr;           // 更新相位
        ost->tincr += ost->tincr2;          // 更新频率增量
    }

    frame->pts = ost->next_pts;             // 设置显示时间戳
    ost->next_pts  += frame->nb_samples;    // 更新下一帧的时间戳

    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
/**
 * 编码并写入一个音频帧
 * 返回 1 表示编码结束，0 表示继续
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    AVCodecContext *c;
    AVFrame *frame;
    int ret;
    int dst_nb_samples;

    c = ost->enc;

    frame = get_audio_frame(ost);   // 获取音频帧

    if (frame) {
        /* convert samples from native format to destination codec format, using the resampler */
        /* compute destination number of samples */
        // 计算目标采样数
        dst_nb_samples = swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples;
        av_assert0(dst_nb_samples == frame->nb_samples);

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally;
         * make sure we do not overwrite it here
         */
        // 确保帧可写
        ret = av_frame_make_writable(ost->frame);
        if (ret < 0)
            exit(1);

        /* convert to destination format */
        // 进行重采样
        ret = swr_convert(ost->swr_ctx,
                          ost->frame->data, dst_nb_samples,
                          (const uint8_t **)frame->data, frame->nb_samples);
        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            exit(1);
        }
        frame = ost->frame;

        // 计算正确的PTS
        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        ost->samples_count += dst_nb_samples;
    }

    // 写入编码的帧
    return write_frame(oc, c, ost->st, frame, ost->tmp_pkt);
}

/**************************************************************/
/* video output */
/**
 * 分配视频帧缓冲区
 * @param pix_fmt 像素格式
 * @param width   宽度
 * @param height  高度
 */
static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *frame;
    int ret;

    // 分配帧结构
    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    // 设置基本参数
    frame->format = pix_fmt;
    frame->width  = width;
    frame->height = height;

    /* allocate the buffers for the frame data */
    // 分配实际的缓冲区
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return frame;
}

/**
 * 打开视频编码器并初始化相关结构
 */
static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    // 1. 打开编码器
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a re-usable frame */
    // 2. 分配并初始化重用帧
    ost->frame = alloc_frame(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    // 3. 如果输出格式不是YUV420P，需要一个临时YUV420P帧
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_frame(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            fprintf(stderr, "Could not allocate temporary video frame\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    // 4. 复制流参数到复用器
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

/* Prepare a dummy image. */
/**
 * 填充YUV测试图像
 * 生成一个移动的颜色图案
 */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    /* Y (亮度) 平面 */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    /* Cb 和 Cr (色度) 平面 */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

/**
 * 获取视频帧
 * 生成一个合成视频帧
 */

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->enc;

    /* check if we want to generate more frames */
    // 检查是否需要生成更多帧
    if (av_compare_ts(ost->next_pts, c->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    // 确保帧数据缓冲区可写
    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    // 如果编码器不使用YUV420P，需要进行格式转换
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        // 延迟初始化转换器
        if (!ost->sws_ctx) {
            ost->sws_ctx = sws_getContext(c->width, c->height,
                                          AV_PIX_FMT_YUV420P,
                                          c->width, c->height,
                                          c->pix_fmt,
                                          SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->sws_ctx) {
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        // 先填充临时YUV帧，然后转换
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx, (const uint8_t * const *) ost->tmp_frame->data,
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        // 直接填充YUV帧
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }

    ost->frame->pts = ost->next_pts++; // 设置显示时间

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
    return write_frame(oc, ost->enc, ost->st, get_video_frame(ost), ost->tmp_pkt);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

/**************************************************************/
/* media file output */
/**
 * 主函数
 * 演示了完整的媒体文件生成流程
 */
int main(int argc, char **argv)
{   
    // 1. 初始化输出流结构
    OutputStream video_st = { 0 }, audio_st = { 0 };    // 输出流结构
    const AVOutputFormat *fmt;                          // 输出格式
    const char *filename;                               // 输出文件名
    AVFormatContext *oc;                                // 输出上下文
    const AVCodec *audio_codec, *video_codec;           // 音视频编码器
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    AVDictionary *opt = NULL;
    int i;

    if (argc < 2) {
        printf("usage: %s output_file\n"
               "API example program to output a media file with libavformat.\n"
               "This program generates a synthetic audio and video stream, encodes and\n"
               "muxes them into a file named output_file.\n"
               "The output format is automatically guessed according to the file extension.\n"
               "Raw images can also be output by using '%%d' in the filename.\n"
               "\n", argv[0]);
        return 1;
    }

    filename = argv[1];
    for (i = 2; i+1 < argc; i+=2) {
        if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
            av_dict_set(&opt, argv[i]+1, argv[i+1], 0);
    }

    /* allocate the output media context */
    // 2. 创建输出上下文
    avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if (!oc) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
    }
    if (!oc)
        return 1;

    fmt = oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    // 3. 添加音视频流
    if (fmt->video_codec != AV_CODEC_ID_NONE) {
        add_stream(&video_st, oc, &video_codec, fmt->video_codec);
        have_video = 1;
        encode_video = 1;
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE) {
        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);
        have_audio = 1;
        encode_audio = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    // 4. 打开编码器并写入文件头
    if (have_video)
        open_video(oc, video_codec, &video_st, opt);

    if (have_audio)
        open_audio(oc, audio_codec, &audio_st, opt);

    av_dump_format(oc, 0, filename, 1); // 打印输出格式信息

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE); // 打开输出文件
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s': %s\n", filename,
                    av_err2str(ret));
            return 1;
        }
    }

    /* Write the stream header, if any. */
    // 5. 写入文件头
    ret = avformat_write_header(oc, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        return 1;
    }

    // 6. 编码循环
    while (encode_video || encode_audio) {
        /* select the stream to encode */
        // 选择下一个要编码的流
        if (encode_video && // 第一部分：检查是否还需要编码视频
            (!encode_audio ||   // 第二部分：检查是否应该编码视频而不是音频
            av_compare_ts(video_st.next_pts,        // 视频的下一个时间戳
                          video_st.enc->time_base,  // 视频的时间基
                          audio_st.next_pts,        // 音频的下一个时间戳
                          audio_st.enc->time_base   // 音频的时间基
                          ) <= 0)) { // 比较两个时间戳，但考虑了它们不同的时间基. 返回 ≤0 表示视频时间戳更早或相等
            encode_video = !write_video_frame(oc, &video_st);
        } else {
            encode_audio = !write_audio_frame(oc, &audio_st);
        }
    }

    // 7. 完成文件写入
    av_write_trailer(oc);

    /* Close each codec. */
    // 8. 清理资源
    if (have_video)
        close_stream(oc, &video_st);
    if (have_audio)
        close_stream(oc, &audio_st);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&oc->pb);

    /* free the stream */
    avformat_free_context(oc);

    return 0;
}
