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
 * @file
 * libavformat API example.
 *
 * Output a media file in any supported libavformat format. The default
 * codecs are used.
 * @example muxing.c
 */

#define _XOPEN_SOURCE 600 /* for usleep */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/inotify.h>
#include <sys/socket.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "libavutil/audio_fifo.h"

#include "font.h"

// Missing from #include <libavfilter/lavfutils.h>
int ff_load_image(uint8_t *data[4], int linesize[4],
                  int *w, int *h, enum AVPixelFormat *pix_fmt,
                  const char *filename, void *log_ctx);

#include "list.h"

#define ARRAY_SIZE(arr) \
    (sizeof(arr) / sizeof((arr)[0]) \
     + sizeof(typeof(int[1 - 2 * \
           !!__builtin_types_compatible_p(typeof(arr), \
                 typeof(&arr[0]))])) * 0)

//#define MAX(X,Y)                ((X)>(Y) ? (X):(Y))
#define MAX(x, y) __extension__ ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 > _min2 ? _min1 : _min2; })

//#define MIN(X,Y)                ((X)<(Y) ? (X):(Y))
#define MIN(x, y) __extension__ ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 < _min2 ? _min1 : _min2; })

#define MILLI_SECOND      (1000)
#define MICRO_SECOND      (1000000)
#define NANO_SECOND       (1000000000)

// Video Output Settings
#define SCREEN_WIDTH      1280
#define SCREEN_HEIGHT     720
#define STREAM_FRAME_RATE 25        /* 25 images/s */
#define STREAM_BITRATE    (2000 * 1024)
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */
#define SCALE_FLAGS       SWS_BICUBIC
static const char        *video_filter_descr = "scale=w=%d:h=%d:force_original_aspect_ratio=decrease,scale=%d:%d,fps=fps=%d";

// Audio Output Settings
#define AUDIO_BITRATE     (128 * 1024)
#define AUDIO_FORMAT      AV_SAMPLE_FMT_FLTP
static const enum AVSampleFormat out_sample_fmts[]     = { AV_SAMPLE_FMT_FLTP, -1 };
static const int64_t      out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
static const int          out_sample_rates[]    = { 44100, -1 };
static const char        *audio_filter_descr    = "aresample=%d,aformat=sample_fmts=fltp:channel_layouts=stereo";

// General
static uint64_t           calibrate_overhead;
static pthread_t          inotify_thread_id;
static int                inotify_thread_fd;
static int                verbose;

typedef struct {
    int                frames;
    AVCodecContext    *dec_ctx;
    // int64_t            last_pts;
    // struct SwrContext *swr_ctx;

    AVFilterGraph     *filter_graph;
    AVFilterContext   *buffersink_ctx;
    AVFilterContext   *buffersrc_ctx;
    AVCodec           *dec;

    int                index;
    int64_t            total_duration;
} input_stream_t;

// a wrapper around a single output AVStream
typedef struct {
    int                frames;
    AVCodecContext    *enc_ctx;
    int64_t            last_pts;
    int64_t            next_pts;
    struct SwsContext *sws_ctx;

    AVStream          *st;

    int                samples_count;

    AVFrame           *frame;
    AVFrame           *tmp_frame;

    AVAudioFifo       *fifo;
} output_stream_t;

typedef struct {
    AVFormatContext    *oc;
    AVDictionary       *opt;
    AVFrame            *overlay_frame;
    char               *overlay_file;
    output_stream_t     audio_out_ctx;
    output_stream_t     video_out_ctx;
    // sem_t               packets_available;
    sem_t               audio_available;
    sem_t               video_available;
    sem_t               free_to_start;
    sem_t               start_file_sending;
    // sem_t               packet_slots;
    pthread_t           thread_id;
    uint32_t            play_silence_packets;
    char               *output_filename;
    bool                play_silence;
    bool                live_stream;
    pthread_mutex_t     packet_mutex;
    list( AVFrame *,    video_packets);
    // list( int64_t,      video_ppts);
    list( AVFrame *,    audio_packets);
    // list( int64_t,      audio_ppts);
    bool                need_audio;
    bool                need_video;
    struct timeval      start_time;
    uint64_t            bitrate;
    uint64_t            frame_rate;
    uint64_t            screen_width;
    uint64_t            screen_height;
    uint64_t            audio_bitrate;
    int                 chapter_id;
    time_t              total_time;
    uint64_t            total_frames;
    char               *current_name;
    int                 current_year;
    int64_t             video_pts;
} output_t;

typedef struct {
    AVFormatContext   *ifmt_ctx;
    AVDictionary      *opt;
    char              *filename;
    output_stream_t   *audio_output;
    output_stream_t   *video_output;
    void              *output;
    bool               intro_only;
} input_thread_t;

typedef struct {
    list( char *, files);
    pthread_t thread_id;
    output_t *output;
    char     *directory;
    bool      loop;
    bool      random;
} input_t;

typedef struct {
    int       fd;
    void    (*deleted)( bool isdir, char *name, void *passed_);
    void    (*written)( bool isdir, char *name, void *passed_);
    void     *ptr;
    char     *name;
    output_t *output;
    input_t  *input;
} inotify_t;

static list( output_t *,  outputs_running);
static list( input_t *,   inputs_running);
static list( inotify_t *, notifications);

// static pthread_cond_t   output_cond;
// static pthread_mutex_t  output_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile bool    input_finished;

#define VALID_PACKET          ((void *)0x41424241)
#define VALID_PACKET_CHECK(X) (list_length(X) && (1 || (VALID_PACKET == list_front(X)->opaque)))

static const char *type_string(int type)
{
    switch (type) {
    case AVIO_ENTRY_DIRECTORY:
        return "<DIR>";
    case AVIO_ENTRY_FILE:
        return "<FILE>";
    case AVIO_ENTRY_BLOCK_DEVICE:
        return "<BLOCK DEVICE>";
    case AVIO_ENTRY_CHARACTER_DEVICE:
        return "<CHARACTER DEVICE>";
    case AVIO_ENTRY_NAMED_PIPE:
        return "<PIPE>";
    case AVIO_ENTRY_SYMBOLIC_LINK:
        return "<LINK>";
    case AVIO_ENTRY_SOCKET:
        return "<SOCKET>";
    case AVIO_ENTRY_SERVER:
        return "<SERVER>";
    case AVIO_ENTRY_SHARE:
        return "<SHARE>";
    case AVIO_ENTRY_WORKGROUP:
        return "<WORKGROUP>";
    case AVIO_ENTRY_UNKNOWN:
    default:
        break;
    }
    return "<UNKNOWN>";
}

