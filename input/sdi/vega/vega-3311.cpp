#if HAVE_VEGA3311_CAP_TYPES_H

/* Comments / Questions:
 *
 *                      3301  3311 
 *     Audio channels      8    16
 *   608/708 Captions      ?     Y
 *                AFD      ?     Y
 *             Decode      N     Y
 *            Latency    2-5   2-5  frames.
 *          gstreamer      N     N
 * 
 * Move this entire file into a vega3311 specific section and restore the vega 3301 implementation.
 * 
 * Duration / runtime issue with clocks going negative when the PCR
 * wraps. Fix this.
 * 
 * 2022-06-10 Testign3311 with:
 * vega3311_0_89_20210910_03_0000_3GSD_wt.bin
 * 4x 1080p60 HEVC encodes, 4xmp2, latency is 110ms, cpuload 20% per encoder instance.
 * 
 * What does LOS look like? (especially, blue video but no audio?).
 * What does bitstream audio look like?
 * 
 * Does the PCR keep rolling when the signal is disconnected?
 * 
 * Is the audio from SDI bitstream capable?
 * - They think they're just collecting and handing us HANC.
 * - so in principle yes.
 * 
 * Encode some actual 10bit video, check we're not losing 10-to-8 in the encode because
 * of the defined colorspace.
 * 
 * Test interlaced formats.
 *
 * How do I test HDR encoding? What does the workflow look like?
 * 
 * Can I write into the NV12 pixels pre-encode? (video injection).
 * Enable V210 burnwriter codes pre-encode.
 * 
 * Increase/test 8 additional audio channels on the 3311.
 * 
 * Mediainfo not working on ts files
 */

/*****************************************************************************
 * vega.cpp: Audio/Video from the VEGA encoder cards
 *****************************************************************************
 * Copyright (C) 2021-2022 LTN
 *
 * Authors: Steven Toth <steven.toth@ltnglobal.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <iostream>
#include <limits.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>
#include <encoders/video/sei-timestamp.h>
#include <input/sdi/yuv422p10le.h>
#include "histogram.h"
#include "vega-3311.h"
#include <sys/stat.h>

#define MODULE_PREFIX "[vega]: "

#define LOCAL_DEBUG 0

#define FORCE_10BIT 1

#define CAP_DBG_LEVEL API_VEGA3311_CAP_DBG_LEVEL_3

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VEGA_VERSION STR(VEGA331X_KERNEL_VERSION) "." STR(VEGA331X_MAJOR_VERSION) "." STR(VEGA331X_MINOR_VERSION)

#define MAX_AUDIO_CHANNELS 16
#define MAX_AUDIO_PAIRS (MAX_AUDIO_CHANNELS / 2)

using namespace std;

const char *vega3311_sdk_version = VEGA_VERSION;

extern int g_decklink_monitor_hw_clocks;
extern int g_decklink_render_walltime;
extern int g_decklink_histogram_print_secs;
extern int g_decklink_histogram_reset;

typedef struct
{
	/* Device input related */
	obe_device_t *device;
	obe_t *h;

        /* Capture layer and encoder layer configuration management */
        API_VEGA_BQB_INIT_PARAM_T init_params;
        API_VEGA3311_CAP_INIT_PARAM_T ch_init_param;

        uint32_t framecount;
        int bLastFrame;

        /* Audio - Sample Rate Conversion. We convert S32 interleaved into S32P planer. */
        struct SwrContext *avr;

        /* Video timing tracking */
        int64_t videoLastPCR;

        /* Audio timing tracking */
        int64_t audioLastPCR;
        struct ltn_histogram_s *hg_callback_audio;
        struct ltn_histogram_s *hg_callback_video;

} vega_ctx_t;

typedef struct
{
    vega_ctx_t ctx;

    /* Input */
    int  brd_idx;       /* Board instance # */
    int card_idx;                       /* Port index # */

    int video_format;                   /* Eg. INPUT_VIDEO_FORMAT_720P_5994 */
    int num_audio_channels;             /* MAX_AUDIO_CHANNELS */

    int probe;                          /* Boolean. True if the hardware is currently in probe mode. */

    int probe_success;                  /* Boolean. Signal to the outer OBE core that probing is done. */

    int width;                          /* Eg. 1280 */
    int height;                         /* Eg. 720 */
    int timebase_num;                   /* Eg.  1001 */
    int timebase_den;                   /* Eg. 60000 */

    int interlaced;                     /* Boolean */
    //int tff;                            /* Boolean */

    /* configuration for the codec. */
    struct {
            int                             interlaced;
            int bitrate_kbps;
            API_VEGA_BQB_GOP_SIZE_E         gop_size;
            API_VEGA_BQB_B_FRAME_NUM_E      bframes;
            int width;
            int height;
            API_VEGA_BQB_RESOLUTION_E       encodingResolution;
            API_VEGA_BQB_IMAGE_FORMAT_E     eFormat;
            API_VEGA_BQB_FPS_E              fps;
            API_VEGA_BQB_CHROMA_FORMAT_E    chromaFormat;
            API_VEGA_BQB_BIT_DEPTH_E        bitDepth;
            API_VEGA3311_CAP_INPUT_MODE_E   inputMode;
            API_VEGA3311_CAP_INPUT_SOURCE_E inputSource;
            API_VEGA3311_CAP_SDI_LEVEL_E    sdiLevel;
            API_VEGA3311_CAP_AUDIO_LAYOUT_E audioLayout;
            API_VEGA3311_CAP_IMAGE_FORMAT_E pixelFormat;
    } codec;

} vega_opts_t;

