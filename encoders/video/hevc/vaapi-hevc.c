/*****************************************************************************
 * vaapi.c : AVC encoding functions
 *****************************************************************************
 * Copyright (C) 2018 LTN
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
#include <assert.h>
#include <sys/mman.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include "va_display.h"
#include <libyuv.h>

#define LOCAL_DEBUG 0

#define MESSAGE_PREFIX "[avc-vaapi]:"

#include "hevcencode.c"

struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	uint64_t      raw_frame_count;

	/* VAAPI */
	struct SwsContext *encoderSwsContext;
	FILE *srcyuv_fp;
	FILE *recyuv_fp;
	FILE *coded_fp;
	int frame_width;
	int frame_height;
	double frame_size;

	pthread_mutex_t encode_mutex;
	pthread_cond_t encode_cond;

	int frame_coded;
	int srcyuv_fourcc;

	/* tmp */
	time_t tmp_last;
	int tmp_count;
	int tmp_bytes;

};

struct userdata_s
{
	struct avfm_s avfm;
};

struct userdata_s *userdata_calloc()
{
	struct userdata_s *ud = calloc(1, sizeof(*ud));
	return ud;
}

int userdata_set(struct userdata_s *ud, struct avfm_s *s)
{
	memcpy(&ud->avfm, s, sizeof(struct avfm_s));
	return 0;
}

void userdata_free(struct userdata_s *ud)
{
	memset(ud, 0, sizeof(*ud));
	free(ud);
}


static size_t hevc_vaapi_deliver_nals(struct context_s *ctx, uint8_t *buf, int lengthBytes, obe_raw_frame_t *rf, int frame_type)
{
	size_t len = lengthBytes;

	ctx->tmp_count++;
	ctx->tmp_bytes += lengthBytes;
	time_t now;
	time(&now);
	if (ctx->tmp_last != now) {
		printf("%s() nal buffers per sec = %d, bps = %d\n", __func__, ctx->tmp_count, ctx->tmp_bytes * 8);
		ctx->tmp_count = 0;
		ctx->tmp_last = now;
		ctx->tmp_bytes = 0;
	}
#if 0
	int hexlen = lengthBytes;
	if (hexlen > 16)
		hexlen = 16;

	printf("%s -- ",
		frame_type == FRAME_P ? "  P" :
		frame_type == FRAME_B ? "  B" :
		frame_type == FRAME_I ? "  I" :
		frame_type == FRAME_IDR ? "IDR" : "?");

	for (int i = 0; i < hexlen; i++)
		printf("%02x ", buf[i]);
	printf("\n");

//	printf("Write %d bytes\n", lengthBytes);
	if (ctx->coded_fp) {
		fwrite(buf, 1, lengthBytes, ctx->coded_fp);
		fflush(ctx->coded_fp);
	}
#endif

	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, lengthBytes);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
		return 0;
	}

	struct avfm_s *avfm = &rf->avfm;
	memcpy(&cf->avfm, &rf->avfm, sizeof(struct avfm_s));
	memcpy(cf->data, buf, lengthBytes);
	cf->len                      = lengthBytes;
	cf->type                     = CF_VIDEO;
	cf->arrival_time             = rf->arrival_time;

	int64_t offset = 24299700;
	offset = 450000 * 0;

	cf->real_pts                 = avfm->audio_pts + offset;
	cf->real_dts                 = avfm->audio_pts + offset;
	cf->pts                      = cf->real_pts;
	cf->cpb_initial_arrival_time = cf->real_dts - offset;

	double estimated_final = ((double)cf->len / 0.0810186) + (double)cf->cpb_initial_arrival_time;
	cf->cpb_final_arrival_time   = estimated_final;

	cf->priority = (frame_type == FRAME_IDR);
	cf->random_access = (frame_type == FRAME_IDR);

	//coded_frame_print(cf);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		//cf->arrival_time = arrival_time;
		add_to_queue(&ctx->h->mux_queue, cf);
	} else {
		add_to_queue(&ctx->h->enc_smoothing_queue, cf);
	}

	return len;
}

