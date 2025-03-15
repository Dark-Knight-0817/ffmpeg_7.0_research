/*
 * Copyright (c) 2015 Stephan Holljes
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
 * @file libavformat multi-client network API usage example 多客户端网络 API 使用示例
 * @example avio_http_serve_files.c
 *
 * Serve a file without decoding or demuxing it over the HTTP protocol. Multiple
 * clients can connect and will receive the same file.
 * 通过 HTTP 协议提供文件服务，支持多个客户端同时连接并获取相同的文件
 * 文件传输过程中不进行解码或解复用
 */

#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <unistd.h>

// 处理单个客户端连接的函数
static void process_client(AVIOContext *client, const char *in_uri)
{
    AVIOContext *input = NULL;                  // 输入文件的 IO 上下文
    uint8_t buf[1024];                          // 数据传输缓冲区
    int ret, n, reply_code;                     // 返回值、读取字节数、HTTP响应码
    uint8_t *resource = NULL;                   // 请求的资源路径

    // 进行 HTTP 握手，直到获取到客户端请求的资源路径
    while ((ret = avio_handshake(client)) > 0) {
        // 获取客户端请求的资源路径
        av_opt_get(client, "resource", AV_OPT_SEARCH_CHILDREN, &resource);
        // check for strlen(resource) is necessary, because av_opt_get()
        // may return empty string.
        if (resource && strlen(resource))
            break;
        av_freep(&resource);
    }
    if (ret < 0)
        goto end;
    av_log(client, AV_LOG_TRACE, "resource=%p\n", resource);
    // 检查请求的资源是否匹配
    if (resource && resource[0] == '/' && !strcmp((resource + 1), in_uri)) {
        reply_code = 200;   // HTTP 200 OK
    } else {
        reply_code = AVERROR_HTTP_NOT_FOUND;    // HTTP 404 Not Found
    }
    // 设置 HTTP 响应码
    if ((ret = av_opt_set_int(client, "reply_code", reply_code, AV_OPT_SEARCH_CHILDREN)) < 0) {
        av_log(client, AV_LOG_ERROR, "Failed to set reply_code: %s.\n", av_err2str(ret));
        goto end;
    }
    av_log(client, AV_LOG_TRACE, "Set reply code to %d\n", reply_code);
    // 完成 HTTP 握手过程
    while ((ret = avio_handshake(client)) > 0);

    if (ret < 0)
        goto end;
    
    fprintf(stderr, "Handshake performed.\n");
    // 如果不是 200 OK，结束处理
    if (reply_code != 200)
        goto end;
    fprintf(stderr, "Opening input file.\n");
    // 打开输入文件
    if ((ret = avio_open2(&input, in_uri, AVIO_FLAG_READ, NULL, NULL)) < 0) {
        av_log(input, AV_LOG_ERROR, "Failed to open input: %s: %s.\n", in_uri,
               av_err2str(ret));
        goto end;
    }
    // 循环读取文件内容并发送给客户端
    for(;;) {
        n = avio_read(input, buf, sizeof(buf));
        if (n < 0) {
            if (n == AVERROR_EOF)   // 文件结束
                break;
            av_log(input, AV_LOG_ERROR, "Error reading from input: %s.\n",
                   av_err2str(n));
            break;
        }
        avio_write(client, buf, n); // 发送数据给客户端
        avio_flush(client);         // 刷新输出缓冲区
    }
end:
    // 清理资源
    fprintf(stderr, "Flushing client\n");
    avio_flush(client);
    fprintf(stderr, "Closing client\n");
    avio_close(client);
    fprintf(stderr, "Closing input\n");
    avio_close(input);
    av_freep(&resource);
}

int main(int argc, char **argv)
{
    AVDictionary *options = NULL;                   // 服务器选项
    AVIOContext *client = NULL, *server = NULL;     // 客户端和服务器的 IO 上下文
    const char *in_uri, *out_uri;                   // 输入文件和服务器地址
    int ret, pid;
    // 设置日志级别为 TRACE
    av_log_set_level(AV_LOG_TRACE);
    // 检查命令行参数
    if (argc < 3) {
        printf("usage: %s input http://hostname[:port]\n"
               "API example program to serve http to multiple clients.\n"
               "\n", argv[0]);
        return 1;
    }

    in_uri = argv[1];       // 输入文件路径
    out_uri = argv[2];      // 服务器地址

    // 初始化网络功能
    avformat_network_init();

    // 设置服务器为监听模式，最大连接数为 2
    if ((ret = av_dict_set(&options, "listen", "2", 0)) < 0) {
        fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
        return ret;
    }
    // 打开服务器
    if ((ret = avio_open2(&server, out_uri, AVIO_FLAG_WRITE, NULL, &options)) < 0) {
        fprintf(stderr, "Failed to open server: %s\n", av_err2str(ret));
        return ret;
    }
    fprintf(stderr, "Entering main loop.\n");
    // 主循环：接受客户端连接
    for(;;) {
        if ((ret = avio_accept(server, &client)) < 0)
            goto end;
        fprintf(stderr, "Accepted client, forking process.\n");
        // XXX: Since we don't reap our children and don't ignore signals
        //      this produces zombie processes.
        // 对每个客户端连接创建新进程
        pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            ret = AVERROR(errno);
            goto end;
        }
        if (pid == 0) { // 子进程处理客户端请求
            fprintf(stderr, "In child.\n");
            process_client(client, in_uri);
            avio_close(server);
            exit(0);
        }
        if (pid > 0)   // 父进程关闭客户端连接
            avio_close(client);
    }
end:
    // 关闭服务器
    avio_close(server);
    // 错误处理
    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Some errors occurred: %s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