static int configureCodec(vega_opts_t *opts)
{
        vega_ctx_t *ctx = &opts->ctx;

        obe_output_stream_t *os = obe_core_get_output_stream_by_index(ctx->h, 0);
        if (os->stream_format != VIDEO_HEVC_VEGA3311) {
		fprintf(stderr, MODULE_PREFIX "unable to query encoder parameters\n");
		return -1;
        }

        x264_param_t *p = &os->avc_param;

        opts->codec.inputMode    = API_VEGA3311_CAP_INPUT_MODE_4CHN;
        opts->codec.inputSource  = API_VEGA3311_CAP_INPUT_SOURCE_SDI;
        opts->codec.sdiLevel     = API_VEGA3311_CAP_SDI_LEVEL_A;
        opts->codec.audioLayout  = API_VEGA3311_CAP_AUDIO_LAYOUT_16P0;
        opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV16;
        opts->codec.bitrate_kbps = p->rc.i_bitrate;
        opts->codec.gop_size     = (API_VEGA_BQB_GOP_SIZE_E)p->i_keyint_max;
        opts->codec.bframes      = (API_VEGA_BQB_B_FRAME_NUM_E)p->i_bframe;
        opts->codec.width        = p->i_width;
        opts->codec.height       = p->i_height;

        const struct obe_to_vega_video *fmt = lookupVegaStandardByResolution(opts->codec.width, opts->codec.height, API_VEGA3311_CAP_FPS_60);
        if (!fmt) {
		fprintf(stderr, MODULE_PREFIX "unable to query encoder parameters for specific width, height and framerate\n");
		return -1;
        }
        opts->codec.encodingResolution = (API_VEGA_BQB_RESOLUTION_E)fmt->vegaEncodingResolution;
        opts->codec.interlaced = p->b_interlaced;

        if (lookupVegaFramerate(p->i_fps_den, p->i_fps_num, &opts->codec.fps) < 0) {
		fprintf(stderr, MODULE_PREFIX "unable to query encoder framerate %d, %d\n", p->i_fps_num, p->i_fps_den);
		return -1;
        }

#if FORCE_10BIT
#pragma message "Force compile into 10bit only mode"
        // Manually enable 10bit.
        p->i_csp |= X264_CSP_HIGH_DEPTH;
#endif

        if ((p->i_csp & X264_CSP_I420) && ((p->i_csp & X264_CSP_HIGH_DEPTH) == 0)) {
                /* 4:2:0 8bit via NV12 */
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_420;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_8;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV12;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_NV12;
                // OK
        } else
        if ((p->i_csp & X264_CSP_I420) && (p->i_csp & X264_CSP_HIGH_DEPTH)) {
                /* 4:2:0 10bit via PP01 */
		fprintf(stderr, MODULE_PREFIX "using colorspace 4:2:0 10bit (not supported)\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_420;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_10;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_P010;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_PP01;
        } else
        if ((p->i_csp & X264_CSP_I422) && ((p->i_csp & X264_CSP_HIGH_DEPTH) == 0)) {
                /* 4:2:2 8bit via NV16 */
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_8;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV16;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_NV16;
                // NOT ok according to VLC
        } else
        if ((p->i_csp & X264_CSP_I422) && (p->i_csp & X264_CSP_HIGH_DEPTH)) {
                /* 4:2:2 10bit via Y210 and colorspace conversion  */
		fprintf(stderr, MODULE_PREFIX "using colorspace 4:2:2 10bit\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_10;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_Y210;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_YUV422P010;
        } else {
		fprintf(stderr, MODULE_PREFIX "unable to determine colorspace, i_csp = 0x%x\n", p->i_csp);
		return -1;
        }

        printf(MODULE_PREFIX "encoder.device       = %d\n", opts->brd_idx);
        printf(MODULE_PREFIX "encoder.eFormat      = %d\n", opts->codec.eFormat);
        printf(MODULE_PREFIX "encoder.inputsource  = %d '%s'\n", opts->codec.inputSource, lookupVegaInputSourceName(opts->codec.inputSource));
        printf(MODULE_PREFIX "encoder.inputport    = %d\n", opts->card_idx);
        printf(MODULE_PREFIX "encoder.sdilevel     = %d '%s'\n", opts->codec.sdiLevel, lookupVegaSDILevelName(opts->codec.sdiLevel));
        printf(MODULE_PREFIX "encoder.inputmode    = %d '%s'\n", opts->codec.inputMode, lookupVegaInputModeName(opts->codec.inputMode));
        printf(MODULE_PREFIX "encoder.bitrate_kbps = %d\n", opts->codec.bitrate_kbps);
        printf(MODULE_PREFIX "encoder.gop_size     = %d\n", opts->codec.gop_size);
        printf(MODULE_PREFIX "encoder.bframes      = %d\n", opts->codec.bframes);
        printf(MODULE_PREFIX "encoder.width        = %d\n", opts->codec.width);
        printf(MODULE_PREFIX "encoder.height       = %d\n", opts->codec.height);
        printf(MODULE_PREFIX "encoder.resolution   = %d '%s'\n", opts->codec.encodingResolution, lookupVegaEncodingResolutionName(opts->codec.encodingResolution));
        printf(MODULE_PREFIX "encoder.fps          = %d\n", opts->codec.fps);
        printf(MODULE_PREFIX "encoder.chroma       = %d '%s'\n", opts->codec.chromaFormat, lookupVegaEncodingChromaName(opts->codec.chromaFormat));
        printf(MODULE_PREFIX "encoder.bitdepth     = %d '%s'\n", opts->codec.bitDepth, lookupVegaBitDepthName(opts->codec.bitDepth));
        printf(MODULE_PREFIX "encoder.pixelformat  = %d '%s'\n", opts->codec.pixelFormat, lookupVegaPixelFormatName(opts->codec.pixelFormat));
        printf(MODULE_PREFIX "encoder.interlaced   = %d\n", opts->codec.interlaced);

        return 0; /* Success */
}

/* Take PCM, convrt it into frames and push it into the downstream workflow. */
static void deliver_audio_frame(vega_opts_t *opts, unsigned char *plane, int sizeBytes, int sampleCount, int64_t frametime, int channelCount)
{
	vega_ctx_t *ctx = &opts->ctx;

#if 0
	int offset = 0;
	for (int i = offset; i < (offset + 64); i++)
		printf("%02x", plane[i]);
	printf("\n");
#endif

	//printf("%s() %d bytes numsamples %d\n", __func__, sizeBytes, sampleCount);
	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf)
		return;

	rf->audio_frame.num_samples  = sampleCount;
	rf->audio_frame.num_channels = MAX_AUDIO_CHANNELS;
	rf->audio_frame.sample_fmt   = AV_SAMPLE_FMT_S32P; /* Destination format */
	rf->audio_frame.linesize     = rf->audio_frame.num_channels * sizeof(uint16_t);

	if (av_samples_alloc(rf->audio_frame.audio_data, &rf->audio_frame.linesize, rf->audio_frame.num_channels,
                              rf->audio_frame.num_samples, (AVSampleFormat)rf->audio_frame.sample_fmt, 0) < 0)
	{
		fprintf(stderr, MODULE_PREFIX "av_samples_alloc failed\n");
                free(rf);
		return;
	}

        /* Convert input samples from S16 interleaved (native from the h/w) into S32P planer. */
        if (swr_convert(ctx->avr,
                rf->audio_frame.audio_data,
                rf->audio_frame.num_samples,
                (const uint8_t**)&plane,
                rf->audio_frame.num_samples) < 0)
        {
                fprintf(stderr, MODULE_PREFIX "Sample format conversion failed\n");
                /* TODO: Free the audio plane? */
                free(rf);
                return;
        }

	avfm_init(&rf->avfm, AVFM_AUDIO_PCM);

	avfm_set_hw_status_mask(&rf->avfm, AVFM_HW_STATUS__MASK_VEGA); // probably not required.

	/* Hardware clock of the dektec starts up non-zero, increments regardless,
	 * out stack assumes the clock starts at zero. Compensate by subtracting
	 * the base start value.
	 */
	static int64_t frametime_base = 0;
	if (frametime_base == 0)
		frametime_base = frametime;

	avfm_set_pts_audio(&rf->avfm, frametime - frametime_base);
	avfm_set_hw_received_time(&rf->avfm);
	//avfm_set_video_interval_clk(&rf->avfm, decklink_ctx->vframe_duration);

	rf->release_data = obe_release_audio_data;
	rf->release_frame = obe_release_frame;
	rf->input_stream_id = 1; // TODO: Is this correct?

	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
		fprintf(stderr, MODULE_PREFIX "Unable to add audio frame to the filter q.\n");
	}
}

