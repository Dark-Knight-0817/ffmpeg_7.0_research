/*
 * Copyright (c) 2014 Lukasz Marek
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
 * @file libavformat AVIOContext list directory API usage example
*           展示如何使用 libavformat AVIOContext API 列出目录内容
 * @example avio_list_dir.c
 *
 * Show how to list directories through the libavformat AVIOContext API.
 * 支持多种文件系统和网络协议
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>

// 根据文件类型返回对应的字符串描述
static const char *type_string(int type)
{
    switch (type) {
    case AVIO_ENTRY_DIRECTORY:              // 目录
        return "<DIR>";
    case AVIO_ENTRY_FILE:                   // 普通文件
        return "<FILE>";
    case AVIO_ENTRY_BLOCK_DEVICE:           // 块设备
        return "<BLOCK DEVICE>";
    case AVIO_ENTRY_CHARACTER_DEVICE:       // 字符设备
        return "<CHARACTER DEVICE>";
    case AVIO_ENTRY_NAMED_PIPE:             // 命名管道
        return "<PIPE>";
    case AVIO_ENTRY_SYMBOLIC_LINK:          // 符号链接 
        return "<LINK>";
    case AVIO_ENTRY_SOCKET:                 // 套接字
        return "<SOCKET>";
    case AVIO_ENTRY_SERVER:                 // 服务器
        return "<SERVER>";
    case AVIO_ENTRY_SHARE:                  // 共享目录
        return "<SHARE>";
    case AVIO_ENTRY_WORKGROUP:              // 工作组
        return "<WORKGROUP>";
    case AVIO_ENTRY_UNKNOWN:                // 未知类型
    default:
        break;
    }
    return "<UNKNOWN>";
}

// 列出目录内容的核心函数
static int list_op(const char *input_dir)
{
    AVIODirEntry *entry = NULL;             // 目录项指针
    AVIODirContext *ctx = NULL;             // 目录上下文
    int cnt, ret;                           // 计数器和返回值
    char filemode[4], uid_and_gid[20];      // 存储文件权限的字符串、存储用户ID和组ID的字符串
    // 打开目录
    if ((ret = avio_open_dir(&ctx, input_dir, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open directory: %s.\n", av_err2str(ret));
        goto fail;
    }

    cnt = 0;
    for (;;) {
        // 读取目录项
        if ((ret = avio_read_dir(ctx, &entry)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot list directory: %s.\n", av_err2str(ret));
            goto fail;
        }
        // 如果没有更多项，退出循环
        if (!entry)
            break;
        // 格式化文件权限
        if (entry->filemode == -1) {
            snprintf(filemode, 4, "???");
        } else {
            snprintf(filemode, 4, "%3"PRIo64, entry->filemode);
        }
        // 格式化用户ID和组ID
        snprintf(uid_and_gid, 20, "%"PRId64"(%"PRId64")", entry->user_id, entry->group_id);
        // 打印表头（仅第一次）
        if (cnt == 0)
            av_log(NULL, AV_LOG_INFO, "%-9s %12s %30s %10s %s %16s %16s %16s\n",
                   "TYPE", "SIZE", "NAME", "UID(GID)", "UGO", "MODIFIED",
                   "ACCESSED", "STATUS_CHANGED");
        // 打印文件信息
        av_log(NULL, AV_LOG_INFO, "%-9s %12"PRId64" %30s %10s %s %16"PRId64" %16"PRId64" %16"PRId64"\n",
               type_string(entry->type),            // 文件类型
               entry->size,                         // 文件大小
               entry->name,                         // 文件名  
               uid_and_gid,                         // 用户ID和组ID
               filemode,                            // 文件权限         
               entry->modification_timestamp,       // 修改时间
               entry->access_timestamp,             // 访问时间
               entry->status_change_timestamp);     // 状态改变时间

        // 释放目录项
        avio_free_directory_entry(&entry);
        cnt++;
    };

  fail:
    // 关闭目录
    avio_close_dir(&ctx);
    return ret;
}
// 打印使用帮助
static void usage(const char *program_name)
{
    fprintf(stderr, "usage: %s input_dir\n"
            "API example program to show how to list files in directory "
            "accessed through AVIOContext.\n", program_name);
}

int main(int argc, char *argv[])
{
    int ret;
    // 设置日志级别为调试级别
    av_log_set_level(AV_LOG_DEBUG);
    // 检查命令行参数
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    // 初始化网络功能（支持网络协议）
    avformat_network_init();
    // 执行目录列举操作
    ret = list_op(argv[1]);
    // 清理网络功能
    avformat_network_deinit();

    return ret < 0 ? 1 : 0;
}
