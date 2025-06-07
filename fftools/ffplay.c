/*
 * Copyright (c) 2003 Fabrice Bellard
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

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include "config_components.h"
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/tx.h"
#include "libswresample/swresample.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"
#include "ffplay_renderer.h"
#include "opt_common.h"

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)   // 15M 这里只是一个经验计算值，比如 4K 视频的码率以 50Mbps 计算, 则 15MB 可以缓存 2.4s， 真 4K 则不够了
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

typedef struct MyAVPacketList {
    AVPacket *pkt;      // 解封装后的数据
    /* 标记当前的播放序列号,ffplay中多次用到,主要用来区分是否连续*/
    int serial;         // 播放序列
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;   // 指向 packet_list ,AVFifoBuffer 是一个circular buffer FIFO ,一个先进先出的环形缓存实现。里面存储的是 struct MyAVPacketList 结构的数据。
    int nb_packets;     // 包数量,也就是队列元素数量
    int size;           // 队列所有元素的数据大小总和，是所有 AVPacket + AVPacket->size
    int64_t duration;   // 队列所有元素的数据播放持续时间
    int abort_request;  // 用户请求退出标志, =1 则 audio_thread 和 video_thread 退出
    int serial;         // 播放序列号,和 MyAVPacketList 的 serial 作用相同，每次跳转播放时间点，serial 就会 +1 
    SDL_mutex *mutex;   // 用于维持 PacketQueue 的多线程安全(SDL_mutex可以按pthread_mutex_t理解)
    SDL_cond *cond;     // 用于读、写线程相互筒子(SDL_cond可以按pthread_cond_t理解)
} PacketQueue;          // 音频、视频、字幕流都有自己独立的PacketQueue

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq;                   // 采样率，表示每秒采样的次数（例如 44100Hz）
    AVChannelLayout ch_layout;  // 音频通道布局，描述声道的数量和类型（如立体声、5.1环绕声等）
    enum AVSampleFormat fmt;    // 音频采样格式，定义每个采样的数据类型（如 16位整数、32位浮点等）
    int frame_size;             // 音频帧大小，表示每个音频帧包含的采样数
    int bytes_per_sec;          // 每秒音频数据的字节数，用于计算缓冲大小和数据速率 
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base 时钟基础,当前帧(待播放)显示时间戳,播放后,当前帧变成上一帧  */
    // 当前 pts 与当前系统时钟的差值, audio、video对于该值是独立的
    double pts_drift;     /* clock base minus time at which we updated the clock */
    /**
     * @note last_updated 的核心作用是协助计算当前的时钟值。
     *       在音视频播放中，时钟需要随着时间推移自动前进，但直接访问系统时钟可能不精确。
     *       因此，last_updated 提供了一个参考点，用于计算经过的时间并推导出当前的时钟值。
     */
    double last_updated;  // 最后一次更新的系统时钟。
    double speed;         // 时钟速度控制,用于控制播放速度
    // 播放序列,就是一段连续的播放动作,一个seek操作会启动一段新的播放序列
    /** 两个serial通过比较匹配来丢弃无效的缓存帧，
     *  例如：PacketQueue 队列里缓存了8个帧，但是这8个帧都第30min才开始播放的，
     *  如果你通过->按键前进到第35min的位置播放，则队列的 8个缓存帧就无效了，需要丢弃。
     *  由于每次跳转播放时间点， PacketQueue::serial 都会+1，而 MyAVPacketList::serial 的值还是原来的，两个 serial 不一样，就会丢弃帧。 */
    int serial;           /* clock is based on a packet with this serial.  这里是播放所处状态处 serial  */
    int paused;           //  = 1 说明是暂停状态
    // 指向 packet_serial, 确定读取的数据属于哪一个 packet_queue
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;       // 指向数据帧
    AVSubtitle sub;       // 用于字幕
    int serial;           // 播放序列, 在 seek 的操作时 serial 会变化
    double pts;           /* presentation timestamp for the frame. 时间戳,单位为秒  */
    double duration;      /* estimated duration of the frame. 该帧持续时间,单位为秒  */
    int64_t pos;          /* byte position of the frame in the input file. 该帧在输入文件中的字节位置  */
    int width;            // 图像宽度
    int height;           // 图像高度
    int format;           // 对于图像为 enum AVPixelFormat.
                          // 对于声音为 enum AVSAmpleFormat.
    AVRational sar;       // 图像的宽高比,如果未知or未指定则为 0/1
    int uploaded;         // 用来记录该帧是否已经显示过
    int flip_v;           //  = 1 则旋转180, = 0 则正常播放
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE]; // FRAME_QUEUE_SIZE 最大size, 数字太大时会占用大量的内存,需要注意该值的设置
    int rindex;           // 读索引. 待播放时读取此帧进行播放,播放后此帧成为上一帧
    int windex;           // 写索引
    int size;             // 当前总帧数
    int max_size;         // 可存储最大帧数
    /**
     * @param keep_last = 1  使得最后一帧不会被释放
     *       当 rindex_shown = 0 时，调用 frame_queue_next 只会把 rindex_shown 设为 1，而不会实际移除这一帧
     *       这样就能保持当前画面不变，直到下一帧到来。
     * @param rindex_shown 用于标记当前位置的帧是否已经显示
     *       初始化为 0，表示新的一帧
     *       设为 1，表示这一帧已经显示过
     * @note 在暂停时，keep_last = 1，当 rindex_shown = 0 时，调用 frame_queue_next 
     *      只会把 rindex_shown 设为1，而不会实际移除这一帧.这样就能保持当前画面不变
     * @note 在继续播放时，当进入下一帧时，此时 rindex_shown = 1, frame_queue_next 会正常执行，释放当前帧
     *      并移动到下一帧。
     * @note 这种机制确保了暂停时画面能保持不变，音频播放也能平滑过渡，在队列销毁时才真正释放所有资源
     */
    int keep_last;        //  = 1 说明要在队列里面保持最后一帧的数据不释放,只在销毁队列的时候才将其真正的释放
    int rindex_shown;     // 初始化为 0, 配合keep_last = 1 使用
    SDL_mutex *mutex;     // 互斥量
    SDL_cond *cond;       // 条件变量
    /**
     * @note 指针指向数据包缓冲队列
     * @brief 同步控制、序列一致性、终止管理
     * @note pktq->abort_request 用来检查是否需要终止
     * @note pktq->serial 检查帧序列号是否与包队列序列号匹配
     * @note 设计的原因是：
     *          一个 Frame 是从 Packet 解码而来，它们需要保持序列上的一致性，尤其是在播放控制(seek、暂停等)时。
     *          通过 pktq 可以让帧队列和包队列之间建立这种关联关系。
     * @note 1.seek 首先控制 packet 队列
     * @note 2.packet 队列改变序列号
     * @note 3.decoder 检测到序列号变化，清空解码器
     * @note 4.frame 队列通过序列号检查来丢弃旧的帧
     * 
     */
    PacketQueue *pktq;    
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
    AVPacket *pkt;                  // 指向数据包
    PacketQueue *queue;             // 数据包队列
    AVCodecContext *avctx;          // 解码器上下文
    int pkt_serial;                 // 包序列
    int finished;                   //  = 0 ,解码器处于工作状态; != 0 ,解码器处于空闲状态
    int packet_pending;             //  = 0 ,解码器处于异常状态,需要考虑重置解码器; = 1 ,
    SDL_cond *empty_queue_cond;     // 检查到 packet 队列空时发送, signal 缓存 read_thread 读取数据
    int64_t start_pts;              // 初始化时是 stream 的 start time
    AVRational start_pts_tb;        // 初始化时时 stream 的 time_base
    int64_t next_pts;               // 记录最近一次解码后的 frame 的 pts, 当解出来的部分帧没有有效的 pts 时,则使用 next_pts 进行推算
    AVRational next_pts_tb;         // next_pts 的单位
    SDL_Thread *decoder_tid;        // 线程句柄
} Decoder;

typedef struct VideoState {
    SDL_Thread *read_tid;           // 读线程句柄
    const AVInputFormat *iformat;   // 指向 dmuxer
    int abort_request;              //  = 1 请求退出播放
    int force_refresh;              //  = 1 刷新画面,请求刷新画面
    int paused;                     //  = 1 暂停, = 0 播放
    int last_paused;                // 保存“暂停”/“播放”状态
    int queue_attachments_req;      // 
    int seek_req;                   // 标识一次 seek 请求
    int seek_flags;                 // seek 标志,比如 AVSEEK_FLAG_BYTE等    
    int64_t seek_pos;               // 请求 seek 的目标位置(当前位置+增量)
    int64_t seek_rel;               // 本次 seek 的位置增量
    int read_pause_return;          // 用来存储暂停操作的返回值的，记录暂停操作是否成功，用于后续的错误检查和处理
    AVFormatContext *ic;            // informat 的上下文
    int realtime;                   //  =1 实时流

    Clock audclk;                   // 音频时钟,记录音频流目前的播放时刻
    Clock vidclk;                   // 视频时钟，记录视频流目前的播放时刻
    /*  取第一帧音频or视频的 pts 作为 起始时间， 然后随着物理时间的消逝而增长，所以是物理时间的当前时刻。
        到底是以音频的第一帧or视频的第一帧，取决于 av_read_frame() 函数第一次独到的是音频or视频     */
    Clock extclk;                   // 外部时钟

    FrameQueue pictq;               // 视频 Frame 队列
    FrameQueue subpq;               // 字幕 Frame 队列
    FrameQueue sampq;               // 采样 Frame 队列

    Decoder auddec;                 // 音频解码器
    Decoder viddec;                 // 视频解码器
    Decoder subdec;                 // 字幕解码器

    int audio_stream;               // 音频流索引，音频独有

    int av_sync_type;               // 视频同步类型, 默认 audio master，一共3种，以 音频/视频/外部 时钟为准。

    double audio_clock;             // 当前音频帧的 PTS + 当前帧 Duration
    int audio_clock_serial;         // 播放序列, seek 可改变该值.是一个序列号,用于标识音频时钟的连续性

    /* 以下4个参数, 非audio master同步方式使用 */
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;

    AVStream *audio_st;             // 音频流
    PacketQueue audioq;             // 音频 packet 队列
    int audio_hw_buf_size;          // SDL 音频缓冲区的大小(字节为单位)
    /* 指向待播放的一帧音频数据,指向的数据区将被拷入 SDL 音频缓冲区.
    *  若经过重采样,则指向 audio_buf1
    */
    uint8_t *audio_buf;             // 指向需要重采样的数据
    uint8_t *audio_buf1;            // 指向重采样后的数据
    /* in bytes */
    unsigned int audio_buf_size;    // 待播放的一帧音频数据(audio_buf指向)的大小
    unsigned int audio_buf1_size;   // 申请到的音频缓冲区audio_buf1的实际尺寸
    /* in bytes */
    int audio_buf_index;            // 更新拷贝位置, 当前音频帧中已拷入 SDL 音频缓冲区的位置索引(指向第一个待拷贝字节)
    /* 当前音频帧中尚未拷入 SDL 音频缓冲区的数据量:
    *   audio_buf_size = audio_buf_index + audio_write_buf_size
    */
    int audio_write_buf_size;       // 
    int audio_volume;               // 音量
    int muted;                      //  = 1 静音, = 0 正常
    struct AudioParams audio_src;   // 音频 frame 的参数
    struct AudioParams audio_filter_src; // 
    struct AudioParams audio_tgt;   // SDL 支持的音频参数,重采样转换: audio_src -> audio_tgt
    struct SwrContext *swr_ctx;     // 音频重采样 context
    int frame_drops_early;          // 丢弃视频 packet 计数
    int frame_drops_late;           // 丢弃视频 frame 计数

    enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode;

    /*  音频波形显示使用    */
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;


    int xpos;                   // 
    double last_vis_time;       // 
    SDL_Texture *vis_texture;   // 
    SDL_Texture *sub_texture;   // 字幕显示
    SDL_Texture *vid_texture;   // 视频显示

    int subtitle_stream;        // 字幕流索引
    AVStream *subtitle_st;      // 字幕流
    PacketQueue subtitleq;      // 字幕 packet 队列

    double frame_timer;         // 记录最后一帧播放的时刻
    double frame_last_returned_time; // 记录最后一帧返回的时刻
    double frame_last_filter_delay;  // 记录最后一帧滤镜处理后的延迟

    int video_stream;           // 视频流索引
    AVStream *video_st;         // 视频流
    PacketQueue videoq;         // 视频队列
    double max_frame_duration;  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity. 一帧的最大间隔
    struct SwsContext *sub_convert_ctx; // 字幕尺寸格式变换
    int eof;                    // 是否读取结束

    char *filename;             // 文件名
    int width, height, xleft, ytop; // 宽、高、x起始坐标, y起始坐标
    int step;                   //  = 1 步进播放模式, = 0 其他模式

    int vfilter_idx;            //
    AVFilterContext *in_video_filter;   // the first filter in the video chain. 指向视频滤镜链中第一个滤镜的指针,代表了视频数据进入滤镜处理链的入口点
    AVFilterContext *out_video_filter;  // the last filter in the video chain. 指向视频滤镜链中最后一个滤镜的指针,代表了处理后的视频数据离开滤镜链的出口点
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain. 指向音频滤镜链中第一个滤镜的指针,音频数据进入滤镜处理链的入口点
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain. 指向音频滤镜链中最后一个滤镜的指针,处理后的音频数据离开滤镜链的出口点
    AVFilterGraph *agraph;              // audio filter graph. 指向音频滤镜图的指针,滤镜图是一个包含多个相互连接的滤镜的结构，用于复杂的音频处理

    /* 保留最近的相应的 audio,video,subtitle 流的steam index */
    int last_video_stream, last_audio_stream, last_subtitle_stream;

    /*  进入休眠条件——1. PacketQueue队列满了，无法再塞进去数据 2. 超过最小缓存Size  */
    SDL_cond *continue_read_thread; // 当读取数据队列满了后,进入休眠,可以通过该 continue 唤醒读线程。
} VideoState; // 对播放器的参数封装

/* options specified by the user */
static const AVInputFormat *file_iformat;
static const char *input_filename;
static const char *window_title;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwaysontop;
static int startup_volume = 100;
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static const char **vfilters_list = NULL;
static int nb_vfilters = 0;
static char *afilters = NULL;
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;
static int enable_vulkan = 0;
static char *vulkan_params = NULL;
static const char *hwaccel = NULL;

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;                      // 播放窗口
static SDL_Renderer *renderer;                  // 播放窗口的渲染器
static SDL_RendererInfo renderer_info = {0};    // 渲染器信息
static SDL_AudioDeviceID audio_dev;             // 音频设备ID