/* Called by the vega sdk when a raw audio frame is ready. */
static void callback__a_capture_cb_func(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void* pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
	vega_ctx_t *ctx = &opts->ctx;

        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to capturing, source signal recovery\n", u32DevId, eCh);
                } else if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_WAITING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to waiting, source signal loss\n", u32DevId, eCh);
                } else if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_STANDBY) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to standby, callback function is terminated\n", u32DevId, eCh);
                }

                return;
        }

	ltn_histogram_interval_update(ctx->hg_callback_audio);

	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, ctx->hg_callback_audio, g_decklink_histogram_print_secs);
        }

	if (g_decklink_histogram_reset) {
		g_decklink_histogram_reset = 0;
		ltn_histogram_reset(ctx->hg_callback_audio);
		ltn_histogram_reset(ctx->hg_callback_video);
	}

        /* Vega deliveres the samples in s16P planar format already. We don't need to
         * convert from INterleaved to planar. Yay.
         */

        //int64_t pcr = convertSCR_to_PCR(st_input_info->tCurrentPCR.u64Dword);
        int64_t pcr = st_input_info->tCurrentPCR.u64Dword;
//        printf("%" PRIi64 "\n", st_input_info->tCurrentPCR.u64Dword);

#if LOCAL_DEBUG
        int64_t pts = pcr / 300;
        printf(MODULE_PREFIX "pushed raw audio frame to encoder, PCR %016" PRIi64 ", PTS %012" PRIi64 " %p length %d delta: %016" PRIi64 "\n",
                pcr, pts,
                st_frame_info->u8pDataBuf, st_frame_info->u32BufSize,
                pcr - ctx->audioLastPCR);
#endif
        int channels = MAX_AUDIO_CHANNELS;
        int samplesPerChannel = st_frame_info->u32BufSize / channels / sizeof(uint16_t);

        deliver_audio_frame(opts, st_frame_info->u8pDataBuf, st_frame_info->u32BufSize, samplesPerChannel, pcr, channels);

        ctx->audioLastPCR = pcr;
}

/* Take a single NAL, convert to a raw frame and push it into the downstream workflow. */
/* TODO: Coalesce these at a higher level and same some memory allocs? */
static void deliver_video_nal(vega_opts_t *opts, unsigned char *buf, int lengthBytes, int64_t pts, int64_t dts)
{
	vega_ctx_t *ctx = &opts->ctx;

        obe_raw_frame_t *rf = new_raw_frame();
        if (!rf) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
                return;
        }

        rf->alloc_img.planes = 1;
        rf->alloc_img.width = lengthBytes;
        rf->alloc_img.height = 1;
        rf->alloc_img.format = opts->video_format;
        rf->alloc_img.csp = AV_PIX_FMT_QSV; /* Magic type to indicate to the downstream filters that this isn't raw video */
        rf->timebase_num = opts->timebase_num;
        rf->timebase_den = opts->timebase_den;
        rf->pts = pts * 300; /* Converts 90KHz to 27MHz */

        //printf("%s() format = %d, timebase_num %d, timebase_den %d\n", __func__, rf->alloc_img.format, rf->timebase_num, rf->timebase_den);  // TODO: FIxme IE INPUT_VIDEO_FORMAT_2160P_25

        rf->alloc_img.stride[0] = lengthBytes;
        rf->alloc_img.plane[0] = (uint8_t *)malloc(((lengthBytes / 4096) + 1) * 4096);

        if (rf->alloc_img.plane[0] == NULL) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate nal video plane\n");
                free(rf);
                return;
        }
        memcpy(rf->alloc_img.plane[0], buf, lengthBytes);

        /* AVFM */
        avfm_init(&rf->avfm, AVFM_VIDEO);
        avfm_set_hw_status_mask(&rf->avfm, AVFM_HW_STATUS__MASK_VEGA); // TODO: We probably don't need this

        /* Remember that we drive everything in the pipeline from the audio clock. */
        avfm_set_pts_video(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */
        avfm_set_dts_video(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */
        avfm_set_pts_audio(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */

        avfm_set_hw_received_time(&rf->avfm);

        /* Safety div by zero */
        double dur = 0;
        if (rf->timebase_num && rf->timebase_den) {
                dur = 27000000.0 / ((double)rf->timebase_den / (double)rf->timebase_num);
        }

        avfm_set_video_interval_clk(&rf->avfm, dur);
        //avfm_dump(&rf->avfm);

        if (add_to_filter_queue(ctx->h, rf) < 0 ) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
                free(rf->alloc_img.plane[0]);
                free(rf);
                return;
        }
}

/* Called by the vega sdk when a newly compressed video frame is ready. */
static void callback__process_video_coded_frame(API_VEGA_BQB_HEVC_CODED_PICT_T *p_pict, void *args)
{
        vega_opts_t *opts = (vega_opts_t *)args;
	//vega_ctx_t *ctx = &opts->ctx;

        for (unsigned int i = 0; i < p_pict->u32NalNum; i++) {

                if (g_decklink_monitor_hw_clocks) {
                        printf(MODULE_PREFIX "pts: %15" PRIi64 "  dts: %15" PRIi64 " base %012" PRIu64 " ext %012d frametype: %s  addr: %p length %7d -- ",
                                p_pict->pts,
                                p_pict->dts,
                                p_pict->u64ItcBase,
                                p_pict->u32ItcExt,
                                vega_lookupFrameType(p_pict->eFrameType),
                                p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                        int dlen = p_pict->tNalInfo[i].u32Length;
                        if (dlen > 32)
                                dlen = 32;

                        for (int j = 0; j < dlen; j++)
                                printf("%02x ", p_pict->tNalInfo[i].pu8Addr[j]);
                        printf("\n");
                }

                if (g_sei_timestamping) {
                        /* Find the latency SEI and update it. */
                        if (p_pict->tNalInfo[i].u32Length == (7 + SEI_TIMESTAMP_PAYLOAD_LENGTH)) {
                                int offset = ltn_uuid_find(p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                                if (offset >= 0) {
                                        struct timeval tv;
                                        gettimeofday(&tv, NULL);

                                        /* Add the time exit from compressor seconds/useconds. */
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 6, tv.tv_sec);
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 7, tv.tv_usec);
                                }
                        }
                }
                /* TODO: Decision here. Coalesce all nals into a single allocation - better performance, one raw frame downstream. */

                deliver_video_nal(opts, p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length, p_pict->pts, p_pict->dts);
        }
}

/* Callback from the video capture device */
static void callback__v_capture_cb_func(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void *pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
	vega_ctx_t *ctx = &opts->ctx;

        API_VEGA_BQB_IMG_T img;

        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to capturing, source signal recovery\n", u32DevId, eCh);
                        return;
                }
                else if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_WAITING) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to waiting, source signal loss\n", u32DevId, eCh);
                        return;
                }
                else if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_STANDBY) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to standby, callback function is terminated\n", u32DevId, eCh);
                }
        }

	ltn_histogram_interval_update(ctx->hg_callback_video);
	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, ctx->hg_callback_video, g_decklink_histogram_print_secs);
        }

        //if (ops->cb_param_p->last_frame == 1)
        {
         //       sem_post(&cb_param_p->capture_sem);
        //        printf("vega processed last frame\n");
        }

        ctx->framecount++;