static int list_op(const char *input_dir, input_t *input)
{
    AVIODirEntry *entry = NULL;
    AVIODirContext *ctx = NULL;
    int cnt, ret;
    char filemode[4], uid_and_gid[20];

    if ((ret = avio_open_dir(&ctx, input_dir, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open directory: %s.\n", av_err2str(ret));
        goto fail;
    }

    cnt = 0;
    for (;;) {
        if ((ret = avio_read_dir(ctx, &entry)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot list directory: %s.\n", av_err2str(ret));
            goto fail;
        }
        if (!entry)
            break;
        if (entry->filemode == -1) {
            snprintf(filemode, 4, "???");
        } else {
            snprintf(filemode, 4, "%3"PRIo64, entry->filemode);
        }
        snprintf(uid_and_gid, 20, "%"PRId64"(%"PRId64")", entry->user_id, entry->group_id);
        if( verbose) {
            if (cnt == 0)
                printf( "%-9s %12s %64s %10s %s %16s %16s %16s\n",
                    "TYPE", "SIZE", "NAME", "UID(GID)", "UGO", "MODIFIED",
                    "ACCESSED", "STATUS_CHANGED");
            printf( "%-9s %12"PRId64" %64s %10s %s %16"PRId64" %16"PRId64" %16"PRId64"\n",
               type_string(entry->type),
               entry->size,
               entry->name,
               uid_and_gid,
               filemode,
               entry->modification_timestamp,
               entry->access_timestamp,
               entry->status_change_timestamp);
        }
        if( AVIO_ENTRY_FILE == entry->type) {
            const size_t len = strlen( entry->name) + 2 + strlen( input_dir);
            char *name = calloc( 1, len);
            snprintf( name, len, "%s%s", input_dir, entry->name);
            list_push( input->files, name);
        }
        avio_free_directory_entry(&entry);
        cnt++;
    };

  fail:
    avio_close_dir(&ctx);
    return ret;
}

#define BUF_LEN (32 * (sizeof(struct inotify_event) + NAME_MAX + 1))

static void displayInotifyEvent(struct inotify_event *i, inotify_t *inot)
{
    printf("    wd =%2d; ", i->wd);
    if (i->cookie > 0)
        printf("cookie =%4d; ", i->cookie);
    if (i->len > 0)
        printf("name = %s; \n", i->name);

    printf("mask = ");
    if (i->mask & IN_ACCESS)        printf("IN_ACCESS ");
    if (i->mask & IN_ATTRIB)        printf("IN_ATTRIB ");
    if (i->mask & IN_CLOSE_NOWRITE) printf("IN_CLOSE_NOWRITE ");
    if (i->mask & IN_CLOSE_WRITE) {
        printf("IN_CLOSE_WRITE ");
        if( inot->written) {
            inot->written( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_CREATE)        printf("IN_CREATE ");
    if (i->mask & IN_DELETE) {
        printf("IN_DELETE ");
        if( inot->deleted) {
            inot->deleted( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_DELETE_SELF)   printf("IN_DELETE_SELF ");
    if (i->mask & IN_IGNORED) {
        printf("IN_IGNORED ");
        if( inot->deleted) {
            inot->deleted( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_ISDIR)         printf("IN_ISDIR ");
    if (i->mask & IN_MODIFY) {
        printf("IN_MODIFY ");
        if( inot->written) {
            inot->written( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_MOVE_SELF)     printf("IN_MOVE_SELF ");
    if (i->mask & IN_MOVED_FROM) {
        printf("IN_MOVED_FROM ");
        if( inot->deleted) {
            inot->deleted( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_MOVED_TO) {
        printf("IN_MOVED_TO ");
        if( inot->written) {
            inot->written( i->mask & IN_ISDIR, i->name, inot);
        }
    }
    if (i->mask & IN_OPEN)          printf("IN_OPEN ");
    if (i->mask & IN_Q_OVERFLOW)    printf("IN_Q_OVERFLOW ");
    if (i->mask & IN_UNMOUNT)       printf("IN_UNMOUNT ");
    printf("\n");
}

static void *inotify_thread( void *_passed)
{
    char buf[BUF_LEN] __attribute__ ((aligned(8)));

    inotify_thread_fd = inotify_init();

    while( !input_finished) {
        ssize_t numRead = read(inotify_thread_fd, buf, BUF_LEN);
        if (numRead <= 0) {
            break;
        }
        /* Process all of the events in buffer returned by read() */
        for (char *p = buf; p < buf + numRead; ) {
            struct inotify_event *event = (struct inotify_event *) p;
            list_each( notifications, _not) {
                if( _not->fd == event->wd) {
                    displayInotifyEvent(event, _not);
                }
            }
            p += sizeof(struct inotify_event) + event->len;
        }
    }
    close( inotify_thread_fd);

    return NULL;
}

static int doubleHeight = 1;
static int doubleWidth = 1;

static void block_fill( int x0, int y0, int w0, int h0, colour_e colour, AVFrame *pict)
{
    if( pict && pict->data[0]) {

        if( x0 < 0) { w0 += x0; x0 = 0; }
        if( y0 < 0) { h0 += y0; y0 = 0; }

        uint8_t *Y   = pict->data[0] + x0;
        uint8_t *Cr  = pict->data[1] + (x0/2);
        uint8_t *Cb  = pict->data[2] + (x0/2);
        while( h0 > 0) {
            memset( Y  + (pict->linesize[0]*y0),     colour>>16, w0);
            memset( Cr + (pict->linesize[1]*(y0/2)), colour>>8,  w0/2);
            memset( Cb + (pict->linesize[2]*(y0/2)), colour,     w0/2);
            y0++;
            h0--;
        }
    }
}

static void draw_string( int px, int py, int pw, int ph, const char *string, const colour_e colour, AVFrame *pict, const justify_e justification, const colour_e background)
{
    if( !string)
        return;

    int height  = (_9ptDescriptors[1].offset - _9ptDescriptors[0].offset) + 1;
    const char *s = string;
    int tw = 0;
    int lines = 1;
    int _tw = 0;
    while( *s) {
        int off = *s;
        if(off>=_9ptFontInfo.start_character && off<=_9ptFontInfo.end_character) {
            int _width;

            off -= _9ptFontInfo.start_character;
            _width = _9ptDescriptors[off].width;
            _tw += _width * doubleWidth;
            _tw += 2;
        }
        else if( 13 == *s) {
            tw = MAX( _tw, tw);
            _tw = 0;
        }
        else if( '\t' == *s) {
            _tw += 8 * doubleWidth;
            _tw &= ~7;
        }
        else {
            _tw += 2;
        }
        s++;
    }
    tw = MAX( _tw, tw);
    const int height2 = lines* height * doubleHeight;

    if( -1 == ph) {
        ph = height2;
    }
    if( -1 == pw) {
        pw = tw;
    }

    switch( justification&(FONT_JUSTIFY_LEFT|FONT_JUSTIFY_CENTER|FONT_JUSTIFY_RIGHT)) {
        case FONT_JUSTIFY_OFF:
            px = px;
            break;

        case FONT_JUSTIFY_LEFT:
            px = px;
            break;

        case FONT_JUSTIFY_CENTER:
            px += (pw - tw)/2;
            break;

        case FONT_JUSTIFY_RIGHT:
            // printf( "Position %d, %d, %d, %d %d %d\n", px, py, pw, ph, tw, height2);
            px += (pw - tw);
            // printf( "Position %d, %d, %d, %d %d %d\n", px, py, pw, ph, tw, height2);
            break;

    }

    if( justification & FONT_JUSTIFY_MIDDLE) {
        ; // py -= (ph - height2)/2;
    }
    else if( justification & FONT_JUSTIFY_BOTTOM) {
        py = (py + ph) - height2;
    }
    else if( justification & FONT_JUSTIFY_ABOVE) {
        py -= height2;
    }
    else if( justification & FONT_JUSTIFY_BELOW) {
        ; // py -= ph;
    }

    // if( px < 0) {
    //     px += tile->scroll_offset/8;
    //     if( 0 == px) {
    //         tile->scroll_offset = 0;
    //     }
    //     else {
    //         tile->scroll_offset++;
    //     }
    // }

    int mx = px;
    int my = py;
    int mw = pw;
    int mh = ph;


    if( background) {
        int       _px = px - 3;
        const int _py = py - 3;
        const int _ph = height2 + 6;
        int       _tw2 = tw + 6;
        block_fill( _px, _py, _tw2, _ph, background, pict);
    }

    uint8_t *Y   = pict->data[0];
    uint8_t *Cr  = pict->data[1];
    uint8_t *Cb  = pict->data[2];

    while( *string) {
        const int off = *string++;

        if(off>=_9ptFontInfo.start_character && off<=_9ptFontInfo.end_character) {
            const uint8_t *data = _9ptBitmaps+_9ptDescriptors[off-_9ptFontInfo.start_character].offset;
            int width = _9ptDescriptors[off-_9ptFontInfo.start_character].width;
            int h = height;
            int _py = py;

            while( h--) {
                short mask = *data++<<8;
                int hh = doubleHeight;

                if(width>7) {
                    mask |= *data++;
                }
                while( hh--) {
                    if( (py >= my) && (py < (my + mh))) {
                        uint8_t *_Y  = Y  + (_py)   * pict->linesize[0];
                        uint8_t *_Cr = Cr + (_py/2) * pict->linesize[1];
                        uint8_t *_Cb = Cb + (_py/2) * pict->linesize[2];
                        int w = width;
                        int pos = px;
                        short mask2 = mask;

                        _py++;
                        while( w) {
                            int ww = doubleWidth;

                            while( ww--) {
                                if( mask2 & 0x8000) {
                                    if( (pos >= mx) && (pos <= (mx + mw))) {
                                        _Y[pos]    = colour>>16;
                                        _Cr[pos/2] = colour>>8;
                                        _Cb[pos/2] = colour;
                                    }
                                }
                                pos++;
                            }
                            mask2 <<= 1;
                            w--;
                        }
                    }
                }
            }
            px += (width) * doubleWidth;
            px += 2;
        }
        else if( *s=='\t') {
            px += 8 * doubleWidth;
            px &= ~7;
        }
        else {
            px += 2;
        }
    }
}

static uint64_t evaluate( const char *string)
{
    uint64_t result = 0;
    uint32_t base = 10;

    while ( *string && (isxdigit( *string) || ('x' == *string))) {
        uint32_t temp = 0;
        char c = tolower( *string);

        if( 'x' == tolower(c)) {
            if( (0 == result) && (10 == base)) {
                base = 16;
            }
            else {
                break;
            }
        }
        else if( isdigit(c)) {
            temp = c - '0';
        }
        else if( (16 == base) && isxdigit( *string)) {
            temp = c - 'a';
            temp += 10;
        }
        else {
            break;
        }
        result *= base;
        result += temp;
        string++;
    }

    if ( *string) {
        switch ( *string) {
        case 'p':
        case 'P':
            result *= (1024LL * 1024LL * 1024LL * 1024LL * 1024LL);
            break;

        case 't':
        case 'T':
            result *= (1024LL * 1024LL * 1024LL * 1024LL);
            break;

        case 'g':
        case 'G':
            result *= (1024LL * 1024LL * 1024LL);
            break;

        case 'm':
        case 'M':
            result *= (1024LL * 1024LL);
            break;

        case 'k':
        case 'K':
            result *= 1024LL;
            break;

        default:
            break;
        }
    }

    return result;
}

static uint64_t timeval_diff_usec(struct timeval *start_ts, struct timeval *end_ts)
{
    return ((end_ts->tv_sec - start_ts->tv_sec) * MICRO_SECOND) + (end_ts->tv_usec - start_ts->tv_usec);
}

static void log_packet2(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag, AVRational *time_base)
{
    printf("%-5s: %c pts:%-8s pts_time:%-8s dts:%-8s dts_time:%-8s duration:%-8s duration_time:%-9s stream_index:%d\n",
           tag, !pkt->stream_index && (pkt->flags & AV_PKT_FLAG_KEY) ? 'K':' ',
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    log_packet2( fmt_ctx, pkt, tag, time_base);

    // printf("%-5s: %c pts:%-8s pts_time:%-8s dts:%-8s dts_time:%-8s duration:%-8s duration_time:%-9s stream_index:%d\n",
    //        tag, !pkt->stream_index && (pkt->flags & AV_PKT_FLAG_KEY) ? 'K':' ',
    //        av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
    //        av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
    //        av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
    //        pkt->stream_index);
}

/**
 * Initialize a FIFO buffer for the audio samples to be encoded.
 * @param[out] fifo                 Sample buffer
 * @param      output_codec_context Codec context of the output file
 * @return Error code (0 if successful)
 */
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /* Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static AVFrame *load_image( const char *filename, int width, int height)
{
    AVFrame *frame = av_frame_alloc();

    int ret = ff_load_image( frame->data, frame->linesize, &frame->width, &frame->height, &frame->format, filename, NULL);

    // printf( "Loaded %s, %d %d %d\n", filename, ret, frame->width, frame->height);

    if( !ret) {
        if( (frame->format != STREAM_PIX_FMT) || (width && (width != frame->width)) || (height && (height != frame->height))) {
            AVFrame *frame2 = av_frame_alloc();

            frame2->format = STREAM_PIX_FMT;
            frame2->width  = width ? width:frame->width;
            frame2->height = height ? height:frame->height;

            ret = av_image_alloc( frame2->data, frame2->linesize, frame2->width, frame2->height, frame2->format, 16);
            if( ret) {
                struct SwsContext *fswitch = sws_getContext( frame->width, frame->height, frame->format,
                                          frame2->width, frame2->height, frame2->format, SCALE_FLAGS, NULL, NULL, NULL);

                ret = sws_scale( fswitch, (const uint8_t * const*)frame->data, frame->linesize, 0,
                    frame->height, frame2->data, frame2->linesize);

                sws_freeContext(fswitch);
                av_freep(&frame->data[0]);
                av_frame_free(&frame);

                return frame2;
            }
            else {
                av_freep(&frame2->data[0]);
                av_frame_free(&frame2);
            }
        }
    }

    if( ret) {
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
        return NULL;
    }

    return frame;
}

/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 * @param[out] frame                Frame to be initialized
 * @param      output_codec_context Codec context of the output file
 * @param      frame_size           Size of the frame
 * @return Error code (0 if successful)
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /* Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_err2str(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}

/**
 * Add converted input audio samples to the FIFO buffer for later processing.
 * @param fifo                    Buffer to add the samples to
 * @param converted_input_samples Samples to be added. The dimensions are channel
 *                                (for multi-channel audio), sample.
 * @param frame_size              Number of samples to be converted
 * @return Error code (0 if successful)
 */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO %u %u\n", av_audio_fifo_size(fifo), frame_size);
        return error;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, bool video)
{
    (void)video;
    int ret;

    // send the frame to the encoder
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame to the encoder: %s\n",
                av_err2str(ret));
        exit(1);
    }

    while (ret >= 0) {
        AVPacket pkt = { 0 };

        ret = avcodec_receive_packet(c, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
        pkt.stream_index = st->index;

        /* Write the compressed frame to the media file. */
    // printf( "[%4zu %4zu] ", list_length( video_packets), list_length(audio_packets));
    // log_packet(fmt_ctx, &pkt, video ? "Vrite":"Arite");
        ret = av_interleaved_write_frame(fmt_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    return ret == AVERROR_EOF ? 1 : 0;
}

/* Add an output stream. */
static void add_stream(output_stream_t *ost, output_t *output, AVCodec **codec,enum AVCodecID codec_id)
{
    AVFormatContext *oc = output->oc;
    AVCodecContext *c;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc_ctx = c;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt  = (*codec)->sample_fmts ?
            (*codec)->sample_fmts[0] : AUDIO_FORMAT;
        c->bit_rate    = output->audio_bitrate;
        c->sample_rate = out_sample_rates[0];
        if ((*codec)->supported_samplerates) {
            c->sample_rate = (*codec)->supported_samplerates[0];
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = out_channel_layouts[0];
        if ((*codec)->channel_layouts) {
            c->channel_layout = (*codec)->channel_layouts[0];
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        ost->st->time_base = (AVRational){ 1, c->sample_rate };
        break;

    case AVMEDIA_TYPE_VIDEO:
        c->codec_id = codec_id;

        c->bit_rate       = output->bitrate;
        c->rc_buffer_size = output->bitrate * 2;
        c->rc_max_rate    = output->bitrate * 1.25;
        c->rc_min_rate    = output->bitrate * 0.75;

        /* Resolution must be a multiple of two. */
        c->width    = output->screen_width;
        c->height   = output->screen_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, output->frame_rate };
        c->time_base       = ost->st->time_base;

        c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = STREAM_PIX_FMT;

        if (codec_id == AV_CODEC_ID_H264) {
            av_opt_set(c->priv_data, "preset", "fast", 0);
        }
        else if (codec_id == AV_CODEC_ID_H265) {
            av_opt_set(c->priv_data, "preset", "fast", 0);
        }
        else if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;
        }
        else if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
        break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ost->last_pts = AV_NOPTS_VALUE;
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  uint64_t channel_layout,
                                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    int ret;

    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

static void open_audio(output_t *output, AVCodec *codec, output_stream_t *ost)
{
    // AVFormatContext *oc      = output->oc;
    AVDictionary    *opt_arg = output->opt;
    AVCodecContext  *c       = ost->enc_ctx;
    AVDictionary    *opt     = NULL;
    int nb_samples;
    int ret;


    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    ost->frame     = alloc_audio_frame(AUDIO_FORMAT, c->channel_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AUDIO_FORMAT, c->channel_layout,
                                       c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

static AVFrame *get_audio_frame(output_stream_t *ost)
{
    AVFrame *frame = ost->tmp_frame;

    // printf( "Samples %d, channels %d, format %d\n", frame->nb_samples, ost->enc_ctx->channels, ost->enc_ctx->sample_fmt);

    av_samples_set_silence( &frame->data[0], 0, frame->nb_samples, ost->enc_ctx->channels, ost->enc_ctx->sample_fmt);

    if(0)
    switch( ost->enc_ctx->sample_fmt) {
        case AV_SAMPLE_FMT_U8P: {
            // int8_t *q = (int8_t*)frame->data[0];
            }
            break;

        case AV_SAMPLE_FMT_S16: {
            // int16_t *q = (int16_t*)frame->data[0];
            }
            break;

        case AV_SAMPLE_FMT_S32P: {
            // int32_t *q = (int32_t*)frame->data[0];
            }
            break;

        case AV_SAMPLE_FMT_FLTP: {
            // float *q = (float*)frame->data[0];
            }
            break;

        case AV_SAMPLE_FMT_DBLP: {
            // double *q = (double*)frame->data[0];
            }
            break;

        default:
            break;
    }

    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(output_t *output)
{
    if( !input_finished) {
        AVFrame *frame = NULL;
        AVFormatContext *oc = output->oc;
        output_stream_t *ost = &output->audio_out_ctx;
        bool clear = true;

        if( !output->play_silence) {
            pthread_mutex_lock(&output->packet_mutex);
            frame = list_front(output->audio_packets);
            // AVRational *time_baseV = &ost->enc_ctx->time_base;
            // printf( "POPped Audio %ld %s\n", ost->next_pts, av_ts2timestr(frame->nb_samples, time_baseV));
            list_pop_front(output->audio_packets);
            // list_pop_front(output->audio_ppts);
            // sem_post( &output->packet_slots);
            pthread_mutex_unlock(&output->packet_mutex);
        }
        else {
            // printf( "Create Audio frame %ld\n", ost->next_pts);
            frame = get_audio_frame(ost);
            clear = false;
        }

        ost->frames++;
        frame->pts = ost->next_pts;
        ost->next_pts  += frame->nb_samples;
        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, ost->enc_ctx->sample_rate}, ost->enc_ctx->time_base);
        ost->samples_count += frame->nb_samples;

        int ret = write_frame(oc, ost->enc_ctx, ost->st, frame, false);

        if( frame && clear) {
            av_frame_free(&frame);
        }

        return ret;
    }

    return 1;
}

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}

static void open_video(output_t *output, AVCodec *codec, output_stream_t *ost)
{
    // AVFormatContext *oc      = output->oc;
    AVDictionary    *opt_arg = output->opt;
    AVCodecContext  *c       = ost->enc_ctx;
    AVDictionary    *opt     = NULL;
    int ret;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            fprintf(stderr, "Could not allocate temporary picture\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    const int i = frame_index;

    /* Y */
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static AVFrame *get_video_frame(output_stream_t *ost)
{
    AVCodecContext *c = ost->enc_ctx;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if (!ost->sws_ctx) {
            printf( "NOT YUV420P\n");
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
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx, (const uint8_t * const *) ost->tmp_frame->data,
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static uint64_t delays[25];

static int write_video_frame(output_t *output)
{
    if( !input_finished) {
        AVFrame *frame = NULL;
        AVFormatContext *oc = output->oc;
        output_stream_t *ost = &output->video_out_ctx;
        bool clear = true;

        if( !output->play_silence) {
            pthread_mutex_lock(&output->packet_mutex);
            frame = list_front( output->video_packets);
            // printf( "POPped Video %ld\n", ost->next_pts);
            list_pop_front(output->video_packets);
            // list_pop_front(output->video_ppts);
            // sem_post( &output->packet_slots);
            pthread_mutex_unlock(&output->packet_mutex);
            // if (av_frame_make_writable(ost->frame) == 0) {
            //     draw_string( 0, 0, output->screen_width - 1, output->screen_height - 1, "Hello", YCrCb_WHITE, frame, FONT_JUSTIFY_LEFT    | FONT_JUSTIFY_TOP,    0x000000);
            //     draw_string( 0, 0, output->screen_width - 1, output->screen_height - 1, "Hello", YCrCb_WHITE, frame, FONT_JUSTIFY_CENTER, | FONT_JUSTIFY_MIDDLE, 0x000000);
            //     draw_string( 0, 0, output->screen_width - 1, output->screen_height - 1, "Hello", YCrCb_WHITE, frame, FONT_JUSTIFY_RIGHT,  | FONT_JUSTIFY_BOTTOM, 0x000000);
            // }
        }
        else {
            frame = get_video_frame(ost);
            clear = false;
            output->play_silence = (--output->play_silence_packets);
            if( !output->play_silence) {
                output->video_pts = ost->next_pts;
            }
        }

        ost->frames++;
        ost->frame->pts = ost->next_pts++;
        frame->pts = ost->frame->pts;

        if( output->live_stream) {
#if 1
#elif 1
            if( ost->last_pts != AV_NOPTS_VALUE) {
                struct timeval tv1;

                gettimeofday(&tv1, NULL);
                const uint64_t diff = timeval_diff_usec(&output->start_time, &tv1);
                const int64_t  delay = ((MICRO_SECOND/output->frame_rate)-diff)-calibrate_overhead;
                for( int i = 0; i < 24; i++) {
                    delays[i] = delays[ i + 1];
                }
                delays[24] = delay;
                if( delay > 0)
                    usleep( delay);
                // gettimeofday(&output->start_time, NULL);
                // printf( "Start:%ld.%ld - ", output->start_time.tv_sec, output->start_time.tv_usec);
                output->start_time.tv_usec += (MICRO_SECOND/output->frame_rate);
                output->start_time.tv_sec  += output->start_time.tv_usec / MICRO_SECOND;
                output->start_time.tv_usec %= MICRO_SECOND;
                // printf( ", Next:%ld.%ld\n", output->start_time.tv_sec, output->start_time.tv_usec);
            }
            ost->last_pts = frame->pts;
#else
            if (frame && frame->pts != AV_NOPTS_VALUE) {
                if (ost->last_pts != AV_NOPTS_VALUE) {
                    /* sleep roughly the right amount of time;
                    * usleep is in microseconds, just like AV_TIME_BASE. */
                    const int64_t delay = av_rescale_q(frame->pts - ost->last_pts,
                                        ost->enc_ctx->time_base, AV_TIME_BASE_Q) - calibrate_overhead;
                    if (delay > 0 && delay < 1000000) {
                        usleep(delay);
                        // printf( "Delayed %ld, Last %ld, PTS %ld\n", delay, ost->last_pts, frame->pts);
                    }
                }
                else {
                    printf( "LIVE\n");
                }
                ost->last_pts = frame->pts;
            }
#endif
        }
        frame->key_frame = 1;

        AVFrame *back = output->overlay_frame;
        if( back) {
            av_image_copy( frame->data, frame->linesize,(const uint8_t **)(back->data),
                back->linesize, back->format, back->width, back->height);
        }
        if( output->current_name) {
            if (av_frame_make_writable(ost->frame) == 0) {
                doubleHeight = 3;
                draw_string( 16, output->screen_height - 1, -1, -1, output->current_name, YCrCb_WHITE, frame, FONT_JUSTIFY_LEFT | FONT_JUSTIFY_ABOVE, YCrCb_BLACK);
                doubleHeight = 1;
            }
            const int64_t position = av_rescale_q(frame->pts - output->video_pts, ost->enc_ctx->time_base, AV_TIME_BASE_Q);
            if( position >= (10 * AV_TIME_BASE)) {
                free( output->current_name);
                output->current_name = NULL;
            }
        }

        // if( output->current_year) {
        //     char str[32];

        //     snprintf( str, sizeof( str), "%d", output->current_year);
        //     doubleHeight = 3;
        //     doubleWidth = 3;
        //     draw_string( 0, 0, output->screen_width - 1, output->screen_height - 1, str, YCrCb_WHITE, frame, FONT_JUSTIFY_RIGHT | FONT_JUSTIFY_BOTTOM, YCrCb_BLACK);
        //     doubleHeight = 1;
        //     doubleWidth = 1;
        // }

        int ret = write_frame(oc, ost->enc_ctx, ost->st, frame, true);

        if( frame && clear) {
            av_frame_free(&frame);
        }

        return ret;
    }

    return 1;
}

static void close_stream( output_t *output, output_stream_t *ost)
{
    // AVFormatContext *oc = output->oc;
    avcodec_free_context(&ost->enc_ctx);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    sws_freeContext(ost->sws_ctx);
    // swr_free(&ost->swr_ctx);
}

static void delete_background( bool isdir, char *name, void *_inot)
{
    (void)isdir;
    (void)name;
    inotify_t *inot = _inot;

    av_frame_free(inot->ptr);
}

static void written_background( bool isdir, char *name, void *_inot)
{
    (void)isdir;
    (void)name;
    inotify_t *inot = _inot;
    AVFrame **frame = inot->ptr;

    av_frame_free(inot->ptr);
    *frame = load_image( inot->name, 0, 0);
}

static void *output_thread( void *_passed)
{
    output_t        *output = _passed;
    AVCodec         *audio_codec;
    AVCodec         *video_codec;
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;

    if( !memcmp( output->output_filename, "http", 4) || !memcmp( output->output_filename, "udp", 3) || !memcmp( output->output_filename, "rtsp", 4)) {
        output->live_stream = true;
    }

    av_dict_set(&output->opt, "threads", "0", 0);

    pthread_mutex_init(&output->packet_mutex, NULL);

    if( output->overlay_file) {
        output->overlay_frame = load_image( output->overlay_file, 0, 0);

        inotify_t *back_check = calloc( 1, sizeof( *back_check));
        back_check->deleted = delete_background;
        back_check->written = written_background;
        back_check->output  = output;
        back_check->ptr     = (void *)&output->overlay_frame;
        back_check->name    = output->overlay_file;
        back_check->fd      = inotify_add_watch( inotify_thread_fd, output->overlay_file, IN_DELETE | IN_CLOSE_WRITE);
        list_push( notifications, back_check);
    }

    /* allocate the output media context */
    avformat_alloc_output_context2(&output->oc, NULL, NULL, output->output_filename);
    if (!output->oc) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&output->oc, NULL, "mpegts", output->output_filename);
    }
    if (!output->oc) {
        return NULL;
    }

    AVOutputFormat *fmt = output->oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE) {
        add_stream(&output->video_out_ctx, output, &video_codec, AV_CODEC_ID_H264);
        have_video = 1;
        encode_video = 1;
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE) {
        add_stream(&output->audio_out_ctx, output, &audio_codec, AV_CODEC_ID_AAC);
        have_audio = 1;
        encode_audio = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        open_video(output, video_codec, &output->video_out_ctx);

    if (have_audio)
        open_audio(output, audio_codec, &output->audio_out_ctx);

    av_dump_format(output->oc, 0, output->output_filename, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output->oc->pb, output->output_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open %s': %s\n", output->output_filename,
                    av_err2str(ret));
            return NULL;
        }
    }

    /* Initialize the FIFO buffer to store audio samples to be encoded. */
    if (init_fifo(&output->audio_out_ctx.fifo, output->audio_out_ctx.enc_ctx))
        return NULL;

    /* Write the stream header, if any. */
    ret = avformat_write_header(output->oc, &output->opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        return NULL;
    }

    output->chapter_id = 1;

    sem_post( &output->start_file_sending);
    gettimeofday(&output->start_time, NULL);

    time_t start = time(NULL);
    size_t v_count = 0;
    while (!input_finished && (encode_video || encode_audio)) {
#if 0
        if( !output->play_silence)
            sem_wait( &output->packets_available);

        pthread_mutex_lock(&output->packet_mutex);
        const size_t vp = list_length(output->video_packets) + output->play_silence;
        const size_t ap = list_length(output->audio_packets) + output->play_silence;
        pthread_mutex_unlock(&output->packet_mutex);

        if (encode_video || encode_audio) {
            /* select the stream to encode */
            if (encode_video &&
                (!encode_audio || av_compare_ts(output->video_out_ctx.next_pts, output->video_out_ctx.enc_ctx->time_base,
                                               output->audio_out_ctx.next_pts, output->audio_out_ctx.enc_ctx->time_base) <= 0)) {
                if( vp)
                    encode_video = !write_video_frame(oc, output);
            } else {
                if( ap)
                    encode_audio = !write_audio_frame(oc, output);
            }
        }
#else
        // printf( "Waiting for data\n");
        pthread_mutex_lock(&output->packet_mutex);
        const size_t vp = list_length(output->video_packets);
        const size_t ap = list_length(output->audio_packets) + output->play_silence;
        pthread_mutex_unlock(&output->packet_mutex);
        if( !output->need_audio && !output->need_video) {
            if( encode_audio && encode_video) {
                const int diff = av_compare_ts(output->video_out_ctx.next_pts, output->video_out_ctx.enc_ctx->time_base,
                                               output->audio_out_ctx.next_pts, output->audio_out_ctx.enc_ctx->time_base);
                output->need_video = ( diff <= 0);
                output->need_audio = ( diff >= 0);
            }
            else {
                output->need_video = encode_video;
                output->need_audio = encode_audio;
            }
        }
        else if( output->need_audio && encode_audio && (ap || output->play_silence)) {
            if( !output->play_silence)
                sem_wait( &output->audio_available);
            output->need_audio = false;
            encode_audio = !write_audio_frame(output);
        }
        else if( output->need_video && encode_video && (vp || output->play_silence)) {
            if( !output->play_silence)
                sem_wait( &output->video_available);
            output->need_video = false;
            encode_video = !write_video_frame(output);
            if( output->live_stream) {
                if( output->video_out_ctx.last_pts != AV_NOPTS_VALUE) {
                    struct timeval tv1;

                    gettimeofday(&tv1, NULL);
                    const uint64_t diff = timeval_diff_usec(&output->start_time, &tv1);
                    const int64_t  delay = ((MICRO_SECOND/output->frame_rate)-diff)-calibrate_overhead;
                    for( int i = 0; i < 24; i++) {
                        delays[i] = delays[ i + 1];
                    }
                    delays[24] = delay;
                    if( delay > 0)
                        usleep( delay);
                    gettimeofday(&output->start_time, NULL);
                    // printf( "Start:%ld.%ld - ", output->start_time.tv_sec, output->start_time.tv_usec);
                    // output->start_time.tv_usec += (MICRO_SECOND/output->frame_rate);
                    // output->start_time.tv_sec  += output->start_time.tv_usec / MICRO_SECOND;
                    // output->start_time.tv_usec %= MICRO_SECOND;
                }
                output->video_out_ctx.last_pts = 0;
            }
            v_count++;
        }
#endif

        const time_t now = time(NULL);
        if( start != now) {
            if( output->live_stream) {
                printf( "%8ld %zu [", start, v_count);
                for( int i = 0; i < 25; i++) {
                    printf( "%8ld ", delays[i]);
                }
                printf( "]\n");
                v_count = 0;
            }
            start = now;
        }
    }
    // printf( "Output Finished %d\n", input_finished);

    // printf( "Final PTSs are Video:%s, Audio %s\n", av_ts2timestr(video_out_ctx.next_pts, &video_out_ctx.enc_ctx->time_base),
    //             av_ts2timestr(audio_out_ctx.next_pts, &audio_out_ctx.enc_ctx->time_base));

    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(output->oc);

    if (output->audio_out_ctx.fifo)
        av_audio_fifo_free(output->audio_out_ctx.fifo);
    /* Close each codec. */
    if (have_video)
        close_stream(output, &output->video_out_ctx);
    if (have_audio)
        close_stream(output, &output->audio_out_ctx);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&output->oc->pb);

    /* free the stream */
    avformat_free_context(output->oc);
    av_frame_free(&output->overlay_frame);

    const int fps = output->total_frames / (output->total_time + 1);
    printf( "%s, Time to process %ld, Total Frames %ld, FPS %d\n", output->output_filename, output->total_time, output->total_frames, fps);

    free( output->output_filename);
    free( output);

    return NULL;
}

static int open_input_file(input_stream_t *video, input_stream_t *audio, AVFormatContext *ifmt_ctx)
{
    int ret;

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &video->dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    video->index = ret;
    // printf( "Using video %d\n", ret);

    /* create decoding context */
    video->dec_ctx = avcodec_alloc_context3(video->dec);
    if (!video->dec_ctx)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(video->dec_ctx, ifmt_ctx->streams[video->index]->codecpar);

    /* init the video decoder */
    if ((ret = avcodec_open2(video->dec_ctx, video->dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }

    if( audio) {
        /* select the audio stream */
        ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio->dec, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in the input file\n");
            return ret;
        }
        audio->index = ret;
        // printf( "Using audio %d\n", ret);

        /* create decoding context */
        audio->dec_ctx = avcodec_alloc_context3(audio->dec);
        if (!audio->dec_ctx)
            return AVERROR(ENOMEM);
        avcodec_parameters_to_context(audio->dec_ctx, ifmt_ctx->streams[audio->index]->codecpar);

        /* init the audio decoder */
        if ((ret = avcodec_open2(audio->dec_ctx, audio->dec, NULL)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
            return ret;
        }
    }

    return 0;
}

static int init_audio_filters(const char *filters_descr, input_stream_t *audio, AVFormatContext *ifmt_ctx)
{
    char args[512];
    int ret = 0;
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    const AVFilterLink *outlink;
    AVRational time_base = ifmt_ctx->streams[audio->index]->time_base;

    audio->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !audio->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (!audio->dec_ctx->channel_layout)
        audio->dec_ctx->channel_layout = av_get_default_channel_layout(audio->dec_ctx->channels);
    snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
             time_base.num, time_base.den, audio->dec_ctx->sample_rate,
             av_get_sample_fmt_name(audio->dec_ctx->sample_fmt), audio->dec_ctx->channel_layout);
    ret = avfilter_graph_create_filter(&audio->buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, audio->filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&audio->buffersink_ctx, abuffersink, "out",
                                       NULL, NULL, audio->filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(audio->buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
        goto end;
    }

    ret = av_opt_set_int_list(audio->buffersink_ctx, "channel_layouts", out_channel_layouts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
        goto end;
    }

    ret = av_opt_set_int_list(audio->buffersink_ctx, "sample_rates", out_sample_rates, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
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
    outputs->filter_ctx = audio->buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = audio->buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(audio->filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(audio->filter_graph, NULL)) < 0)
        goto end;

    /* Print summary of the sink buffer
     * Note: args buffer is reused to store channel layout string */
    outlink = audio->buffersink_ctx->inputs[0];
    av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
    // av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
    //        (int)outlink->sample_rate,
    //        (char *)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
    //        args);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int init_video_filters(const char *filters_descr, input_stream_t *video, AVFormatContext *ifmt_ctx)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = ifmt_ctx->streams[video->index]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    video->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !video->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            video->dec_ctx->width, video->dec_ctx->height, video->dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            video->dec_ctx->sample_aspect_ratio.num, video->dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&video->buffersrc_ctx, buffersrc, "in",
                                       args, NULL, video->filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&video->buffersink_ctx, buffersink, "out",
                                       NULL, NULL, video->filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(video->buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
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
    outputs->filter_ctx = video->buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = video->buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(video->filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(video->filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int interrupt_cb(void *ctx)
{
    return input_finished;
}

static void *input_thread( void *_passed)
{
    input_thread_t  *passed = _passed;
    output_t        *output = passed->output;
    AVFormatContext *ifmt_ctx = NULL;
    input_stream_t   audio_in_ctx = {0};
    input_stream_t   video_in_ctx = {0};
    time_t           start = time(NULL);
    bool             lock_audio_pts = false;
    bool             lock_video_pts = false;
    int64_t          audio_start_dts = 0;
    int64_t          audio_start_pts = 0;
    int64_t          video_start_dts = 0;
    int64_t          video_start_pts = 0;
    char            *in_filename = passed->filename;
    int              ret;
    size_t           max_audio = 0;
    size_t           max_video = 0;
    AVChapter       *chapter = NULL;

    pthread_detach( pthread_self());

    printf( "Preparing %s\n", in_filename);

    av_dict_set(&passed->opt, "threads", "0", 0);

    ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, &passed->opt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        goto end;
    }
    ifmt_ctx->interrupt_callback.callback = interrupt_cb;
    ifmt_ctx->interrupt_callback.opaque = NULL;
    ifmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;

    ret = open_input_file(&video_in_ctx, !passed->intro_only ? &audio_in_ctx:NULL, ifmt_ctx);
    if (ret < 0)
        goto end;
    char new_filt[512];
    if( !passed->intro_only) {
        snprintf( new_filt, sizeof( new_filt), video_filter_descr, output->screen_width, output->screen_height, output->screen_width,
                    output->screen_height, output->frame_rate);
    }
    else {
        snprintf( new_filt, sizeof( new_filt), video_filter_descr, output->screen_width / 4, output->screen_height / 4, output->screen_width / 4,
                    output->screen_height / 4, output->frame_rate);
    }
    ret = init_video_filters(new_filt, &video_in_ctx, ifmt_ctx);
    if (ret < 0)
         goto end;

    snprintf( new_filt, sizeof( new_filt), audio_filter_descr, out_sample_rates[0]);
    ret = init_audio_filters(new_filt, &audio_in_ctx, ifmt_ctx);
    if (ret < 0)
          goto end;

    bool synced = false;
    bool skip_read = true;

    AVPacket pkt;
    // while( !synced) {
    //     ret = av_read_frame(ifmt_ctx, &pkt);
    //     if (ret < 0)
    //         break;

    //     if (pkt.stream_index == video_in_ctx.index) {
    //         if( pkt.flags & AV_PKT_FLAG_KEY) {
    //             start = time(NULL);
    //             synced = true;
    //         }
    //         else {
    //             av_packet_unref(&pkt);
    //         }
    //     }
    //     else {
    //         av_packet_unref(&pkt);
    //     }
    // }
    synced = true;
    skip_read = false;

    sem_wait( &output->start_file_sending);

    char *p = passed->filename - 1;
    if( p) {
        while( strchr( p + 1, '/')) {
            p = strchr( p + 1, '/');
        }
        output->current_name = strdup( p + 1);
    }
    p = strchr( output->current_name, '(');
    if( p) {
        *p = 0;
        output->current_year = atoi( p + 1);
    }
    p = strchr( output->current_name, '[');
    if( p) {
        *p = 0;
    }
    p = strchr( output->current_name, '.');
    if( p) {
        *p = 0;
    }
    p = output->current_name;
    while( *p) {
        if( '_' == *p) {
            *p = ' ';
        }
        p++;
    }

    if (!(output->oc->oformat->flags & AVFMT_NOFILE)) {
        chapter            = calloc( 1, sizeof( *chapter));
        chapter->id        = output->chapter_id++;
        chapter->time_base = output->video_out_ctx.enc_ctx->time_base;
        chapter->start     = output->video_out_ctx.next_pts;
        chapter->end       = chapter->start;
        av_dict_set(&chapter->metadata, "title", output->current_name, 0);
        // chapter->metadata  = dict;

        output->oc->nb_chapters++;
        output->oc->chapters = realloc( output->oc->chapters, output->oc->nb_chapters * sizeof( AVChapter *));
        output->oc->chapters[output->oc->nb_chapters - 1] = chapter;
    }

    const int output_frame_size = passed->audio_output->enc_ctx->frame_size;
    if( output_frame_size < 0) {
        fprintf( stderr, "Frame size negative for %s\n", passed->filename);
        synced = false;
    }
    time_t secs = time(NULL);
    while( !input_finished && synced) {
        pthread_mutex_lock(&output->packet_mutex);
        max_audio = MAX( max_audio, list_length( output->audio_packets));
        max_video = MAX( max_video, list_length( output->video_packets));
        const bool skip = (list_length( output->video_packets) > output->frame_rate)  || (list_length( output->audio_packets) > (output->frame_rate * 2));
        pthread_mutex_unlock(&output->packet_mutex);

#if 0
        sem_wait( &output->packet_slots);
#elif 0
        int s;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += NANO_SECOND / 50;
        while ((s = sem_timedwait(&output->packet_slots, &ts)) == -1 && errno == EINTR)
            ;       /* Restart if interrupted by handler */
        if( ETIMEDOUT == errno) {
            printf( "Timed out waiting for the buffer to be used\n");
            continue;
        }
#else
        if( skip && !(output->need_audio || output->need_video)) {
            usleep( MICRO_SECOND / output->frame_rate);
            // printf( "[%4zu %4zu] %d %d\n", list_length( output->video_packets), list_length( output->audio_packets), output->need_video, output->need_audio);
            continue;
        }
#endif

        if( !skip_read) {
            ret = av_read_frame(ifmt_ctx, &pkt);
            if (ret < 0)
                break;
        }
        skip_read = false;

        if ( synced && (pkt.stream_index == video_in_ctx.index)) {
            // log_packet(ifmt_ctx, &pkt, "Video");
            video_in_ctx.frames++;
            video_in_ctx.total_duration += pkt.duration;
            if( lock_video_pts) {
                lock_video_pts = false;
                video_start_pts = pkt.pts;
                video_start_dts = pkt.pts;
            }
            pkt.pts -= video_start_pts;
            pkt.dts -= video_start_dts;
            // av_packet_rescale_ts(&pkt, video_in_ctx.dec_ctx->time_base, AV_TIME_BASE_Q); // passed->video_output->enc_ctx->time_base);

            if( pkt.flags & AV_PKT_FLAG_KEY) {
                if( verbose) log_packet(ifmt_ctx, &pkt, "KEY"); // , &AV_TIME_BASE_Q);
                // printf( "Video PTS %ld %ld %ld, Dec:(%d/%d) Enc:(%d/%d)\n", pkt.pts, pkt.dts, pkt.duration,
                //         video_in_ctx.dec_ctx->time_base.num, video_in_ctx.dec_ctx->time_base.den,
                //         passed->video_output->enc_ctx->time_base.num, passed->video_output->enc_ctx->time_base.den);
            }

            ret = avcodec_send_packet(video_in_ctx.dec_ctx, &pkt);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Video Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                AVFrame frame = {0};
                ret = avcodec_receive_frame(video_in_ctx.dec_ctx, &frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                frame.pts = frame.best_effort_timestamp;

                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(video_in_ctx.buffersrc_ctx, &frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                    break;
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
                    AVFrame *filt_frame = av_frame_alloc();
                    ret = av_buffersink_get_frame(video_in_ctx.buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        av_frame_free(&filt_frame);
                        break;
                    }
                    else if (ret < 0) {
                        av_frame_free(&filt_frame);
                        goto end;
                    }
                    pthread_mutex_lock(&output->packet_mutex);
                    list_push( output->video_packets, filt_frame);
                    // list_push( output->video_ppts, av_rescale_q( pkt.pts, ifmt_ctx->streams[pkt.stream_index]->time_base, AV_TIME_BASE_Q));
                    sem_post( &output->video_available);
                    pthread_mutex_unlock(&output->packet_mutex);
                }
                av_frame_unref(&frame);
            }
        }
        else if( synced && (pkt.stream_index == audio_in_ctx.index)) {
            // log_packet(ifmt_ctx, &pkt, "Audio");
            audio_in_ctx.frames++;
            audio_in_ctx.total_duration += pkt.duration;
            if( lock_audio_pts) {
                lock_audio_pts = false;
                audio_start_pts = pkt.pts;
                audio_start_dts = pkt.pts;
            }
            pkt.pts -= audio_start_pts;
            pkt.dts -= audio_start_dts;
            // av_packet_rescale_ts(&pkt, audio_in_ctx.dec_ctx->time_base, passed->audio_output->enc_ctx->time_base);
            // printf( "Audio PTS %ld %ld %ld, Dec:(%d/%d) Enc:(%d/%d)\n", pkt.pts, pkt.dts, pkt.duration,
            //         audio_in_ctx.dec_ctx->time_base.num, audio_in_ctx.dec_ctx->time_base.den,
            //         audio_out_ctx.enc_ctx->time_base.num, audio_out_ctx.enc_ctx->time_base.den);
            // log_packet(ifmt_ctx, &pkt, "A_RAW");
            int finished = 0;
            ret = avcodec_send_packet(audio_in_ctx.dec_ctx, &pkt);
            if( ret < 0)
                goto cleanup;

            while (ret >= 0) {
                AVFrame input_frame = {0};
                ret = avcodec_receive_frame(audio_in_ctx.dec_ctx, &input_frame);
                if( ret == AVERROR_EOF) {
                    finished = 1;
                    ret = 0;
                    break;
                }
                else if (ret == AVERROR(EAGAIN)) {
                    ret = 0;
                    break;
                }
                else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    break;
                }

                /* push the audio data from decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(audio_in_ctx.buffersrc_ctx, &input_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
                    break;
                }
                /* pull filtered audio from the filtergraph */
                while (1) {
                    AVFrame filt_frame  = {0};
                    ret = av_buffersink_get_frame(audio_in_ctx.buffersink_ctx, &filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                     if (ret < 0)
                        goto end;
                    // print_frame(filt_frame);
                    if (add_samples_to_fifo(passed->audio_output->fifo, filt_frame.extended_data, filt_frame.nb_samples))
                        goto cleanup;
                    av_frame_unref(&filt_frame);
                }
                av_frame_unref(&input_frame);
            }
            while (av_audio_fifo_size(passed->audio_output->fifo) >= output_frame_size ||
                (finished && av_audio_fifo_size(passed->audio_output->fifo) > 0)) {
                /* Temporary storage of the output samples of the frame written to the file. */
                AVFrame *output_frame;

                const int frame_size = FFMIN(av_audio_fifo_size(passed->audio_output->fifo), output_frame_size);

                /* Initialize temporary storage for one output frame. */
                if (init_output_frame(&output_frame, passed->audio_output->enc_ctx, frame_size))
                    break;

                if (av_audio_fifo_read(passed->audio_output->fifo, (void **)output_frame->data, frame_size) < frame_size) {
                    fprintf(stderr, "Could not read data from FIFO\n");
                    av_frame_free(&output_frame);
                    break;
                }

                pthread_mutex_lock(&output->packet_mutex);
                output_frame->pts = pkt.pts;
                list_push( output->audio_packets, output_frame);
                // list_push( output->audio_ppts, av_rescale_q( pkt.pts, ifmt_ctx->streams[pkt.stream_index]->time_base, AV_TIME_BASE_Q));
                sem_post( &output->audio_available);
                // sem_post( &output->packets_available);
                pthread_mutex_unlock(&output->packet_mutex);
            }
        }
        av_packet_unref(&pkt);

        const time_t now = time(NULL);
        if( secs != now) {
            // int val;
            // sem_getvalue( &output->packet_slots, &val);
            // printf( "%08ld %d\n", now, val);
            secs = now;
        }
    }
    // printf( "Input Finished %d\n", input_finished);
    if (!(output->oc->oformat->flags & AVFMT_NOFILE)) {
        chapter->end = output->video_out_ctx.next_pts;
    }
    goto ok;

cleanup:;
    printf( "Cleanup\n");
    goto ok;
end:;
    printf( "End\n");
ok:;

    const time_t elapsed = time(NULL) - start;
    output->total_time   += elapsed;
    output->total_frames += video_in_ctx.frames;

    if( elapsed) {
        const int fps = video_in_ctx.frames / elapsed;
        printf( "%s, Time to process %ld, Total Frames %d, FPS %d\n", in_filename, elapsed, video_in_ctx.frames, fps);
    }

    // printf( "MAX Video:%zu, Audio:%zu\n", max_video, max_audio);
    // AVRational *time_baseV = &ifmt_ctx->streams[video_in_ctx.index]->time_base;
    // printf( "Video Duration %6s (%d)\n", av_ts2timestr(video_in_ctx.total_duration, time_baseV), output->video_out_ctx.frames);
    // AVRational *time_baseA = &ifmt_ctx->streams[audio_in_ctx.index]->time_base;
    // printf( "Video Duration %6s (%d)\n", av_ts2timestr(audio_in_ctx.total_duration, time_baseA), output->audio_out_ctx.frames);

    bool more = true;
    while( more) {
        pthread_mutex_lock(&output->packet_mutex);
        more = VALID_PACKET_CHECK( output->audio_packets) && VALID_PACKET_CHECK( output->video_packets);
        if( more) {
            if( list_length( output->audio_packets)) { sem_post( &output->audio_available); /* sem_post( &output->packets_available); */ }
            if( list_length( output->video_packets)) { sem_post( &output->video_available); /* sem_post( &output->packets_available);*/ }
        }
        pthread_mutex_unlock(&output->packet_mutex);
        usleep(100);
    }
    // printf( "V:%zu, A:%zu\n", list_length( output->video_packets), list_length( output->audio_packets));

    pthread_mutex_lock(&output->packet_mutex);
    list_each( output->video_packets, _fr) {
        av_frame_free( &_fr);
    }
    list_clear( output->video_packets);
    // list_clear( output->video_ppts);
    list_each( output->audio_packets, _fr) {
        av_frame_free( &_fr);
    }
    list_clear( output->audio_packets);
    // list_clear( output->audio_ppts);
    pthread_mutex_unlock(&output->packet_mutex);

    // printf( "Final PTSs are Video:%s, Audio %s\n", av_ts2timestr(video_out_ctx.next_pts, &video_out_ctx.enc_ctx->time_base),
    //              av_ts2timestr(audio_out_ctx.next_pts, &audio_out_ctx.enc_ctx->time_base));

#if 1
    output->current_year = 0;
    output->play_silence_packets = 5 * output->frame_rate;
    output->play_silence = true;
    sem_post( &output->audio_available);
    sem_post( &output->video_available);
    // sem_post( &output->packets_available);
    while( output->play_silence && output->play_silence_packets) {
        // printf( "Silence %d\n", play_silence_packets);
        usleep(100);
    }
#elif 1
    const int max = video_out_ctx.frames + (5 * 25);
    play_silence = true;
    while( video_out_ctx.frames < max) {
        // printf( "Video Packets:%d (%d)\n", video_out_ctx.frames, max);
        sem_post( &packets_available);
        usleep(100);
    }
    play_silence = false;
#endif

    sem_post( &output->start_file_sending);
    sem_post( &output->free_to_start);

    // swr_free(&audio_in_ctx.swr_ctx);
    avfilter_graph_free(&video_in_ctx.filter_graph);
    if( video_in_ctx.dec_ctx)
        avcodec_free_context(&video_in_ctx.dec_ctx);
    avfilter_graph_free(&audio_in_ctx.filter_graph);
    if( audio_in_ctx.dec_ctx)
        avcodec_free_context(&audio_in_ctx.dec_ctx);
    avformat_close_input(&ifmt_ctx);

    free( passed->filename);
    free( passed);

    return NULL;
}

static void deleted_from_dir( bool isdir, char *name, void *_not)
{
    (void)isdir;
    inotify_t *inot = _not;
    char full[ 1024];

    snprintf( full, sizeof( full), "%s%s", inot->name, name);
    printf( "[Removing %s] ", full);
    list_remove( inot->input->files, full);
}

static void written_to_dir( bool isdir, char *name, void *_not)
{
    (void)isdir;
    inotify_t *inot = _not;
    char full[ 1024];

    snprintf( full, sizeof( full), "%s%s", inot->name, name);
    printf( "[Adding %s] ", full);
    list_push( inot->input->files, full);
}

static void *start_input_processing( void *_passed)
{
    input_t *input = _passed;

    printf( "Videos Available at %s\n", input->directory);

    const int ret = list_op( input->directory, input);

    inotify_t *dir_check = calloc( 1, sizeof( *dir_check));
    dir_check->deleted = deleted_from_dir;
    dir_check->written = written_to_dir;
    dir_check->input   = input;
    dir_check->ptr     = (void *)&input->files;
    dir_check->name    = input->directory;
    dir_check->fd      = inotify_add_watch( inotify_thread_fd, input->directory, IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CLOSE_WRITE);
    list_push( notifications, dir_check);

    if( ret < 0) {
        input_thread_t *pass_on = calloc( 1, sizeof( *pass_on));
        pthread_t nowt;

        pass_on->filename     = strdup( input->directory);
        pass_on->output       = input->output;
        pass_on->audio_output = &input->output->audio_out_ctx;
        pass_on->video_output = &input->output->video_out_ctx;
        pass_on->intro_only   = false;
        pthread_create( &nowt, NULL, input_thread, pass_on);
        sem_wait( &input->output->free_to_start);
    }
    else {
        size_t rcnt = list_length( input->files);
        // const size_t num = list_length(input->files);
        sem_post( &input->output->free_to_start);
        do {
            char *f = list_front( input->files);
            if( input->random) {
                size_t where = rand() % rcnt;
                printf( "Choosing %zu of %zu (%zu)\n", where, rcnt, list_length( input->files));
                list_each( input->files, _f) {
                    if( !where) {
                        f = _f;
                        break;
                    }
                    where--;
                }
                if( !--rcnt) {
                    rcnt = list_length( input->files);
                }
            }

            input_thread_t *pass_on = calloc( 1, sizeof( *pass_on));
            pthread_t nowt;

            pass_on->filename     = strdup( f);
            pass_on->output       = input->output;
            pass_on->audio_output = &input->output->audio_out_ctx;
            pass_on->video_output = &input->output->video_out_ctx;
            pass_on->intro_only   = false;
            pthread_create( &nowt, NULL, input_thread, pass_on);
            list_remove( input->files, f);
            if( input->loop) {
                list_push( input->files, f);
            }
            else {
                free( f);
            }
            sem_wait( &input->output->free_to_start);
        } while( !input_finished && list_length( input->files));
        sem_wait( &input->output->free_to_start);
        list_clear( input->files);
    }
    free( input->directory);
    free( _passed);

    return NULL;
}

static void * calibrate_sleep(void *_config)
{
    (void)_config;

    struct timeval tv1, tv2;
    uint64_t diff = 0;
    int loops = 0;

    printf("Calibrating sleep timeout...\r\n");
    do {
        gettimeofday(&tv1, NULL);
        usleep(1);
        gettimeofday(&tv2, NULL);
        diff += timeval_diff_usec(&tv1, &tv2) - 1;
    } while (loops++ != 10000);

    calibrate_overhead = diff / loops;

    printf("usleep(1) overhead: %ld us\r\n", calibrate_overhead);

    pthread_exit(0);
}

static void exit_program()
{
    list_each( notifications, _inot) {
        inotify_rm_watch( inotify_thread_fd, _inot->fd);
        free( _inot);
    }
    shutdown( inotify_thread_fd, SHUT_RDWR);

    list_each( inputs_running, input) {
        pthread_join(input->thread_id, NULL);
    }

    input_finished = true;

    list_each( outputs_running, output) {
        sem_post(&output->audio_available);
        sem_post(&output->video_available);
        // sem_post(&output->packets_available);
        pthread_join(output->thread_id, NULL);
    }

    avformat_network_deinit();
}

static struct sigaction act;

static void sig_handler2( int signal, siginfo_t *info, void *ptr)
{
    (void)signal;
    (void)info;
    (void)ptr;
    input_finished = true;

    sleep(1);
    exit(0);
}

/**************************************************************/
/* media file output */
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <options> \042video_directory\042 \042output_name\042\n", argv[0]);
        printf( "  -flags/-fflags <flag>  Pass on flags to ffmpeg libraries\n");
        printf( "  -re                    Rate limited playout\n");
        printf( "  -loop                  Play in loop\n");
        printf( "  -random                Randomize playout\n");
        printf( "  -v                     Verbose Level\n");
        printf( "Output Parameters\n");
        printf( " Video\n");
        printf( "  -bitrate               Bitrate                          (default: %0.2f kb/s)\n", STREAM_BITRATE / 1024.0);
        printf( "  -fps                   Framerate                        (default: %d)\n", STREAM_FRAME_RATE);
        printf( "  -width                 Screen Width                     (default: %d)\n", SCREEN_WIDTH);
        printf( "  -height                Screen Height                    (default: %d)\n", SCREEN_HEIGHT);
        printf( "  -overlay               Overlay Image\n");
        printf( " Audio\n");
        printf( "  -arate                 Bitrate                          (default: %0.2f kb/s\n", AUDIO_BITRATE / 1024.0);
        return 1;
    }

    act.sa_sigaction = sig_handler2;
    act.sa_flags = SA_SIGINFO;
    sigaction(SIGINT, &act, NULL);

    atexit( exit_program);

    pthread_t calibrate;
    const int error = pthread_create( &calibrate, NULL, calibrate_sleep, (void *)NULL);
    if (!error) {
        pthread_join(calibrate, NULL);
    }

    srand(time(NULL));

    pthread_create( &inotify_thread_id, NULL, inotify_thread, NULL);

    avformat_network_init();

    output_t *output = calloc( 1, sizeof( *output));
    output->bitrate              = STREAM_BITRATE;
    output->frame_rate           = STREAM_FRAME_RATE;
    output->screen_width         = SCREEN_WIDTH;
    output->screen_height        = SCREEN_HEIGHT;
    output->audio_bitrate        = AUDIO_BITRATE;

    input_t *input = calloc( 1, sizeof( *input));
    input->output = output;

    bool ok = true;
    for( int i = 1; ok && i < argc; i++) {
        bool another = (i + 1) != argc;
        if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags")) {
            if( another) {
                av_dict_set(&output->opt, argv[i]+1, argv[ i+1], 0);
                i++;
            }
            else {
                printf( "ERROR:\042%s\042 needs a parameter\n", argv[i]);
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-re")) {
            output->live_stream = true;
        }
        else if (!strcmp(argv[i], "-bitrate")) {
            if( another) {
                output->bitrate = evaluate( argv[i+1]);
                i++;
            }
            if( !another || !output->bitrate) {
                printf( "ERROR: Bitate not valid\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-fps")) {
            if( another) {
                output->frame_rate = evaluate( argv[i+1]);
                i++;
            }
            if( !another || !output->frame_rate) {
                printf( "ERROR: Frame rate not valid\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-width")) {
            if( another) {
                output->screen_width = evaluate( argv[i+1]);
                i++;
            }
            if( !another || !output->screen_width) {
                printf( "ERROR: Screen Width not valid\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-height")) {
            if( another) {
                output->screen_height = evaluate( argv[i+1]);
                i++;
            }
            if( !another || !output->screen_height) {
                printf( "ERROR: Screen Height not valid\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-arate")) {
            if( another) {
                output->audio_bitrate = evaluate( argv[i+1]);
                i++;
            }
            if( !another || !output->audio_bitrate) {
                printf( "ERROR: Audio bitrate not valid\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-overlay")) {
            if( another) {
                output->overlay_file = strdup( argv[i+1]);
                i++;
            }
            if( !another || !output->overlay_file) {
                printf( "ERROR: Overlay file error\n");
                ok = false;
            }
        }
        else if (!strcmp(argv[i], "-loop")) {
            input->loop = true;
        }
        else if (!strcmp(argv[i], "-random")) {
            input->random = true;
        }
        else if (!memcmp(argv[i], "-v", 2)) {
            verbose = 0;
            char *p = &argv[i][1];
            while( 'v' == *p) {
                verbose++;
                p++;
            }
        }
        else if( '-' == argv[i][0]) {
            printf( "ERROR: What is this options '%s', ignoring\n", argv[i]);
            ok = false;
        }
        else {
            if( input->directory) {
                if( output->output_filename) {
                    printf( "Starting Output to '%s' from '%s'\n", output->output_filename, input->directory);
                    list_push( outputs_running, output);
                    pthread_create( &output->thread_id, NULL, output_thread, output);
                    list_push( inputs_running, input);
                    pthread_create( &input->thread_id, NULL, start_input_processing, input);

                    output = calloc( 1, sizeof( *output));
                    output->bitrate              = STREAM_BITRATE;
                    output->frame_rate           = STREAM_FRAME_RATE;
                    output->screen_width         = SCREEN_WIDTH;
                    output->screen_height        = SCREEN_HEIGHT;
                    output->audio_bitrate        = AUDIO_BITRATE;

                    input = calloc( 1, sizeof( *input));
                    input->output = output;
                    input->directory = strdup( argv[i]);
                }
                else {
                    output->output_filename = strdup( argv[i]);
                }
            }
            else {
                input->directory = strdup( argv[i]);
            }
        }
    }

    if( !input->directory) {
        printf( "ERROR:Needs an video directory\n");
        return 1;
    }

    if( !output->output_filename) {
        printf( "ERROR:Needs an output filename\n");
        return 1;
    }

    printf( "Starting Output to '%s' from '%s'\n", output->output_filename, input->directory);
    // pthread_mutex_init(&output_mutex, NULL);
    // pthread_cond_init(&output_cond, NULL);

    av_log_set_level(AV_LOG_QUIET);

    list_push( outputs_running, output);
    pthread_create( &output->thread_id, NULL, output_thread, output);

    // av_log_set_level(AV_LOG_DEBUG);

    list_push( inputs_running, input);
    pthread_create( &input->thread_id, NULL, start_input_processing, input);

    return 0;
}