static VkRenderer *vk_renderer;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
};

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    int ret = GROW_ARRAY(vfilters_list, nb_vfilters);
    if (ret < 0)
        return ret;

    vfilters_list[nb_vfilters - 1] = av_strdup(arg);
    if (!vfilters_list[nb_vfilters - 1])
        return AVERROR(ENOMEM);

    return 0;
}

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request)
       return -1;


    pkt1.pkt = pkt; // 拷贝 AVPacket (浅拷贝,AVPacket.data等内存并没有拷贝)
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret;

    /*  分配一个新的 AVPacket   */
    pkt1 = av_packet_alloc();
    if (!pkt1) {
        /*  如果分配失败，释放原始包的引用并返回错误    */
        av_packet_unref(pkt);
        return -1;
    }
    /*  将原始包的引用移动到新分配的包  */
    av_packet_move_ref(pkt1, pkt);

    /*  加锁，确保线程安全  */
    SDL_LockMutex(q->mutex);
    /*  调用私有函数将包放入队列    */
    ret = packet_queue_put_private(q, pkt1);
    /*  解锁    */
    SDL_UnlockMutex(q->mutex);

    /*  如果入队失败，释放新分配的包    */
    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    /**
     * @brief 
     * @param 1 初始队列容量(number of elements)，这里是1个元素
     * @param sizeof(MyAVPacketList): 每个元素的大小
     * @param AV_FIFO_FLAG_AUTO_GROW: 自动增长标志
     */
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW); // 分配和初始化一个 AVFifo（先进先出队列）结构

    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    /*  如果 q->abort_request 置为1，则audio_thread() 和 video_thread()解码线程就会退出，
        所以在创建解码线程之前，ffplay 会把 q->abort_request 置为0    */
    q->abort_request = 1;  
    return 0;
}

/* 清空数据包队列，释放所有数据包内存 */
/**
 * @brief 关于packet和serial
 * @param A. 时间范围筛选(pkt_in_play_range)
 * 在read_thread中进行
 * 在最早期就过滤不需要的包
 * 筛选发生在包进入队列之前
 * 主要用于播放特定时间段的内容
 * @param B. 序列号筛选(serial)
 * 贯穿整个播放流程
 * 在多个环节都有检查点
 * 用于处理播放序列的切换
 * 确保系统状态的同步
 * 
 * 范围筛选和serial机制解决不同层面的问题
 * 范围筛选提供了效率优势
 * serial提供了状态管理能力
 * 两种机制互补，不能只用其中之一
 * 
 */
static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;    // 临时存储读取的数据包节点

    SDL_LockMutex(q->mutex);    // 加锁，保护队列操作的线程安全
    /*  从FIFO队列中循环读取所有数据包，直到读完    */
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        /*  释放数据包占用的内存    */
        av_packet_free(&pkt1.pkt);
    /* 重置队列状态 */
    /*  // 范围检查：内容选择
     *   |--0s--|--5s--|--10s--|--15s--|
     *       ↑      ↑
     *       想要的范围
     * 
     *   // serial：状态同步
     *   |--旧播放--|--seek--|--新播放--|
     *   serial=1         serial=2
    */
    q->nb_packets = 0;  // 包数量清零
    q->size = 0;        // 队列大小清零
    q->duration = 0;    // 总时长清零
    q->serial++;        // 播放序列号增1，表示开始新的播放序列  
    SDL_UnlockMutex(q->mutex);  // 解锁
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

/**
 * @brief 新版本中移除了 flush_pkt 操作可能基于以下几个原因
 * 新版本通过序列号(serial)来标记队列的重置/重启状态
 * 当队列需要清空或重启时，增加序列号就足以标记这个状态变化
 * 其他组件可以通过检查序列号来判断队列是否被重置
 * 移除了显式的 flush 包，简化了队列管理
 * 减少了内存操作和额外的数据包处理
 */
static void packet_queue_start(PacketQueue *q) // 初始化或重新启动一个数据包队列
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++; // 序列号用于跟踪队列的状态变化，有助于其他部分的代码识别队列何时被重置或重新启动
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    /*  加锁，确保线程安全  */
    SDL_LockMutex(q->mutex);

    for (;;) {
        /*  检查是否有中止请求  */
        if (q->abort_request) {
            ret = -1;
            break;
        }

        /*  尝试从队列中读取一个包    */
        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            /*  成功读取包，更新队列状态    */
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            /*  将包的引用移动到输出参数 pkt    */
            av_packet_move_ref(pkt, pkt1.pkt);
            /*  如果需要，设置序列号    */
            if (serial)
                *serial = pkt1.serial;
            /*  释放临时包  */
            av_packet_free(&pkt1.pkt);
            ret = 1; // 表示成功获取包
            break;
        } else if (!block) { // 如果是非阻塞模式且队列为空，立即返回
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex); // 阻塞模式下，如果队列为空，等待条件变量
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) { // 初始化解码器结构
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc(); // 为解码器分配一个 AVPacket 结构
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx; // 设置解码器上下文（AVCodecContext）
    d->queue = queue; // 设置与解码器关联的数据包队列
    d->empty_queue_cond = empty_queue_cond; // 设置空队列条件变量，用于线程同步
    d->start_pts = AV_NOPTS_VALUE; //初始化时间戳
    d->pkt_serial = -1; // 初始化序列号
    return 0;
}

/**
 * @brief 尝试从解码器获取已解码的帧 -> 如果没有帧，从队列获取新的 packet -> 发送 packet 到解码器 -> 循环这个过程直到获得解码帧
 * 为什么不是packet->decoder->frame的顺序？
 * 一个packet可能解码出多个frame：
 * packet1 -> |decoder| -> frame1
 *                   -> frame2
 *                   -> frame3
 * avcodec_send_packet(): 向解码器填充数据， 可能返回EAGAIN(缓冲区满)
 * avcodec_receive_frame():从解码器取出解码后的帧，可能返回EAGAIN(缓冲区空)
 * 所以，解码器可能1次产生多个frame，要优先处理解码器中已有的帧，然后再开始读取新的packet，充分利用了解码器的缓冲区
 * 
 * 本质：这里是利用了流水线技术，
 *      扔个packet给decoder去解，而我不用同步等待，而是去读已经解好的frame_queue，这样就并发操作了
 */
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    /*  大循环  */
    for (;;) {
        /* 1.流连续情况下获取解码后的帧 */
        if (d->queue->serial == d->pkt_serial) {    //  先判断是否是统一播放序列的数值
            do {
                if (d->queue->abort_request)
                    return -1;  //  是否请求退出
                /*  获取解码    */
                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) { // 收到数据
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;  //  best_effort_timestamp 是vlc的概念. 是经过解码器，算法计算出来的值，主要是“尝试为可能有错误的时间戳猜测出适当单调的时间戳”
                                                                            //  大部分情况下还是frame->pts，或者就是frame->pkt_dts
                                                                            // 0 frame的pts使用pkt_dts , 1 frame保留自己的pts， -1 best_effort_timestamp
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                /*  如果frame->pts正常，则先将其从pkt_timebase转成{1,frame->sample_rate}
                                    pkt_timebase实质就是stream->time_base    */
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                /*  如果frame->pts不正常，则使用上一帧更新的next_pts和next_pts_nb
                                    转成{1,frame_sample_rate}   */
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);

                            if (frame->pts != AV_NOPTS_VALUE) {
                                /*  根据当前帧的pts和nb_sampples预估下一帧的pts */
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;    // 设置timebase
                            }
                        }
                        break;
                }
                /*  检查解码是否已经结束，解码结束返回 0 */
                if (ret == AVERROR_EOF) {    // 解码器遇到了文件结尾
                    d->finished = d->pkt_serial;         // 1. 标记当前序列已完成
                    avcodec_flush_buffers(d->avctx);     // 2. 清空解码器缓冲区
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        /*  2.获取一个packet，如果播放顺序不一致（数据不连续）则过滤掉“过时”的packet    */
        do {
            if (d->queue->nb_packets == 0)  //  如果没有数据可读，则唤醒read_thread
                /**
                 * read_thread不停地向packet读取，如果满了就休眠，否则就继续读取packet，
                 * video_thread则是不停的解析每一个packet，如果空了就唤醒read_thread，否则就继续读，
                 * 那么就会出现，一边读一遍解析的情况，所以这个同步是为了协调双方速度的
                 */
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {    //  如果还有 pending 的pakcet则使用它
                d->packet_pending = 0;
            } else {
                /*  阻塞式读取packet，这里好理解，就是读packet并获取serial  */
                int old_serial = d->pkt_serial; // 保存旧的序列号，d->pkt_serial是decoder的serial
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)   // 获取新的packet和序列号
                    return -1;
                /**
                 * @brief 就是说，这一步是，因为seek打断了继续读取当前packet转换为frame的过程，
                 * 然后发现正在转换的这个packet与新seek要求读取的packet不是同一个，所以对其进行释放
                 * 并读取新的packet
                 */
                if (old_serial != d->pkt_serial) {   // 序列号变化（比如发生了seek）
                    avcodec_flush_buffers(d->avctx); // 清空里面的缓存真
                    d->finished = 0;                 // 重置为 0 
                    d->next_pts = d->start_pts;      // 主要用在了 audio
                    d->next_pts_tb = d->start_pts_tb;// 主要用在了 audio
                }
            }
            if (d->queue->serial == d->pkt_serial) // // 检查packet队列和当前packet的序列号是否一致. 旧版是根据 flush_pkt.data 来判断是否是下一个
                break;
            av_packet_unref(d->pkt);    // 不一致则释放这个packet
        } while (1);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !d->pkt->data) {
                    d->packet_pending = 1;  //  
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        } else {
            if (d->pkt->buf && !d->pkt->opaque_ref) {
                FrameData *fd;

                d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!d->pkt->opaque_ref)
                    return AVERROR(ENOMEM);
                fd = (FrameData*)d->pkt->opaque_ref->data;
                fd->pkt_pos = d->pkt->pos;
            }

            /* 3.将packet送入解码器 */
            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;  //  
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

/**
 * @note 基础数据结构 (每个格子代表一帧)
 * FrameQueue (size=3):
┌──────────┬──────────┬──────────┐
│ Frame[0] │ Frame[1] │ Frame[2] │
├──────────┼──────────┼──────────┤
│ 已显示帧 │ 当前帧   │ 待显示帧 │
└──────────┴──────────┴──────────┘

 * @note 正常播放流程
 *步骤1: 初始状态
┌──────────┬──────────┬──────────┐
│ Frame1   │ Frame2   │ Frame3   │
│(已显示)  │(当前显示)│(待显示)  │
└──────────┴──────────┴──────────┘
        ↑
        rindex   

步骤2: 显示完Frame2后移动rindex
┌──────────┬──────────┬──────────┐
│ Frame2   │ Frame3   │ Frame4   │
│(已显示)  │(当前显示)│(待显示)  │
└──────────┴──────────┴──────────┘
                    ↑
                  rindex
*暂停前:
┌──────────┬──────────┬──────────┐
│ Frame2   │ Frame3   │ Frame4   │
│(已显示)  │(当前显示)│(待显示)  │
└──────────┴──────────┴──────────┘
           ↑
         rindex
         rindex_shown=0
         keep_last=1

*@note 暂停机制
暂停时:
┌──────────┬──────────┬──────────┐
│ Frame2   │ Frame3   │ Frame4   │
│(已显示)  │(持续显示)│(待显示)  │
└──────────┴──────────┴──────────┘
           ↑
         rindex
         rindex_shown=1  // 标记为已显示但不移动
 */
static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) // 初始化帧队列
{
    int i;
    memset(f, 0, sizeof(FrameQueue));   // 帧队列置空
    if (!(f->mutex = SDL_CreateMutex())) { // 创建互斥锁
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {  // 创建条件变量
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    /**
     * @brief 这里 FFMIN(max_size, FRAME_QUEUE_SIZE) 是为了适应不同帧队列选择最小的滑动窗口，
     * @param FRAME_QUEUE_SIZE 是所有队列中最大的那个值(16)
     * @note 但初始化时传入的 max_size 是各自需要的大小,FFMIN 确保不会超过这个上限
     * @note 视频队列：min(3, 16) = 3
     * @note 音频队列：min(9, 16) = 9
     * @note 字幕队列：min(16, 16) = 16
     * 
     * @param paused 控制是否继续播放
     * @param keep_last 控制当前帧是否保留
     * 两者互相独立但共同作用
     * @brief 设计的目的是:即使暂停，也需要保护机制防止帧被意外释放,即使播放，也需要保护机制确保平滑过渡
     * @note 字幕设为 0 的原因:
     *          字幕不需要保持平滑过渡,切换时可以直接清除,不参与音视频同步,显示时间由PTS控制，不需要额外保护机制
     * @note 音视频设为 1 的原因：
     *          需要保证画面/声音的平滑过渡,暂停时需要保持当前状态,参与音视频同步，需要保护机制,避免播放卡顿或跳跃
     */
    f->pktq = pktq;                     // 关联相应的packet队列，用于序列同步
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);// 设置最大大小
    f->keep_last = !!keep_last;         // 这里就是把 大于1 的数字准换成1。 如果keep_last = 5，!5 = 0， !0 = 1
    
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc())) // 创建 frame 并进行初始化.
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/*
 * 获取帧队列中下一帧的指针,但不移除该帧
 * 参数:
 *     f: 帧队列结构体指针
 * 返回值:
 *     返回队列中下一帧的地址(指针)
 * 说明:
 *     1. 使用(f->rindex + f->rindex_shown)计算下一帧的实际位置
 *     2. 对max_size取模以处理循环队列的边界情况
 *     3. &f->queue[...]返回该位置帧的内存地址
 */
static Frame *frame_queue_peek(FrameQueue *f) 
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

/*
 * 获取帧队列中当前显示帧的指针
 * 参数:
 *     f: 帧队列结构体指针
 * 返回值:
 *     返回当前显示帧的地址(指针)
 * 说明:
 *     1. rindex指向当前读取位置
 *     2. 直接返回rindex位置帧的地址
 *     3. 这是一个只读操作,不会修改队列状态
 */
static Frame *frame_queue_peek_last(FrameQueue *f) // 获取当前显示的帧，而不影响队列的状态
{
    return &f->queue[f->rindex]; // 返回帧队列中最后一个已读取的帧，而不是从队列中移除它
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {   /*  检查是否需要退出    */
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)  /*  检查是否需要退出    */
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f) // 将一个帧推入帧队列
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    /*  如果需要保留最后一帧(keep_last=1)且当前帧还未显示 
        仅标记该帧为已显示，不进行实际的队列操作
        这通常用于暂停时保留当前画面   */
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1; // 标记当前帧已显示
        return;  // 直接返回，不实际移除帧
    }
    /*  释放当前帧引用的资源    */
    frame_queue_unref_item(&f->queue[f->rindex]);
    /*  更新读索引，如果到达最大大小则循环回0   */
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    /*   线程安全操作   */
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
/* 
 * 计算帧队列中未显示帧的数量
 * 参数:
 *     f: 帧队列结构体指针
 * 返回值:
 *     帧队列中未显示的帧数量
 * 计算方法: 
 *     用队列总大小(f->size)减去已显示帧的读索引位置(f->rindex_shown)
 */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);  // 终止 packet 队列， packetQueue 的 abort_request 被置为1
    frame_queue_signal(fq);        // 唤醒 Frame 队列，以便退出
    SDL_WaitThread(d->decoder_tid, NULL); // 等待解码线程退出
    d->decoder_tid = NULL;         // 线程 ID 重置
    packet_queue_flush(d->queue);  // 清空 packet 队列，并释放数据 
}