#if 0
	static time_t last_report = 0;
	time_t now;
	now = time(NULL);
	if (now != last_report) {
		printf(MODULE_PREFIX "[DEV%d:CH%d] Video drops: %" PRIu64 " Audio drops: %" PRIu64 "\n",
		       u32DevId, eCh, st_input_info->u64VideoFrameDropped, st_input_info->u64AudioFrameDropped);
		last_report = now;
	}
#endif

        /* Capture an output frame to disk, for debug */
        {
                struct stat s;
                const char *tf = "/tmp/vega-frame-capture-input.touch";
                int r = stat(tf, &s);
                if (r == 0) {
                        char fn[64];
                        sprintf(fn, "/tmp/%08d-vega-capture-input-port%d.bin", ctx->framecount, opts->card_idx);
                        FILE *fh = fopen(fn, "wb");
                        if (fh) {
                                fwrite(st_frame_info->u8pDataBuf, 1, st_frame_info->u32BufSize, fh);
                                fclose(fh);
                        }
                        remove(tf);
                }
        }

        //int64_t pts = convertSCR_to_PCR(st_input_info->u64CurrentPCR) / 300;
        memset(&img, 0 , sizeof(img));
        uint8_t *dst[3] = { 0, 0, 0 };

	/* Burnreader to validate frames on capture */
	if (ctx->ch_init_param.eFormat == API_VEGA3311_CAP_IMAGE_FORMAT_Y210) {
		/* These are the default constants in the burnwriter */
		int startline = 1;
		int bitwidth = 30;
		int bitheight = 30;

		/* Check the line at the vertical center of the boxes */
		int checkline = startline + (bitheight / 2);
		uint16_t *pic = (uint16_t *)&st_frame_info->u8pDataBuf[0];
		uint16_t *x;
		uint32_t bits = 0;
		int bitcount = 0;
		static int64_t framecnt = 0;

		/* Check to ensure counters are actually present */
		x = pic + (checkline * opts->width) + 1;
		for (int c = 30; c >= 0; c--) {
			x += (bitwidth / 2 * 2);
			if ((*x >> 6) > 0x195 && (*x >> 6) < 0x205)
				bitcount++;
			x += (bitwidth / 2 * 2);
		}

		/* Now extract the count */
		x = pic + (checkline * opts->width);
		for (int c = 31; c >= 0; c--) {
                        x += (bitwidth / 2 * 2);
                        if ((*x >> 6) > 0x200)
				bits |= (1 << c);
                        x += (bitwidth / 2 * 2);
		}

		if ((bitcount == 31) && (framecnt && framecnt + 1 != bits)) {
                        char ts[64];
                        obe_getTimestamp(ts, NULL);
                        printf("%s: OSD counter discontinuity, expected %08" PRIx64 " got %08" PRIx32 "\n",
                               ts, framecnt + 1, bits);
		}
		framecnt = bits;
	}

        if (opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_YUV422P10LE) {
                /* If 10bit 422 .... Colorspace convert Y210 into yuv422p10le */

                /*  len as expressed in bytes. No stride */
                int ylen = (opts->width * opts->height) * 2;
                int ulen = ylen / 2;
                int vlen = ylen / 2;
                int dstlen = ylen + ulen + vlen;

                dst[0] = (uint8_t *)malloc(dstlen); /* Y - Primary plane - we submit this single address the the GPU */
                dst[1] = dst[0] + ylen; /* U plane */
                dst[2] = dst[1] + ulen; /* V plane */

#if 0
                printf("dstlen %d   ylen %d  ulen %d vlen %d\n", dstlen, ylen, ulen, vlen);
                for (int i = 0; i < 3; i++)
                        printf("dst[%d] %p\n", i, dst[i]);
#endif

                /* Unpack all of the 16 bit Y/U/Y/V words, shift to correct for padding and write new planes */
                uint16_t *wy = (uint16_t *)dst[0];
                uint16_t *wu = (uint16_t *)dst[1];
                uint16_t *wv = (uint16_t *)dst[2];
                uint16_t *p = (uint16_t *)&st_frame_info->u8pDataBuf[0];

                /* For all 4xUnsignedShort sized blocks in the source frame....*/
                for (int i = 0 ; i < opts->width * opts->height * 4; i += 8) {
                        *(wy++) = *(p++) >> 6;
                        *(wu++) = *(p++) >> 6;
                        *(wy++) = *(p++) >> 6;
                        *(wv++) = *(p++) >> 6;
                }

                img.pu8Addr     = dst[0];
                img.u32Size     = dstlen;
                img.eFormat     = opts->codec.eFormat;

                if (g_decklink_render_walltime) {
                        struct YUV422P10LE_painter_s pctx;
                        YUV422P10LE_painter_reset(&pctx, dst[0], opts->width, opts->height, opts->width);

                        char ts[64];
                        obe_getTimestamp(ts, NULL);
                        YUV422P10LE_painter_draw_ascii_at(&pctx, 2, 2, ts);
                }
        } else if (opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_YUV422P010) {
                /* If 10bit 422 .... Colorspace convert Y210 into NV20 (mislabeled by Advantech as P210) */

                /*  len as expressed in bytes. No stride */
                int ylen = (opts->width * opts->height) * 5 / 4;
                int uvlen = ylen;
                int dstlen = ylen + uvlen;

                dst[0] = (uint8_t *)malloc(dstlen); /* Y - Primary plane - we submit this single address the the GPU */
                dst[1] = dst[0] + ylen; /* UV plane */

                /* Unpack all of the 16 bit Y/U/Y/V words, shift to correct for padding and write new planes */
                uint8_t *wy = dst[0];
		uint8_t *wuv = dst[1];
                uint16_t *p = (uint16_t *)&st_frame_info->u8pDataBuf[0];

                for (int i = 0 ; i < opts->width * opts->height; i += 4) {
			uint16_t y0, y1, y2, y3;
			uint16_t uv0, uv1, uv2, uv3;
			y0 = *(p++) >> 6;
			uv0 = *(p++) >> 6;
			y1 = *(p++) >> 6;
			uv1 = *(p++) >> 6;
			y2 = *(p++) >> 6;
			uv2 = *(p++) >> 6;
			y3 = *(p++) >> 6;
			uv3 = *(p++) >> 6;

			*(wy++) = y0;
                        *(wy++) = (y0 >> 8) | (y1 << 2);
                        *(wy++) = (y1 >> 6) | (y2 << 4);
                        *(wy++) = (y2 >> 4) | (y3 << 6);
                        *(wy++) = (y3 >> 2);

			*(wuv++) = uv0;
                        *(wuv++) = (uv0 >> 8) | (uv1 << 2);
                        *(wuv++) = (uv1 >> 6) | (uv2 << 4);
                        *(wuv++) = (uv2 >> 4) | (uv3 << 6);
                        *(wuv++) = (uv3 >> 2);
                }

                img.pu8Addr     = dst[0];
                img.u32Size     = dstlen;
                img.eFormat     = opts->codec.eFormat;
        } else {
                img.pu8Addr     = st_frame_info->u8pDataBuf;
                img.u32Size     = st_frame_info->u32BufSize;
                //img.eFormat     = API_VEGA_BQB_IMAGE_FORMAT_NV16; /* 4:2:2 8bit */
        }

        img.eFormat     = opts->codec.eFormat;
        img.pts         = 0;
        img.eTimeBase   = API_VEGA_BQB_TIMEBASE_90KHZ;
        img.bLastFrame  = ctx->bLastFrame;

        if (g_sei_timestamping) {
                /* Create the SEI for the LTN latency tracking. */
                img.u32SeiNum = 1;
                img.bSeiPassThrough = true;
                img.tSeiParam[0].ePayloadLoc   = API_VEGA_BQB_SEI_PAYLOAD_LOC_PICTURE;
                img.tSeiParam[0].ePayloadType  = API_VEGA_BQB_SEI_PAYLOAD_TYPE_USER_DATA_UNREGISTERED;
                img.tSeiParam[0].u8PayloadSize = SEI_TIMESTAMP_PAYLOAD_LENGTH;

                struct timeval tv;
                gettimeofday(&tv, NULL);

                unsigned char *p = &img.tSeiParam[0].u8PayloadData[0];

                if (sei_timestamp_init(p, SEI_TIMESTAMP_PAYLOAD_LENGTH) < 0) {
                        /* This should never happen unless the vega SDK sei header shrinks drastically. */
                        fprintf(stderr, MODULE_PREFIX "SEI space too small\n");
                        return;
                }

                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 1, ctx->framecount);
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, tv.tv_sec);  /* Rx'd from hw/ */
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 3, tv.tv_usec);
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, tv.tv_sec);  /* Tx'd to codec */
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, tv.tv_usec);
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 0); /* time exit from compressor seconds/useconds. */
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 0); /* time exit from compressor seconds/useconds. */
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 8, 0); /* time transmit to udp seconds/useconds. */
                sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 9, 0); /* time transmit to udp seconds/useconds. */
                /* SEI: End */
        }

        /* Capture an output frame to disk, for debug, when you touch this file. */
        {
                struct stat s;
                const char *tf = "/tmp/vega-frame-capture-output.touch";
                int r = stat(tf, &s);
                if (r == 0) {
                        char fn[64];
                        sprintf(fn, "/tmp/%08d-vega-capture-output-port%d.bin", ctx->framecount, opts->card_idx);
                        FILE *fh = fopen(fn, "wb");
                        if (fh) {
                                fwrite(img.pu8Addr, 1, img.u32Size, fh);
                                fclose(fh);
                        }
                        remove(tf);
                }
        }

        /* Submit the picture to the codec */
        if (VEGA_BQB_ENC_PushImage((API_VEGA_BQB_DEVICE_E)u32DevId, (API_VEGA_BQB_CHN_E)eCh, &img)) {
                fprintf(stderr, MODULE_PREFIX "Error: PushImage failed\n");
        }

        if (dst[0])
                free(dst[0]); /* Free the CSCd frame */

