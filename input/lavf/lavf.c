/* Network input through libavformat/libavcodec.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "common/common.h"
#include "input/input.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

enum { VIDEO, AUDIO, STREAMS };
typedef struct
{
    obe_t *h;
    obe_input_probe_t *probe;
    obe_input_params_t *params;
    obe_device_t *device; /* owned only during probe */
    AVFormatContext *format;
    AVCodecContext *decoder[STREAMS];
    AVFrame *frame, *first[STREAMS];
    AVPacket *packet;
    AVDictionary *options;
    SwrContext *swr;
    struct SwsContext *sws;
    int index[STREAMS];
    int video_format;
    int64_t deadline, origin, wall_start, last[STREAMS];
    unsigned frames[STREAMS];
    obe_raw_frame_t *pending;
} url_input_t;

static int interrupted( void *opaque )
{
    url_input_t *ctx = opaque;
    return av_gettime_relative() >= ctx->deadline;
}

/* FFmpeg owns internal locks/threads: only permit cancellation at our safe
 * boundaries. Its interrupt callback bounds blocking network operations. */
static void cancellation_point( void )
{
    pthread_setcancelstate( PTHREAD_CANCEL_ENABLE, NULL );
    pthread_testcancel();
    pthread_setcancelstate( PTHREAD_CANCEL_DISABLE, NULL );
}

static void release_pending( url_input_t *ctx )
{
    if( ctx->pending )
    {
        ctx->pending->release_data( ctx->pending );
        ctx->pending->release_frame( ctx->pending );
        ctx->pending = NULL;
    }
}

static void close_input( void *opaque )
{
    url_input_t *ctx = opaque;
    if( ctx->params )
        fprintf(stderr, "URL input: delivered %u video frames and %u audio blocks\n",
                ctx->frames[VIDEO], ctx->frames[AUDIO]);
    release_pending( ctx );
    swr_free( &ctx->swr );
    sws_freeContext( ctx->sws );
    av_packet_free( &ctx->packet );
    av_frame_free( &ctx->frame );
    for( int i = 0; i < STREAMS; i++ )
    {
        av_frame_free( &ctx->first[i] );
        avcodec_free_context( &ctx->decoder[i] );
    }
    avformat_close_input( &ctx->format );
    av_dict_free( &ctx->options );
    if( ctx->device )
        destroy_device( ctx->device );
    if( ctx->probe )
    {
        free( ctx->probe->user_opts.location );
        free( ctx->probe );
    }
    free( ctx->params );
}

static void report_error( const char *operation, int error )
{
    char reason[AV_ERROR_MAX_STRING_SIZE];
    av_strerror( error, reason, sizeof(reason) );
    fprintf( stderr, "URL input: %s: %s\n", operation, reason );
}