static inline void fill_rectangle(int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

/**
 * @brief 用来计算视频在屏幕上的显示矩形的。
 * 它考虑了视频的原始尺寸、宽高比（纵横比），以及屏幕的尺寸，以确定视频应该如何在屏幕上显示
 * 
 * @param rect: 输出参数，用于存储计算出的显示矩形
 * @param scr_xleft, scr_ytop, scr_width, scr_height: 屏幕上可用于显示的区域
 * @param pic_width, pic_height: 原始图片（视频帧）的宽度和高度
 * @param pic_sar: 图片的样本宽高比（Sample Aspect Ratio）
 */
static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

/**
 * @brief 获取输入参数 format(FFmpeg像素格式)在SDL中的像素格式，取到的SDL像素格式存储在输出参数 sdl_pix_fmt 中
 */
static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map); i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(SDL_Texture **tex, AVFrame *frame)
{
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    /*  根据 AVFrame 的格式确定对应的 SDL 像素格式和混合模式    */
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    /*  如果需要，重新分配纹理以匹配新的帧尺寸和格式    */
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_IYUV:
            /** 对于 YUV 格式（SDL_PIXELFORMAT_IYUV
             *  处理正常和翻转的行序（正负行距）
             *  使用 SDL_UpdateYUVTexture 分别更新 Y、U、V 平面
             */
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
                /**
                 *  对于其他格式
                 *  处理正常和翻转的行序
                 *  使用 SDL_UpdateTexture 更新整个纹理
                 */
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8) // 这个函数只在 SDL 版本 2.0.8 及以上时有效
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC; // 默认模式设置
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) { // 帧格式和颜色空间检查
        /*  颜色范围和色彩空间判断  */
        if (frame->color_range == AVCOL_RANGE_JPEG) // JPEG
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709) // BT.709
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) // BT.601 
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode); /* FIXME: no support for linear transfer.设置 YUV 转换模式 */
#endif
}

/**
 * @brief 这个函数的主要作用是将解码后的视频帧和字幕正确地渲染到屏幕上，处理了视频和字幕的同步、缩放和位置调整等问题
 */
static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    /*  获取最新视频帧  */
    vp = frame_queue_peek_last(&is->pictq); // 获取当前显示的帧，而不影响队列的状态

    /*  如果使用 Vulkan 渲染器，直接调用 Vulkan 显示函数并返回  */
    if (vk_renderer) {
        vk_renderer_display(vk_renderer, vp->frame); // 
        return;
    }

    /*  处理字幕    */
    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            /*  获取当前字幕帧  */
            sp = frame_queue_peek(&is->subpq);

            /*  检查字幕是否应该显示    */
            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;

                    /*  如果字幕尺寸未设置，使用视频帧尺寸  */
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    /*  重新分配字幕纹理
                        用于显示 texture 还没有分配
                        SDL_QueryTexture无效
                        目前 texture 的 width, height, format 和 要显示的 Frame 不一致    */
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    /*  处理每个字幕矩形    */
                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        /*  裁剪字幕矩形以确保在视频范围内  */
                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        /*  初始化字幕颜色转换上下文
                            第一个参数 *img_convert_ctx 对应形参 struct SwsContext *context
                            如果 context 是NULL，调用 sws_getContext() 重新获取一个 context
                            如果 context 不是NULL，检查其他项输入参数是否和 context 中存储的各参数一样，若不一样，
                            则先释放 context 再按照新的输入参数重新分配一个 context.
                            若一样，直接使用现有的 context    */
                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                            0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }

                        /*  锁定纹理并进行颜色转换  */
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            /**
                             * @brief 主要是用来做视频像素格式和分辨率转换
                             * @li 优势：同一函数内实现：1.图像色彩空间转换 2.分辨率缩放 3.前后图像滤波处理 
                             * @li 劣势：效率相对较低，不如 libyuv 或 shader,
                             * 关联函数：
                             * @param sws_getContext: 分配和返回一个 SwsContext, 需要传入输入参数和输出参数
                             * @param sws_getCachedContext: 检查传入的上下文是否可以用，如果不可用，则重新分配一个，如果可用则返回传入的
                             * @param sws_freeContext: 释放 SwsContext 结构体
                             * @param sws_scale: 转换一帧图像
                             */
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }

    /*  计算显示矩形    */
    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

    /*  设置 SDL YUV 转换模式   */
    set_sdl_yuv_conversion_mode(vp->frame);

    /*  如果视频帧未上传，上传到纹理    */
    if (!vp->uploaded) {
        if (upload_texture(&is->vid_texture, vp->frame) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    /*  渲染视频帧  */
    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);

    /*  重置 SDL YUV 转换模式   */
    set_sdl_yuv_conversion_mode(NULL);

    /*  渲染字幕    */
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        /*  一次性渲染所有字幕*/
        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
        /*  分别渲染每个字幕矩形*/
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                               .y = rect.y + sub_rect->y * yratio,
                               .w = sub_rect->w * xratio,
                               .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

/**
 * @brief 用于显示音频可视化效果的
 * 它有两种主要的显示模式：
 * 波形图（SHOW_MODE_WAVES）
 * 频谱图（默认）
 * 
 * 计算显示索引
 *      如果不是暂停状态，计算当前输出样本的中心位置
 *      考虑音频缓冲区大小和时间差异来精确定位
 * 
 * 波形图模式（SHOW_MODE_WAVES）
 *      为每个通道绘制波形
 *      使用 SDL 绘制白色波形线
 *      在通道之间绘制蓝色分隔线
 * 
 * 频谱图模式
 *      重新分配纹理以适应当前显示尺寸
 *      使用 RDFT 计算频谱数据
 *      将频谱数据转换为可视化图像
 *      使用 SDL 纹理渲染频谱图
 * 
 * 特殊处理
 *      处理暂停状态  
 *      处理内存分配错误，在失败时切换到波形图模式
 * 
 * 渲染
 *      对于波形图，直接在 SDL 渲染器上绘制
 *      对于频谱图，使用 SDL 纹理进行渲染
 */
static void video_audio_display(VideoState *s) // 用于显示音频可视化效果的
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    /*  计算RDFT位数，确保足够覆盖显示高度  */
    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1); // 计算频率数量

    /* compute display index : center on currently output samples 
        获取音频通道数  */
    channels = s->audio_tgt.ch_layout.nb_channels;
    nb_display_channels = channels;
    if (!s->paused) {
        /*  非暂停状态，计算显示索引    */
        int data_used= s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation 
           考虑上次缓冲计算后的时间差   */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        /*  计算起始索引    */
        i_start= x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);

        /*  波形模式下找到最佳起始点    */
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            /*  波形模式的起始点查找逻辑    */
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        /*  暂停状态，使用上次的起始索引    */
        i_start = s->last_i_start;
    }

    if (s->show_mode == SHOW_MODE_WAVES) {
        /*  波形显示模式    */
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // 设置绘制颜色为白色

        /* total height for one channel */
        h = s->height / nb_display_channels; // 每个通道的总高度
        /* graph height / 2 */
        h2 = (h * 9) / 20; // 图形高度的一半
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        /*  绘制通道分隔线  */
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255); // 设置绘制颜色为蓝色

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(s->xleft, y, s->width, 1);
        }
    } else {
        /*  频谱显示模式    */
        int err = 0;
        /*  重新分配纹理（如果需要）    */
        if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        /*  重置 x 位置（如果需要） */
        if (s->xpos >= s->width)
            s->xpos = 0;
        nb_display_channels= FFMIN(nb_display_channels, 2); // 限制显示通道数

        /*  如果 RDFT 位数改变，重新初始化 RDFT */
        if (rdft_bits != s->rdft_bits) {
            /*  RDFT 重初始化逻辑   */
            const float rdft_scale = 1.0;
            av_tx_uninit(&s->rdft);
            av_freep(&s->real_data);
            av_freep(&s->rdft_data);
            s->rdft_bits = rdft_bits;
            s->real_data = av_malloc_array(nb_freq, 4 *sizeof(*s->real_data));
            s->rdft_data = av_malloc_array(nb_freq + 1, 2 *sizeof(*s->rdft_data));
            err = av_tx_init(&s->rdft, &s->rdft_fn, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !s->rdft_data) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            /*  RDFT 计算和频谱显示 */
            float *data_in[2];
            AVComplexFloat *data[2];
            SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
            uint32_t *pixels;
            int pitch;
            /*  对每个通道进行 RDFT 计算    */
            for (ch = 0; ch < nb_display_channels; ch++) {
                /*  RDFT 计算逻辑   */
                data_in[ch] = s->real_data + 2 * nb_freq * ch;
                data[ch] = s->rdft_data + nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data_in[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                s->rdft_fn(s->rdft, data[ch], data_in[ch], sizeof(float));
                data[ch][0].im = data[ch][nb_freq].re;
                data[ch][nb_freq].re = 0;
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. 
             * 将 RDFT 结果渲染到纹理   */
            if (!SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                /*  频谱渲染逻辑    */
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
                    int b = (nb_display_channels == 2 ) ? sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                                        : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            /*  将纹理复制到渲染器  */
            SDL_RenderCopy(renderer, s->vis_texture, NULL, NULL);
        }
        /*  在非暂停状态下移动 x 位置   */
        if (!s->paused)
            s->xpos++;
    }
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudioDevice(audio_dev);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);
    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (vk_renderer)
        vk_renderer_destroy(vk_renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
    for (int i = 0; i < nb_vfilters; i++)
        av_freep(&vfilters_list[i]);
    av_freep(&vfilters_list);
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&input_filename);
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = screen_width  ? screen_width  : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

static int video_open(VideoState *is)
{
    int w,h;

    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;

    if (!window_title)
        window_title = input_filename;
    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    is->width  = w;
    is->height = h;

    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is) // 显示当前的视频帧或音频可视化效果
{
    /*  检查并初始化视频窗口    */
    if (!is->width)
        video_open(is);

    /*  设置渲染器颜色和清除    */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // 渲染器设置为黑色
    SDL_RenderClear(renderer);

    /*  选择显示内容    */
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO) // 如果有音频流且显示模式不是纯视频模式
        video_audio_display(is); // 用于显示音频可视化效果的
    else if (is->video_st) // 如果有视频流
        video_image_display(is); // 
    SDL_RenderPresent(renderer);
}

/**
 * @brief 实际流逝时间 × 速度
 */
static double get_clock(Clock *c)
{
    /**
     * @brief 检查序列号是否匹配
     * 用于处理seek等导致的播放序列改变
     * 如果序列号不匹配，返回NAN表示时钟无效
     */
    if (*c->queue_serial != c->serial)
        return NAN;
    /*  如果时钟暂停，直接返回暂停时的时间点    */
    if (c->paused) {
        return c->pts;
    } else {
        /*  获取当前的物理时间（以秒为单位）    */
        double time = av_gettime_relative() / 1000000.0;
        /**
         * @brief 计算实际的时钟时间：
         * @param c->pts_drift: 时钟漂移基准点（上一次设置时钟时的基准时间）
         * @param time: 当前物理时间
         * @param (time - c->last_updated): 从上次更新到现在经过的物理时间
         * @param (1.0 - c->speed): 速度调整因子
         *  @li  - 当speed = 1.0时，(1.0 - speed) = 0，不影响时钟
         *  @li  - 当speed = 2.0时，(1.0 - speed) = -1.0，时钟走得更快
         *  @li  - 当speed = 0.5时，(1.0 - speed) = 0.5，时钟走得更慢
         * 
         * 重新理解：
         * c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed)可以理解为：
         * 偏移量 + 当前时间 + 持续时间*相对速度
         * 原先的设计是：当前时间 + 持续时间*相对速度
         * 这里的偏移量是校正 frame 的解码时间消耗，解决了延迟补偿、硬件差异适应、音视频同步维护、速度变化平滑处理、系统时间抖动处理
         */
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);   // 可以重写为 pts_drift + time * speed： 时钟值 = 基准时间(pts_drift) + 流逝的时间(time)【 = 实际流逝时间 × 速度】


    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;                   // 保存时间戳
    c->last_updated = time;         // 用来记录上次的时间戳
    c->pts_drift = c->pts - time;   // 时间戳（pts）与上次更新时间戳（last_updated）的差值，表示从 pts 的基点到 last_updated 的偏移
    c->serial = serial;             // 当前使用的队列
}

/**
 * @brief 这个函数用于设置时钟的当前时间。
 * @param pts 参数是要设置的播放时间戳
 * @param serial 是用于跟踪时钟更新的序列号
 */
static void set_clock(Clock *c, double pts, int serial) // 用于设置时钟的当前时间
{
    double time = av_gettime_relative() / 1000000.0; // 获取一个连续单增的时间标尺
    set_clock_at(c, pts, serial, time);              // 实际设置时钟
}

/**
 *@brief 改变播放速度：

 * 通过修改时钟速度，可以实现快进、慢放等功能。
 * 例如，speed = 2.0 会使播放速度加快一倍，speed = 0.5 会使播放速度减慢一半。
 *
 * 同步调整：
 * 在音视频同步过程中，可能需要微调时钟速度以保持同步。
 *
 * 实现特殊效果：
 * 在某些场景下，可能需要动态调整播放速度以实现特定的视觉或听觉效果。
 *
 * 保持时钟连续性： 
 * 在改变速度时，先调用 set_clock 确保时钟的当前值是最新的，然后再改变速度，这样可以避免时钟值的突变。
 *
 * 适应不同的播放需求：
 * 在直播、流媒体等场景中，可能需要动态调整播放速度以适应网络条件或缓冲状态。
 */
static void set_clock_speed(Clock *c, double speed) // 改变播放速度
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial; // 确定是哪一个packet_queue
    set_clock(c, NAN, -1);          // 这里 NAN 就是表示未定义或者不可表示的值  0.0/0.0
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) { // 用于确定主同步类型
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}
/**
 * @brief 调整外部时钟速度，以平衡音视频包的缓冲状态
 */
static void check_external_clock_speed(VideoState *is) { 
    /* 当缓冲不足时减慢时钟 */
   if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
       is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
       set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
       /*  当缓冲过多时加快时钟 */
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
       set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
    /*  正常情况下逐渐调整到正常速度  */
       double speed = is->extclk.speed;
       if (speed != 1.0)
           set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed)); // 使用 (1.0 - speed) / fabs(1.0 - speed) 来决定调整的方向（加速或减速）
   }
}

