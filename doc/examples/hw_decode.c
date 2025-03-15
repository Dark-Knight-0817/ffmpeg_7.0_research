/*
 * Copyright (c) 2017 Jun Zhao
 * Copyright (c) 2017 Kaixuan Liu
 *
 * HW Acceleration API (video decoding) decode sample
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
 * @file HW-accelerated decoding API usage.example
 * @example hw_decode.c
 *
 * Perform HW-accelerated decoding with output frames from HW video
 * surfaces.
 */

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

/**
 * HW加速视频解码流程：
 * Input -> HW Surface -> CPU Memory -> Output
 */

// 全局变量
static AVBufferRef *hw_device_ctx = NULL;       // 硬件设备上下文
static enum AVPixelFormat hw_pix_fmt;           // 硬件像素格式
static FILE *output_file = NULL;                // 输出文件

/**
 * 初始化硬件解码器
 */
static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;
    // 1. 创建硬件设备上下文
    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    // 2. 设置解码器的硬件上下文
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

/**
 * 选择硬件像素格式
 * 从解码器支持的格式列表中选择我们需要的硬件格式
 */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    // 遍历支持的格式
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

/**
 * 解码数据包并将解码后的帧写入文件
 * 支持硬件解码，会自动处理硬件帧到软件帧的转换
 *
 * @param avctx 编解码器上下文
 * @param packet 要解码的数据包
 * @return 成功返回0,失败返回负的错误码
 */
static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
    // 声明帧和缓冲区指针
    AVFrame *frame = NULL, *sw_frame = NULL;    // 用于接收解码后的帧(可能是硬件帧)，用于存储从硬件帧转换后的软件
    AVFrame *tmp_frame = NULL;                  // 指向最终要处理的帧(软件帧或硬件帧)
    uint8_t *buffer = NULL;                     // 用于存储帧数据的缓冲区
    int size;                                   // 帧数据大小
    int ret = 0;                                // 返回值

    // 1. 发送包到解码器
    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    // 2. 接收解码后的帧
    while (1) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) { // 分配帧结构体
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        // 从解码器接收帧
        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // 需要更多数据或已到达文件结尾
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        // 3. 如果是硬件帧，需要传输到CPU内存
        if (frame->format == hw_pix_fmt) {
            /* retrieve data from GPU to CPU */
            // 从GPU传输到CPU
            if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                fprintf(stderr, "Error transferring the data to system memory\n");
                goto fail;
            }
            tmp_frame = sw_frame;   // 使用转换后的软件帧
        } else
            tmp_frame = frame;      // 直接使用原始帧
        // 4. 拷贝帧数据到缓冲区
        size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
                                        tmp_frame->height, 1);
        // 分配缓冲区
        buffer = av_malloc(size);
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        // 将帧数据复制到连续的缓冲区
        ret = av_image_copy_to_buffer(buffer, size,
                                      (const uint8_t * const *)tmp_frame->data,
                                      (const int *)tmp_frame->linesize, tmp_frame->format,
                                      tmp_frame->width, tmp_frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }
        // 5. 写入文件
        if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
            fprintf(stderr, "Failed to dump raw data.\n");
            goto fail;
        }

    // 错误处理和清理
    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
}

int main(int argc, char *argv[])
{
    AVFormatContext *input_ctx = NULL;  // 输入文件上下文
    int video_stream, ret;              // 视频流索引
    AVStream *video = NULL;             // 视频流
    AVCodecContext *decoder_ctx = NULL; // 解码器上下文
    const AVCodec *decoder = NULL;      // 解码器
    AVPacket *packet = NULL;            // 包
    enum AVHWDeviceType type;           // 硬件设备类型
    int i;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <device type> <input file> <output file>\n", argv[0]);
        return -1;
    }
    // 1. 查找指定的硬件设备类型
    type = av_hwdevice_find_type_by_name(argv[1]);
    if (type == AV_HWDEVICE_TYPE_NONE) {    // 硬件设备类型无效
        fprintf(stderr, "Device type %s is not supported.\n", argv[1]);
        fprintf(stderr, "Available device types:");
        while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    packet = av_packet_alloc(); // 分配AVPacket
    if (!packet) {
        fprintf(stderr, "Failed to allocate AVPacket\n");
        return -1;
    }

    /* open the input file */
    // 2. 打开输入文件
    if (avformat_open_input(&input_ctx, argv[2], NULL, NULL) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /* find the video stream information */
    // 3. 查找视频流
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream = ret;
    // 4. 检查解码器是否支持硬件加速
    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);  // 获取解码器的硬件配置
        if (!config) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n",
                    decoder->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            break;
        }
    }
    // 5. 初始化解码器
    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;

    decoder_ctx->get_format  = get_hw_format;

    if (hw_decoder_init(decoder_ctx, type) < 0)// 初始化解码器
        return -1;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {// 打开解码器
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    /* open the file to dump raw data */
    output_file = fopen(argv[3], "w+b");

    /* actual decoding and dump the raw data */
    // 6. 主解码循环
    while (ret >= 0) {
        // 读取一个包
        if ((ret = av_read_frame(input_ctx, packet)) < 0)
            break;
        // 解码和写入
        if (video_stream == packet->stream_index)
            ret = decode_write(decoder_ctx, packet);

        av_packet_unref(packet);
    }

    /* flush the decoder */
    ret = decode_write(decoder_ctx, NULL);

    if (output_file)
        fclose(output_file);
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    return 0;
}
