/*****************************************************************************
 * vega.c : vega encoding passthru functions
 *****************************************************************************
 * Copyright (C) 2022 LiveTimeNet Inc. All Rights Reserved.
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
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
 *
 ******************************************************************************/

#include "common/common.h"
#include "encoders/video/video.h"
#include <libavutil/mathematics.h>

#define LOCAL_DEBUG 0

#define MESSAGE_PREFIX "[vega-encoder-passthru]:"

#define SERIALIZE_CODED_FRAMES 0

int g_vega_nal_debug = 0;

#if SERIALIZE_CODED_FRAMES
static FILE *sfh = NULL;
static void serialize_coded_frame(obe_coded_frame_t *cf)
{
    if (!sfh)
        sfh = fopen("/storage/dev/x265-out.cf", "wb");
    if (sfh)
        coded_frame_serializer_write(sfh, cf);
}
#endif

/* end - 64 doubles */
struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	uint64_t      raw_frame_count;
};

/* Take a raw buffer of nals, and some timing information, construct a fully formed coded_frame obe
 * object and pass thus downstream.
 */
static int dispatch_payload(struct context_s *ctx, const unsigned char *buf, int lengthBytes, obe_raw_frame_t *rf)
{
	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, lengthBytes);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
		return -1;
	}

	memcpy(cf->data, buf, lengthBytes);
	cf->len                      = lengthBytes;
	cf->type                     = CF_VIDEO;

	/* Recalculate a new real pts/dts based on the hardware time, not the codec time. */
	cf->pts                      = rf->avfm.video_pts + (1 * 450450);
	cf->real_pts                 = rf->avfm.video_pts + (1 * 450450);
	cf->real_dts                 = rf->avfm.video_dts + (1 * 450450);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
        	cf->cpb_initial_arrival_time = cf->real_dts - 1351350; /* 3x frame interval */
	} else {
        	cf->cpb_initial_arrival_time = cf->real_pts - 1351350; /* 3x frame interval */
	}

	double bit_rate = ctx->enc_params->avc_param.rc.i_vbv_max_bitrate;
	double fraction = bit_rate / 216000.0;
	double estimated = ((double)cf->len / fraction);
	double estimated_final = estimated + (double)cf->cpb_initial_arrival_time;
	cf->cpb_final_arrival_time  = estimated_final;

	cf->priority = 0;
	cf->random_access = 0;

	/* If interlaced is active, and the pts time has repeated, increment clocks
	 * by the field rate (frame_duration / 2). Why do we increment clocks? We can either
	 * take the PTS from the codec (which repeats) or derive the PTS from the actual
	 * capture hardware. The codec PTS is guaranteed sequence, a problem during signal loss.
	 * The hardware clock is non-sequential - a better clock to handle signal loss.
	 */
#if 0
	static int64_t last_dispatch_pts = 0;
	if (ctx->enc_params->avc_param.b_interlaced) {
		if (cf->pts == last_dispatch_pts) {
			cf->pts += (avfm_get_video_interval_clk(&rf->avfm) / 2);
			/* Recalculate a new real pts/dts based on the hardware time, not the codec time. */
			cf->real_pts = cf->pts;
			cf->real_dts = cf->pts;
		}

	}
	last_dispatch_pts = cf->pts;
#endif

	if (g_vega_nal_debug & 0x02) {
		printf(MESSAGE_PREFIX " --    output                                                                            pts %13" PRIi64 " dts %13" PRIi64 "\n",
			cf->pts,
			cf->real_dts);
	}

	if (g_vega_nal_debug & 0x04)
		coded_frame_print(cf);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->arrival_time = rf->arrival_time;
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->mux_queue, cf);
		//printf(MESSAGE_PREFIX " Encode Latency %"PRIi64" \n", obe_mdate() - cf->arrival_time);
	} else {
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->enc_smoothing_queue, cf);
	}
	
	return 0;
}

/* OBE will pass us a AVC struct initially. Pull out any important pieces
 * and pass those to x265.
 */
static void *vega_start_encoder( void *ptr )
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;
	obe_encoder_t *encoder = ctx->encoder;

	printf(MESSAGE_PREFIX "Starting encoder: %s\n",
		stream_format_name(obe_core_encoder_get_stream_format(ctx->encoder)));

	/* Lock the mutex until we verify and fetch new parameters */
	pthread_mutex_lock(&ctx->encoder->queue.mutex);

	encoder->encoder_params = malloc( sizeof(ctx->enc_params->avc_param) );
	if(!encoder->encoder_params)
	{
		pthread_mutex_unlock( &ctx->encoder->queue.mutex );
		syslog(LOG_ERR, "Malloc failed\n" );
		goto end;
	}
	memcpy( encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param) );

	ctx->encoder->is_ready = 1;

	/* Wake up the muxer */
	pthread_cond_broadcast(&ctx->encoder->queue.in_cv);
	pthread_mutex_unlock(&ctx->encoder->queue.mutex);

	while (1) {
		pthread_mutex_lock(&ctx->encoder->queue.mutex);

		while (!ctx->encoder->queue.size && !ctx->encoder->cancel_thread) {
			pthread_cond_wait(&ctx->encoder->queue.in_cv, &ctx->encoder->queue.mutex);
		}

		if (ctx->encoder->cancel_thread) {
			pthread_mutex_unlock(&ctx->encoder->queue.mutex);
			break;
		}

		/* Reset the speedcontrol buffer if the source has dropped frames. Otherwise speedcontrol
		 * stays in an underflow state and is locked to the fastest preset.
		 */
		pthread_mutex_lock(&ctx->h->drop_mutex);
		if (ctx->h->video_encoder_drop) {
			pthread_mutex_lock(&ctx->h->enc_smoothing_queue.mutex);
			ctx->h->enc_smoothing_buffer_complete = 0;
			pthread_mutex_unlock(&ctx->h->enc_smoothing_queue.mutex);
#if 0
			fprintf(stderr, MESSAGE_PREFIX " Speedcontrol reset\n");
			x264_speedcontrol_sync( s, enc_params->avc_param.sc.i_buffer_size, enc_params->avc_param.sc.f_buffer_init, 0 );
#endif
			ctx->h->video_encoder_drop = 0;
		}
		pthread_mutex_unlock(&ctx->h->drop_mutex);

		obe_raw_frame_t *rf = ctx->encoder->queue.queue[0];
		ctx->raw_frame_count++;
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);

		if (rf->alloc_img.csp != AV_PIX_FMT_QSV) {
			fprintf(stderr, MESSAGE_PREFIX "popped a non VEGA incoming frame, %p, really bad.\n", rf);
		} else {
			//printf(MESSAGE_PREFIX "popped an incoming frame, %p\n", rf);
		}

		/* Convert the nals into a fully formed coded_frame and destroy the original raw_frame */
		dispatch_payload(ctx, rf->alloc_img.plane[0], rf->alloc_img.width, rf);

		remove_from_queue(&ctx->encoder->queue);

		free(rf->alloc_img.plane[0]);
		free(rf);
		//rf->release_data(rf);
		//rf->release_frame(rf);

	} /* While (1) */

end:
	free(ctx->enc_params);

	return NULL;
}

const obe_vid_enc_func_t vega_obe_encoder = { vega_start_encoder };