/* seek in the stream */
/**
* @brief 请求一个seek操作
* @param is VideoState结构体,包含播放器状态
* @param pos 目标seek位置
* @param rel 相对位置偏移量
* @param by_bytes 是否按字节seek(1:按字节, 0:按时间戳)
*/
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    /*  检查是否已经有pending的seek请求 */
    if (!is->seek_req) {
        /*  设置seek目标位置    */
        is->seek_pos = pos;
        /*  设置相对偏移量  */
        is->seek_rel = rel;
        /*  清除字节seek标志    */
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        /*  如果是按字节seek,设置对应标志   */
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        /*  标记有seek请求  */
        is->seek_req = 1;
        /*  唤醒读取线程处理seek请求    */
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video 
   切换视频的暂停/播放状态  */
static void stream_toggle_pause(VideoState *is)
{
    /*  如果当前是暂停状态，准备恢复播放    */
    if (is->paused) {
        /**
         * 更新frame_timer，补偿暂停期间的时间
         * 当前时间减去视频时钟最后更新时间，得到暂停的持续时间
         */
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        /**
         * 检查是否支持暂停操作
         * AVERROR(ENOSYS)表示功能未实现，即不支持暂停
         */
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            /*  如果支持暂停操作，将视频时钟的暂停状态设为0（播放状态） */
            is->vidclk.paused = 0;
        }
        /**
         * 重新设置视频时钟，保持时钟连续性
         * 使用当前时钟值和序列号更新时钟
         */
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    /*  更新外部时钟    */
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    /**
     * 切换所有时钟的暂停状态
     * 将主暂停状态和所有时钟(音频、视频、外部)的暂停状态取反
     */
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

/*  切换暂停    */
static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

static void update_volume(VideoState *is, int sign, double step)
{
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

/*
* 计算视频帧显示的目标延迟时间
* 参数:
*     delay: 当前的延迟时间
*     is: 播放器状态结构体指针
* 返回值:
*     返回经过同步计算后的目标延迟时间
*/
static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
     /* 只有在视频不是主同步源时才需要调整延迟 */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        /* 
        * 如果视频是从设备,需要通过以下方式修正较大的延迟:
        * 1. 重复帧(增加延迟)
        * 2. 丢弃帧(减少延迟)
        */
        diff = get_clock(&is->vidclk) - get_master_clock(is); // 计算视频时钟和主时钟的差值

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        /* 
        * 计算同步阈值
        * 在最小阈值和最大阈值之间选择一个合适的值
        * 这个阈值用来判断是否需要进行同步调整
        */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay)); // 同步阈值，根据延迟时间动态调整
        /* 
        * 只有当差值是有效的且小于最大帧持续时间时才进行处理 
        */
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                /* 视频落后太多,需要减小延迟追赶进度 */
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                /* 视频太快了,且当前延迟大于帧重复阈值,增加延迟 */
                delay = delay + diff;
            else if (diff >= sync_threshold)
                /* 视频太快了,但当前延迟较小,将延迟加倍 */
                delay = 2 * delay;
        }
    }
    /* 记录日志,输出延迟时间和音视频差值 */
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

    return delay;
}

/* 计算上一帧需要持续的duration,这里有校正算法 */
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) { //  同一播放序列，序列连续的情况下
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) // duration 数值异常
         || duration <= 0   // pts 值没有递增时
         || duration > is->max_frame_duration   // 超过了最大帧范围
         )  // 异常情况
         /* 1.异常情况用之前入队列的时候的帧间隔(主要根据帧率去计算)    */
            return vp->duration;    // 异常时以帧时间为基准(1s/帧率)
        else   // 2. 相邻pts 
            return duration;    // 使用两帧pts差值计算duration,一般情况下也是走的这个分支
    } else {    // 不同播放序列，序列不连续则返回0
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
/* 用于显示每一帧的函数 */
static void video_refresh(void *opaque, double *remaining_time) 
{
    VideoState *is = opaque;  // 获取视频状态结构体
    double time;

    Frame *sp, *sp2; // 用于字幕处理的帧指针

    /*  如果未暂停且使用外部时钟同步,检查并调整外部时钟速度 */
    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) 
        check_external_clock_speed(is); // 调整外部时钟速度，以平衡音视频包的缓冲状态

    /*  处理音频可视化显示  */
    if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is); // 显示音频可视化
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    /*  处理视频显示    */
    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
            // 尝试从帧队列中获取一帧视频，但没有图片可显示
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp; 

            /* dequeue the picture */
            /* 从队列中取出图片 */
            /**
             * @brief 帧队列使用了两个重要索引：
             * @param rindex：读取索引
             * @param rindex_shown：标记当前帧是否已显示
             * 
             * @brief 循环处理流程:
             * 先通过peek系列函数获取帧引用
             * 处理完成后调用frame_queue_next
             * frame_queue_next会更新rindex，实现循环
             * 原数据在unref后被释放
             * 新的数据会被写入到释放的位置
             * 
             * @li av_frame_move_ref：移动frame引用而不是拷贝数据
             * @li av_frame_unref：减少引用计数
             * @li 当引用计数为0时自动释放相关内存
             */
            lastvp = frame_queue_peek_last(&is->pictq); // 读取上一帧,已经显示的帧(当前屏幕上的帧)
            vp = frame_queue_peek(&is->pictq);      // 读取待显示帧(待显示的帧)

            /*  检查序列号是否匹配
                在做seek操作、flush操作时，会更新 srial  */
            if (vp->serial != is->videoq.serial) {
                /*  如果不是最新的播放序列，则将其出队列，以尽快读取最新序列的帧    */
                frame_queue_next(&is->pictq);
                goto retry;
            }

            /*  如果序列号变化,重置帧计时器 */
            if (lastvp->serial != vp->serial)   // 新的播放序列重置当前时间
                is->frame_timer = av_gettime_relative() / 1000000.0;

            /*  如果暂停,直接跳到显示   */
            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            /* 计算当前帧持续时间和目标延迟 */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {   // 还没有到播放时间
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time); // 更新延迟时间
                goto display;
            }

            /*  更新帧计时器    */
            /**
             * @param frame_timer 表示理论上帧应该播放的时间点
             * 通过 +=delay 累加来计算下一帧的理论播放时间
             * @param 实际播放时间 是由 av_gettime_relative() 获取的系统时间
             */
            is->frame_timer += delay; // 至少该到vp播放的时间了.  
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time; // 实时更新时间

            /*  更新视频时钟    */
            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);   // 更新video的时钟

            /*  处理帧丢弃  */
            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);  // 下一次要显示的帧(排在vp后面)
                duration = vp_duration(is, vp, nextvp); // 计算上一帧需要持续的duration,这里有校正算法
                /**
                 * @param !is->step : 不是逐帧播放模式
                 * @param framedrop>0: 允许丢帧(framedrop>0)
                 *        (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) ： 或者允许丢帧且视频不是主时钟源
                 * @param  time > is->frame_timer + duration: 当前时间已经超过了这一帧应该播放的时间
                 */
                if(!is->step && (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
                    is->frame_drops_late++; // 增加丢帧计数，统计丢帧情况
                    frame_queue_next(&is->pictq); // 将当前帧出队，丢帧操作
                    goto retry; // 检测下一帧
                }
            }

            /*  处理字幕    */
            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial
                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            /*  移动到下一帧    */
            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            /*  如果是单步模式且未暂停,切换暂停状态 */
            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
display:
        /* display picture */
        /* 显示图片 */
        if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display(is);
    }
    is->force_refresh = 0;

    /*  显示状态信息    */
    if (show_status) {
        AVBPrint buf;
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0; // audio quenue size
            vqsize = 0; // video queue size
            sqsize = 0; // subtitle queue size
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                      "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
                      get_master_clock(is),
                      (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                      av_diff,
                      is->frame_drops_early + is->frame_drops_late,
                      aqsize / 1024,
                      vqsize / 1024,
                      sqsize);

            if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(NULL, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, NULL);

            last_time = cur_time;
        }
    }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))  // 检查队列是否有可写空间
        return -1;          // Frame 队列满了则返回-1

    // 执行到这不说说可以获取到了可写入的Frame
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);    // 将src中所有数据拷贝到dst中，并复位src，拷贝后的src_frame就是无效的
    frame_queue_push(&is->pictq);   // 更新写索引位置
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    /*  1.获得解码后的视频帧    */
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1; // 返回 -1 意味着要退出解码线程，所以要分析decoder_decode_frame什么情况下返回-1

    if (got_picture) {  // 2. 分析获取到的帧是否要drop掉，该机制的目的是放入帧队列前先drop掉过时的视频帧
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;    // 计算出秒为单位的pts

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        /**
         * @brief 允许丢帧的3种情况
         * 1. 控制是否丢帧的开关变量是 framedrop 为1，则始终判断是否丢帧
         * 2. framedrop 为0， 则始终不丢帧
         * 3. framedrop 为-1(默认值)，则在主时钟不是video的时候，判断是否丢帧。
         */
        if (framedrop>0     // 允许drop掉帧——早期丢帧过程
            || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {   //  非视频同步模式
            /**
             * @brief drop的条件
             * @note !isnan(diff): 
             *          当前 pts 和 主时钟的差值是有效值
             * @note fabs(diff)<AV_NOSYNC_THRESHOLD: 
             *          差值在可同步范围内，这里设置的是10s，意思是如果差值太大，这里就不管了，
             *          可能流本身录制的时候就有问题，这里不能随便把帧都drop掉
             * @note diff - is->frame_last_filter_delay < 0; 
             *          考虑滤镜延迟后依然落后
             * @note is->viddec.pkt_serial == is->vidclk.serial:
             *           解码器的serial和时钟的serial相同(未seek)，即是至少显示了一帧图像，
             *           因为只有显示的时候才调用update_video_pts()设置到video clk的serial；
             * @note is->videoq.nb_packets:
             *           至少packetqueue有1个包，表示还存在待播放的包
             */
            if (frame->pts != AV_NOPTS_VALUE) {     //  pts值有效
                /**
                 * diff > 0: 帧比主时钟快（提前）
                 * diff < 0: 帧比主时钟慢（延迟）
                 */
                double diff = dpts - get_master_clock(is);  // 差值有效
                if (!isnan(diff) &&     // 差值有效
                    /*  比如阈值0.1s，只处理延迟不超过0.1s的情况    */
                    fabs(diff) < AV_NOSYNC_THRESHOLD &&     // 差值在可同步范围内
                    /**
                     * 考虑滤镜延迟后，帧依然是延迟的
                     * 说明这帧即使现在处理也会显示得太晚
                     */
                    diff - is->frame_last_filter_delay < 0 &&   //  和过滤器有关系
                    /*  确保是同一个播放序列（比如没有seek）    */
                    is->viddec.pkt_serial == is->vidclk.serial &&   // 同一序列的包
                    /*  队列还有帧，丢掉当前帧不会导致画面卡住  */
                    is->videoq.nb_packets) {    //  帧队列至少有1帧数据
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    const AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    for (i = 0; i < renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }

    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);


    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = frame->format;
    par->time_base           = is->video_st->time_base;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
    par->frame_rate          = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;

    ret = avfilter_init_dict(filt_src, NULL);
    if (ret < 0)
        goto fail;

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                           "ffplay_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
        goto fail;
    if (!vk_renderer &&
        (ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_color_spaces),
                                AV_OPT_TYPE_INT, sdl_supported_color_spaces)) < 0)
        goto fail;

    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (autorotate) {
        double theta = 0.0;
        int32_t *displaymatrix = NULL;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd)
                displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0)
                INSERT_FILT("hflip", NULL);
            if (displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);
    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0)
        goto end;

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int audio_thread(void *arg)
{
    VideoState *is = arg;   
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    Frame *af;          // 音频帧
    int last_serial = -1;// 上一个pkt的序列号
    int reconfigure;    // 是否重新配置过滤器
    int got_frame = 0;  // 是否读取到解码帧
    AVRational tb;      // timebase 时基
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        /*  1.读取解码帧  */
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                /*  2.设置时基为采样率 */
                tb = (AVRational){1, frame->sample_rate};   //  设置为  sample_rate为timebase
                /*  3.检查是否需要重新配置过滤器    */
                reconfigure =
                    /*  音频格式发生变化  */
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                                   frame->format, frame->ch_layout.nb_channels)    ||
                    /*  声道布局发生变化  */
                    av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                    /*  采样率发生变化  */
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    /*  packet 序列号发生变化  */
                    is->auddec.pkt_serial               != last_serial;

                /*  4.如果需要重新配置  */
                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1)); // 获取源音频格式
                    av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));  // 获取目标音频格式
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);
                    /*  更新音频源格式参数  */
                    is->audio_filter_src.fmt            = frame->format;
                    ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                    if (ret < 0)
                        goto the_end;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;
                    /*  重新配置音频过滤器  */
                    if ((ret = configure_audio_filters(is, afilters, 1)) < 0) // 如果格式不匹配时会再次调用: force_output_format=1,强制输出硬件格式  
                        goto the_end;
                }
            /*  5. 将解码帧送入过滤器   */
            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;
            /*  6. 不断从过滤器获取处理后的帧   */
            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL; // 获取FrameData结构
                tb = av_buffersink_get_time_base(is->out_audio_filter); // 获取过滤器的时间基

                /*  获取可写 Frame 节点  */
                // Frame队列的结构(环形缓冲区) [已播放的帧|待播放的帧|可写入位置|...]
                if (!(af = frame_queue_peek_writable(&is->sampq))) 
                    goto the_end;
                /*  设置 Frame 参数   */
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);   // 帧时间戳
                af->pos = fd ? fd->pkt_pos : -1;                                            // 帧位置
                af->serial = is->auddec.pkt_serial;                                         // 数据包序列号
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate}); // 帧持续时间
                /*  将帧移入Frame队列   */
                av_frame_move_ref(af->frame, frame); // 将解码帧移动到 FrameData 结构中
                frame_queue_push(&is->sampq);        // 将 FrameData 结构放入 Frame队列
                /* 检查音频包的连续性，用于处理seek或切换音频流的情况   */
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
    avfilter_graph_free(&is->agraph);   // 释放过滤器图
    av_frame_free(&frame);  // 释放解码帧
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg) // 创建解码器线程
{
    packet_queue_start(d->queue); // 初始化或重新启动一个数据包队列
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg); // 使用 SDL 库创建一个新的线程
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int video_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    double pts;                         // pts
    double duration;                    // 帧持续时间
    int ret;
    /*  1. 获取时间基(用于计算每帧持续时间) */
    AVRational tb = is->video_st->time_base; // 获取 stream timebase
    /*  2. 获取帧率，以便计算每帧 picture 的 duration  */
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;        // 过滤器图
    AVFilterContext *filt_out = NULL, *filt_in = NULL;  // 过滤器输入输出的上下文
    /*  用于检测视频帧参数变化  */
    int last_w = 0;                     // 上一帧宽度
    int last_h = 0;                     // 上一帧高度
    enum AVPixelFormat last_format = -2;// 上一帧格式
    int last_serial = -1;               // 上一帧序列号
    int last_vfilter_idx = 0;           // 上一帧滤镜索引

    if (!frame)
        return AVERROR(ENOMEM);

    /*  循环取出视频解码的帧数据    */
    for (;;) {
        /*  3.解码获取一帧视频画面  */
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;   // 解码结束，什么时候回结束
        if (!ret)           // ret = 0, 没有解码得到画面，什么情况下会得不到解后的帧
            continue;
        /*   检查视频帧参数是否变化 */
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            /*  如果变化了，重新配置视频过滤器  */
            avfilter_graph_free(&graph);    // 释放滤镜图
            graph = avfilter_graph_alloc(); // 重新分配一个滤镜图
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads; // 设置滤镜图线程数
            /*  配置新的视频过滤器  */
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            /*  更新状态    */
            filt_in  = is->in_video_filter;  // 获取输入滤镜
            filt_out = is->out_video_filter; // 获取输出滤镜
            last_w = frame->width;           // 更新视频帧的宽
            last_h = frame->height;          // 更新视频帧的高
            last_format = frame->format;     // 更新视频帧的格式
            last_serial = is->viddec.pkt_serial; // 更新视频帧的序列号
            last_vfilter_idx = is->vfilter_idx; // 更新滤镜索引
            frame_rate = av_buffersink_get_frame_rate(filt_out); // 获取帧率
        }
        /* 4. 向过滤器添加帧 */
        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;
        /*  5. 从过滤器获取处理后的帧   */
        while (ret >= 0) {
            FrameData *fd;
            /*  记录帧返回时间(用于同步)    */
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;
            /*  获取过滤器处理后的帧    */
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }
            /*  获取帧的附加数据，主要用于获取packet位置信息，用于后续的定位和管理功能    */
            fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;    
            /*  计算过滤器延迟——每个 frame 过滤所需时间  */
            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
            /** 计算帧持续时间和换算pts值为秒
             *  1/帧率 = duration 单位秒，没有帧率时则设置为0，有帧率计算出帧间隔
             */
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            /*  根据 AVStream timebase 计算出 pts 值，单位为秒  */
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            /*   6.将处理后的帧放入显示队列    */
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            /*  释放 frame 对应的数据   */
            av_frame_unref(frame);  // 正常情况下 frame 对应的 buf 应被 av_frame_move_ref
            /*  检查序列号是否变化(用于处理seek)    */
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
 the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);  // 释放frame
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock

 * 如果sync_type为video，
 * 则返回所需的样本数量以获得更好的同步或外部主时钟 */
