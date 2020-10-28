/*****************************************************************************
 * audio.c: basic audio filtering system
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 */

#include "common/common.h"
#include "audio.h"

#define LOCAL_DEBUG 0

/* Bitmask:
 * 0 = mute right
 * 1 = mute left
 * 2 = static right
 * 3 = static left
 * 4 = buzz right
 * 5 = buzz left
 * 6 = attenuate right
 * 7 = attenuate left
 * 8 = clip right
 * 9 = clip left
 */
int g_filter_audio_effect_pcm = 0;

static void applyEffects(obe_raw_frame_t *rf)
{
    if (g_filter_audio_effect_pcm & 0x03) {
        /* Mute audio right (or left or both) - assumption 32bit samples S32P from decklink */
        uint32_t *l = (uint32_t *)rf->audio_frame.audio_data[0];
        uint32_t *r = (uint32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 0)) {
                *(r++) = 0; /* Mute Right */
            }
            if (g_filter_audio_effect_pcm & (1 << 1)) {
                *(l++) = 0; /* Mute Left */
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x0c) {
        /* Static audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 2)) {
                *(r++) = rand(); /* Right */
            }
            if (g_filter_audio_effect_pcm & (1 << 3)) {
                *(l++) = rand(); /* Left */
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x30) {
        /* Buzz audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples / 16; i++) {
            if (g_filter_audio_effect_pcm & (1 << 4)) {
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                r += 12;
            }
            if (g_filter_audio_effect_pcm & (1 << 5)) {
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                l += 12;
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0xf0) {
        /* attenuate audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 6)) {
                *r /= 4; /* Right */
                 r++;
            }
            if (g_filter_audio_effect_pcm & (1 << 7)) {
                *l /= 4; /* Right */
                 l++;
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x300) {
        /* amplify and clip audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 8)) {
                *r *= 8; /* Right */
                 r++;
            }
            if (g_filter_audio_effect_pcm & (1 << 9)) {
                *l *= 8; /* left */
                 l++;
            }
        }
    }
}

static void *start_filter_audio( void *ptr )
{
    obe_raw_frame_t *raw_frame, *split_raw_frame;
    obe_aud_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_output_stream_t *output_stream;
    int num_channels;

    while( 1 )
    {
        pthread_mutex_lock( &filter->queue.mutex );

        while( !filter->queue.size && !filter->cancel_thread )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            break;
        }

        raw_frame = filter->queue.queue[0];
        pthread_mutex_unlock( &filter->queue.mutex );

#if LOCAL_DEBUG
        printf("%s() raw_frame->input_stream_id = %d, num_encoders = %d\n", __func__,
            raw_frame->input_stream_id, h->num_encoders);
        printf("%s() linesize = %d, num_samples = %d, num_channels = %d, sample_fmt = %d\n",
            __func__,
            raw_frame->audio_frame.linesize,
            raw_frame->audio_frame.num_samples, raw_frame->audio_frame.num_channels,
            raw_frame->audio_frame.sample_fmt);
#endif

        /* ignore the video track, process all PCM encoders first */
        for (int i = 1; i < h->num_encoders; i++)
        {
            output_stream = get_output_stream_by_id(h, h->encoders[i]->output_stream_id);
            if (output_stream->stream_format == AUDIO_AC_3_BITSTREAM)
                continue; /* Ignore downstream AC3 bitstream encoders */

            if (raw_frame->audio_frame.sample_fmt == AV_SAMPLE_FMT_NONE)
                continue; /* Ignore non-pcm frames */

//printf("output_stream->stream_format = %d other\n", output_stream->stream_format);
            num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

#if LOCAL_DEBUG
            printf("%s() encoder#%d: output_stream->sdi_audio_pair %d, num_channels %d\n", __func__, i,
                output_stream->sdi_audio_pair, num_channels);
#endif
            split_raw_frame = new_raw_frame();
            if (!split_raw_frame)
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }
            memcpy(split_raw_frame, raw_frame, sizeof(*split_raw_frame));
            memset(split_raw_frame->audio_frame.audio_data, 0, sizeof(split_raw_frame->audio_frame.audio_data));
            split_raw_frame->audio_frame.linesize = split_raw_frame->audio_frame.num_channels = 0;
            split_raw_frame->audio_frame.channel_layout = output_stream->channel_layout;
            split_raw_frame->audio_frame.num_channels = num_channels;

            if (av_samples_alloc(split_raw_frame->audio_frame.audio_data, &split_raw_frame->audio_frame.linesize, num_channels,
                              split_raw_frame->audio_frame.num_samples, split_raw_frame->audio_frame.sample_fmt, 0) < 0)
            {
                syslog(LOG_ERR, "Malloc failed\n");
                return NULL;
            }

            /* Copy samples for each channel into a new buffer, so each downstream encoder can
             * compress the channels the user has selected via sdi_audio_pair.
             */
            av_samples_copy(split_raw_frame->audio_frame.audio_data, /* dst */
                            &raw_frame->audio_frame.audio_data[((output_stream->sdi_audio_pair - 1) << 1) + output_stream->mono_channel], /* src */
                            0, /* dst offset */
                            0, /* src offset */
                            split_raw_frame->audio_frame.num_samples,
                            num_channels,
                            split_raw_frame->audio_frame.sample_fmt);

            applyEffects(split_raw_frame);

#if LOCAL_DEBUG
            obe_raw_frame_printf(split_raw_frame);
#endif
            add_to_encode_queue(h, split_raw_frame, h->encoders[i]->output_stream_id);
        } /* For all PCM encoders */

        /* ignore the video track, process all AC3 bitstream encoders.... */
	/* TODO: Only one buffer can be passed to one encoder, as the input SDI
	 * group defines a single stream of data, so this buffer can only end up at one
	 * ac3bitstream encoder.
	 * That being said, the decklink input creates one bitstream buffer per detected pair.
	 */
        int didForward = 0;
        for (int i = 1; i < h->num_encoders; i++)
        {
            output_stream = get_output_stream_by_id(h, h->encoders[i]->output_stream_id);
            if (output_stream->stream_format != AUDIO_AC_3_BITSTREAM)
                continue; /* Ignore downstream AC3 bitstream encoders */

            if (raw_frame->audio_frame.sample_fmt != AV_SAMPLE_FMT_NONE)
                continue; /* Ignore pcm frames */

            /* TODO: Match the input raw frame to the output encoder, else we could send
             * frames for ac3 encoder #2 to ac3 encoder #1.
             */
            if (raw_frame->input_stream_id != h->encoders[i]->output_stream_id)
                continue;

#if LOCAL_DEBUG
            printf("%s() adding A52 frame for input %d to encoder output %d sdi_audio_pair %d\n", __func__,
                raw_frame->input_stream_id, h->encoders[i]->output_stream_id, output_stream->sdi_audio_pair);
#endif

            remove_from_queue(&filter->queue);
            add_to_encode_queue(h, raw_frame, h->encoders[i]->output_stream_id);
            didForward = 1;
            break;

        } /* For each AC3 bitstream encoder */

        if (!didForward) {
            remove_from_queue(&filter->queue);
            raw_frame->release_data(raw_frame);
            raw_frame->release_frame(raw_frame);
            raw_frame = NULL;
        }
    }

    free( filter_params );

    return NULL;
}

const obe_aud_filter_func_t audio_filter = { start_filter_audio };