#if LOCAL_DEBUG
        printf(MODULE_PREFIX "pushed raw video frame to encoder, PCR %016" PRIi64 ", PTS %012" PRIi64 "\n",
                st_input_info->tCurrentPCR.u64Dword, img.pts);
#endif

#if 1
        printf("Video last pcr minus current pcr %12" PRIi64 " (are we dropping frames?)\n",
                st_input_info->tCurrentPCR.u64Dword - ctx->videoLastPCR
        );
#endif
        ctx->videoLastPCR = st_input_info->tCurrentPCR.u64Dword;

        /* Check the audio PCR vs the video PCR is never more than 200 ms apart */
        int64_t avdeltams = (ctx->videoLastPCR - ctx->audioLastPCR) / 300 / 90;
        int64_t avdelta_warning = 150;
        if (abs(avdeltams) >= avdelta_warning) {
                printf(MODULE_PREFIX "warning: video PCR vs audio PCR delta %12" PRIi64 " (ms), exceeds %" PRIi64 ".\n",
                        (ctx->videoLastPCR - ctx->audioLastPCR) / 300 / 90,
                        avdelta_warning);
        }
}

#if 0
static void callback__process_capture(API_VEGA330X_PICT_INFO_T *pPicInfo, const API_VEGA330X_PICT_INFO_CALLBACK_PARAM_T *args)
{
        printf("%s() pic %d  sei %d\n", __func__,
                pPicInfo->u32PictureNumber,
                pPicInfo->u32SeiNum);
}
#endif

static void close_device(vega_opts_t *opts)
{
	vega_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Closing device#%d port#%d\n", opts->brd_idx, opts->card_idx);

        /* Stop all of the hardware */
        //ctx->bLastFrame = true;
        //usleep(100 * 1000);

        /* TODO: card hangs afetr a while. Suspect I'm not closing it down properly. */

	VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_VIDEO);
	VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_AUDIO);