static int synchronize_audio(VideoState *is, int nb_samples)    // 通过 synchronize_audio() 调整音频采样数,补偿音频与主时钟的差异
{
    int wanted_nb_samples = nb_samples; // 想要的样本数

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) { // 如果不是主时钟，则尝试移除或添加样本以纠正时钟
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;   
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 * 
 * @brief 并没有真正意义上的 decode 代码，最多是进行了重采样
 * 1. 从 sampq 取一帧，必要时丢帧。如发生了 seek ，此时 serial 会不连续，就需要丢帧处理
 * 2. 计算这一帧， 通过 av_samples_get_buffer_size 可以方便计算出结果
 * 3. 获取这一帧的数据。对于 frame 格式和输出设备不同的，需要重采样；如果格式相同，则直接拷贝指针输出即可。
 * 总之，需要 audio_buf 中保存于输出设备格式相同的音频数据
 * 4. 更新 audio_clock ,audio_clock_serial. 用于设置 audclk
 */
static int audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size; // 原始数据大小和重采样后的数据大小
    av_unused double audio_clock0;      // 用于记录更新前的音频时钟
    int wanted_nb_samples;              // 期望的采样点数
    Frame *af;                          // 音频帧

    /*  暂停状态直接返回-1, sdl_audil_callback 会处理为输出静音   */
    if (is->paused)
        return -1;

    do {
#if defined(_WIN32) // Windows平台下的特殊处理：检查音频缓冲区超时
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            /*  如果回调时间超过了预期的一半缓冲时间，则返回错误    */
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif  
        /*  从帧队列中获取一个可读帧,必要时丢帧. af 指向可读帧    */
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);  // 确保帧序列号匹配

    /*  根据 frame 中指定的音频参数获取缓冲区的大小   */
    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    /*  获取期望的采样点数（用于音频同步）
        若同步时钟是音频，则不调整样本数；
        否则根据同步需要调整样本数  */
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);   // 音频数据直接送到声卡播放,由声卡控制播放速度

    /*  检查是否需要重采样  */
    /**
     * is->audio_tgt 是 SDL 可接受的音频帧数，是 audio_open() 中取得的参数
     * 在 audio_open() 函数中又有" is->audio_src = is->audio_tgt "
     * 此处表示: 如果 frame 中的音频参数 == is->audio_src == is->audio_tgt
     * 那音频重采样的过程就免了(因此时 is->swr_ctr 是 NULL )
     * 否则使用 frame(源) 和 is->audio_tgt(目标) 中的音频参数来设置 is->swr_ctx
     * 并使用 frame 中的音频参数来赋值 is->audio_src
     */
    if (af->frame->format        != is->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        /*  需要重采样，配置重采样上下文    */
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,    // 目标输出
                            &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,   // 数据源
                            0, NULL);
        /*  初始化重采样器  */
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        /*  更新音频源格式信息  */
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    /*  如果需要重采样  */
    if (is->swr_ctx) {
        /*  重采样输入参数1: 输入音频样本数是 af->frame->nb_samples */
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        /*  重采样输入参数2: 输入音频缓冲区 */
        uint8_t **out = &is->audio_buf1;
        /*  计算输出缓冲区需要的大小    */
        /*  重采样输出参数1: 输出音频缓冲区尺寸 */
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        /*  重采样输出参数2: 输出音频缓冲区 */
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        /*  设置重采样补偿. 如果 frame 中的样本数经过校正，则条件成立  */
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        /*  分配输出缓冲区  */
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        /*  执行重采样. 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数  */
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        /*  设置音频缓冲区和大小. 重采样返回的一帧音频数据大小(以字节为单位)    */
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        /*  不需要重采样，直接使用原始数据. 未经重采样，则将指针指向 frame 中的音频数据  */
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    /*  更新音频时钟    */
    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size; // 返回处理后的数据大小
}

/* prepare a new audio buffer */
/**
 * @brief SDL音频回调函数，用于准备新的音频缓冲区
 * 也就是说，主动解码视频帧，会因为加码的效率导致播放的丢帧问题，但是人眼是不会感知到的
 * 而如果同样主动解码音频帧，人耳是会对断音感知到，所以选择回调函数形式，需要时去准备数据。
 * 那么这样不会造成需求和准备、返回数据的时间延迟吗？
 * 
 * SDL 和 FFmpeg 都为此做了优化处理：双缓冲机制
 * SDL音频系统：
 * 缓冲区A：正在播放
 * 缓冲区B：准备新数据
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;    // audio_size:解码后的音频大小，len1:本次处理的数据长度

    audio_callback_time = av_gettime_relative();    // 记录回调函数调用的时间戳

    /*  持续处理，直到填满SDL要求的音频缓冲区   */
    while (len > 0) {
        /*  如果当前音频缓冲区已经用完  */
        if (is->audio_buf_index >= is->audio_buf_size) {
            /*  解码音频帧，获取更多的音频数据  */
           audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence
                   如果解码出错，输出静音数据 */
               is->audio_buf = NULL;
               /*   设置最小缓冲区大小，确保是帧大小的整数倍    */
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
                /*  如果不是视频显示模式，更新音频波形显示  */
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0; // 重置缓冲区索引
        }
        len1 = is->audio_buf_size - is->audio_buf_index;    // 计算本次可以处理的数据长度
        if (len1 > len)
            len1 = len; // 确保不超过SDL要求的长度
        /*  音频数据拷贝：未静音且音量最大的情况    */
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            /*  其他情况：先填充静音数据    */
            memset(stream, 0, len1);
            /*  如果未静音且有音频数据，则混合音频数据（应用音量）  */
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        /*  更新处理进度    */
        len -= len1;    // 减少待处理的数据长度
        stream += len1; // 移动目标缓冲区指针
        is->audio_buf_index += len1; // 移动源缓冲区索引
    }
    /*  计算还未写入的音频数据大小  */
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
     /* SDL音频驱动假定有两个周期 */
    if (!isnan(is->audio_clock)) {
        /*  更新音频时钟，考虑硬件缓冲区大小和未写入数据的延迟  */
        /**
         * [SDL内部缓冲区] [SDL外部缓冲区] [audio_write_buf]
         * |--hw_buf_size--|--hw_buf_size--|--write_buf_size--|
         * 已经在播放        等待播放         还未写入SDL的数据
         */
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        /*  同步外部时钟到音频时钟  */
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

/**
 * @brief audio_open()填入期望的参数，打开音频设备后，将实际的音频参数存入输出参数is->audio_tgt中，
 * 后面音频播放线程会用到此参数，使用此参数将原始音频数据重采样，转换为音频设备支持的格式
 * (如果是，单纯提高采样率适配硬件，对音质没有影响，不会提高，仅为了适配)
 * 1. 参数初始化：
    wanted_channel_layout(函数参数) ----→ wanted_nb_channels
                                            ↓
    环境变量(SDL_AUDIO_CHANNELS) --------→ 覆盖
                                            ↓
    2. 设置 wanted_spec：
                                    wanted_nb_channels
                                            ↓
                                    wanted_spec.channels
                                    wanted_spec.freq
                                    wanted_spec.format
                                            ↓
    3. SDL打开设备：
                                    wanted_spec
                                            ↓
                                SDL_OpenAudioDevice()
                                            ↓
                                        spec(实际参数)
 */
static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;    // wanted_spec——期望的参数， spec——实际参数
    int next_nb_channels = 0;          // 期望的声道数
    const char *env;                    // 环境变量
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;   // 采样率数组索引
    int wanted_nb_channels = wanted_channel_layout->nb_channels;        // 期望的声道数

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {  // 若环境变量有设置，优先从环境变量去的声道数和声道布局，覆盖初始参数
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) { // 如果不是原生顺序，重新设置默认布局
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    /*  根据channel_layout获取nb_channels，当传入参数wanted_nb_channels不匹配时，此处会做出修正 */
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    /*  设置期望参数   */ 
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--; // 从采样率数组中找到第一个不大于传入参数wanted_sample_rate的值
    /**
     * @brief 音频采样格式有两大类：planar 和 packed，假设一个双声道音频文件，
     * 一个左声道采样点记作L，一个右声道采样点记作R，
     * planar存储格式：(plane1)LLLL...LLL; (plane2) RRR...RRRR
     * packet存储格式：(plane1)LRLRLRLR...LRLR;
     * 在这两种采样类型下，又细分多种采样格式，如AV_SAMPLE_FMT_S16,AV_SAMPLE_FMT_16P等
     * 注意: SDL 2.0目前不支持planer格式
     * channel_layout 是 int64_t类型，表示音频声道布局，每bit代表一个特定的声道，参考channel_layout.h的定义，一目了然
     * 数据量(bits/秒) = 采样率(Hz) * 采样深度(bit) * 声道数
     */
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    /**
     * 一次读取多长的数据
     * SDL_AUDIO_MAX_CALLBACKS_PER_SEC 一秒最多回调次数，避免频繁的回调
     * Audio buffer size in samples (power of 2)
     */
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    /**
     * @brief 打开音频设备并创建音频处理线程。期望的参数是wanted_spec ,实际得到的硬件参数是 spec
     * @param 1. SDL 提供两种使用音频设备去的音频数据方法
     * a) push, SDL以特定的频率调用回调函数，在回调函数中取得音频数据
     * b) pull，用户程序以特定的频率调用SDL_QueAudio(),向音频设备提供数据。此种情况wanted_spec.callback = NULL
     * 
     * @param 2. 音频设备打开后播放静音，不启动回调，调用SDL_PauseAudio(0)后启动回调，开始正常播放音频
     * SDL_OpenAudioDevice()第一个参数为NULL时，等价于SDL_OpenAudio()
     */
    /*  尝试打开设备    */ 
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());

        /*  如果失败，尝试降级参数  */
        // 1. 先尝试降低声道数
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        // 2. 如果声道数降到0，则尝试降低采样率
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    /*  检查打开音频设备的实际参数：采样格式    */
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    /*  检查打开音频设备的实际参数：声道数  */
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    /**
     * wanted_spec是期望的参数，spec是实际的参数，wanted_spec和spec都是SDL中的结构
     * 此处audio_hw_params是FFmpeg中的参数，输出参数供上级函数使用
     * audio_hw_params保存的参数，就是在做重采样的时候要转成的格式
     */
    // 将协商好的硬件参数保存到audio_hw_params
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
        return -1;
    /*  audio_hw_params->frame_size 这里只是计算一个采样点占用的字节数  */
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    // 比如2帧数据，一帧就是1024个采样点， 1024 * 2 * 2 * 2 = 8096字节
    return spec.size;   // 硬件内部缓存的数据字节，samples * channels * byte_per_sample
}

/**
* @brief 创建硬件加速设备上下文
* @param device_ctx 输出参数，用于存储创建的硬件设备上下文
* @return 成功返回0，失败返回错误码
*/
static int create_hwaccel(AVBufferRef **device_ctx)
{
    enum AVHWDeviceType type;   // 硬件设备类型
    int ret;                    // 返回值
    AVBufferRef *vk_dev;        // Vulkan设备引用

    *device_ctx = NULL;         // 初始化输出参数

    if (!hwaccel)               // 如果没有指定硬件加速类型，直接返回
        return 0;
    /* 选择硬件加速类型 */
    type = av_hwdevice_find_type_by_name(hwaccel);
    if (type == AV_HWDEVICE_TYPE_NONE)  // 如果找不到对应类型，返回不支持错误
        return AVERROR(ENOTSUP);

    /* 尝试获取Vulkan渲染器的硬件设备 */
    ret = vk_renderer_get_hw_dev(vk_renderer, &vk_dev);
    if (ret < 0)
        return ret;

    /* 尝试从Vulkan设备派生出指定类型的硬件设备 */
    ret = av_hwdevice_ctx_create_derived(device_ctx, type, vk_dev, 0);
    if (!ret) // 如果派生成功，直接返回
        return 0;

    /* 如果错误不是"功能未实现"，返回错误码 */
    if (ret != AVERROR(ENOSYS))
        return ret;

    /* 如果无法从Vulkan派生，输出警告信息 */
    av_log(NULL, AV_LOG_WARNING, "Derive %s from vulkan not supported.\n", hwaccel);

    /*  创建硬件设备上下文  */
    ret = av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0);
    return ret;
}

