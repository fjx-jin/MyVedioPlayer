// MyVedioPlayer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <list>
#include <Windows.h>
#include "tools.h"
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavutil/channel_layout.h"
#include "libswresample/swresample.h"
#include "libavutil/time.h"
#include "SDL2/SDL.h"
};

//#ifdef WIN32 //vs配置win32环境的时候会使用x86的环境 导致我用的64位的库用不了
//extern "C"
//{
//#include "libavcodec/avcodec.h"
//#include "libavformat/avformat.h"
//#include "libswscale/swscale.h"
//#include "libavutil/imgutils.h"
//#include "SDL2/SDL.h"
//};
//#else
//#ifdef __cplusplus
//extern "C"
//{
//#endif // __cplusplus
//#include "libavcodec/avcodec.h"
//#include "libavformat/avformat.h"
//#include "libswscale/swscale.h"
//#include "libavutil/imgutils.h"
//#include "SDL2/SDL.h"
//#include "MyVedioPlayer.h"
//#ifdef __cplusplus
//};
//#endif // __cplusplus
//#endif // WIN32

#define MIN_FRAMES 25
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_REFRESH_EVENT (SDL_USEREVENT + 1)
#define SDL_BREAK_EVENT (SDL_USEREVENT + 2)

static SDL_AudioDeviceID audio_dev;

typedef struct MyAVPacketList {
    AVPacket* pkt;
    int serial;
};

typedef struct PacketQueue {
    std::queue<MyAVPacketList> pkt_list;
    int packets_nums;
    int size;
    int64_t duration;
    int serial;
    SDL_mutex* mutex;
    SDL_cond* cond;
}PacketQueue;

typedef struct AudioFrameQueue {
    std::list<AVFrame*> frame_list;
    SDL_mutex* mutex;
    SDL_cond* cond;
};

typedef struct FrameQueue {
    std::queue<AVFrame*> frame_list;
    SDL_mutex* mutex;
    SDL_cond* cond;
};

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;
    double pts_drift;
};

typedef struct VideoState {
    char* filename;
    int width, height, x, y;
    int vedioindex, audioindex;
    AVFormatContext* ic;
    PacketQueue video_q;
    PacketQueue audio_q;
    FrameQueue video_fq;
    AudioFrameQueue audio_fq;
    AVCodecContext* auddec;
    AVCodecContext* viddec;
    SDL_Thread* video_decoder_tid;
    struct SwsContext* sub_convert_ctx;
    struct SwrContext* swr_ctx;
    SDL_Texture* sdlTexture;
    int is_paused;
    int audio_volume;
    struct AudioParams audio_tgt;
    double audio_clock; // 当前音频frame播放之后 音频流的pts是多少
    int audio_hw_buf_size; //SDL剩余多少长度的数据之后才会来拿数据

    Clock aud_clock;
    Clock vid_clock;

    AVRational video_src_timebase;
    AVRational video_dst_timebase;
    AVRational audio_src_timebase;
    AVRational audio_dst_timebase;
    double per_video_frame_duration;
    double per_audio_frame_duration;
    double per_audio_duration;

    uint8_t* audio_buf;  //音频流字节数据
    uint8_t* audio_buf1;
    int audio_buf_index; /* in bytes */
    int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
};

static void set_clock(Clock& c, double pts, double time)
{
    c.pts = pts;
    c.pts_drift = pts - time;
}

static double get_clock(const Clock& c)
{
    double time = av_gettime_relative() / 1000000.0;
    return c.pts_drift + time;  //返回的是当前帧的pts+消逝的时间
}