static int open_input( url_input_t *ctx, const char *location )
{
    /* Network URLs only; protocols still depend on the linked FFmpeg build. */
    const char *schemes[] = { "udp://", "rtp://", "http://", "https://", "rtsp://", "tcp://" };
    int valid = 0, ret;
    for( unsigned i = 0; i < sizeof(schemes)/sizeof(*schemes); i++ )
        valid |= !strncmp( location, schemes[i], strlen(schemes[i]) );
    if( !valid )
    {
        fprintf( stderr, "URL input: supported schemes are udp, rtp, http(s), rtsp and tcp\n" );
        return AVERROR(EINVAL);
    }
    ctx->format = avformat_alloc_context();
    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    if( !ctx->format || !ctx->frame || !ctx->packet )
        return AVERROR(ENOMEM);
    ctx->format->interrupt_callback = (AVIOInterruptCB){ interrupted, ctx };
    av_dict_set( &ctx->options, "protocol_whitelist", "udp,rtp,tcp,http,https,tls,crypto,httpproxy", 0 );
    av_dict_set( &ctx->options, "rw_timeout", "2000000", 0 );
    /* RTP disables libavformat's UDP FIFO. Retain enough socket buffering
     * while consuming the packets collected by find_stream_info at CFR pace. */
    av_dict_set( &ctx->options, "buffer_size", "4194304", 0 );
    av_dict_set( &ctx->options, "probesize", "2000000", 0 );
    av_dict_set( &ctx->options, "analyzeduration", "1000000", 0 );
    if( (ret = avformat_open_input( &ctx->format, location, NULL, &ctx->options )) < 0 ||
        (ret = avformat_find_stream_info( ctx->format, NULL )) < 0 )
        return ret;
    cancellation_point();

    ctx->index[VIDEO] = av_find_best_stream( ctx->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0 );
    if( ctx->index[VIDEO] < 0 )
        return ctx->index[VIDEO];
    ctx->index[AUDIO] = av_find_best_stream( ctx->format, AVMEDIA_TYPE_AUDIO, -1, ctx->index[VIDEO], NULL, 0 );
    /* MPEG-TS discovery order can change on reopening a live URL. Preserve
     * the probed track IDs (PIDs for TS), not libavformat's array indices. */
    if( ctx->params )
        for( int i = 0; i < ctx->params->device->num_input_streams; i++ )
        {
            int index = -1;
            for( unsigned j = 0; j < ctx->format->nb_streams; j++ )
                if( ctx->format->streams[j]->id == ctx->params->device->input_streams[i]->pid &&
                    ctx->format->streams[j]->codecpar->codec_type == (i == VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO) )
                    index = j;
            if( index < 0 ) return AVERROR_INPUT_CHANGED;
            ctx->index[i] = index;
        }
    for( int i = 0; i < STREAMS; i++ )
    {
        if( ctx->index[i] < 0 )
            continue;
        AVStream *stream = ctx->format->streams[ctx->index[i]];
        const AVCodec *codec = avcodec_find_decoder( stream->codecpar->codec_id );
        if( !codec )
            return AVERROR_DECODER_NOT_FOUND;
        ctx->decoder[i] = avcodec_alloc_context3( codec );
        if( !ctx->decoder[i] )
            return AVERROR(ENOMEM);
        if( (ret = avcodec_parameters_to_context( ctx->decoder[i], stream->codecpar )) < 0 )
            return ret;
        ctx->decoder[i]->pkt_timebase = stream->time_base;
        ctx->decoder[i]->thread_count = 2;
        if( (ret = avcodec_open2( ctx->decoder[i], codec, NULL )) < 0 )
            return ret;
    }
    return 0;
}

/* Match OBE's field-aware filter formats without guessing from resolution. */
static int video_format( AVFrame *f, AVRational rate )
{
    int interlaced = !!(f->flags & AV_FRAME_FLAG_INTERLACED);
    if( f->width == 720 && f->height == 576 && interlaced && av_cmp_q(rate, (AVRational){25,1}) == 0 )
        return INPUT_VIDEO_FORMAT_PAL;
    if( f->width == 720 && f->height == 480 && interlaced && av_cmp_q(rate, (AVRational){30000,1001}) == 0 )
        return INPUT_VIDEO_FORMAT_NTSC;
    if( f->width == 1920 && f->height == 1080 && interlaced )
    {
        if( !av_cmp_q(rate, (AVRational){25,1}) ) return INPUT_VIDEO_FORMAT_1080I_50;
        if( !av_cmp_q(rate, (AVRational){30000,1001}) ) return INPUT_VIDEO_FORMAT_1080I_5994;
        if( !av_cmp_q(rate, (AVRational){30,1}) ) return INPUT_VIDEO_FORMAT_1080I_60;
    }
    /* Progressive formats have the same filter path; fps remains exact. */
    if( !interlaced && f->width > 0 && f->width <= 1920 && f->height > 0 && f->height <= 1080 &&
        av_q2d(rate) >= 1 && av_q2d(rate) <= 60 )
        return INPUT_VIDEO_FORMAT_UNDEFINED;
    return -1;
}