/* open a given stream. Return 0 if OK 
 * 负责打开和初始化特定的媒体流（音频、视频或字幕） */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    int ret = 0;
    int stream_lowres = lowres;

    /*  检查流索引是否有效  */
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    /*  分配新的 AVCodecContext */
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    /*  从流参数复制编解码器参数到上下文，解码时用    */
    /**
     * @param AVCodecParameters (codecpar) 主要用于存储编解码器的静态参数信息
     * 属于 AVStream，跟随流的生命周期
     * @param AVCodecContext (avctx) 包含了更多运行时需要的状态信息和上下文数据
     * 独立分配的，可以单独管理其生命周期，便于解码器的初始化和清理
     * @brief 线程安全考虑:
     * 多个线程可能需要同时访问同一个流的编解码器参数
     * 通过复制出独立的 AVCodecContext，每个解码线程都有自己的上下文，避免了数据竞争
     */
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    /*  设置 pkt_timebase，设置时间基，是帧率的倒数   */
    /** 
     * @brief  假设30fps
     * fps = 30帧/秒
     * time_base = 1/30秒/帧  // 这是最直观的理解
     * 
     * time_base = {1, 90000}  // 表示时间单位是1/90000秒
     * // 不是90000fps, 而是把1秒分成了90000份
     * 
     * // 对于30fps视频
     * 一帧的duration = 90000/30 = 3000个time_base单位
     * 实际时间 = 3000 * (1/90000) = 1/30秒
     */
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    /*  ffmpeg 自动查找解码器  */
    codec = avcodec_find_decoder(avctx->codec_id);

    /**
     * @brief 根据媒体类型设置相应的流索引和强制编解码器名称
     * @note  记录流索引：用于记住每种类型最后处理的流、在切换流或重新打开流时有用
     * @note  强制编解码器: forced_codec_name 允许用户指定使用特定的编解码器,而不是使用 FFmpeg 自动选择的编解码器
     */
    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
    }
    /*  如果用户指定了强制编解码器，尝试查找.  */
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);

    /*  如果找不到编解码器，报错并退出.  */
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    /*  设置 编解码器 ID 和 低分辨率模式  */
    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        /*  处理低分辨率设置 */
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    /*  设置快速解码标志（如果适用），会牺牲一些解码质量来换取更快的解码速度    */
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    /*  过滤和设置编解码器选项opts   */
    ret = filter_codec_opts(codec_opts, avctx->codec_id, ic,
                            ic->streams[stream_index], codec, &opts, NULL);
    if (ret < 0)
        goto fail;

    /*  设置线程数和其他选项    */
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres) // 如果低分辨率
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    /**
     * @brief 为视频设置硬件加速的上下文
     * @note 硬件加速类型：
     * @li GPU 解码 (NVIDIA NVDEC, AMD VCE)
     * @li 专用硬件解码器 (Intel QSV)
     * @li 移动设备硬件解码器 (Android MediaCodec)
     * @li Apple 视频工具箱 (VideoToolbox)
     */
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {     //  当创建解码器上下文时，这些参数被复制过去
        ret = create_hwaccel(&avctx->hw_device_ctx);   //  创建硬件加速设备上下文
        if (ret < 0)
            goto fail;
    }

    /*  打开编解码器  */
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }

    /*  检查选项    */
    ret = check_avoptions(opts);
    if (ret < 0)
        goto fail;

    /*  重置 EOF 标志和设置流丢弃策略   */
    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    /**
     * @brief 关于 audio 参数的统一流程
     * @note  1.记录Frame格式(is->audio_filter_src)
     * @note  2.第一次配置过滤器(configure_audio_filters, force_output_format=0)
     * @li         - 输入：Frame格式        - 输出：暂时不固定
     * @note  3.打开音频设备(audio_open)
     * @li      输入：用户期望格式           - 输出：输出：SDL实际支持的格式(is->audio_tgt)
     * @note  4.第二次配置过滤器(configure_audio_filters, force_output_format=1)
     * @li          - 输入：Frame格式       - 输出：强制使用SDL硬件格式(is->audio_tgt)
     */
    /*  根据媒体类型进行特定处理    */
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        {
            /*  配置音频过滤器和输出    */
            AVFilterContext *sink;  // case 后不能直接定义变量，这里的{}是将其定义为局部变量，作用域仅限于{}，然后继续执行{}后的内容
            /*  1.这3步首先记录解码后的 Frame 格式参数（输入参数）   */
            is->audio_filter_src.freq           = avctx->sample_rate;   // 获得 Frame 采样率(每秒采样的次数)
            ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);   // 输入声道布局
            if (ret < 0)
                goto fail;
            is->audio_filter_src.fmt            = avctx->sample_fmt;    // 输入采样格式——采样点的存储格式(整数、浮点数、平面)

            /*  2. 配置并创建能完成采样格式转换的过滤器  */
            if ((ret = configure_audio_filters(is, afilters, 0)) < 0) //  audio_open 之前: afilters=0,使用frame格式作为输入
                goto fail;

            /*  3.获取过滤器的输出格式，这些参数后面 audio_thread 会用来配置 SDL   */
            sink = is->out_audio_filter;                                // 配置过滤器
            sample_rate    = av_buffersink_get_sample_rate(sink);       // 输出采样率
            ret = av_buffersink_get_ch_layout(sink, &ch_layout);        // 输出声道布局
            if (ret < 0)
                goto fail;
        }

        /* prepare audio output——在这里打开音频的硬件设备 */
        /* 调用audio_open打开sdl音频输出，实际打开的设备参数保存在audio_tgt,返回值表述输出设备的缓冲区大小
           用过滤器的输出格式去打开音频设备,获取硬件实际支持的格式   */
        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        /*  设置硬件缓冲区大小和初始化音频缓冲区参数    */
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;  // 暂且将数据源参数等同于目标输出参数
        /*  初始化audio_buf相关参数
            audio_buf: 从要输出的 AVFrame 中取出的音频数据PCM，如果有必要，则对该数据重采样
            audio_write_buf_size: audio_buf剩余的buffer长度，即audio_buf_size - audio_buf_index  */
        is->audio_buf_size  = 0;    // audio_buf的总大小
        is->audio_buf_index = 0;    // 下一次可读的audio_buf的index位置

        /**
         * @brief init averaging filter
         * @note 初始化音频同步平均滤波器，用于平滑音频同步差异
         * @param AUDIO_DIFF_AVG_NB 是采样数量
         * @param 0.01 目标权重 
         * @note 作用：避免因瞬时差异导致的频繁调整  */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;

        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        /**
         * @brief 设置音频同步阈值，由于我们没有精确的音频数据填充FIFO，故只有当同步差异大于此阈值时才进行校正
         * @note 硬件缓冲区 = 4096 bytes
         *       每秒字节数 = 44100Hz * 2channels * 2bytes = 176400 bytes/sec
         *       阈值 = 4096/176400 ≈ 0.023秒
         * @note 只有当音视频差异超过这个阈值才进行同步调整
         */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec; // 将硬件缓冲区大小转换为秒数

        /*  保存音频流的索引和指针, 用于后续音频数据的读取和处理  */
        is->audio_stream = stream_index;    // 获取audio的stream索引
        is->audio_st = ic->streams[stream_index];   // 获取audio的stream指针

        /*  初始化并启动ffplay封装的音频解码器  */
        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
            goto fail;

        /*  处理无时间戳的格式  */    
        if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }

        /*  启动音频解码线程    */
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0) 
            goto out;

        /*  开始音频播放    */
        SDL_PauseAudioDevice(audio_dev, 0); 
        break;
    case AVMEDIA_TYPE_VIDEO:
        /*  初始化并启动视频解码器  */
        is->video_stream = stream_index;    // 获取 video 的 stream 索引
        is->video_st = ic->streams[stream_index];   // 获取 video 的 stream 指针

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        is->queue_attachments_req = 1;  // 使能请求mp3、aac等音频文件的封面
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        /*  初始化并启动字幕解码器  */
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    /*  错误处理：释放资源  */
    avcodec_free_context(&avctx);