static int _save_codeddata(struct context_s *ctx, uint64_t display_order, uint64_t encode_order, obe_raw_frame_t *rf, int frame_type)
{
    VACodedBufferSegment *buf_list = NULL;
    VAStatus va_status;
    unsigned int coded_size = 0;

    va_status = vaMapBuffer(va_dpy, coded_buf[display_order % SURFACE_NUM],(void **)(&buf_list));
    CHECK_VASTATUS(va_status,"vaMapBuffer");
    while (buf_list != NULL) {
        coded_size = hevc_vaapi_deliver_nals(ctx, buf_list->buf, buf_list->size, rf, frame_type);
        buf_list = (VACodedBufferSegment *) buf_list->next;

        ctx->frame_size += coded_size;
    }
    vaUnmapBuffer(va_dpy, coded_buf[display_order % SURFACE_NUM]);

    return 0;
}

static void _storage_task(struct context_s *ctx, unsigned long long display_order, unsigned long long encode_order, obe_raw_frame_t *rf, int frame_type)
{
    VAStatus va_status = vaSyncSurface(va_dpy, src_surface[display_order % SURFACE_NUM]);
    CHECK_VASTATUS(va_status,"vaSyncSurface");
    _save_codeddata(ctx, display_order, encode_order, rf, frame_type);

    pthread_mutex_lock(&ctx->encode_mutex);
    srcsurface_status[display_order % SURFACE_NUM] = SRC_SURFACE_IN_ENCODING;
    pthread_mutex_unlock(&ctx->encode_mutex);
}

static int vaapi_encode_frame(struct context_s *ctx, obe_raw_frame_t *rf, const uint8_t *image_y, const uint8_t *image_u, const uint8_t *image_v)
{
	encoding2display_order(current_frame_encoding, intra_period, intra_idr_period, ip_period,
                               &current_frame_display, &current_frame_type);

	if (current_frame_type == FRAME_IDR) {
		numShortTerm = 0;
		current_frame_num = 0;
		current_IDR_display = current_frame_display;
	}

//printf("%s : %lld %s : %lld type : %d\n", "encoding order", current_frame_encoding, "Display order", current_frame_display, current_frame_type);

	/* check if the source frame is ready */
	while (srcsurface_status[current_slot] != SRC_SURFACE_IN_ENCODING) {
		usleep(1);
	}

	/* Upload YUV into ctx->src_surface[current_slot] */

	upload_surface_yuv(va_dpy, src_surface[current_slot],
		srcyuv_fourcc, frame_width, frame_height,
		(unsigned char *)image_y, /* Plane Y */
		(unsigned char *)image_u, /* Plane U */
		(unsigned char *)image_v);/* Plane V */

	VAStatus va_status = vaBeginPicture(va_dpy, context_id, src_surface[current_slot]);
	CHECK_VASTATUS(va_status,"vaBeginPicture");
	fill_vps_header(&vps);
	fill_sps_header(&sps, 0);
	fill_pps_header(&pps, 0, 0);

        if (current_frame_type == FRAME_IDR) {
            render_sequence(&sps);
            render_packedvideo();
            render_packedsequence();
        }
        render_packedpicture();
        render_picture(&pps);
        fill_slice_header(0, &pps, &ssh);
	render_slice();
        
	va_status = vaEndPicture(va_dpy, context_id);
	CHECK_VASTATUS(va_status,"vaEndPicture");;

	_storage_task(ctx, current_frame_display, current_frame_encoding, rf, current_frame_type);
        
	update_ReferenceFrames();

	current_frame_encoding++;
    
	return 0;
}

/* OBE will pass us a AVC struct initially. Pull out any important pieces
 * and pass those to x265.
 */