#if 1
        VEGA_BQB_ENC_Stop((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
#else
        int i = 3000;
        printf("calling stop\n");
        while (VEGA_BQB_ENC_Stop(opts->brd_idx, (API_VEGA330X_CHN_E)opts->card_idx)) {
                printf(".");
                usleep(100 * 1000);
                i -= 100;
                if (i <= 0)
                        break;
        }
        printf("calling stopped\n");
#endif

	VEGA_BQB_ENC_Exit((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
	VEGA3311_CAP_Exit(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);

        if (ctx->avr)
                swr_free(&ctx->avr);

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(vega_opts_t *opts, int probe)
{
	printf(MODULE_PREFIX "%s() probe = %s\n", __func__, probe == 1 ? "true" : "false");
	API_VEGA3311_CAP_RET_E capret;
        API_VEGA_BQB_RET encret;

	vega_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Searching for device#0 port#%d\n", opts->card_idx);

        API_VEGA3311_CAPTURE_DEVICE_INFO_T st_dev_info;
        API_VEGA3311_CAPTURE_FORMAT_T input_src_info;

        //VEGA3311_CAP_ResetChannel(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);
        //sleep(3);
        capret = VEGA3311_CAP_Init(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, CAP_DBG_LEVEL);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to initialize the capture input\n");
                return -1;
        }
        printf(MODULE_PREFIX "Found SDI hardware device#0 port#%d\n", opts->card_idx);
		
        capret = VEGA3311_CAP_GetProperty(opts->brd_idx,
                (API_VEGA3311_CAP_CHN_E)opts->card_idx, &st_dev_info);

        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to get hardware properties\n");
                return -1;
        }

        capret = VEGA3311_CAP_QueryStatus(opts->brd_idx,
                (API_VEGA3311_CAP_CHN_E)opts->card_idx, &input_src_info);

        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to get signal properties\n");
                return -1;
        }

	if (probe == 1) {
                if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_SDI)
                        printf("\tinput source: SDI\n");
                else if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_HDMI)
                        printf("\tinput source: HDMI\n");
                else
                        printf("\tinput source: DisplayPort\n");

                if (st_dev_info.eInputMode == API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI)
                        printf("\tinput mode: 1 Channel UltraHD\n");
                else
                        printf("\tinput mode: 4 Channel FullHD\n");

                switch (st_dev_info.eImageFmt) {
                case API_VEGA3311_CAP_IMAGE_FORMAT_NV12: printf("\tImage Fmt: NV12\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_NV16: printf("\tImage Fmt: NV16\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_YV12: printf("\tImage Fmt: YV12\n"); break;
                //case API_VEGA3311_CAP_IMAGE_FORMAT_I420: printf("\tImage Fmt: I420\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_YV16: printf("\tImage Fmt: YV16\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_YUY2: printf("\tImage Fmt: YUY2\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_P010: printf("\tImage Fmt: P010\n"); break;
                case API_VEGA3311_CAP_IMAGE_FORMAT_P210: printf("\tImage Fmt: P210\n"); break;
                default:
                        printf("\tImage Fmt: NV16\n");
                        break;
                }

                switch (st_dev_info.eAudioLayouts) {
                case API_VEGA3311_CAP_AUDIO_LAYOUT_MONO:      printf("\tAudio layouts: mono\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_STEREO:    printf("\tAudio layouts: stereo\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_2P1:       printf("\tAudio layouts: 2.1 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_3P0:       printf("\tAudio layouts: 3.0 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD:      printf("\tAudio layouts: quad\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD_SIDE: printf("\tAudio layouts: quad side\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_3P1:       printf("\tAudio layouts: 3.1 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0:       printf("\tAudio layouts: 5.0 channel \n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0_SIDE:  printf("\tAudio layouts: 5.0 channel side\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1:       printf("\tAudio layouts: 5.1 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1_SIDE:  printf("\tAudio layouts: 5.1 channel side\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_7P0:       printf("\tAudio layouts: 7.0 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_7P1:       printf("\tAudio layouts: 7.1 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_16P0:      printf("\tAudio layouts: 16.0 channel\n"); break;
                case API_VEGA3311_CAP_AUDIO_LAYOUT_PASS_THROUGH:      printf("\tAudio layouts: fpga format\n"); break;
                default:
                        break;
                }

                if (input_src_info.eSourceSdiLocked == API_VEGA3311_CAP_SRC_STATUS_LOCKED) {
                        printf("\tchannel is locked\n");
                } else {
                        printf("\tchannel is unlocked\n");
                }

                switch (input_src_info.eSourceSdiResolution) {
                case API_VEGA3311_CAP_RESOLUTION_720x480:   printf("\tResolution: 720x480\n"); break;
                case API_VEGA3311_CAP_RESOLUTION_720x576:   printf("\tResolution: 720x576\n"); break;
                case API_VEGA3311_CAP_RESOLUTION_1280x720:  printf("\tResolution: 1280x720\n"); break;
                case API_VEGA3311_CAP_RESOLUTION_1920x1080: printf("\tResolution: 1920x1080\n"); break;
                case API_VEGA3311_CAP_RESOLUTION_3840x2160: printf("\tResolution: 3840x2160\n"); break;
                default:
                        printf("\tResolution: unknown\n");
                        break;
                }

                switch (input_src_info.eSourceSdiChromaFmt) {
                case API_VEGA3311_CAP_CHROMA_FORMAT_420: printf("\tChroma Fmt: 420\n"); break;
                case API_VEGA3311_CAP_CHROMA_FORMAT_444: printf("\tChroma Fmt: 444\n"); break;
                case API_VEGA3311_CAP_CHROMA_FORMAT_422: printf("\tChroma Fmt: 422\n"); break;
                default:
                        printf("\tChroma Fmt: unknown\n");
                        break;
                }

                switch (input_src_info.eSourceSdiSignalLevel) {
                case API_VEGA3311_CAP_SDI_LEVEL_B: printf("\tSDI signal: level B\n"); break;
                default:
                case API_VEGA3311_CAP_SDI_LEVEL_A: printf("\tSDI signal: level A\n"); break;
                }

                if (input_src_info.bSourceSdiInterlace) {
                        if (input_src_info.bSourceSdiFieldTop) {
                                printf("\tSDI scan type: interlace, top field\n");
                        } else {
                                printf("\tSDI scan type: interlace, bottom field\n");
                        }
                } else {
                        printf("\tSDI scan type: progressive\n");
                }

                switch (input_src_info.eSourceSdiBitDepth) {
                case API_VEGA3311_CAP_BIT_DEPTH_12: printf("\tBit Depth: 12 bits\n"); break;
                case API_VEGA3311_CAP_BIT_DEPTH_8:  printf("\tBit Depth: 8 bits\n");  break;
                case API_VEGA3311_CAP_BIT_DEPTH_10: printf("\tBit Depth: 10 bits\n"); break;
                default:
                        printf("\tBit Depth: unknown\n");
                        break;
                }

                switch (input_src_info.eSourceSdiFrameRate) {
                case API_VEGA3311_CAP_FPS_24:    printf("\tFrame per sec: 24\n"); break;
                case API_VEGA3311_CAP_FPS_25:    printf("\tFrame per sec: 25\n"); break;
                case API_VEGA3311_CAP_FPS_29_97: printf("\tFrame per sec: 29.97\n"); break;
                case API_VEGA3311_CAP_FPS_30:    printf("\tFrame per sec: 30\n"); break;
                case API_VEGA3311_CAP_FPS_50:    printf("\tFrame per sec: 50\n"); break;
                case API_VEGA3311_CAP_FPS_59_94: printf("\tFrame per sec: 59.94\n"); break;
                case API_VEGA3311_CAP_FPS_60:    printf("\tFrame per sec: 60\n"); break;
                default:
                        printf("\tFrame per sec: unknown\n");
                        break;
                }

	} // if probe == 1

	if (input_src_info.eSourceSdiLocked != API_VEGA3311_CAP_SRC_STATUS_LOCKED) {
		fprintf(stderr, MODULE_PREFIX "No signal found\n");
		return -1;
	}

	/* We need to understand how much VANC we're going to be receiving. */
	const struct obe_to_vega_video *std = lookupVegaCaptureResolution(
                input_src_info.eSourceSdiResolution,
                input_src_info.eSourceSdiFrameRate,
                input_src_info.bSourceSdiInterlace);
	if (std == NULL) {
		fprintf(stderr, MODULE_PREFIX "No detected standard for vega aborting\n");
		exit(0);
	}

	opts->brd_idx = (API_VEGA_BQB_DEVICE_E)0;
	opts->width = std->width;
	opts->height = std->height;
	opts->interlaced = std->progressive ? 0 : 1;
	opts->timebase_den = std->timebase_den;
	opts->timebase_num = std->timebase_num;
	opts->video_format = std->obe_name;

	fprintf(stderr, MODULE_PREFIX "Detected resolution %dx%d%c @ %d/%d\n",
		opts->width, opts->height,
                opts->interlaced ? 'i' : 'p',
		opts->timebase_den, opts->timebase_num);

	if (probe == 0) {

                ctx->avr = swr_alloc();
                if (!ctx->avr) {
                        fprintf(stderr, MODULE_PREFIX "Could not alloc libswresample context\n");
                        return -1;
                }

                ltn_histogram_alloc_video_defaults(&ctx->hg_callback_audio, "audio arrival latency");
                ltn_histogram_alloc_video_defaults(&ctx->hg_callback_video, "video arrival latency");

                /* Give libswresample a made up channel map.
                 * Convert S16 interleaved to S32P planar.
                 */
                av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_audio_channels) - 1, 0 );
                av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S16, 0 );
                av_opt_set_int(ctx->avr, "in_sample_rate",      48000, 0 );
                av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->num_audio_channels) - 1, 0 );
                av_opt_set_int(ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );
                av_opt_set_int(ctx->avr, "out_sample_rate",     48000, 0 );

                if (swr_init(ctx->avr) < 0) {
                        fprintf(stderr, MODULE_PREFIX "couldn't setup sample rate conversion\n");
                        return -1;
                }

// API_VENC_INIT_PARAM_T
                ctx->init_params.eCodecType             = API_VEGA_BQB_CODEC_TYPE_HEVC;
                ctx->init_params.eOutputFmt             = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

                /* HEVC */
                ctx->init_params.tHevcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_DATA;  /* Source data from Host */
                //ctx->init_params.tHevcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_VIF_SQUARE;
                ctx->init_params.tHevcParam.eInputPort       = (API_VEGA_BQB_VIF_MODE_INPUT_PORT_E)opts->card_idx; // API_VEGA_BQB_VIF_MODE_INPUT_PORT_A;
                ctx->init_params.tHevcParam.eRobustMode      = API_VEGA_BQB_VIF_ROBUST_MODE_BLUE_SCREEN;
                ctx->init_params.tHevcParam.eProfile         = API_VEGA_BQB_HEVC_MAIN422_10_PROFILE;
                ctx->init_params.tHevcParam.eLevel           = API_VEGA_BQB_HEVC_LEVEL_50;
                ctx->init_params.tHevcParam.eTier            = API_VEGA_BQB_HEVC_MAIN_TIER;
                ctx->init_params.tHevcParam.eResolution      = opts->codec.encodingResolution;
                ctx->init_params.tHevcParam.eAspectRatioIdc  = API_VEGA_BQB_HEVC_ASPECT_RATIO_IDC_1;

//
// TODO
//
//
                ctx->init_params.tHevcParam.u32SarWidth     = opts->width;
                ctx->init_params.tHevcParam.u32SarHeight    = opts->height;
                ctx->init_params.tHevcParam.bDisableVpsTimingInfoPresent = false;
                ctx->init_params.tHevcParam.eFormat          = opts->codec.eFormat;
                ctx->init_params.tHevcParam.eChromaFmt       = opts->codec.chromaFormat;
                ctx->init_params.tHevcParam.eBitDepth        = opts->codec.bitDepth;
                ctx->init_params.tHevcParam.bInterlace       = opts->codec.interlaced;
                ctx->init_params.tHevcParam.bDisableSceneChangeDetect       = false;
// tcrop
                ctx->init_params.tHevcParam.eTargetFrameRate = opts->codec.fps;
// tcustomedframerateinfo
                ctx->init_params.tHevcParam.ePtsMode         = API_VEGA_BQB_PTS_MODE_AUTO;
                ctx->init_params.tHevcParam.eIDRFrameNum     = API_VEGA_BQB_IDR_FRAME_ALL;
                //ctx->init_params.tHevcParam.eIDRType         = API_VEGA_BQB_HEVC_IDR_TYPE_W_RADL;
                ctx->init_params.tHevcParam.eIDRType         = API_VEGA_BQB_HEVC_IDR_TYPE_N_LP;
                if (opts->codec.bframes) {
                        ctx->init_params.tHevcParam.eGopType = API_VEGA_BQB_GOP_IPB;
                } else {
                        ctx->init_params.tHevcParam.eGopType = API_VEGA_BQB_GOP_IP;
                }
// eGopHeirarchy
                ctx->init_params.tHevcParam.eGopSize         = opts->codec.gop_size;
                ctx->init_params.tHevcParam.eBFrameNum       = opts->codec.bframes;
                ctx->init_params.tHevcParam.bDisableTemporalId = false;
                ctx->init_params.tHevcParam.eRateCtrlAlgo    = API_VEGA_BQB_RATE_CTRL_ALGO_CBR;
// u32FillerTriggerLevel
                ctx->init_params.tHevcParam.u32Bitrate       = opts->codec.bitrate_kbps; /* TODO: We need to drive this from external to the module. */
// u32MaxVBR
// u32AveVBR
// u32MinVBR
// u32CpbDelay
                ctx->init_params.tHevcParam.tCoding.bDisableDeblocking  = false;
// tHdrConfig
                ctx->init_params.tHevcParam.bEnableUltraLowLatency = false;
                //ctx->init_params.bDisableVpsTimingInfoPresent = false;
                //ctx->init_params.tVideoSignalType.bPresentFlag = true;

                //VEGA_BQB_ENC_SetDbgMsgLevel((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, API_VEGA_BQB_DBG_LEVEL_3);
                fprintf(stderr, "AAAAAAA\n");

                if (VEGA_BQB_ENC_IsDeviceModeConfigurable((API_VEGA_BQB_DEVICE_E)opts->brd_idx)) {
                     fprintf(stderr, "DEVICE MODE IS CONFIGURABLE\n");
                        API_VEGA_BQB_ENCODE_CONFIG_T encode_config;
                        memset(&encode_config, 0, sizeof(API_VEGA_BQB_ENCODE_CONFIG_T));
                        switch (ctx->init_params.tHevcParam.eResolution) {
                        case API_VEGA_BQB_RESOLUTION_4096x2160:
                        case API_VEGA_BQB_RESOLUTION_3840x2160:
                                encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                                break;
                        case API_VEGA_BQB_RESOLUTION_2048x1080:
                        case API_VEGA_BQB_RESOLUTION_1920x1080:
                                encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_4CH_1080P;
                                break;
                        case API_VEGA_BQB_RESOLUTION_1280x720:
                                encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_8CH_720P;
                                break;
                        case API_VEGA_BQB_RESOLUTION_720x576:
                        case API_VEGA_BQB_RESOLUTION_720x480:
                        case API_VEGA_BQB_RESOLUTION_416x240:
                        case API_VEGA_BQB_RESOLUTION_352x288:
                                encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_16CH_SD;
                                break;
                        default:
                                encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                                break;
                        }

                        VEGA_BQB_ENC_ConfigDeviceMode((API_VEGA_BQB_DEVICE_E)opts->brd_idx, &encode_config);
                }

                API_VEGA_BQB_DESC_T desc = { { 0 } };
                VEGA_BQB_ENC_InitParamToString(ctx->init_params, &desc);
                fprintf(stderr, "stoth initial param to string: %s\n", desc.content);

		/* Open the capture hardware here */
		if (VEGA_BQB_ENC_Init((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, &ctx->init_params)) {
			fprintf(stderr, MODULE_PREFIX "VEGA_BQB_ENC_Init: failed to initialize the encoder\n");
			return -1;
		}
                fprintf(stderr, "BAAAAAA\n");

                encret = VEGA_BQB_ENC_RegisterCallback((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, callback__process_video_coded_frame, opts);
                if (encret!= API_VEGA_BQB_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to register encode callback function\n");
                        return -1;
                }


                printf(MODULE_PREFIX "Registering Capture Video callback\n");

                capret = VEGA3311_CAP_RegisterVideoCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                        callback__v_capture_cb_func, opts);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS)
                {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to register video capture callback function\n");
                        return -1;
                }
                printf(MODULE_PREFIX "Registering Capture Audio callback\n");

                capret = VEGA3311_CAP_RegisterAudioCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                        callback__a_capture_cb_func, opts);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to register audio capture callback function\n");
                        return -1;
                }


#if 0
                /* Experimental, what is this callback for? and how does it relate to the existing a/v callbacks? */
                encret = VEGA_BQB_ENC_RegisterPictureInfoCallback((API_VEGA330X_BOARD_E)opts->brd_idx, (API_VEGA330X_CHN_E)opts->card_idx,
                        callback__process_capture, opts);
                if (encret != API_VEGA330X_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to enc pictures info callback\n");
                }
#endif

                printf(MODULE_PREFIX "Starting hardware encoder\n");

                encret = VEGA_BQB_ENC_Start((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
                if (encret != API_VEGA_BQB_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to enc start\n");
                        return -1;
                }
                
                ctx->ch_init_param.eFormat       = opts->codec.pixelFormat;
#if 1
                //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_P010;
                //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_P210;
                //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_V210;
                ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_Y210;
#endif
                ctx->ch_init_param.eInputMode    = opts->codec.inputMode;
                ctx->ch_init_param.eInputSource  = opts->codec.inputSource;
                ctx->ch_init_param.eAudioLayouts = opts->codec.audioLayout;
//	API_VEGA3311_CAP_ANC_WINDOW_T          tAncWindowSetting;
//	API_VEGA3311_CAP_ANC_REMAPPING_T       tAncRemapSetting;

                printf("--- Capture Configuration VEGA3311_CAP_Config(%d, %d, %%p);\n", opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);
                printf("capture.eFormat             = %2d %s\n", ctx->ch_init_param.eFormat, lookupVegaPixelFormatName(ctx->ch_init_param.eFormat));
                printf("capture.eInputMode          = %2d %s\n", ctx->ch_init_param.eInputMode, lookupVegaInputModeName(ctx->ch_init_param.eInputMode));
                printf("capture.eInputSource        = %2d %s\n", ctx->ch_init_param.eInputSource, lookupVegaInputSourceName(ctx->ch_init_param.eInputSource));
                printf("capture.eAudioPacketSize    = %2d %s\n", ctx->ch_init_param.eAudioPacketSize, lookupVegaAudioPacketSizeName(ctx->ch_init_param.eAudioPacketSize));
                printf("capture.eRobustModeEn       = %2d\n", ctx->ch_init_param.eRobustMode);
                printf("capture.bAudioAutoRestartEn = %2d\n", ctx->ch_init_param.bAudioAutoRestartEn);
                printf("capture.eAudioLayouts       = %2d %s\n", ctx->ch_init_param.eAudioLayouts, lookupVegaAudioLayoutName(ctx->ch_init_param.eAudioLayouts));
                printf("---\n");
#if 0
/* Intensional segfault */
printf("Segfaulting to debug, gdb print this: ctx->ch_init_param\n");
char *p = 0;
printf("%d\n", *p);
#endif
                printf(MODULE_PREFIX "Configuring Capture Interface\n");

                capret = VEGA3311_CAP_Config(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, &ctx->ch_init_param);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to cap config\n");
                        return -1;
                }

                printf(MODULE_PREFIX "Starting Capture Interface\n");

                capret = VEGA3311_CAP_Start(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                        API_VEGA3311_CAP_ENABLE_ON, API_VEGA3311_CAP_ENABLE_ON, API_VEGA3311_CAP_ENABLE_ON);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to cap start\n");
                        return -1;
                }

                printf(MODULE_PREFIX "The vega encoder device#0 port%d was started\n", opts->card_idx);
	}

	return 0; /* Success */
}

