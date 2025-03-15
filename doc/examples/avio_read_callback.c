/*
 * Copyright (c) 2014 Stefano Sabatini
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
 * @file libavformat AVIOContext read callback API usage example
 * 展示如何通过自定义的 AVIOContext 读取回调函数来访问媒体内容
 * @example avio_read_callback.c
 *
 * Make libavformat demuxer access media content through a custom
 * AVIOContext read callback.
 * 这个示例将文件内容读入内存，然后通过自定义回调来访问
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/mem.h>

// 用于存储缓冲区信息的结构体
struct buffer_data {
    uint8_t *ptr;       // 缓冲区指针
    size_t size;        // 缓冲区中剩余的数据大小
};

/**
 * 自定义读取回调函数
 * @param opaque    用户自定义数据，这里是 buffer_data 结构体
 * @param buf       目标缓冲区
 * @param buf_size  要读取的字节数
 * @return 实际读取的字节数，或错误码
 */
static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    struct buffer_data *bd = (struct buffer_data *)opaque;
    // 取要读取的大小和剩余数据大小中的较小值
    buf_size = FFMIN(buf_size, bd->size);

    // 如果没有数据可读，返回 EOF
    if (!buf_size)
        return AVERROR_EOF;
    printf("ptr:%p size:%zu\n", bd->ptr, bd->size);

    /* copy internal buffer data to buf */
    // 将数据从内部缓冲区复制到输出缓冲区
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr  += buf_size;           // 更新缓冲区指针
    bd->size -= buf_size;           // 更新剩余大小

    return buf_size;                // 返回实际读取的字节数
}

int main(int argc, char *argv[])
{
    AVFormatContext *fmt_ctx = NULL;                    // 格式上下文
    AVIOContext *avio_ctx = NULL;                       // IO上下文
    uint8_t *buffer = NULL, *avio_ctx_buffer = NULL;    // 文件数据缓冲区、AVIO上下文缓冲区
    size_t buffer_size, avio_ctx_buffer_size = 4096;    // 文件大小、 AVIO缓冲区大小
    char *input_filename = NULL;                        // 
    int ret = 0;
    struct buffer_data bd = { 0 };                      // 缓冲区数据结构

    if (argc != 2) {                                    // 检查命令行参数
        fprintf(stderr, "usage: %s input_file\n"
                "API example program to show how to read from a custom buffer "
                "accessed through AVIOContext.\n", argv[0]);
        return 1;
    }
    input_filename = argv[1];

    /* slurp file content into buffer */
    // 将整个文件映射到内存
    ret = av_file_map(input_filename, &buffer, &buffer_size, 0, NULL);
    if (ret < 0)
        goto end;

    /* fill opaque structure used by the AVIOContext read callback */
    // 初始化缓冲区数据结构
    bd.ptr  = buffer;
    bd.size = buffer_size;

    // 分配格式上下文
    if (!(fmt_ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // 分配 AVIO 缓冲区
    avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    // 创建 AVIO 上下文
    // 参数：缓冲区、缓冲区大小、写标志、用户数据、读回调、写回调、seek回调
    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                  0, &bd, &read_packet, NULL, NULL);
    if (!avio_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    fmt_ctx->pb = avio_ctx;     // 设置自定义 IO 上下文

    // 打开输入
    ret = avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input\n");
        goto end;
    }

    // 读取媒体流信息
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto end;
    }

    // 打印格式信息
    av_dump_format(fmt_ctx, 0, input_filename, 0);

end:
    // 清理资源
    avformat_close_input(&fmt_ctx);

    /* note: the internal buffer could have changed, and be != avio_ctx_buffer */
    // 释放 AVIO 上下文和缓冲区
    if (avio_ctx)
        av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);

    // 解除文件映射
    av_file_unmap(buffer, buffer_size);
    // 错误处理
    if (ret < 0) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}