static int describe_video( url_input_t *ctx, AVFrame *f, obe_int_input_stream_t *s )
{
    AVStream *st = ctx->format->streams[ctx->index[VIDEO]];
    AVRational rate = av_guess_frame_rate( ctx->format, st, f );
    AVRational sar = av_guess_sample_aspect_ratio( ctx->format, st, f );
    if( rate.num <= 0 || rate.den <= 0 || (ctx->video_format = video_format(f, rate)) < 0 )
        return AVERROR(ENOTSUP);
    switch( f->format )
    {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV422P10LE:
            s->csp = f->format;
            break;
        case AV_PIX_FMT_YUV422P:
            /* The existing field-aware 422 -> 420 filter takes 10-bit planes. */
            s->csp = AV_PIX_FMT_YUV422P10LE;
            break;
        default:
            return AVERROR(ENOTSUP);
    }
    /* HDR requires a separate end-to-end metadata/tone-mapping path. */
    if( f->color_trc == AVCOL_TRC_SMPTE2084 || f->color_trc == AVCOL_TRC_ARIB_STD_B67 )
        return AVERROR(ENOTSUP);
    s->width = f->width;
    s->height = f->height;
    s->interlaced = !!(f->flags & AV_FRAME_FLAG_INTERLACED);
    s->tff = s->interlaced && !!(f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
    s->timebase_num = rate.den;
    s->timebase_den = rate.num;
    s->sar_num = sar.num > 0 ? sar.num : 1;
    s->sar_den = sar.den > 0 ? sar.den : 1;
    s->color_primaries = f->color_primaries;
    s->color_trc = f->color_trc;
    s->colorspace = f->colorspace;
    s->color_range = f->color_range;
    return 0;
}

static int probe_frame( url_input_t *ctx, int kind )
{
    if( !ctx->first[kind] )
    {
        ctx->first[kind] = av_frame_clone( ctx->frame );
        if( !ctx->first[kind] ) return AVERROR(ENOMEM);
    }
    return 0;
}

static int queue_full( obe_queue_t *queue )
{
    pthread_mutex_lock( &queue->mutex );
    int full = queue->size >= 100;
    pthread_mutex_unlock( &queue->mutex );
    return full;
}

static int deliver_frame( url_input_t *ctx, int kind )
{
    AVFrame *f = ctx->frame;
    obe_int_input_stream_t *s = ctx->params->device->input_streams[kind];
    AVStream *st = ctx->format->streams[ctx->index[kind]];
    int64_t timestamp = f->best_effort_timestamp;
    int ret;
    if( timestamp == AV_NOPTS_VALUE )
        return AVERROR_INVALIDDATA;
    timestamp = av_rescale_q( timestamp, st->time_base, (AVRational){1, OBE_CLOCK} );
    if( ctx->origin == AV_NOPTS_VALUE )
    {
        ctx->origin = timestamp - OBE_CLOCK;
        ctx->wall_start = av_gettime_relative();
    }
    int64_t pts = timestamp - ctx->origin;
    if( pts < 0 || (ctx->last[kind] != AV_NOPTS_VALUE &&
        (pts <= ctx->last[kind] || pts - ctx->last[kind] > 2 * OBE_CLOCK)) )
        return AVERROR_INVALIDDATA;
    int64_t due = ctx->wall_start + av_rescale( pts - OBE_CLOCK, 1000000, OBE_CLOCK );
    if( due - av_gettime_relative() > 2000000 )
        return AVERROR_INVALIDDATA;
    int64_t remaining;
    while( (remaining = due - av_gettime_relative()) > 0 )
    {
        cancellation_point();
        av_usleep( MIN( remaining, 10000 ) );
    }
    cancellation_point();
    for( int i = 0; i < ctx->h->num_filters; i++ )
        if( queue_full(&ctx->h->filters[i]->queue) ) return AVERROR(ENOBUFS);
    for( int i = 0; i < ctx->h->num_encoders; i++ )
        if( queue_full(&ctx->h->encoders[i]->queue) ) return AVERROR(ENOBUFS);
    if( queue_full(&ctx->h->enc_smoothing_queue) || queue_full(&ctx->h->mux_queue) ||
        queue_full(&ctx->h->mux_smoothing_queue) ) return AVERROR(ENOBUFS);

    obe_raw_frame_t *rf = ctx->pending = new_raw_frame();
    if( !rf ) return AVERROR(ENOMEM);
    rf->release_data = kind == VIDEO ? obe_release_video_data : obe_release_audio_data;
    rf->release_frame = obe_release_frame;
    rf->input_stream_id = s->input_stream_id;
    rf->pts = pts;
    rf->arrival_time = obe_mdate();
    if( kind == VIDEO )
    {
        obe_int_input_stream_t current = {0};
        if( (ret = describe_video(ctx, f, &current)) < 0 ) return ret;
        if( current.width != s->width || current.height != s->height || current.csp != s->csp ||
            current.interlaced != s->interlaced || current.tff != s->tff ||
            current.timebase_num != s->timebase_num || current.timebase_den != s->timebase_den ||
            current.sar_num != s->sar_num || current.sar_den != s->sar_den ||
            current.color_primaries != s->color_primaries || current.color_trc != s->color_trc ||
            current.colorspace != s->colorspace || current.color_range != s->color_range )
            return AVERROR_INPUT_CHANGED;
        rf->alloc_img.csp = s->csp;
        rf->alloc_img.width = s->width;
        rf->alloc_img.height = s->height;
        rf->alloc_img.planes = 3;
        rf->alloc_img.format = ctx->video_format;
        if( (ret = av_image_alloc(rf->alloc_img.plane, rf->alloc_img.stride, s->width, s->height, s->csp, 32)) < 0 )
            return ret;
        if( f->format == s->csp )
            av_image_copy( rf->alloc_img.plane, rf->alloc_img.stride,
                           (const uint8_t **)f->data, f->linesize, s->csp, s->width, s->height );
        else
        {
            ctx->sws = sws_getCachedContext(ctx->sws, s->width, s->height, f->format,
                s->width, s->height, s->csp, SWS_POINT, NULL, NULL, NULL);
            if( !ctx->sws ) return AVERROR(ENOMEM);
            if( sws_scale(ctx->sws, (const uint8_t *const *)f->data, f->linesize, 0, s->height,
                          rf->alloc_img.plane, rf->alloc_img.stride) != s->height )
                return AVERROR_INVALIDDATA;
        }
        rf->img = rf->alloc_img;
        rf->sar_width = s->sar_num;
        rf->sar_height = s->sar_den;
        rf->timebase_num = s->timebase_num;
        rf->timebase_den = s->timebase_den;
    }
    else
    {
        /* OBE's audio pair selector expects stereo planar PCM. */
        if( f->sample_rate != 48000 || f->ch_layout.nb_channels != 2 )
            return AVERROR_INPUT_CHANGED;
        if( ctx->first[AUDIO] && (f->format != ctx->first[AUDIO]->format ||
            av_channel_layout_compare(&f->ch_layout, &ctx->first[AUDIO]->ch_layout)) )
            return AVERROR_INPUT_CHANGED;
        if( !ctx->swr )
        {
            ctx->first[AUDIO] = av_frame_clone(f);
            if( !ctx->first[AUDIO] ) return AVERROR(ENOMEM);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            ret = swr_alloc_set_opts2(&ctx->swr, &stereo, AV_SAMPLE_FMT_S32P, 48000,
                                      &f->ch_layout, f->format, 48000, 0, NULL);
            if( ret < 0 || (ret = swr_init(ctx->swr)) < 0 ) return ret;
        }
        int samples = swr_get_out_samples(ctx->swr, f->nb_samples);
        if( (ret = av_samples_alloc(rf->audio_frame.audio_data, &rf->audio_frame.linesize,
                                   2, samples, AV_SAMPLE_FMT_S32P, 0)) < 0 ) return ret;
        ret = swr_convert(ctx->swr, rf->audio_frame.audio_data, samples,
                          (const uint8_t **)f->extended_data, f->nb_samples);
        if( ret <= 0 ) return ret < 0 ? ret : AVERROR_INVALIDDATA;
        rf->audio_frame.num_samples = ret;
        rf->audio_frame.channel_layout = AV_CH_LAYOUT_STEREO;
        rf->audio_frame.num_channels = 2;
        rf->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
    }
    avfm_init( &rf->avfm, kind == VIDEO ? AVFM_VIDEO : AVFM_AUDIO_PCM );
    avfm_set_pts_video( &rf->avfm, pts );
    avfm_set_pts_audio( &rf->avfm, pts );
    avfm_set_hw_received_time( &rf->avfm );
    obe_int_input_stream_t *video = ctx->params->device->input_streams[VIDEO];
    avfm_set_video_interval_clk( &rf->avfm, av_rescale(video->timebase_num, OBE_CLOCK, video->timebase_den) );
    if( kind == VIDEO ) obe_clock_tick( ctx->h, pts );
    if( (ret = add_to_filter_queue(ctx->h, rf)) < 0 ) return ret;
    ctx->pending = NULL;
    ctx->last[kind] = pts;
    ctx->frames[kind]++;
    return 0;
}

static int decode_packet( url_input_t *ctx, int kind, AVPacket *packet, int probe )
{
    int ret = avcodec_send_packet(ctx->decoder[kind], packet);
    if( ret < 0 ) return ret;
    while( (ret = avcodec_receive_frame(ctx->decoder[kind], ctx->frame)) >= 0 )
    {
        ret = probe ? probe_frame(ctx, kind) : deliver_frame(ctx, kind);
        av_frame_unref(ctx->frame);
        if( ret < 0 ) return ret;
        cancellation_point();
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

static void *probe_input( void *opaque )
{
    obe_input_probe_t *args = opaque;
    url_input_t ctx = { .h = args->h, .probe = args };
    int ret;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_push(close_input, &ctx);
    ctx.deadline = av_gettime_relative() + args->h->probe_time_seconds * 1000000LL;
    if( (ret = open_input(&ctx, args->user_opts.location)) < 0 ) goto end;
    while( !ctx.first[VIDEO] || (ctx.decoder[AUDIO] && !ctx.first[AUDIO]) )
    {
        if( (ret = av_read_frame(ctx.format, ctx.packet)) < 0 ) goto end;
        for( int i = 0; i < STREAMS; i++ )
            if( ctx.packet->stream_index == ctx.index[i] && (ret = decode_packet(&ctx, i, ctx.packet, 1)) < 0 ) goto end;
        av_packet_unref(ctx.packet);
        cancellation_point();
        if( interrupted(&ctx) ) { ret = AVERROR(ETIMEDOUT); goto end; }
    }
    ctx.device = new_device();
    if( !ctx.device ) { ret = AVERROR(ENOMEM); goto end; }
    ctx.device->device_type = INPUT_URL;
    for( int i = 0; i < STREAMS && ctx.first[i]; i++ )
    {
        obe_int_input_stream_t *s = calloc(1, sizeof(*s));
        if( !s ) { ret = AVERROR(ENOMEM); goto end; }
        ctx.device->input_streams[ctx.device->num_input_streams++] = s;
        s->lavf_stream_idx = ctx.index[i];
        s->pid = ctx.format->streams[ctx.index[i]]->id;
        s->stream_type = i == VIDEO ? STREAM_TYPE_VIDEO : STREAM_TYPE_AUDIO;
        s->stream_format = i == VIDEO ? VIDEO_UNCOMPRESSED : AUDIO_PCM;
        if( i == VIDEO )
        {
            if( (ret = describe_video(&ctx, ctx.first[i], s)) < 0 ) goto end;
        }
        else
        {
            if( ctx.first[i]->sample_rate != 48000 || ctx.first[i]->ch_layout.nb_channels != 2 )
            {
                fprintf(stderr, "URL input: audio must be stereo at 48000 Hz\n");
                ret = AVERROR(ENOTSUP); goto end;
            }
            s->channel_layout = AV_CH_LAYOUT_STEREO;
            s->num_channels = 2;
            s->sample_rate = 48000;
            s->sample_format = AV_SAMPLE_FMT_S32P;
            s->sdi_audio_pair = 1;
        }
        pthread_mutex_lock(&ctx.h->device_list_mutex);
        s->input_stream_id = ctx.h->cur_input_stream_id++;
        pthread_mutex_unlock(&ctx.h->device_list_mutex);
    }
    ctx.device->user_opts = args->user_opts;
    args->user_opts.location = NULL;
    add_device(ctx.h, ctx.device);
    ctx.device = NULL;
    ret = 0;
end:
    if( ret < 0 ) report_error("probe failed", ret);
    pthread_cleanup_pop(1);
    return NULL;
}

static void *start_input( void *opaque )
{
    obe_input_params_t *params = opaque;
    url_input_t ctx = { .h = params->h, .params = params, .origin = AV_NOPTS_VALUE,
                       .last = { AV_NOPTS_VALUE, AV_NOPTS_VALUE } };
    int ret;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_push(close_input, &ctx);
    ctx.deadline = av_gettime_relative() + 5000000;
    if( (ret = open_input(&ctx, params->device->user_opts.location)) < 0 ) goto end;
    /* Track IDs were remapped in open_input; adding/removing audio still
     * requires probing and rebuilding the configured pipeline. */
    if( (params->device->num_input_streams == 2) != (ctx.index[AUDIO] >= 0) )
    { ret = AVERROR_INPUT_CHANGED; goto end; }
    fprintf(stderr, "URL input: receiving video%s\n", ctx.decoder[AUDIO] ? " and stereo audio" : "");
    while( 1 )
    {
        ctx.deadline = av_gettime_relative() + 2000000;
        ret = av_read_frame(ctx.format, ctx.packet);
        cancellation_point();
        if( ret == AVERROR_EOF )
        {
            for( int i = 0; i < STREAMS; i++ )
                if( ctx.decoder[i] && (ret = decode_packet(&ctx, i, NULL, 0)) < 0 ) goto end;
            ret = AVERROR_EOF; break;
        }
        if( ret < 0 ) break;
        for( int i = 0; i < STREAMS; i++ )
            if( ctx.packet->stream_index == ctx.index[i] && (ret = decode_packet(&ctx, i, ctx.packet, 0)) < 0 ) goto end;
        av_packet_unref(ctx.packet);
    }
end:
    report_error("stopped (restart input after fixing the source)", ret);
    pthread_cleanup_pop(1);
    return NULL;
}

const obe_input_func_t lavf_input = { probe_input, start_input };