out:
    /*  清理和返回  */
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx) // 中断回调函数，用于检查是否应该中断当前的解码操作
{
    VideoState *is = ctx; // VideoState *is = (VideoState *)ctx; 可能需要强转
    return is->abort_request; // 返回 0 继续操作, 返回 非0 表示中断
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||                 // 没有该流，流没有打开，没有相应的流返回逻辑 true
           queue->abort_request ||          // 请求退出
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||       // 是ATTACHED_PIC
           queue->nb_packets > MIN_FRAMES                           // packet 数大于25
           && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0); // 满足PacketQueue 总时长为 0，或者 总时长超过 1s
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                 || !strncmp(s->url, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg) // 该线程从本地硬盘or网络获取流数据
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];  // 由于 stream 需要 index，所以这个数组用来保存多个 stream 的索引
    AVPacket *pkt = NULL;
    int64_t stream_start_time;      // 流的开始时间，用于记录媒体流的起始时间戳
    int pkt_in_play_range = 0;      // 标记当前包是否在播放范围内. 当设置了播放起止时间时，用来判断当前包是否应该被处理
    const AVDictionaryEntry *t;     // 用于存储和访问媒体文件的元数据（metadata）. 元数据(metadata)是对媒体文件的描述性信息，存储在封装格式中
    SDL_mutex *wait_mutex = SDL_CreateMutex(); // 
    int scan_all_pmts_set = 0;      // PMT (Program Map Table) 扫描标志. 用于控制是否扫描所有的 PMT 表，主要用于 MPEG-TS 流
    int64_t pkt_ts;                 // packet 的时间戳. 用于记录当前处理的数据包的时间信息

    /*  准备流程    */
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    /*  初始化索引 */
    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc(); // 创建 packet
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context(); // 创建并初始化 avformatcontext 这个上下文
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    /**
     * @brief 这是设置一个中断检查机制
     * decode_interrupt_cb 是一个回调函数，在 FFmpeg 执行耗时操作时会周期性调用它
     * @note  读取网络流时检查是否超时、检查用户是否请求退出、检查是否出现错误需要中断操作、防止程序在IO操作时永久阻塞
     */
    ic->interrupt_callback.callback = decode_interrupt_cb;  // 中断回调函数，用于检查是否应该中断当前的解码操作
    ic->interrupt_callback.opaque = is;                     // 传递参数(宽度、名字、最大、最小...)
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {  // 如果没有设置扫描所有PMT选项
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE); // 设置扫描所有PMT选项
        scan_all_pmts_set = 1;
    }
    //打开媒体文件，读取文件头信息，初始化解复用器。初步确定流的数量和基本信息
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts); 
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    /** 
     * PMT (Program Map Table) 是MPEG传输流中的节目映射表
     * 这行代码在完成PMT扫描后清除相关选项
     * AV_DICT_MATCH_CASE 表示大小写敏感的匹配
     */
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    remove_avoptions(&format_opts, codec_opts); // 从一个字典中移除另一个字典中存在的所有键
    /**
     * @brief 这是一个安全检查，确保所有传入的选项都被正确处理
     * 检查format_opts字典是否为空
     * 如果字典不为空，说明有未处理的选项参数
     * 返回负值表示存在错误（未识别的选项）
     */
    ret = check_avoptions(format_opts); // 检查一个字典是否为空，如果不为空，则报告第一个未处理的选项
    if (ret < 0)
        goto fail;

    /*  保存 avformatContext */
    is->ic = ic;    // 保存解封装上下文

    /**
     * @brief genpts是一个选项，用于生成缺失的PTS（显示时间戳）
     * AVFMT_FLAG_GENPTS 标志告诉FFmpeg为 没有PTS的帧生成时间戳
     * 这对于一些格式不完整或损坏的视频很有用
     * |= 操作是将这个标志添加到现有标志中
     */
    if (genpts) // 是否为缺失 pts 的帧补全 pts
        ic->flags |= AVFMT_FLAG_GENPTS;

    /* 在获取媒体文件中各个流(stream)的详细信息 */
    if (find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic->nb_streams;   // 记录流的数量
 
        /**
         * @brief 为每个流分配 AVDictionary，根据 codec_opts 设置每个流的解码器选项，返回配置好的 opts 数组
         * @li 读取一些数据包来分析流的具体参数
         * @li 确定编解码器参数
         * @li 获取更详细的流信息（如帧率、采样率等）
         */
        err = setup_find_stream_info_opts(ic, codec_opts, &opts); // 用于更高级的兼容性，填充opts参数，可以暂时不设置
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error setting up avformat_find_stream_info() options\n");
            ret = err;
            goto fail;
        }

        /**
         * @brief
         * 读取足够的数据包来获取每个流的编解码参数
         * 确定流的时间基准和持续时间
         * 检测视频流的帧率
         * 识别音频流的采样率、通道数和格式
         * 提取额外的编解码器数据（如 H.264 的 SPS 和 PPS）
         */
        err = avformat_find_stream_info(ic, opts);  

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb) // 重置 EOF 标志
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end.
                                 //这是一个临时解决方案，可能是为了避免 ffplay 错误地使用 avio_feof() 来检测文件结束

    if (seek_by_bytes < 0) // 设置基于字节的搜索标志
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);
    /**
     * @brief 设置最大帧持续时间
     * @param AVFMT_TS_DISCONT 如果时间戳可能不连续，设为 10 秒
     * @param 3600.0 否则，设置1小时
     */
    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0; 
    /**
     * @brief 从媒体文件元数据中获取标题, 组合文件名生成窗口标题
     */
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it
     * 执行媒体文件的初始搜索操作
     */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time. 设置了初始时间，解复用使用 */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        /*  seek 的制定的位置开始播放   */
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    /* 是否为网络实时流媒体 */
    is->realtime = is_realtime(ic); // 判断实时流的协议类型

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0); // 显示格式信息（如果需要）

    /*   遍历查找AVStream,根据用户指定来查找流   */
    for (i = 0; i < ic->nb_streams; i++) { // 遍历所有流并设置初始丢弃状态
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1) // wanted_stream_spec 用户指定的流类型
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i; // 首先流要存在
    }

    for (i = 0; i < AVMEDIA_TYPE_NB; i++) { // 检查流选择器匹配情况
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }

    /*  选择最佳流  */
    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = show_mode; // 设置显示模式
    /*  从待处理流中获取相关参数，设置默认窗口大小（如果有视频流）    */
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        /**
         * 根据流和帧的宽高比猜测帧的样本宽高比
         * 由于帧宽高比由解码器设置，但流宽高比由解复用器设置，因此这两者可能不相等
         * 此函数会尝试返回待显示帧应当使用的宽高比值
         * 基本逻辑是优先使用流宽高比（前提是值是合理的），其次使用帧宽高比
         * 这样，流宽高比（容器设置，易于修改）可以覆盖
         */
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);    // 这里只是设置了 default_width, default_height 变量的大小，没有真正的改变窗口的大小
    }

    /* open the streams 
     * 打开相应的视频、音频解码器。在此会打开相应解码器，并创建响应的解码线程。   */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]); // stream_component_open 打开和初始化媒体流组件的关键函数
    }
    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    /* 设置显示模式 */
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    /*  显示字幕流 */
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }
    /*  流打开失败 */
    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;    // 如果是实时流

    /*   for 循环   */
    for (;;) {
        /*  检测是否退出线程    */
        if (is->abort_request)
            break;
        /*  检测是否暂停/继续   */
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic); // 网络流的时候有用
            else
                av_read_play(ic); // 恢复读取
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL  // 对RTSP或MMSH协议的特殊处理
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        /* 检测是否 seek */
        if (is->seek_req) { //  是否有 seek 请求
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables
// FIXME: +-2是因为在生成seek_pos/seek_rel变量时，舍入没有在正确的方向上进行

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url); // 搜索失败，记录错误
            } else {
                /**
                 * seek 的时候，要把原先的数据清空，并重启解码器，put flush_pkt 的目的是告知解码线程需要 reset decoder
                 */
                if (is->audio_stream >= 0) 
                    packet_queue_flush(&is->audioq); // 搜索成功，清空所有数据包队列
                // 旧版ffmpeg， 放入flush pkt,用来打开新的一个播放序列，解码器读取到flush_pkt也清空解码器
                // packet_queue_put(&is->audioq, &flush_pkt);
                /**
                 * @brief 关于新版和旧版的设计思路差别
                 * @param 旧版：通过空包显式通知
                 * packet_queue -> flush_pkt -> decoder知道要重置
                 * @param 新版：通过serial隐式通知
                 * packet_queue(serial变化) -> decoder检测到serial变化 -> 重置
                 * 
                 * 减少不必要的包处理、降低内存占用
                 * serial机制足够处理状态转换、不需要额外的标记机制
                 * 减少特殊情况处理、统一状态管理方式
                 */
                if (is->subtitle_stream >= 0)
                    packet_queue_flush(&is->subtitleq);
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {  // 根据搜索标志设置时钟
                   set_clock(&is->extclk, NAN, 0);        // 如果是字节搜索，设置时钟为NAN（Not a Number）
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0); // 否则，设置时钟为搜索目标时间
                }
            }
            is->seek_req = 0;   // 重置seek请求标志为0，表示 seek 请求已经被处理完成
            is->queue_attachments_req = 1;  // 设置队列附件请求标志为1，表示需要处理附件.seek 后设置为1是因为位置改变了，可能需要重新加载新位置相关的附加数据
            is->eof = 0;        // 重置eof标志为0，表示播放未结束.seek 操作后，即使之前已经到达文件末尾，现在位置已经改变，所以需要重置这个标志
            if (is->paused)
                step_to_next_frame(is); // 如果本身是pause状态的则显示一帧继续暂停
        }
        /*  检测 video是否为attached_pic  */
        if (is->queue_attachments_req) { // 检查是否有队列附件请求
            /*  attached_pic 附带的图片。 比如说一些MP3， AAC 音频文件附带的专辑封面    */
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) { // 检查视频流是否存在，且具有附加图片
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)  // 尝试引用视频流的附加图片
                    goto fail;
                packet_queue_put(&is->videoq, pkt); // 将附加图片数据包放入视频队列
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);  // 在附加图片后放入一个空包，标记结束
            }
            is->queue_attachments_req = 0;  // 重置队列附件请求标志
        }

        /* if the queue are full, no need to read more
         * 如果队列已满，无需继续读取 */
        /*  缓存队列有足够的包，不需要继续读取数据 */
        /**
         * 缓冲区满有2种可能：
         * 1. audioq, videoq, subtitleq 三个PacketQueue 的总字节数达到了 MAX_QUEUE_SIZE(15M，这里是一个经验计算值)
         * 2. 音频、视频、字幕流都已有够用的包(stream_has_enough_packets)，注意要三者同时成立
         */
        if (infinite_buffer<1 &&    // 缓冲区不是无限大
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE  // 缓冲区已满
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms
               PacketQueue 默认情况下会有大小限制，达到这个大小后，就需要等待10ms，以让消费者——解码线程能有时间消耗  */
            SDL_LockMutex(wait_mutex);
            /*  如果没有唤醒则超时10ms退出，比如在 seek 操作时这里会被唤醒  */
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        /* 检查是否需要循环播放或退出 */
        /**
         * @brief 判断已完成的条件需要同时满足
         * 1. 不在暂停状态
         * 2. 音频未打开；或者打开了，但是解码已经解完了所有packet，
         * 自定义的解码器(decoder) serial 等于 PacketQueue 的 serial，并且 FrameQueue 中没有数据帧
         * PacketQueue.serial->packet.serial->decoder.ptk_serial
         * decoder.finished = decoder.pkt_serial
         * is->auddec.finished == is->audioq.serial 最新的播放顺序的 pacekt 都解码完毕
         * frame_queue_nb_remaining(&is->sampq) == 0 对应解码后的数据也播放完毕
         * 3. 视频未打开；或者打开了，但是解码已解完所有packet，自定义的解码器(decoder) serial 等于PacketQueue 的 serial，
         * 并且 FrameQueue 中没有数据帧
         */
        if (!is->paused     // 非暂停
            &&  // 这里的执行是因为 stream 读取完毕后，插入空包所致（旧版）
            (!is->audio_st // 没有音频流
                 || (is->auddec.finished == is->audioq.serial       // 或者音频播放完毕
                 && frame_queue_nb_remaining(&is->sampq) == 0)) 
            && (!is->video_st   // 没有视频流
                 || (is->viddec.finished == is->videoq.serial       // 或者视频播放完毕
                 && frame_queue_nb_remaining(&is->pictq) == 0))) {
            /* 如果需要循环播放 */
            if (loop != 1 && (!loop || --loop)) {
                /* 跳回到开始位置 */
                /*  stream_seek 不是 ffmpeg 的函数，是 ffplay 封装的，每次 seek 的时候都会调用 */
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);   // 如果没有设定则从头开始，否则从指定位置开始。
            } else if (autoexit) {
                /* 如果设置了自动退出，则退出程序 */
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        /*  尝试从输入上下文读取一个数据包  */
        ret = av_read_frame(ic, pkt); // 调用不会释放pkt的数据,需要我们自己去释放 packet 的数据
        if (ret < 0) {
            /*  读取失败的情况  */
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) { // 如果是文件结束（EOF）且之前没有设置过EOF标志
                /*  对每个活跃的流插入一个空包，表示流结束  */
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                /*  设置EOF标志 */
                is->eof = 1;
            }
            /*  检查I/O错误 */
            if (ic->pb && ic->pb->error) {
                if (autoexit)
                    goto fail; // 如果设置了自动退出，则跳转到失败处理
                else
                    break; // 否则跳出循环
            }
            /*  如果不是致命错误，等待一小段时间后继续  */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            /*  读取成功，重置EOF标志   */
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard
         * 检查数据包是否在用户指定的播放范围内，如果是则入队，否则丢弃  */
        stream_start_time = ic->streams[pkt->stream_index]->start_time; // 获取流的开始时间
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts; // 获取数据包的时间戳，优先使用 pts，如果 pts 无效则使用 dts
        /*  判断数据包是否在播放范围内  */
        /**
         * @param duration == AV_NOPTS_VALUE
         * 如果duration是AV_NOPTS_VALUE，表示没有指定播放时长
         * 此时pkt_in_play_range为true，表示所有包都在播放范围内
         * @param a. 计算流的起始时间偏移
         * stream_start_time_offset = (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)
         * @param b. 计算包的实际时间戳（减去流起始时间）
         * actual_ts = pkt_ts - stream_start_time_offset
         * @param c. 转换为秒
         * seconds = actual_ts * av_q2d(ic->streams[pkt->stream_index]->time_base)
         * @param d. 减去开始时间（转换为秒）
         * start_time_seconds = (start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000.0
         * seconds_from_start = seconds - start_time_seconds
         * @param e. 比较与持续时间（转换为秒）
         * duration_seconds = duration / 1000000.0
         * @param f. 最终判断
         * if (seconds_from_start <= duration_seconds) {
         *      pkt_in_play_range = true; }
         */
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        /*  处理音频数据包  */
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
            /*  处理视频数据包（不包括附加图片）  */
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
            /*  处理字幕数据包      */
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
            /*  丢弃不需要的数据包  */
        } else {
            av_packet_unref(pkt);
        }
    }

    /*  退出线程处理    */
    ret = 0;
 fail:
    /*  如果输入上下文存在但尚未被设置到 VideoState 中，关闭它  */
    if (ic && !is->ic)
        avformat_close_input(&ic);

    /*  释放数据包内存  */
    av_packet_free(&pkt);
    /*  如果有错误发生（ret != 0）  */
    if (ret != 0) {
        SDL_Event event;

        /*  创建一个退出事件    */
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        /*  将退出事件推送到 SDL 事件队列   */
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex); // 销毁等待互斥锁
    return 0;
}

/**
 * 打开并初始化一个媒体流
 *
 * 这个函数负责创建一个新的 VideoState 结构体，并初始化媒体流的各个组件。
 * 它设置了解复用、解码和渲染所需的各种参数和线程。
 *
 * @param filename 要打开的媒体文件的路径或 URL
 * @param iformat  指定的输入格式，如果为 NULL，则自动检测
 *
 * @return 成功时返回初始化好的 VideoState 指针，失败时返回 NULL
 *
 * 注意：
 * - 这个函数通常在播放器启动新的媒体文件时调用
 * - 它会启动读取线程，该线程负责持续从媒体源读取数据
 * - 返回的 VideoState 结构包含了播放过程中所需的所有上下文信息
 */
static VideoState *stream_open(const char *filename,
                               const AVInputFormat *iformat) // 
{
    VideoState *is; // 指向播放器

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    /*  对 VideoState 内的参数进行初始化    */
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->filename = av_strdup(filename); // 用户定义的文件名，保存到 VideoState
    if (!is->filename)                  // 表明没有播放文件
        goto fail;
    is->iformat = iformat;              // 保存文件格式相关信息
    is->ytop    = 0;                    // 保存坐标信息
    is->xleft   = 0;

    /**
     * @brief start video display. 初始化帧队列
     * @note 为什么音频也需要 keep_last=1：
     * @li 音频需要平滑过渡，避免爆音
     * @li 在暂停/恢复播放时，保留最后一帧可以帮助实现平滑过渡
     * @li 音频解码和播放是通过回调机制实现的，保留最后一帧有助于处理回调边界情况
     */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0) // 初始化音频、视频、字幕的 packet 队列
        goto fail;
    /* 创建唤醒同步信号量   */ 
    if (!(is->continue_read_thread = SDL_CreateCond())) { // 当读取数据队列满了后,进入休眠,可以通过该 continue 唤醒读线程
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    /**
     * 初始化时钟
     * 时钟序列->queue_serial, 实际上指向的是is->videoq.serial
     */
    init_clock(&is->vidclk, &is->videoq.serial);    // packet和frame都是统一的单独的serial，frame继承packet的serial
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    /*  初始化音量  */
    if (startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);   // 确定音量范围
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME); // 将 0-100 的音量值映射到 SDL 的音量范围（0 到 SDL_MIX_MAXVOLUME）
    is->audio_volume = startup_volume;                  // 起始音量,可设置
    is->muted = 0;                                      // 非静音
    /*  设置音视频同步方式 */
    is->av_sync_type = av_sync_type; 
    /*  创建读线程  */
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is); // ffplay 播放器创建读进程
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }
    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
 the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}


static void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *is)
{
    int next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = next;
    }
}

/**
 * @line 解码层的问题
 * packet延迟 -> 解码慢 -> 渲染延迟
 * @line 显示层的问题
 * 系统负载高 -> 显示跟不上 -> 画面卡顿
 * @brief Packet Queue -> Frame Queue -> Display
 * 多级缓冲可以更好地应对波动
 * 类似弹簧减震器的原理
 */