static void *hevc_vaapi_start_encoder( void *ptr )
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));
	int ret = 0;

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;

	/* Fix this? its AVC specific. */
	ctx->encoder->encoder_params = malloc(sizeof(ctx->enc_params->avc_param) );
	if (!ctx->encoder->encoder_params) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate encoder params\n");
		goto out1;
	}

	memcpy(ctx->encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param));
	init_va();
	//ret = vaapi_init_va(ctx);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to initialize VA-API, ret = %d\n", ret);
		goto out2;
	}

	//ret = vaapi_setup_encode(ctx);
	ret = setup_encode();
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to configure VA-API, ret = %d\n", ret);
		goto out3;
	}

	/* upload RAW YUV data into all surfaces */
	if (ctx->srcyuv_fp != NULL) {
		for (int i = 0; i < SURFACE_NUM; i++) {
			//load_surface(ctx, ctx->src_surface[i], i);
			load_surface(src_surface[i], i);
		}
	} else
		upload_source_YUV_once_for_all(ctx);

	/* Lock the mutex until we verify and fetch new parameters */
	pthread_mutex_lock(&ctx->encoder->queue.mutex);

	ctx->encoder->is_ready = 1;

	int64_t frame_duration = av_rescale_q(1, (AVRational){ ctx->enc_params->avc_param.i_fps_den, ctx->enc_params->avc_param.i_fps_num},
		(AVRational){ 1, OBE_CLOCK } );

	printf("frame_duration = %" PRIi64 "\n", frame_duration);
	//buffer_duration = frame_duration * ctx->enc_params->avc_param.sc.i_buffer_size;

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

		/* Input colorspace from decklink (through the upstream dither filter), is always 8bit YUV420P. */
		obe_raw_frame_t *rf = ctx->encoder->queue.queue[0];
		ctx->raw_frame_count++;
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);

#if LOCAL_DEBUG
		printf(MESSAGE_PREFIX " popped raw_frame[%" PRIu64 "] -- pts %" PRIi64 "\n", ctx->raw_frame_count, rf->avfm.audio_pts);
#endif

		struct userdata_s *ud = userdata_calloc();

		/* Cache the upstream timing information in userdata. */
		userdata_set(ud, &rf->avfm);

		/* If the AFD has changed, then change the SAR. x264 will write the SAR at the next keyframe
		 * TODO: allow user to force keyframes in order to be frame accurate.
		 */
		if (rf->sar_width != ctx->enc_params->avc_param.vui.i_sar_width ||
			rf->sar_height != ctx->enc_params->avc_param.vui.i_sar_height) {

			ctx->enc_params->avc_param.vui.i_sar_width  = rf->sar_width;
			ctx->enc_params->avc_param.vui.i_sar_height = rf->sar_height;
//			pic.param = &enc_params->avc_param;

		}

		ctx->frame_width = frame_width;
		ctx->frame_height = frame_height;

		int leave = 0;
		while (!leave) { 

			/* Compress raw_frame to NALS. */
			/* Once the pipeline is completely full, x265_encoder_encode() will block until the next output picture is complete. */

			//obe_raw_frame_printf(rf);

			/* Technically it should be:
			 *  Y size = (ctx->frame_width * ctx->frame_height)
			 *  U size += ((ctx->frame_width * ctx->frame_height) / 4)
			 *  V size += ((ctx->frame_width * ctx->frame_height) / 4)
			 */
			uint8_t *f = (uint8_t *)malloc(ctx->frame_width * 2 * ctx->frame_height);

			uint8_t *dst_y = f;
			uint8_t *dst_uv = f + (ctx->frame_width * ctx->frame_height);

			/* VAAPI wants the frame in NV12 natively, so perform a csc */
			/* This costs a few percent of a cpu */
			I420ToNV12(
				rf->img.plane[0], ctx->frame_width,
				rf->img.plane[1], ctx->frame_width / 4,
				rf->img.plane[2], ctx->frame_width / 4,
				dst_y, ctx->frame_width,
				dst_uv, ctx->frame_width / 2,
				ctx->frame_width, ctx->frame_height);

			vaapi_encode_frame(ctx, rf, f, dst_uv, NULL);

			free(f);

			rf->release_data(rf);
			rf->release_frame(rf);
			remove_from_queue(&ctx->encoder->queue);

			if (ret == 0) {
				//fprintf(stderr, MESSAGE_PREFIX " ret = %d\n", ret);
				leave = 1;
				continue;
			}

			leave = 1;
		} /* While ! leave */

	} /* While (1) */

#if 0
	if (ctx->encode_syncmode == 0) {
		int ret;
		pthread_join(ctx->encode_thread, (void **)&ret);
	}
#endif

	//vaapi_release_encode(ctx);
	release_encode();
out3:
	//vaapi_deinit_va(ctx);
	deinit_va();
out2:
	free(ctx->enc_params);
out1:
	return NULL;
}

const obe_vid_enc_func_t hevc_vaapi_obe_encoder = { hevc_vaapi_start_encoder };
