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
 * @file libavcodec video decoding API usage example
 * @example decode_video.c *
 *
 * Read from an MPEG1 video file, decode frames, and generate PGM images as
 * output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#define INBUF_SIZE 4096         // 输入缓冲区大小

/**
 * 保存 PGM 格式图像
 * @param buf 图像数据
 * @param wrap 行步长
 * @param xsize 图像宽度
 * @param ysize 图像高度
 * @param filename 输出文件名
 */
static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename,"wb");
    // 写入 PGM 头部信息
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    // 按行写入图像数据
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}

/**
 * 视频帧解码函数
 * @param dec_ctx 解码器上下文
 * @param frame 解码后的帧
 * @param pkt 输入的数据包
 * @param filename 输出文件名基准
 */
static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                   const char *filename)
{
    char buf[1024];
    int ret;
    // 1. 发送数据包到解码器
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }
    // 2. 循环接收解码后的帧
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        printf("saving frame %3"PRId64"\n", dec_ctx->frame_num);
        fflush(stdout);

        /* the picture is allocated by the decoder. no need to
           free it */
        // 3. 生成输出文件名
        snprintf(buf, sizeof(buf), "%s-%"PRId64, filename, dec_ctx->frame_num);
        // 4. 保存为PGM图像
        pgm_save(frame->data[0], frame->linesize[0],
                 frame->width, frame->height, buf);
    }
}

int main(int argc, char **argv)
{
    // 主要变量声明
    const char *filename, *outfilename; 
    const AVCodec *codec;                   // 解码器
    AVCodecParserContext *parser;           // 解析器上下文
    AVCodecContext *c= NULL;                // 解码器上下文
    FILE *f;
    AVFrame *frame;                         // 解码后的帧
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE]; // 输入缓冲区
    uint8_t *data;
    size_t   data_size;
    int ret;
    int eof;
    AVPacket *pkt;                          // 数据包

    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n"
                "And check your input file is encoded by mpeg1video please.\n", argv[0]);
        exit(0);
    }
    filename    = argv[1];
    outfilename = argv[2];

    // 1. 初始化
    // 分配数据包
    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    // 填充输入缓冲区尾部（防止损坏的MPEG流导致越界读取）
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /* find the MPEG-1 video decoder */
    // 2. 查找MPEG1视频解码器
    codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    // 3. 初始化解析器
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }
    // 4. 创建解码器上下文
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

    /* open it */
    // 5. 打开解码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }
    // 6. 分配视频帧
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    // 7. 主解码循环
    do {
        /* read raw data from the input file */
        // 读取输入数据
        data_size = fread(inbuf, 1, INBUF_SIZE, f);
        if (ferror(f))
            break;
        eof = !data_size;

        /* use the parser to split the data into frames */
        // 使用解析器分割数据为帧
        data = inbuf;
        while (data_size > 0 || eof) {
            // 解析数据
            ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "Error while parsing\n");
                exit(1);
            }
            data      += ret;
            data_size -= ret;

            // 如果有完整的包就解码
            if (pkt->size)
                decode(c, frame, pkt, outfilename);
            else if (eof)
                break;
        }
    } while (!eof);

    /* flush the decoder */
    // 8. 冲洗解码器
    decode(c, frame, NULL, outfilename);

    fclose(f);

    // 9. 清理资源
    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}