static int packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
{
    MyAVPacketList pkt1;
    int ret;

    pkt1.pkt = pkt;
    pkt1.serial = q->serial;
    q->pkt_list.push(pkt1);
    q->packets_nums++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
    AVPacket* pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt/*, int block, int* serial*/)
{
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);
    for (;;) {

        if (q->pkt_list.size() > 0)
        {
            pkt1 = q->pkt_list.front();
            q->pkt_list.pop();
            q->packets_nums--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            //if (serial)
            //    *serial = pkt1.serial;
            //av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        }
        else
        {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int decoder_decode_frame(AVCodecContext* decodec, AVFrame* frame, VideoState* is)
{
    int ret = AVERROR(EAGAIN);
    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        LOGINFO("Could not allocate packet.\n");
        return -1;
    }
    for (;;)
    {
        do {
            switch (decodec->codec_type)
            {
            case AVMEDIA_TYPE_VIDEO:
                ret = avcodec_receive_frame(decodec, frame);
                if (ret >= 0)
                {
                    //double real_pts = av_rescale_q(frame->pts, is->video_src_timebase, is->video_dst_timebase) * av_q2d(is->video_dst_timebase);
                    //double real_duration = av_rescale_q(frame->duration, is->video_src_timebase, is->video_dst_timebase) * av_q2d(is->video_dst_timebase);
                    double real_pts = frame->pts * av_q2d(is->video_dst_timebase);
                    double real_duration = frame->duration * av_q2d(is->video_dst_timebase);
                    LOGINFO("video pts = %f  duration = %f\n", real_pts, real_duration);
                    //AVRational dst_time_base = (AVRational){ 1, 90000 };
                    ////double pts = (frame->pts / frame->duration * is->per_video_frame_duration);// *frame.
                    //double pts = av_rescale_q(frame->pts, decodec->pkt_timebase, dst_time_base);
                    //frame->opaque = pts;
                    //frame->opaque = 
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ret = avcodec_receive_frame(decodec, frame);
                if (ret >= 0)
                {
                    double real_pts = frame->pts * av_q2d(is->audio_dst_timebase);
                    double real_duration = frame->duration * av_q2d(is->audio_dst_timebase);
                    //LOGINFO("audio pts = %f  duration = %f\n", real_pts, real_duration);
                    //static int64_t next_pts = 0;
                    //static AVRational next_tb;
                    //AVRational tb;
                    //tb.num = 1;
                    //tb.den = frame->sample_rate;
                    //
                    //if (frame->pts != AV_NOPTS_VALUE)
                    //{
                    //    frame->pts = av_rescale_q(frame->pts, decodec->pkt_timebase, tb);
                    //    next_pts = frame->pts * av_q2d(tb);
                    //}
                    //else if (next_pts != AV_NOPTS_VALUE)
                    //{
                    //    frame->pts = av_rescale_q(next_pts, next_tb, tb);
                    //}
                    //if (frame->pts != AV_NOPTS_VALUE) {
                    //    next_pts = frame->pts + frame->nb_samples;
                    //    next_tb = tb;
                    //}
                    //if (frame->pts != 0)
                    //{
                    //    
                    //    LOGINFO();
                    //}
                }
                break;
            }
            
            if (ret == AVERROR_EOF) {
                avcodec_flush_buffers(decodec);
                return 0;
            }
            if (ret >= 0)
                return 1;
        } while (ret != AVERROR(EAGAIN));

        //if (decodec->codec_type == AVMEDIA_TYPE_AUDIO && is->audio_q.packets_nums == 0)
        //    is->audio_q.cond.notify_all();
        //if (decodec->codec_type == AVMEDIA_TYPE_VIDEO && is->video_q.packets_nums == 0)
        //    is->video_q.cond.notify_all();

        if (decodec->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (packet_queue_get(&is->audio_q, pkt) < 0)
                return -1;
        }
        else if (decodec->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (packet_queue_get(&is->video_q, pkt) < 0)
                return -1;
        }

        if (avcodec_send_packet(decodec, pkt) == AVERROR(EAGAIN)) {
            LOGINFO("Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");

        }
        else {
            av_packet_unref(pkt);
        }
    }
}

static int audio_thread(void* arg)
{
    VideoState* is = (VideoState*)arg;
    AVFrame* frame = av_frame_alloc();
    

    int got_frame = 0;
    int ret;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;)
    {
        if ((got_frame = decoder_decode_frame(is->auddec, frame, is)) < 0)
            goto the_end;
        if (!got_frame)
            continue;

        SDL_LockMutex(is->audio_fq.mutex);
        //if (is->audio_fq.frame_list.size() > 15)
        //{
        //    //SDL_CondWait(is->audio_fq.cond, is->audio_fq.mutex); //TODO 拿取frame之后唤醒一下
        //    is->audio_fq.frame_list.push(frame);
        //    //Sleep(20);
        //    //LOGINFO("push audio frame wait\n");
        //}
        //else
        //{
        
        if (frame->sample_rate == 0 || frame->format == -1)
        {
            LOGINFO();
        }
        AVFrame* frame1 = av_frame_alloc();
        av_frame_ref(frame1, frame);
        is->audio_fq.frame_list.push_back(frame1);
        av_frame_unref(frame);
            //LOGINFO("audio frame sample_rate = %d nb_samples = %d\n", frame->sample_rate, frame->nb_samples);
        //}
        //if (is->audio_fq.frame_list.size() > 5)
        //    SDL_CondSignal(is->audio_fq.cond);
        //SDL_Delay(1);
        //LOGINFO("push audio frame size = %d \n", is->audio_fq.frame_list.size());
        SDL_UnlockMutex(is->audio_fq.mutex);
    }


the_end:
    av_frame_free(&frame);
    return ret;
}

static int get_video_frame(VideoState* is, AVFrame* frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(is->viddec, frame, is)) < 0)
        return -1;

    return got_picture;
}
static int video_thread(void* arg)
{
    VideoState* is = (VideoState*)arg;
    AVFrame* frame = av_frame_alloc();
    int ret;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        SDL_LockMutex(is->video_fq.mutex);
        if (is->video_fq.frame_list.size() > 15)
        {
            SDL_CondWait(is->video_fq.cond, is->video_fq.mutex); //TODO 拿取frame之后唤醒一下
            AVFrame* frame1 = av_frame_alloc();
            av_frame_ref(frame1, frame);
            is->video_fq.frame_list.push(frame1);
            av_frame_unref(frame);
            //is->video_fq.frame_list.push(frame);
        }
        else
        {
            AVFrame* frame1 = av_frame_alloc();
            av_frame_ref(frame1, frame);
            is->video_fq.frame_list.push(frame1);
            av_frame_unref(frame);
            //is->video_fq.frame_list.push(frame);
        }
        if (is->video_fq.frame_list.size() > 5)
            SDL_CondSignal(is->video_fq.cond);

        //LOGINFO("push frame size = %d \n", is->video_fq.frame_list.size());
        SDL_UnlockMutex(is->video_fq.mutex);
    }