static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    /*  休眠等待时间,remaining_time的计算在video_refresh中    */
    double remaining_time = 0.0;
    /**
     * 调用 SDL_PeepEvents 前实现调用 SDL_PumpEvents,将输入设备的事件抽到事件队列中
     *  SDL_PeepEvents 检查是否有事件，比如鼠标移入显示区等
     * 从事件队列中拿一个事件，放到 event 中，如果没有事件，则进入循环中
     *  SDL_PeekEvents 用于读取事件，在调用该函数之前，必须调用 SDL_PumpEvents 搜集键盘等事件
     */
    SDL_PumpEvents(); // 更新 SDL 的事件状态。这意味着它会从操作系统获取所有尚未处理的事件，并将它们放入 SDL 的内部事件队列中。这是确保程序能够接收到外部事件（如键盘和鼠标输入）的重要步骤。
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) { // SDL_PeepEvents 用于查看事件队列，但不会从队列中移除事件
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) { // 这一行检查自上次光标显示以来的时间。
            SDL_ShowCursor(0);  // 如果超过了指定的延迟，光标将被隐藏
            cursor_hidden = 1;
        }
        /**
         *  remaining_time 就是用来进行音视频同步的
         * 在 video_refresh函数中，根据当前帧显示时刻(display time)和实际时刻(actual time)
         * 计算需要 sleep 的时间，保证帧按时显示
         */
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0)); // 这一行代码用于控制刷新率。如果 remaining_time 大于 0，程序将休眠，直到刷新周期到达。这对于视频播放器而言非常重要，因为它需要保持一定的刷新频率。
        remaining_time = REFRESH_RATE; // 设置刷新间隔
        /**
         * @brief 这里有一个判断是否要调用 video_refresh 的前置条件，满足以下条件即可显示：
         * 1.显示模式不为 SHOW_MODE_NONE (如果文件中只有audio， 也会显示其波形或者频谱图等)
         * 2.或者，当前没有被暂停
         * 3.或者，当前设置了 force_refresh, 我们分析了 force_refresh 置为1的场景：
         * @li a. video_refresh 里面帧该显示，这是常规情况：
         * @li b. SDL_WINDOWEVENT_EXPOSED,窗口需要重新绘制
         * @li c. SDL_MOUSEBUTTONDOWN && SDL_BUTTON_LEFT 连续鼠标左键点击2次显示窗口的时间间隔，
         * 小于 0.5s，进行全屏or恢复原始窗口播放
         * 
         * 
         */
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh)) // 这一行代码在没有事件的情况下更新视频帧的显示。
            video_refresh(is, &remaining_time); // 如果播放器不是暂停状态，并且显示模式有效，程序会调用 video_refresh 函数来更新视频输出。
        /**
         * 从输入设备中搜集时间，推动这些事件进入时间队列，更新事件队列的状态，
         * 不过它还有一个作用是进行视频子系统的设备状态更新，如果不调用这个函数
         * 所显示的视频会在大约10s后丢失色彩。没有调用 SDL_PumpEvents,
         * 将不会有任何的输入设备事件进入队列，这种情况下，SDL就无法响应任何的键盘等硬件输入。
         */
        SDL_PumpEvents(); // 更新 SDL 的事件状态。
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        refresh_loop_wait_event(cur_stream, &event); // 等待事件发生,同时更新用户界面.如果没有事件产生，程序会在 refresh_loop_wait_event 函数中循环，直到产生事件,打破循环,返回一个event
        switch (event.type) {
        case SDL_KEYDOWN: // 键盘按下事件
            /**
             * @param exit_on_keydown: 一个布尔标志，表示是否在任意按键时退出
             * @param event.key.keysym.sym == SDLK_ESCAPE: 检测是否按下了 ESC 键
             * SDLK_ESCAPE 是 SDL 中定义的 ESC 键的常量值
             * @param event.key.keysym.sym == SDLK_q: 检测是否按下了 Q 键
             * SDLK_q 是 SDL 中定义的 q 键的常量值
             */
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) { 
                do_exit(cur_stream);
                break;
            }
            // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
            if (!cur_stream->width)
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_f:
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                step_to_next_frame(cur_stream);
                break;
            case SDLK_a:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters)
                        cur_stream->vfilter_idx = 0;
                } else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
                break;
            case SDLK_PAGEUP:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                    if (seek_by_bytes) {
                        pos = -1;
                        if (pos < 0 && cur_stream->video_stream >= 0)
                            pos = frame_queue_last_pos(&cur_stream->pictq);
                        if (pos < 0 && cur_stream->audio_stream >= 0)
                            pos = frame_queue_last_pos(&cur_stream->sampq);
                        if (pos < 0)
                            pos = avio_tell(cur_stream->ic->pb);
                        if (cur_stream->ic->bit_rate)
                            incr *= cur_stream->ic->bit_rate / 8.0;
                        else
                            incr *= 180000.0;
                        pos += incr;
                        stream_seek(cur_stream, pos, incr, 1);
                    } else {
                        pos = get_master_clock(cur_stream);
                        if (isnan(pos))
                            pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                        pos += incr;
                        if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                            pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                        stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                    }
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN: // 鼠标按下事件
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    toggle_full_screen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:   // 鼠标移动事件
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))
                    break;
                x = event.motion.x;
            }
                if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                    uint64_t size =  avio_size(cur_stream->ic->pb);
                    stream_seek(cur_stream, size*x/cur_stream->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns  = cur_stream->ic->duration / 1000000LL;
                    thh  = tns / 3600;
                    tmm  = (tns % 3600) / 60;
                    tss  = (tns % 60);
                    frac = x / cur_stream->width;
                    ns   = frac * tns;
                    hh   = ns / 3600;
                    mm   = (ns % 3600) / 60;
                    ss   = (ns % 60);
                    av_log(NULL, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                            hh, mm, ss, thh, tmm, tss);
                    ts = frac * cur_stream->ic->duration;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                        ts += cur_stream->ic->start_time;
                    stream_seek(cur_stream, ts, 0, 0);
                }
            break;
        case SDL_WINDOWEVENT: // 窗口移动事件
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    screen_width  = cur_stream->width  = event.window.data1;
                    screen_height = cur_stream->height = event.window.data2;
                    if (cur_stream->vis_texture) {
                        SDL_DestroyTexture(cur_stream->vis_texture);
                        cur_stream->vis_texture = NULL;
                    }
                    if (vk_renderer)
                        vk_renderer_resize(vk_renderer, screen_width, screen_height);
                case SDL_WINDOWEVENT_EXPOSED:
                    cur_stream->force_refresh = 1;
            }
            break;
        case SDL_QUIT: // 退出事件，表示应用程序应该终止
        case FF_QUIT_EVENT: // 自定义的一个退出事件类型
            do_exit(cur_stream);
            break;
        default:
            break;
        }
    }
}

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    screen_width = num;
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg)
{
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    screen_height = num;
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_sync(void *optctx, const char *opt, const char *arg)
{
    if (!strcmp(arg, "audio"))
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
    show_mode = !strcmp(arg, "video") ? SHOW_MODE_VIDEO :
                !strcmp(arg, "waves") ? SHOW_MODE_WAVES :
                !strcmp(arg, "rdft" ) ? SHOW_MODE_RDFT  : SHOW_MODE_NONE;

    if (show_mode == SHOW_MODE_NONE) {
        double num;
        int ret = parse_number(opt, arg, OPT_TYPE_INT, 0, SHOW_MODE_NB-1, &num);
        if (ret < 0)
            return ret;
        show_mode = num;
    }
    return 0;
}

static int opt_input_file(void *optctx, const char *filename) // 这里进行解析文件名or网络url
{
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
                filename, input_filename);
        return AVERROR(EINVAL);
    }
    if (!strcmp(filename, "-"))
        filename = "fd:";
    input_filename = av_strdup(filename);
    if (!input_filename)
        return AVERROR(ENOMEM);

    return 0;
}

static int opt_codec(void *optctx, const char *opt, const char *arg)
{
   const char *spec = strchr(opt, ':');
   const char **name;
   if (!spec) {
       av_log(NULL, AV_LOG_ERROR,
              "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
       return AVERROR(EINVAL);
   }
   spec++;

   switch (spec[0]) {
   case 'a' : name = &audio_codec_name;    break;
   case 's' : name = &subtitle_codec_name; break;
   case 'v' : name = &video_codec_name;    break;
   default:
       av_log(NULL, AV_LOG_ERROR,
              "Invalid media specifier '%s' in option '%s'\n", spec, opt);
       return AVERROR(EINVAL);
   }

   av_freep(name);
   *name = av_strdup(arg);
   return *name ? 0 : AVERROR(ENOMEM);
}

static int dummy;

static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "x",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
    { "y",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
    { "fs",                 OPT_TYPE_BOOL,            0, { &is_full_screen }, "force full screen" },
    { "an",                 OPT_TYPE_BOOL,            0, { &audio_disable }, "disable audio" },
    { "vn",                 OPT_TYPE_BOOL,            0, { &video_disable }, "disable video" },
    { "sn",                 OPT_TYPE_BOOL,            0, { &subtitle_disable }, "disable subtitling" },
    { "ast",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
    { "vst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
    { "sst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
    { "ss",                 OPT_TYPE_TIME,            0, { &start_time }, "seek to a given position in seconds", "pos" },
    { "t",                  OPT_TYPE_TIME,            0, { &duration }, "play  \"duration\" seconds of audio/video", "duration" },
    { "bytes",              OPT_TYPE_INT,             0, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
    { "seek_interval",      OPT_TYPE_FLOAT,           0, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
    { "nodisp",             OPT_TYPE_BOOL,            0, { &display_disable }, "disable graphical display" },
    { "noborder",           OPT_TYPE_BOOL,            0, { &borderless }, "borderless window" },
    { "alwaysontop",        OPT_TYPE_BOOL,            0, { &alwaysontop }, "window always on top" },
    { "volume",             OPT_TYPE_INT,             0, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },
    { "f",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_format }, "force format", "fmt" },
    { "stats",              OPT_TYPE_BOOL,   OPT_EXPERT, { &show_status }, "show status", "" },
    { "fast",               OPT_TYPE_BOOL,   OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },
    { "genpts",             OPT_TYPE_BOOL,   OPT_EXPERT, { &genpts }, "generate pts", "" },
    { "drp",                OPT_TYPE_INT,    OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres",             OPT_TYPE_INT,    OPT_EXPERT, { &lowres }, "", "" },
    { "sync",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit",           OPT_TYPE_BOOL,   OPT_EXPERT, { &autoexit }, "exit at the end", "" },
    { "exitonkeydown",      OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
    { "exitonmousedown",    OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
    { "loop",               OPT_TYPE_INT,    OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
    { "framedrop",          OPT_TYPE_BOOL,   OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
    { "infbuf",             OPT_TYPE_BOOL,   OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
    { "window_title",       OPT_TYPE_STRING,          0, { &window_title }, "set window title", "window title" },
    { "left",               OPT_TYPE_INT,    OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
    { "top",                OPT_TYPE_INT,    OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
    { "vf",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af",                 OPT_TYPE_STRING,          0, { &afilters }, "set audio filters", "filter_graph" },
    { "rdftspeed",          OPT_TYPE_INT, OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
    { "showmode",           OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "i",                  OPT_TYPE_BOOL,            0, { &dummy}, "read specified file", "input_file"},
    { "codec",              OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
    { "acodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
    { "scodec",             OPT_TYPE_STRING, OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
    { "vcodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
    { "autorotate",         OPT_TYPE_BOOL,            0, { &autorotate }, "automatically rotate video", "" },
    { "find_stream_info",   OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT, { &find_stream_info },
        "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads",     OPT_TYPE_INT,    OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },
    { "enable_vulkan",      OPT_TYPE_BOOL,            0, { &enable_vulkan }, "enable vulkan renderer" },
    { "vulkan_params",      OPT_TYPE_STRING, OPT_EXPERT, { &vulkan_params }, "vulkan configuration using a list of key=value pairs separated by ':'" },
    { "hwaccel",            OPT_TYPE_STRING, OPT_EXPERT, { &hwaccel }, "use HW accelerated decoding" },
    { NULL, },
};

static void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
           );
}

/* Called from the main */
int main(int argc, char **argv)
{
    int flags, ret;
    VideoState *is; // VideoState 是播放器的封装

    init_dynload(); // 用于初始化动态加载相关功能的函数

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE     // 这个宏用于确定是否启用设备支持
    avdevice_register_all();    // 注册所有可用的设备。这通常是为了使 FFmpeg 能够处理各种输入和输出设备，如摄像头、麦克风等
#endif
    avformat_network_init(); // 初始化网络套接字库,已经确保网络通信安全。它为 FFmpeg 的网络功能（如流媒体协议、RTMP、HTTP 等）做好准备

    /* signal() 函数用于设置程序对特定信号的响应。在这里，SIGINT 和 SIGTERM 信号被配置为调用 sigterm_handler 函数 */
    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    是由终端发送的中断信号，通常是用户按下 Ctrl+C 时触发的  */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  是一个请求终止程序的信号。它可以由操作系统或其他程序发送，用于优雅地终止一个进程  */

    show_banner(argc, argv, options); // 显示程序的横幅信息

    ret = parse_options(NULL, argc, argv, options, opt_input_file); // 解析命令行的函数
    if (ret < 0)
        exit(ret == AVERROR_EXIT ? 0 : 1);

    if (!input_filename) {
        show_usage();
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    if (display_disable) {  // 是否显示视频
        video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable) // 禁用音频
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        /*  如果没有禁用音频，程序会检查环境变量 SDL_AUDIO_ALSA_SET_BUFFER_SIZE 是否已设置。
            如果没有设置，则将其设置为 "1"。这段注释说明了这一设置的目的，
            即尝试解决在使用 ALSA（高级 Linux 声音架构）时可能出现的缓冲区下溢问题，
            尤其是在处理非2的幂（NPOT）大小的音频周期时
        */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (display_disable)  // 禁用视频
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init (flags)) { // 加载 SDL 库并进行必要的设置.初始化多个子系统(音频、视频、定时器、输入)、初始必要的资源、加载动态库、多线程支持等
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE); // 用于处理系统窗口管理事件
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE); // 用户自定义事件，可以由用户程序生成

    if (!display_disable) {
        int flags = SDL_WINDOW_HIDDEN;  // SDL_WINDOW_HIDDEN: 窗口初始状态为隐藏
        if (alwaysontop)    // 是否总是置顶
#if SDL_VERSION_ATLEAST(2,0,5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (borderless)     // 是否无边框
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"); // 使用 SDL 库设置一个特定的提示(hint)
#endif
        if (hwaccel && !enable_vulkan) { // 是否启用硬件加速
            av_log(NULL, AV_LOG_INFO, "Enable vulkan renderer to support hwaccel %s\n", hwaccel);
            enable_vulkan = 1;
        }
        if (enable_vulkan) {    // 是否启用 Vulkan 渲染器
            vk_renderer = vk_get_renderer();    // 获取 Vulkan 渲染器
            if (vk_renderer) {  
#if SDL_VERSION_ATLEAST(2, 0, 6)
                flags |= SDL_WINDOW_VULKAN;
#endif
            } else {
                av_log(NULL, AV_LOG_WARNING, "Doesn't support vulkan renderer, fallback to SDL renderer\n");
                enable_vulkan = 0;
            }
        }
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags); // SDL 创建一个新的窗口
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");                       // 设置 SDL 渲染器的缩放质量的函数.  linear 表示使用线性过滤进行图像缩放
        if (!window) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
            do_exit(NULL);  // 退出处理
        }

        if (vk_renderer) { // vk_renderer 通常用于指代 Vulkan 渲染器的状态或配置
            AVDictionary *dict = NULL;

            if (vulkan_params) {    // Vulkan 参数
                int ret = av_dict_parse_string(&dict, vulkan_params, "=", ":", 0); // 创建的字典 (AVDictionary) 的内存
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Failed to parse, %s\n", vulkan_params);
                    do_exit(NULL);
                }
            }
            ret = vk_renderer_create(vk_renderer, window, dict); // 创建 Vulkan 渲染器，调用Vulkan渲染器自己的create函数
            av_dict_free(&dict);                                // 释放 dict 指向的内存
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL, "Failed to create vulkan renderer, %s\n", av_err2str(ret));
                do_exit(NULL);
            }
        } else {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC); // 创建一个渲染器，将图形内容渲染到给定的窗口
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0); // 创建一个无加速的渲染器（即最后一个参数为 0）
            }
            if (renderer) { // 如果成功创建了渲染器
                if (!SDL_GetRendererInfo(renderer, &renderer_info)) // 调用 SDL_GetRendererInfo 来获取关于渲染器的详细信息，存储在 renderer_info 结构中
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
            if (!renderer || !renderer_info.num_texture_formats) { // 检查 renderer 是否有效，以及 renderer_info 中的 num_texture_formats 是否为 0。这个检查确保了渲染器创建成功，并且支持至少一种纹理格式。
                av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
                do_exit(NULL);
            }
        }
    }

    is = stream_open(input_filename, file_iformat); // 打开视频流,它会启动读取线程，该线程负责持续从媒体源读取数据
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    /**
     * @brief
     * 响应用户输入
     * 从队列中获取数据
     * 控制解码过程
     * 管理音视频同步
     * 处理显示和播放
     */
    event_loop(is); // 事件循环

    /* never returns */

    return 0;
}