/* Called from open_input() */
static void close_thread(void *handle)
{
	if (!handle)
		return;

	vega_opts_t *opts = (vega_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *vega_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 1 + MAX_AUDIO_PAIRS;

	printf(MODULE_PREFIX "%s()\n", __func__);

	vega_ctx_t *ctx;

	vega_opts_t *opts = (vega_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

	/* TODO: support multi-channel */
	opts->num_audio_channels = MAX_AUDIO_CHANNELS;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->probe = 1;

	ctx = &opts->ctx;
	ctx->h = h;

	/* Open device */
	if (open_device(opts, 1) < 0) {
		fprintf(stderr, MODULE_PREFIX "Unable to open device.\n");
		goto finish;
	}

	sleep(1);

	close_device(opts);

	opts->probe_success = 1;
	fprintf(stderr, MODULE_PREFIX "Probe success\n");

	if (!opts->probe_success) {
		fprintf(stderr, MODULE_PREFIX "No valid frames received - check connection and input format\n");
		goto finish;
	}

	/* TODO: probe for SMPTE 337M */
	/* TODO: factor some of the code below out */

	for( int i = 0; i < num_streams; i++ ) {

		streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
		if (!streams[i])
			goto finish;

		/* TODO: make it take a continuous set of stream-ids */
		pthread_mutex_lock( &h->device_list_mutex );
		streams[i]->input_stream_id = h->cur_input_stream_id++;
		pthread_mutex_unlock( &h->device_list_mutex );

		if (i == 0) {
			streams[i]->stream_type   = STREAM_TYPE_VIDEO;
			streams[i]->stream_format = VIDEO_HEVC_VEGA3311;
			streams[i]->width         = opts->width;
			streams[i]->height        = opts->height;
			streams[i]->timebase_num  = opts->timebase_num;
			streams[i]->timebase_den  = opts->timebase_den;
			streams[i]->csp           = AV_PIX_FMT_QSV; /* Special tag. We're providing NALS not raw video. */
			streams[i]->interlaced    = opts->interlaced;
			streams[i]->tff           = 1; /* NTSC is bff in baseband but coded as tff */
			streams[i]->sar_num       = streams[i]->sar_den = 1; /* The user can choose this when encoding */
		}
		else if( i >= 1 ) {
			/* TODO: various assumptions about audio being 48KHz need resolved.
         		 * Some sources could be 44.1 and this module will fall down badly.
			 */
			streams[i]->stream_type    = STREAM_TYPE_AUDIO;
			streams[i]->stream_format  = AUDIO_PCM;
			streams[i]->num_channels   = 2;
			streams[i]->sample_format  = AV_SAMPLE_FMT_S32P;
			streams[i]->sample_rate    = 48000;
			streams[i]->sdi_audio_pair = i;
		}
	}

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = num_streams;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_VEGA3311;
	memcpy(&device->user_opts, user_opts, sizeof(*user_opts));

	/* add device */
	add_device(h, device);

finish:
	opts->probe = 0;
	if (opts)
		free(opts);

	free(probe_ctx);

	return NULL;
}

static void *vega_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	vega_ctx_t *ctx;

	vega_opts_t *opts = (vega_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->num_audio_channels = MAX_AUDIO_CHANNELS;
	opts->card_idx           = user_opts->card_idx;
	opts->video_format       = user_opts->video_format;

	ctx         = &opts->ctx;
	ctx->device = device;
	ctx->h      = h;

        if (configureCodec(opts) < 0) {
                fprintf(stderr, MODULE_PREFIX "invalid encoder parameters, aborting.\n");
                return NULL;
        }

	if (open_device(opts, 0) < 0)
		return NULL;

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t vega3311_input = { vega_probe_stream, vega_open_input };

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
