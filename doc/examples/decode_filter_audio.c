/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2012 Clément Bœsch
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
 * @file audio decoding and filtering usage example
 * @example decode_filter_audio.c
 *
 * Demux, decode and filter audio input file, generate a raw audio
 * file to be played with ffplay.
 */

#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

// 全局变量
static const char *filter_descr = "aresample=8000,aformat=sample_fmts=s16:channel_layouts=mono";
// 音频重采样到8000Hz，格式化为16位有符号整数，单声道
static const char *player       = "ffplay -f s16le -ar 8000 -ac 1 -";
// 使用ffplay播放处理后的音频的命令

// 全局上下文
static AVFormatContext *fmt_ctx;    // 格式上下文
static AVCodecContext *dec_ctx;     // 解码器上下文
AVFilterContext *buffersink_ctx;    // 滤镜输出上下文
AVFilterContext *buffersrc_ctx;     // 滤镜输入上下文
AVFilterGraph *filter_graph;        // 滤镜图
static int audio_stream_index = -1; // 音频流索引

/**
 * 打开输入文件并初始化解码器
 */
static int open_input_file(const char *filename)
{
    const AVCodec *dec;
    int ret;
    // 1. 打开输入文件
    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    // 2. 读取流信息
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the audio stream */
    // 3. 查找音频流
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in the input file\n");
        return ret;
    }
    audio_stream_index = ret;

    /* create decoding context */
    // 4. 创建解码器上下文
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);

    // 5. 复制编解码参数
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);

    /* init the audio decoder */
    // 6. 打开解码器
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        return ret;
    }

    return 0;
}

/**
 * 初始化滤镜
 */
static int init_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    // 1. 获取滤镜
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    static const int out_sample_rate = 8000;
    const AVFilterLink *outlink;
    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
    // 2. 创建滤镜图
    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
    ret = snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=",
             time_base.num, time_base.den, dec_ctx->sample_rate,
             av_get_sample_fmt_name(dec_ctx->sample_fmt));
    av_channel_layout_describe(&dec_ctx->ch_layout, args + ret, sizeof(args) - ret);
    // 3. 创建源滤镜（输入）
    // 配置时间基、采样率、采样格式、声道布局等参数
    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    // 4. 创建接收滤镜（输出）
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "out");
    if (!buffersink_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // 设置输出格式参数
    ret = av_opt_set(buffersink_ctx, "sample_formats", "s16",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
        goto end;
    }

    ret = av_opt_set(buffersink_ctx, "channel_layouts", "mono",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
        goto end;
    }

    ret = av_opt_set_array(buffersink_ctx, "samplerates", AV_OPT_SEARCH_CHILDREN,
                           0, 1, AV_OPT_TYPE_INT, &out_sample_rate);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
        goto end;
    }

    ret = avfilter_init_dict(buffersink_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot initialize audio buffer sink\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    // 5. 链接滤镜
    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Print summary of the sink buffer
     * Note: args buffer is reused to store channel layout string */
    outlink = buffersink_ctx->inputs[0];
    av_channel_layout_describe(&outlink->ch_layout, args, sizeof(args));
    av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
           (int)outlink->sample_rate,
           (char *)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
           args);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

/**
 * 打印音频帧数据
 */
static void print_frame(const AVFrame *frame)
{
    // 计算样本数
    const int n = frame->nb_samples * frame->ch_layout.nb_channels;
    const uint16_t *p     = (uint16_t*)frame->data[0];
    const uint16_t *p_end = p + n;
    // 写入标准输出
    // 每个样本是16位，分两次写入
    while (p < p_end) {
        fputc(*p    & 0xff, stdout);    // 低8位
        fputc(*p>>8 & 0xff, stdout);    // 高8位
        p++;
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int ret;
    // 1. 分配内存
    AVPacket *packet = av_packet_alloc();   // 数据包
    AVFrame *frame = av_frame_alloc();      // 解码后的帧
    AVFrame *filt_frame = av_frame_alloc(); // 滤镜处理后的帧

    if (!packet || !frame || !filt_frame) {
        fprintf(stderr, "Could not allocate frame or packet\n");
        exit(1);
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: %s file | %s\n", argv[0], player);
        exit(1);
    }

    // 2. 打开输入并初始化滤镜
    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;

    /* read all packets */
    // 3. 主处理循环
    while (1) {
        // 读取数据包
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == audio_stream_index) {
            // 发送到解码器
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            // 接收解码后的帧
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame); // 用于从解码器中接收一个解码后的帧
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                if (ret >= 0) {
                    /* push the audio data from decoded frame into the filtergraph */
                    // 送入滤镜处理
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
                        break;
                    }

                    /* pull filtered audio from the filtergraph */
                    // 获取滤镜处理后的帧
                    while (1) {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            break;
                        if (ret < 0)
                            goto end;
                        print_frame(filt_frame);
                        av_frame_unref(filt_frame);
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    if (ret == AVERROR_EOF) {
        /* signal EOF to the filtergraph */
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, NULL, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while closing the filtergraph\n");
            goto end;
        }

        /* pull remaining frames from the filtergraph */
        // 获取滤镜处理后的帧
        while (1) {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                goto end;
            print_frame(filt_frame);
            av_frame_unref(filt_frame);
        }
    }

end:
    // 4. 清理资源
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        exit(1);
    }

    exit(0);
}