the_end:
    //av_frame_free(&frame);
    return 0;
}

static int decoder_start(VideoState* is, int (*fn)(void*), const char* thread_name, void* arg)
{
    is->video_decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!is->video_decoder_tid) {
        LOGINFO("SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}
static int audio_decode_frame(VideoState* is)
{
    if (is->is_paused)
        return -1;
    int data_size = 0, resampled_data_size;;

    if (is->audio_fq.frame_list.size() == 0)
        Sleep(200);

    SDL_LockMutex(is->audio_fq.mutex);
    if (is->audio_fq.frame_list.size() == 0)
    {
        SDL_UnlockMutex(is->audio_fq.mutex);
        return 0;
    }
    AVFrame* frame = is->audio_fq.frame_list.front();
    is->audio_fq.frame_list.pop_front();
    SDL_UnlockMutex(is->audio_fq.mutex);
    data_size = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels,
                                                frame->nb_samples,
                                                (AVSampleFormat)frame->format, 1);

    if (frame->format != is->audio_tgt.fmt ||
        av_channel_layout_compare(&frame->ch_layout, &is->audio_tgt.ch_layout) ||
        frame->sample_rate != is->audio_tgt.freq)
    {
        //重采样
        swr_free(&is->swr_ctx);
        int ret = swr_alloc_set_opts2(&is->swr_ctx,
            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
            &is->auddec->ch_layout, is->auddec->sample_fmt, is->auddec->sample_rate,
            0, NULL);
        swr_init(is->swr_ctx);
    }

    if (is->swr_ctx)
    {
        const uint8_t** in = (const uint8_t**)frame->extended_data;
        uint8_t** out = &is->audio_buf1;
        int out_count = (int64_t)frame->nb_samples * is->audio_tgt.freq / frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        int len2;
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        
        len2 = swr_convert(is->swr_ctx, out, out_count, in, frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);

    }
    else
    {
        is->audio_buf = frame->data[0];
        resampled_data_size = data_size;
    }

    if (!isnan((double)frame->pts))
    {
        is->audio_clock = frame->pts * av_q2d(is->audio_dst_timebase);
    }
    else
        is->audio_clock = NAN;
    //is->audio_buf = out_buffer;

    return resampled_data_size;
}
static void sdl_audio_callback(void* opaque, Uint8* stream, int len)
{
    VideoState* is = (VideoState*)opaque;
    int audio_size, len1;
    int tmp = len;
    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size)
        {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0)
            {
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            }
            else
                is->audio_buf_size = audio_size;

            is->audio_buf_index = 0;
        }
        
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memset(stream, 0, len1);
        if (is->audio_buf)
        {
            SDL_MixAudioFormat(stream, is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }

    return;
}
static int audio_open(void* opaque, AVChannelLayout* wanted_channel_layout, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
    static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    //samples告诉 SDL 每次回调取多少样本数来播放
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / 30));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;

    //while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,0))) {
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
            wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
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
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
            "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    //LOGINFO("%s\n", SDL_GetError());
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
        return -1;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    return spec.size;
}
static int stream_component_open(VideoState* is, int stream_index)
{
    AVFormatContext* ic = is->ic;
    AVCodecContext* avctx;
    const AVCodec* codec;
    int ret = 0;

    if (stream_index < 0)
        return -1;

    codec = avcodec_find_decoder(is->ic->streams[stream_index]->codecpar->codec_id);
    avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(avctx, is->ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;

    
    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0)
        goto fail;

    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO)
    {   
        is->auddec = avctx;
        AVChannelLayout wantedLayout;
        av_channel_layout_default(&wantedLayout, avctx->ch_layout.nb_channels);
        if ((ret = audio_open(is, &wantedLayout, avctx->sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;
        is->audio_buf1_size = 0;
        is->audio_src_timebase = avctx->time_base;
        is->audio_dst_timebase = is->ic->streams[stream_index]->time_base;
        double tmp = is->ic->streams[stream_index]->duration * av_q2d(is->ic->streams[stream_index]->time_base);
        is->per_audio_frame_duration = 1024.0 / avctx->sample_rate;
        //is->per_audio_duration = avctx.
        if ((ret = decoder_start(is, audio_thread, "audio_decoder", is)) < 0)
            goto fail;
        SDL_PauseAudioDevice(audio_dev, 0);
        goto out;
    }
    else if (avctx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        is->viddec = avctx;
        is->video_src_timebase.num = avctx->framerate.den;
        is->video_src_timebase.den = avctx->framerate.num;
        is->video_dst_timebase = is->ic->streams[stream_index]->time_base;
        double tmp = is->ic->streams[stream_index]->duration * av_q2d(is->ic->streams[stream_index]->time_base);
        is->width = avctx->width;
        is->height = avctx->height;
        is->per_video_frame_duration = 1.0 / is->ic->streams[stream_index]->r_frame_rate.num;
        is->sub_convert_ctx = sws_getContext(is->width, is->height, avctx->pix_fmt, is->width, is->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if ((ret = decoder_start(is, video_thread, "video_decoder", is)) < 0)
            goto fail;
        goto out;
    }

fail:
    avcodec_free_context(&avctx);
out:
    return ret;

}

static int read_thread(void* arg)
{
    VideoState* is = (VideoState*)arg;
    AVFormatContext* pFormatCtx = nullptr;
    int videoIndex = -1, audioIndex = -1;
    AVPacket* pkt = nullptr;
    SDL_mutex* wait_mutex = SDL_CreateMutex();
    int err;

    pkt = av_packet_alloc();
    if (!pkt)
    {
        LOGINFO("Could not allocate packet.\n");
        goto exit;
    }

    pFormatCtx = avformat_alloc_context();
    if (!pFormatCtx)
    {
        LOGINFO("Could not allocate context.\n");
        goto exit;
    }

    err = avformat_open_input(&pFormatCtx, is->filename, NULL, NULL);
    if (err < 0)
    {
        LOGINFO("Could not open strem.\n");
        goto exit;
    }
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
    {
        LOGINFO("Could not find strem information.\n");
        goto exit;
    }
    is->ic = pFormatCtx;

    av_dump_format(pFormatCtx, 0, is->filename, 0);

    videoIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (videoIndex < 0)
    {
        LOGINFO("Didn't find a video stream.\n");
    }
    else
    {
        is->vedioindex = videoIndex;
        stream_component_open(is, videoIndex);
    }
    audioIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audioIndex < 0)
    {
        LOGINFO("Didn't find a audio stream.\n");
    }
    else
    {
        is->audioindex = audioIndex;
        stream_component_open(is, audioIndex);
    }

    
    for (;;)
    {
        //缓冲区的AVPacket够用则休眠
        if (is->audio_q.packets_nums > MIN_FRAMES && is->video_q.packets_nums > MIN_FRAMES)
        {
            SDL_LockMutex(wait_mutex);
            Sleep(10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        err = av_read_frame(pFormatCtx, pkt);
        if (err < 0)
        {
            //读完了 怎么处理
            goto exit;
        }
        
        if (pkt->stream_index == videoIndex)
        {
            packet_queue_put(&is->video_q, pkt);
        }
        else if (pkt->stream_index == audioIndex)
        {
            packet_queue_put(&is->audio_q, pkt);
        }
        else
            av_packet_unref(pkt);
    }

exit:
    if (pFormatCtx && !is->ic)
        avformat_close_input(&pFormatCtx);
    av_packet_free(&pkt);

    return 0;
}

int main(int argc, char* argv[])
{
    avformat_network_init();
    VideoState is;
    is.is_paused = 0;
    is.swr_ctx = swr_alloc();
    is.filename = argv[1];
    is.video_q.mutex = SDL_CreateMutex();
    is.video_q.cond = SDL_CreateCond();
    is.video_fq.mutex = SDL_CreateMutex();
    is.video_fq.cond = SDL_CreateCond();

    is.audio_q.mutex = SDL_CreateMutex();
    is.audio_q.cond = SDL_CreateCond();
    is.audio_fq.mutex = SDL_CreateMutex();
    is.audio_fq.cond = SDL_CreateCond();

    is.audio_buf1 = nullptr;

    int startup_volume = 100;
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is.audio_volume = startup_volume;

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_VIDEO))
    {
        LOGINFO("Could not initialize SDL - %s\n", SDL_GetError());
        exit(-1);
    }
    SDL_Thread* rth = SDL_CreateThread(read_thread, "read_thread", &is);

    SDL_Window* screen;
    
    AVFrame* pFrameYUV = av_frame_alloc();
    unsigned char* out_buffer;
    SDL_Renderer* sdlRenderer;
    SDL_Texture* sdlTexture;
    static int flag = 0;
    SDL_Event event;
    double wait_time = 0.01;
    double last_frame_time = 0;
    double frame_last_delay = 0;
    double every_render_time = 0;
    for (;;)
    {
        SDL_PumpEvents();
        while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
            if (!is.is_paused)
            {
                SDL_LockMutex(is.video_fq.mutex);
                if (is.video_fq.frame_list.size() > 0)
                {
                    if (!flag)
                    {
                        screen = SDL_CreateWindow("MyVedioPlayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, is.width, is.height, SDL_WINDOW_OPENGL);
                        if (!screen)
                        {
                            LOGINFO("SDL: could not create window - exiting:%s\n", SDL_GetError());
                            exit(-1);
                        }

                        out_buffer = (unsigned char*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, is.width, is.height, 1));
                        av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, is.width, is.height, 1);
                        sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
                        sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, is.width, is.height);
                    }
                    flag = 1;

                    AVFrame* pFrame = is.video_fq.frame_list.front();
                    is.video_fq.frame_list.pop();
                    if (is.video_fq.frame_list.size() == 15)
                        SDL_CondSignal(is.video_fq.cond);
                    SDL_UnlockMutex(is.video_fq.mutex);
                    /*double duration = 0.01;
                    if (last_frame)
                    {
                        double last_durantion = pFrame->pts - last_frame->pts;
                        if (isnan(last_durantion) || last_durantion <= 0 || last_durantion > 10)
                            last_durantion = last_frame->pts;
                        duration = count_video_delay(last_durantion, &is);
                    }*/
                    //else
                    //{
                    //    double last_durantion = -pFrame->pts;
                    //    duration = count_video_delay(0, &is);
                    //}
                    //if (!isnan((double)pFrame->pts))
                    //    set_clock(is.vid_clock, pFrame->pts, av_gettime_relative() / 1000000.0);
                    //LOGINFO("pop frame size = %d \n", is.video_fq.frame_list.size());
                    //{
                        //double current_pts = pFrame->
                        //double delay = current_pts - frame_last_pts;
                        //if()
                        double cur_frame_time = pFrame->pts * av_q2d(is.video_dst_timebase);
                        //double delay = cur_frame_time - last_frame_time;
                        double delay = pFrame->duration * av_q2d(is.video_dst_timebase);

                        double diff = 0;
                        if (!isnan(is.audio_clock))
                            diff = cur_frame_time - is.audio_clock;
                        double sync_threadshold = FFMAX(0.04, FFMIN(delay, 0.1));
                        if (!isnan(diff) && fabs(diff) < 10)  //误差超过10s就不进行音视频同步了
                        {
                            if (diff <= -sync_threadshold) //视频比音频慢
                            {
                                delay = FFMAX(0, delay + diff);  //正常来说是要休眠delay时长的，现在要加速diff的时长，所以相加，不可以小于0
                            }
                            else if (diff >= sync_threadshold && delay > 0.1)
                                delay = delay + diff;
                            else if (diff >= sync_threadshold)
                                delay = 2 * delay;
                        }

                        double curr_time = av_gettime_relative() / 1000000; 
                        if (every_render_time == 0)
                            every_render_time = curr_time;
                        double actual_delay = every_render_time + delay - curr_time;
                        if (actual_delay <= 0)
                            actual_delay = 0;

                        
                        //if (!isnan(is.audio_clock) && cur_frame_time + cur_frame_duration < is.audio_clock)
                        //    break; //当前帧+duration大于音频的时间戳直接丢
                    //}
                    sws_scale(is.sub_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, is.height, pFrameYUV->data, pFrameYUV->linesize);
                    SDL_UpdateTexture(sdlTexture, nullptr, pFrameYUV->data[0], pFrameYUV->linesize[0]);
                    SDL_RenderClear(sdlRenderer);
                    SDL_RenderCopy(sdlRenderer, sdlTexture, nullptr, nullptr);
                    SDL_RenderPresent(sdlRenderer);

                    //av_frame_copy(last_frame, pFrame);
                    //av_frame_ref(last_frame, pFrame);
                    //av_frame_unref(pFrame);
                    //LOGINFO("delay %f\n", duration);
                    //SDL_Delay(duration / 1000.0);//ms
                     SDL_Delay(actual_delay * 1000);//ms
                     every_render_time = curr_time;
                    //av_usleep((int64_t)(duration * 1000000.0));
                }
                else
                    SDL_UnlockMutex(is.video_fq.mutex);
                    //SDL_CondWait(is.video_fq.cond, is.video_fq.mutex);
                //SDL_UnlockMutex(is.video_fq.mutex);
                
            }
            SDL_PumpEvents();
        }
        if (event.type == SDL_KEYDOWN)
        {
            if (event.key.keysym.sym == SDLK_SPACE)
                is.is_paused = !is.is_paused;
        }
        else if (event.type == SDL_QUIT)
        {
            exit(-1);
        } 
    }
    
    SDL_Quit();
    av_frame_free(&pFrameYUV);
    return 0;
}