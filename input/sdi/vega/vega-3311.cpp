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
#include <libltntstools/hexdump.h>

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define MODULE_PREFIX "[vega]: "

#define LOCAL_DEBUG 0

#define FORCE_10BIT 1
#define INSERT_HDR 0

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
extern int g_decklink_record_audio_buffers;

typedef struct
{
	/* Device input related */
	obe_device_t *device;
	obe_t *h;

        /* Capture layer and encoder layer configuration management */
        API_VEGA_BQB_INIT_PARAM_T init_params;
        API_VEGA_BQB_INIT_PARAM_T init_paramsMACRO;      /* TODO fix this during confirmance checking */
        API_VEGA_BQB_INIT_PARAM_T init_paramsMACROULL;   /* TODO fix this during confirmance checking */
        API_VEGA3311_CAP_INIT_PARAM_T ch_init_param;

        uint32_t framecount;
        int bLastFrame;

        /* VANC Handling, attached the klvanc library for processing and parsing. */
        struct klvanc_context_s *vanchdl;

        /* Audio - Sample Rate Conversion. We convert S32 interleaved into S32P planer. */
        struct SwrContext *avr;

        /* Video timing tracking */
        int64_t videoLastPCR;
        int64_t videoLastDTS, videoLastPTS;
        int64_t videoDTSOffset;
        int64_t videoPTSOffset;
        int64_t lastAdjustedPicDTS;
        int64_t lastAdjustedPicPTS;
        int64_t lastcorrectedPicPTS;
        int64_t lastcorrectedPicDTS;

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

typedef struct
{
    volatile uint32_t pkt_continuity_counter;
    volatile uint32_t block_number: 16;
    volatile uint32_t f_bit: 1;
    volatile uint32_t o_bit: 1;
    volatile uint32_t reserved0: 14;
    volatile uint32_t stc_lsb;
    volatile uint32_t stc_msb;
    volatile API_VEGA3311_CAP_PCR_FORMAT_T ancd_pcr;
} AncdPHD; /* 24 bytes */

typedef struct
{
    volatile uint32_t user_data0: 10;
    volatile uint32_t user_data1: 10;
    volatile uint32_t checksum: 10;
    volatile uint32_t padding2: 2;
} anc_user_data_format; /* 4 bytes */

typedef struct
{
    /* 32 bits */
    volatile uint32_t header_flag: 22;
    volatile uint32_t h_not_v_flag: 1;
    volatile uint32_t post_pre_flag: 1;
    volatile uint32_t block_index: 8;

    /* 32 bits */
    volatile uint32_t header_pattern: 6;
    volatile uint32_t c_not_y_flag: 1;
    volatile uint32_t line_number: 11;
    volatile uint32_t horizontal_offset: 12;
    volatile uint32_t padding0: 2;

    /* 32 bits */
    volatile uint32_t did: 10;
    volatile uint32_t sdid: 10;
    volatile uint32_t data_count: 10;
    volatile uint32_t padding1: 2;
} block_data_t; /* 12 bytes */

/* VANC Callbacks */
static int cb_EIA_708B(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_eia_708b_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_708B(vanchdl, pkt); /* vanc lib helper */
	}

	return 0;
}

static int cb_EIA_608(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_eia_608_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);
	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_608(vanchdl, pkt); /* vanc library helper */
	}

	return 0;
}

#if 0
/* We're given some VANC data, craete a SCTE104 metadata entry for it
 * and we'll send it to the encoder later, by an atachment to the video frame.
 */
static int add_metadata_scte104_vanc_section(decklink_ctx_t *decklink_ctx,
	struct avmetadata_s *md,
	uint8_t *section, uint32_t section_length, int lineNr)
{
	/* Put the coded frame into an raw_frame attachment and discard the coded frame. */
	int idx = md->count;
	if (idx >= MAX_RAW_FRAME_METADATA_ITEMS) {
		printf("%s() already has %d items, unable to add additional. Skipping\n",
			__func__, idx);
		return -1;
	}

	md->array[idx] = avmetadata_item_alloc(section_length, AVMETADATA_VANC_SCTE104);
	if (!md->array[idx])
		return -1;
	md->count++;

	avmetadata_item_data_write(md->array[idx], section, section_length);
	avmetadata_item_data_set_linenr(md->array[idx], lineNr);

        int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, DVB_TABLE_SECTION);
        if (streamId < 0)
                return 0;
	avmetadata_item_data_set_outputstreamid(md->array[idx], streamId);

	return 0;
}

static int transmit_pes_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *buf, uint32_t byteCount)
{
	int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, SMPTE2038);
	if (streamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, byteCount);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, byteCount);
		return -1;
	}
	coded_frame->pts = decklink_ctx->stream_time;
	coded_frame->random_access = 1; /* ? */
	memcpy(coded_frame->data, buf, byteCount);
	add_to_queue(&decklink_ctx->h->mux_queue, coded_frame);

	return 0;
}
#endif

static int cb_SCTE_104(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_scte_104_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_SCTE_104(vanchdl, pkt); /* vanc library helper */
	}

	if (klvanc_packetType1(&pkt->hdr)) {
		/* Silently discard type 1 SCTE104 packets, as per SMPTE 291 section 6.3 */
		return 0;
	}

#if 0

	/* Append all the original VANC to the metadata object, we'll process it later. */
	add_metadata_scte104_vanc_section(decklink_ctx, &decklink_ctx->metadataVANC,
		(uint8_t *)&pkt->hdr.raw[0],
		pkt->hdr.rawLengthWords * 2,
		pkt->hdr.lineNr);

	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
		static time_t lastErrTime = 0;
		time_t now = time(0);
		if (lastErrTime != now) {
			lastErrTime = now;

			char t[64];
			sprintf(t, "%s", ctime(&now));
			t[ strlen(t) - 1] = 0;
			syslog(LOG_INFO, "[decklink] SCTE104 frames present on SDI");
			fprintf(stdout, "[decklink] SCTE104 frames present on SDI  @ %s", t);
			printf("\n");
			fflush(stdout);

		}
	}
#endif
	return 0;
}

static int cb_all(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_header_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s()\n", __func__);
	}

        //klvanc_packet_header_dump_console(pkt); /* Local project fund, see vega3311-misc.cpp */

#if 0

	/* We've been called with a VANC frame. Pass it to the SMPTE2038 packetizer.
	 * We'll be called here from the thread handing the VideoFrameArrived
	 * callback, which calls vanc_packet_parse for each ANC line.
	 * Push the pkt into the SMPTE2038 layer, its collecting VANC data.
	 */
	if (decklink_ctx->smpte2038_ctx) {
		if (klvanc_smpte2038_packetizer_append(decklink_ctx->smpte2038_ctx, pkt) < 0) {
		}
	}

	decklink_opts_t *decklink_opts = container_of(decklink_ctx, decklink_opts_t, decklink_ctx);
	if (OPTION_ENABLED(patch1) && decklink_ctx->vanchdl && pkt->did == 0x52 && pkt->dbnsdid == 0x01) {

		/* Patch#1 -- SCTE104 VANC appearing in a non-standard DID.
		 * Modify the DID to reflect traditional SCTE104 and re-parse.
		 * Any potential multi-entrant libklvanc issues here? No now, future probably yes.
		 */

		/* DID 0x62 SDID 01 : 0000 03ff 03ff 0162 0101 0217 01ad 0115 */
		pkt->raw[3] = 0x241;
		pkt->raw[4] = 0x107;
		int ret = klvanc_packet_parse(decklink_ctx->vanchdl, pkt->lineNr, pkt->raw, pkt->rawLengthWords);
		if (ret < 0) {
			/* No VANC on this line */
			fprintf(stderr, "%s() patched VANC for did 0x52 failed\n", __func__);
		}
	}
#endif
	return 0;
}

static int cb_VANC_TYPE_KL_UINT64_COUNTER(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_kl_u64le_counter_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);

        /* Have the library display some debug */
	static uint64_t lastGoodKLFrameCounter = 0;
        if (lastGoodKLFrameCounter && lastGoodKLFrameCounter + 1 != pkt->counter) {
                char ts[64];
                obe_getTimestamp(ts, NULL);

                fprintf(stderr, MODULE_PREFIX "%s: KL VANC frame counter discontinuity was %" PRIu64 " now %" PRIu64 "\n",
                        ts,
                        lastGoodKLFrameCounter, pkt->counter);
        }
        lastGoodKLFrameCounter = pkt->counter;

        return 0;
}

static struct klvanc_callbacks_s callbacks = 
{
	.afd			= NULL,
	.eia_708b		= cb_EIA_708B,
	.eia_608		= cb_EIA_608,
	.scte_104		= cb_SCTE_104,
	.all			= cb_all,
	.kl_i64le_counter       = cb_VANC_TYPE_KL_UINT64_COUNTER,
	.sdp			= NULL,
};
/* End: VANC Callbacks */

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

/* Convert the Advantech oddly frameswpacket VANC into a standardized version we can 
 * push into libKL vanc, for parsing and calback handling.
 */
static void parse_ancillary_data(vega_ctx_t *ctx, vega_opts_t *opts, uint32_t block_index, uint8_t *src_buf)
{
        block_data_t *block_data_ptr = (block_data_t *)src_buf;

        /* Pointer to vanc data */
        //uint8_t *udw_ptr_last = NULL;
        uint8_t *udw_ptr = (uint8_t *)block_data_ptr + sizeof(block_data_t);
        uint8_t word10b_count;

        /* Bit shifting and unpacking table we use to walk the payload and convert into uint16_t's */
        struct state_s
        {
                int offset;
                int b1mask;
                int b1rshift;
                int b2mask;
                int b2lshift;
        } states[4] = {
            { 0, 0xff, 0, 0x03, 8, },
            { 1, 0xfc, 2, 0x0f, 6, },
            { 2, 0xf0, 4, 0x3f, 4, },
            { 3, 0xc0, 0, 0xff, 0, },
        };

        /* For every VANC messages we're passed .... */
        for (uint32_t i = 0; i < block_index; i++) {

                word10b_count = (uint8_t)block_data_ptr->data_count;

                /* Convert this specifi9c VANC message into uint16_t payload */
                int payloadIdx = 0;
                uint16_t payload[256] = {0};
                payload[payloadIdx++] = 0;
                payload[payloadIdx++] = 0x3ff;
                payload[payloadIdx++] = 0x3ff;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->did;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->sdid;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->data_count;
                //payload[payloadIdx++] = (uint32_t)block_data_ptr->horizontal_offset;

                int cstate = 0; /* We only go through state 0 once, start here then we iterate through 1..3 after this */
                
                /* Calculate and round up number of blocks containing words */
                /* Setup a uint8_t loop, walk thr loop using the state/shift machine rules to unpack the payload */
                uint16_t a, b, c, d;
                int count = ((word10b_count / 5) + 1) * 5;
                for (int j = 0; j < (count * 10) / 8; j += 4)
                {
                        a = 0xffff;
                        if (cstate == 0)
                        {
                                a = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                                a |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                                payload[payloadIdx++] = a;
                                cstate++;
                        }

                        b = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        b |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = b;

                        cstate++;
                        c = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        c |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = c;

                        cstate++;
                        d = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        d |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = d;

#if 0
                        if (a != 0xffff) {
                                printf("\toffset[%3d] %04x %04x %04x %04x\n", j, a, b, c, d);
                        } else {
                                printf("\toffset[%3d]      %04x %04x %04x\n", j,    b, c, d);
                        }
#endif
                        cstate = 1;
                }
                payload[payloadIdx++] = 0xdead;

#if 0
                /* Dump the payload in 8 bit format, so we can compare klvanc in cinterface mode output */
                printf("Reconstructed VANC Payload for parsing:\n");
                for (int j = 1; j < payloadIdx + 1; j++)
                {
                        printf("%04x ", payload[j - 1]);
                        if (j % 16 == 0)
                                printf("\n");
                }
                printf("\n");
#endif

                if (ctx->vanchdl) {
        		int ret = klvanc_packet_parse(ctx->vanchdl,
                                (uint32_t)block_data_ptr->line_number,
                                payload,
                                sizeof(payload) / (sizeof(unsigned short)));
        		if (ret < 0) {
                                /* No VANC on this line */
                                fprintf(stderr, MODULE_PREFIX "No VANC on this line / parse failed or construction bug?\n");
		        }
	        }

#if 0
                /* Dump the raw payload, for debug purposes, in its advantech packed format */
                for (int i = 0; i < word10b_count; i++)
                {
                        printf("%02x ", *(udw_ptr + i));
                }
                printf("\n");
#endif

#if 0
                if (block_data_ptr->h_not_v_flag == 0)
                {
                        printf("\tBlock idx %u: %s-V, %s, line %u, H-offset=%u, DID 0x%X, SDID 0x%X, DC 0x%X\n",
                               i,
                               (block_data_ptr->post_pre_flag == 0) ? "POST" : "PRE",
                               (block_data_ptr->c_not_y_flag == 0) ? "Y" : "C",
                               (uint32_t)block_data_ptr->line_number,
                               (uint32_t)block_data_ptr->horizontal_offset,
                               (uint32_t)block_data_ptr->did,
                               (uint32_t)block_data_ptr->sdid,
                               (uint32_t)block_data_ptr->data_count);
                }
                else
                {
                        printf("\tBlock idx %u: POST-H, %s, line %u, H-offset=%u, DID 0x%X, SDID 0x%X, DC 0x%X\n",
                               i,
                               (block_data_ptr->c_not_y_flag == 0) ? "Y" : "C",
                               (uint32_t)block_data_ptr->line_number,
                               (uint32_t)block_data_ptr->horizontal_offset,
                               (uint32_t)block_data_ptr->did,
                               (uint32_t)block_data_ptr->sdid,
                               (uint32_t)block_data_ptr->data_count);
                }
#endif
                /* setup for next block data */
                /* sizeof(block_data_t) = 3x uint32_t = 12 bytes */
                block_data_ptr = (block_data_t *)(udw_ptr + ((word10b_count + 3) / 3) * sizeof(uint32_t));
                udw_ptr = (uint8_t *)block_data_ptr + sizeof(block_data_t);
#if 0
                if (udw_ptr_last)
                {
                        printf("%ld bytes\n", udw_ptr - udw_ptr_last);
                }
                udw_ptr_last = udw_ptr;
#endif
        } /* For every VANC messages we're passed.... */
}

static void callback__anc_capture_cb_func(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T* st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T* st_input_info,
        void *pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
        vega_ctx_t *ctx = &opts->ctx;

        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to capturing, source signal recovery\n", u32DevId, eCh);
                } else if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_WAITING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to waiting, source signal loss\n", u32DevId, eCh);
                } else if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_STANDBY) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to standby, callback function is terminated\n", u32DevId, eCh);
                }

                return;
        }

        AncdPHD *pkt_hdr = (AncdPHD *)st_frame_info->u8pDataBuf;
#if 0
        printf(MODULE_PREFIX "anc callback PCR %15" PRIi64 " size %d bytes\n", pkt_hdr->ancd_pcr.u64Dword, st_frame_info->u32BufSize);
#if 0
        printf(MODULE_PREFIX "Entire VANC callback data:\n");
        ltntstools_hexdump(st_frame_info->u8pDataBuf, st_frame_info->u32BufSize, 32);
#endif
#endif
        uint8_t *ancd_data_block_p = st_frame_info->u8pDataBuf + sizeof(AncdPHD);
        //int ancd_data_block_size = st_frame_info->u32BufSize - sizeof(AncdPHD);

        parse_ancillary_data(ctx, opts, (uint32_t)pkt_hdr->block_number, ancd_data_block_p);
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

        if (g_decklink_record_audio_buffers) {
            g_decklink_record_audio_buffers--;
            static int aidx = 0;
            char fn[256];
            int channels = MAX_AUDIO_CHANNELS;
            int samplesPerChannel = st_frame_info->u32BufSize / channels / sizeof(uint16_t);
#if AUDIO_DEBUG_ENABLE
            sprintf(fn, "/storage/ltn/stoth/cardindex%d-audio%03d-srf%d.raw", opts->card_idx, aidx++, samplesPerChannel);
#else
            sprintf(fn, "/tmp/cardindex%d-audio%03d-srf%d.raw", opts->card_idx, aidx++, samplesPerChannel);
#endif
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                fwrite(st_frame_info->u8pDataBuf, 1, st_frame_info->u32BufSize, fh);
                fclose(fh);
                printf("Creating %s\n", fn);
            }
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
        int64_t pts = pcr / 300LL;
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

/* Take a single NAL, convert to a raw frame and push it into the downstream workflow.
 * TODO: Coalesce these at a higher level and same some memory allocs?
 * PTS is a 64bit signed value, than doesn't wrap at 33 bites, it constantly accumlates.
 */
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
        rf->pts = pts * 300LL; /* Converts 90KHz to 27MHz */

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
	vega_ctx_t *ctx = &opts->ctx;

#if 0
        FILE *fh = fopen("/storage/ltn/vega/hevc-pic-data.bin", "a+");
        if (fh) {
                fwrite(p_pict, 1, 16 * sizeof(int64_t), fh);
                fclose(fh);
                printf(MODULE_PREFIX "Wrote %d bytes\n", sizeof(*p_pict));
        }
#endif

        /* When the value wraps, make sure we disgregard iffy bits 63-33.
         * The result is a pure 33bit guaranteed positive number.
         */
        int64_t adjustedPicPTS = p_pict->pts & 0x1ffffffffLL;
        int64_t adjustedPicDTS = p_pict->dts & 0x1ffffffffLL;

        if (adjustedPicDTS < ctx->lastAdjustedPicDTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoDTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping DTS base\n");
        }
        if (adjustedPicPTS < ctx->lastAdjustedPicPTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoPTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping PTS base\n");
        }

        /* Compute a new PTS/DTS based on a constant accumulator and guaranteed positive PTS/DTS value */
        int64_t correctedPTS = ctx->videoDTSOffset + adjustedPicPTS;
        int64_t correctedDTS = ctx->videoPTSOffset + adjustedPicDTS;

        /* The upshot is that the correctPTS and correctDTS should always increase over time regardless of
         * wrapping and can be used to pass downstream (same as the decklink).
         */

        for (unsigned int i = 0; i < p_pict->u32NalNum; i++) {

                if (g_decklink_monitor_hw_clocks) {
                        printf(MODULE_PREFIX "corrected  pts: %15" PRIi64 "  dts: %15" PRIi64
                                " adjustedPicPTS %15" PRIi64 " adjustedPicDTS %15" PRIi64
                                " ctx->videoPTSOffset %15" PRIi64 " ctx->videoDTSOffset %15" PRIi64 "\n",
                                correctedPTS, correctedDTS,
                                adjustedPicPTS, adjustedPicDTS,
                                ctx->videoPTSOffset, ctx->videoDTSOffset
                        );
                        printf(MODULE_PREFIX "pic        pts: %15" PRIi64 "  dts: %15" PRIi64 " base %012" PRIu64 " ext %012d frametype: %s  addr: %p length %7d -- ",
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

                deliver_video_nal(opts, p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length, correctedPTS, correctedDTS);
        }

        ctx->videoLastDTS = p_pict->dts;
        ctx->videoLastPTS = p_pict->pts;
        ctx->lastAdjustedPicPTS = adjustedPicPTS;
        ctx->lastAdjustedPicDTS = adjustedPicDTS;
        ctx->lastcorrectedPicPTS = correctedPTS;
        ctx->lastcorrectedPicDTS = correctedDTS;

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
        int64_t pcr = st_input_info->tCurrentPCR.u64Dword;

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

#if INSERT_HDR
    API_VEGA_BQB_SEI_PARAM_T *p_sei = NULL;
    API_VEGA_BQB_HDR_PARAM_T *p_hdr = &t_init_param.tHevcParam.tHdrConfig;
   
    pPicInfo->u32PictureNumber = args->u32CaptureCounter;

    p_sei = &pPicInfo->tSeiParam[pPicInfo->u32SeiNum];
    uint16_t u16MaxContentLightLevel = p_hdr->u16MaxContentLightLevel;
    uint16_t u16MaxPictureAvgLightLevel = p_hdr->u16MaxPictureAvgLightLevel;
    VEGA_BQB_ENC_MakeContentLightInfoSei
    (
        p_sei,
        API_VEGA_BQB_SEI_PAYLOAD_LOC_GOP,
        u16MaxContentLightLevel,
        u16MaxPictureAvgLightLevel
    );
    pPicInfo->u32SeiNum++;

    p_sei = &pPicInfo->tSeiParam[pPicInfo->u32SeiNum];
    uint16_t u16DisplayPrimariesX0 = p_hdr->u16DisplayPrimariesX0;
    uint16_t u16DisplayPrimariesY0 = p_hdr->u16DisplayPrimariesY0;
    uint16_t u16DisplayPrimariesX1 = p_hdr->u16DisplayPrimariesX1;
    uint16_t u16DisplayPrimariesY1 = p_hdr->u16DisplayPrimariesY1;
    uint16_t u16DisplayPrimariesX2 = p_hdr->u16DisplayPrimariesX2;
    uint16_t u16DisplayPrimariesY2 = p_hdr->u16DisplayPrimariesY2;
    uint16_t u16WhitePointX = p_hdr->u16WhitePointX;
    uint16_t u16WhitePointY = p_hdr->u16WhitePointY;
    uint32_t u32MaxDisplayMasteringLuminance = p_hdr->u32MaxDisplayMasteringLuminance;
    uint32_t u32MinDisplayMasteringLuminance = p_hdr->u32MinDisplayMasteringLuminance;
    VEGA_BQB_ENC_MakeMasteringDisplayColourVolumeSei
    (
        p_sei,
        u16DisplayPrimariesX0,
        u16DisplayPrimariesY0,
        u16DisplayPrimariesX1,
        u16DisplayPrimariesY1,
        u16DisplayPrimariesX2,
        u16DisplayPrimariesY2,
        u16WhitePointX,
        u16WhitePointY,
        u32MaxDisplayMasteringLuminance,
        u32MinDisplayMasteringLuminance
    );
    pPicInfo->u32SeiNum++;
#endif

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
        img.pts         = pcr / 300LL;
        img.eTimeBase   = API_VEGA_BQB_TIMEBASE_90KHZ;
        img.bLastFrame  = ctx->bLastFrame;

        if (g_sei_timestamping) {

                // WARNING..... DON'T TRAMPLE THE HDR SEI

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
                pcr, img.pts);
#endif

#if 0
        printf("Video last pcr minus current pcr %12" PRIi64 " (are we dropping frames?)\n",
                pcr - ctx->videoLastPCR
        );
#endif
        ctx->videoLastPCR = pcr;

        /* Check the audio PCR vs the video PCR is never more than 200 ms apart */
        int64_t avdeltams = (ctx->videoLastPCR - ctx->audioLastPCR) / 300LL / 90LL;
        int64_t avdelta_warning = 150;
        if (abs(avdeltams) >= avdelta_warning) {
                printf(MODULE_PREFIX "warning: video PCR vs audio PCR delta %12" PRIi64 " (ms), exceeds %" PRIi64 ".\n",
                        (ctx->videoLastPCR - ctx->audioLastPCR) / 300 / 90,
                        avdelta_warning);
        }
}

static void callback__process_capture(API_VEGA_BQB_PICT_INFO_T *pPicInfo, const API_VEGA_BQB_PICT_INFO_CALLBACK_PARAM_T *args)
{
        printf("%s() pic %d  sei %d\n", __func__,
                pPicInfo->u32PictureNumber,
                pPicInfo->u32SeiNum);

        // HDR Insert happends here?
        // Why not do this in the capture callback?
}

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
        VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_ANC_DATA);

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

        if (ctx->vanchdl) {
                klvanc_context_destroy(ctx->vanchdl);
                ctx->vanchdl = 0;
        }

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(vega_opts_t *opts, int probe)
{
	printf(MODULE_PREFIX "%s() probe = %s\n", __func__, probe == 1 ? "true" : "false");
	API_VEGA3311_CAP_RET_E capret;
        API_VEGA_BQB_RET encret;

	vega_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Searching for device#0 port#%d\n", opts->card_idx);
        
        if (klvanc_context_create(&ctx->vanchdl) < 0) {
                fprintf(stderr, MODULE_PREFIX "Error initializing VANC library context\n");
        } else {
                ctx->vanchdl->verbose = 0;
                ctx->vanchdl->callbacks = &callbacks;
                ctx->vanchdl->callback_context = opts;
                ctx->vanchdl->allow_bad_checksums = 1;
                //ctx->last_vanc_cache_dump = 0;

#if 0
                if (OPTION_ENABLED(vanc_cache)) {
                        /* Turn on the vanc cache, we'll want to query it later. */
                        decklink_ctx->last_vanc_cache_dump = 1;
                        fprintf(stdout, "Enabling option VANC CACHE, interval %d seconds\n", VANC_CACHE_DUMP_INTERVAL);
                        klvanc_context_enable_cache(decklink_ctx->vanchdl);
                }
#endif
        }

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

                printf("\tAncillary data window settings:\n");
                printf("\t\tHANC      space: %d\n", st_dev_info.tAncWindowSetting.bHanc);
                printf("\t\tPRE-VANC  space: %d\n", st_dev_info.tAncWindowSetting.bPreVanc);
                printf("\t\tPOST-VANC space: %d\n", st_dev_info.tAncWindowSetting.bPostVanc);

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
                ctx->init_params.tHevcParam.eLevel           = API_VEGA_BQB_HEVC_LEVEL_41;
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

                /* Prevent 1920x1080 encodes coming out as 1920x1088 */
                ctx->init_params.tHevcParam.tCrop.u32CropLeft   = 0;
                ctx->init_params.tHevcParam.tCrop.u32CropRight  = 0;
                ctx->init_params.tHevcParam.tCrop.u32CropTop    = 0;
                ctx->init_params.tHevcParam.tCrop.u32CropBottom = ctx->init_params.tHevcParam.u32SarHeight % 16;

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

#if 0
/**
@brief Creates a API_VEGA_BQB_INIT_PARAM_X_T structure for HEVC encode.

The \b VEGA_BQB_ENC_MakeInitParamX function helps user to create a VEGA-331X init parameter, which can be used to initialize the 1-N encoder.

@param pApiInitParam    pointer to init parameter to be set; provided by caller.
@param eInputResolution input resolution size
@param eInputChromaFmt  input chroma format
@param eInputBitDepth   input bit depth
@param eInputFrameRate  input frame rate
@param u32NumOfOutputs  output numbers
@param u32OutputId[]    array of output ID
@param eProfile[]       output profile values.
@param eLevel[]         output level values
@param eTier[]          output tier values
@param eResolution[]    output resolution values.
@param eChromaFmt[]     output chroma formats.
@param eBitDepth[]      output bit depth.
@param eTargetFrameRate[] output frame rate values.
@param u32Bitrate[]     output bitrate value in kbps. 0 < u32Bitrate <= API_MAX_BITRATE.
@param eGopHierarchy[]  output GOP hierarchy
@param eGopSize[]       output GOP sizes
@param eGopType[]       output GOP types
@param eBFrameNum[]     number of B frame used in a group
@param u32CpbDelay[]    output CPB delay value.
@return
- API_VEGA_BQB_RET_SUCCESS: Successful
- API_VEGA_BQB_RET_FAIL: Failed
*/
LIBVEGA_BQB_API API_VEGA_BQB_RET
VEGA_BQB_ENC_MakeInitParamX
(
        API_VEGA_BQB_INIT_PARAM_X_T    *pApiInitParam,
  const API_VEGA_BQB_RESOLUTION_E       eInputResolution,
  const API_VEGA_BQB_CHROMA_FORMAT_E    eInputChromaFmt,
  const API_VEGA_BQB_BIT_DEPTH_E        eInputBitDepth,
  const API_VEGA_BQB_FPS_E              eInputFrameRate,
  const uint32_t                        u32NumOfOutputs,
  const uint32_t                        u32OutputId[],
  const API_VEGA_BQB_HEVC_PROFILE_E     eProfile[],
  const API_VEGA_BQB_HEVC_LEVEL_E       eLevel[],
  const API_VEGA_BQB_HEVC_TIER_E        eTier[],
  const API_VEGA_BQB_RESOLUTION_E       eResolution[],
  const API_VEGA_BQB_CHROMA_FORMAT_E    eChromaFmt[],
  const API_VEGA_BQB_BIT_DEPTH_E        eBitDepth[],
  const API_VEGA_BQB_FPS_E              eTargetFrameRate[],
  const uint32_t                        u32Bitrate[],
  const API_VEGA_BQB_GOP_HIERARCHY_E    eGopHierarchy[],
  const API_VEGA_BQB_GOP_SIZE_E         eGopSize[],
  const API_VEGA_BQB_GOP_TYPE_E         eGopType[],
  const API_VEGA_BQB_B_FRAME_NUM_E      eBFrameNum[],
  const uint32_t                        u32CpbDelay[]
);
/**
@brief Creates a API_VEGA_BQB_INIT_PARAM_T structure for HEVC encode.

The \b VEGA_BQB_ENC_MakeInitParam function helps user to create a VEGA331X init parameter, which can be used to initialize the VEGA331X encoder.

@param pApiInitParam    pointer to init parameter to be set; provided by caller.
@param eProfile         profile value.
@param eLevel           level value
@param eTier            tier value.
@param eResolution      resolution value.
@param eChromaFmt       chroma format.
@param eBitDepth        bit depth.
@param eTargetFrameRate frame rate value.
@param u32Bitrate       bitrate value in kbps. 0 < u32Bitrate <= API_MAX_BITRATE.
@param u32CpbDelay      CPB delay value.
@return
- API_VEGA_BQB_RET_SUCCESS: Successful
- API_VEGA_BQB_RET_FAIL: Failed
*/
LIBVEGA_BQB_API API_VEGA_BQB_RET
VEGA_BQB_ENC_MakeInitParam
(
        API_VEGA_BQB_INIT_PARAM_T      *pApiInitParam,
  const API_VEGA_BQB_HEVC_PROFILE_E     eProfile,
  const API_VEGA_BQB_HEVC_LEVEL_E       eLevel,
  const API_VEGA_BQB_HEVC_TIER_E        eTier,
  const API_VEGA_BQB_RESOLUTION_E       eResolution,
  const API_VEGA_BQB_CHROMA_FORMAT_E    eChromaFmt,
  const API_VEGA_BQB_BIT_DEPTH_E        eBitDepth,
  const API_VEGA_BQB_FPS_E              eTargetFrameRate,
  const uint32_t                        u32Bitrate,
  const uint32_t                        u32CpbDelay
);

VEGA_BQB_ENC_MakeULLInitParam
(
pInitParam,
API_VEGA_BQB_HEVC_MAIN_PROFILE,
API_VEGA_BQB_HEVC_LEVEL_AUTO,
API_VEGA_BQB_HEVC_HIGH_TIER,
API_VEGA_BQB_RESOLUTION_3840x2160,
API_VEGA_BQB_CHROMA_FORMAT_420,
API_VEGA_BQB_BIT_DEPTH_8,
API_VEGA_BQB_FPS_60,
60000
);
#endif

                VEGA_BQB_ENC_MakeInitParam
                (
                        &ctx->init_paramsMACRO,
                        API_VEGA_BQB_HEVC_MAIN422_10_PROFILE,
                        API_VEGA_BQB_HEVC_LEVEL_41,
                        API_VEGA_BQB_HEVC_MAIN_TIER,
                        opts->codec.encodingResolution,
                        opts->codec.chromaFormat,
                        opts->codec.bitDepth,
                        opts->codec.fps,
                        opts->codec.bitrate_kbps,
                        270000
                );
                ctx->init_paramsMACRO.eOutputFmt = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

                VEGA_BQB_ENC_MakeULLInitParam
                (
                        &ctx->init_paramsMACROULL,
                        API_VEGA_BQB_HEVC_MAIN422_10_PROFILE,
                        API_VEGA_BQB_HEVC_LEVEL_AUTO,
                        API_VEGA_BQB_HEVC_MAIN_TIER,
                        opts->codec.encodingResolution,
                        opts->codec.chromaFormat,
                        opts->codec.bitDepth,
                        opts->codec.fps,
                        opts->codec.bitrate_kbps
                );
                ctx->init_paramsMACRO.eOutputFmt = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

#if INSERT_HDR
            pInitParam->tHevcParam.tVideoSignalType.bPresentFlag = true;
            pInitParam->tHevcParam.tVideoSignalType.bVideoFullRange = false;
            pInitParam->tHevcParam.tVideoSignalType.eVideoFormat = API_VEGA_BQB_VIDEO_FORMAT_UNSPECIFIED;
            pInitParam->tHevcParam.tVideoSignalType.tColorDesc.bPresentFlag = true;
            pInitParam->tHevcParam.tVideoSignalType.tColorDesc.eColorPrimaries = API_VEGA_BQB_COLOR_PRIMARY_BT2020;
            pInitParam->tHevcParam.tVideoSignalType.tColorDesc.eTransferCharacteristics = API_VEGA_BQB_TRANSFER_CHAR_SMPTE_ST_2084;
            pInitParam->tHevcParam.tVideoSignalType.tColorDesc.eMatrixCoeff = API_VEGA_BQB_MATRIX_COEFFS_BT2020NC;

            pInitParam->tHevcParam.tHdrConfig.bEnable = true;
            pInitParam->tHevcParam.tHdrConfig.u8LightContentLocation = 0;
            pInitParam->tHevcParam.tHdrConfig.u16MaxContentLightLevel = 10000;
            pInitParam->tHevcParam.tHdrConfig.u16MaxPictureAvgLightLevel = 4000;

            pInitParam->tHevcParam.tHdrConfig.u8AlternativeTransferCharacteristicsLocation = 1;
            pInitParam->tHevcParam.tHdrConfig.eAlternativeTransferCharacteristics = API_VEGA_BQB_TRANSFER_CHAR_BT2020_10;

            pInitParam->tHevcParam.tHdrConfig.u8MasteringDisplayColourVolumeLocation = 2;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesX0 = 13250;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesY0 = 34500;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesX1 = 7500;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesY1 = 3000;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesX2 = 34000;
            pInitParam->tHevcParam.tHdrConfig.u16DisplayPrimariesY2 = 16000;
            pInitParam->tHevcParam.tHdrConfig.u16WhitePointX = 15635;
            pInitParam->tHevcParam.tHdrConfig.u16WhitePointY = 16450;
            pInitParam->tHevcParam.tHdrConfig.u32MaxDisplayMasteringLuminance = 10000000;
            pInitParam->tHevcParam.tHdrConfig.u32MinDisplayMasteringLuminance = 500;
#endif

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

#if 1
                /* Register callback to insert SEI in VIF mode */
                encret = VEGA_BQB_ENC_RegisterPictureInfoCallback((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx,
                        callback__process_capture, opts);
                if (encret != API_VEGA_BQB_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to enc pictures info callback\n");
                }
                printf(MODULE_PREFIX "Registering Picture Info callback\n");
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

                /* Configure HANd process. We don't want HANC audio packets but we'll take everything else */
                ctx->ch_init_param.tAncWindowSetting.bHanc = false;     /* Enable HANC ancillary data processing (Audio packet) */
                ctx->ch_init_param.tAncWindowSetting.bPreVanc = true;
                ctx->ch_init_param.tAncWindowSetting.bPostVanc = true;

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
printf("Segfaulting to debug, gdb print this: ctx->ch_init_param or ctx->init_paramsMACRO, ctx->init_paramsMACROULL\n");
char *p = 0;
printf("%d\n", *p);
#endif
                printf(MODULE_PREFIX "Configuring Capture Interface\n");

                capret = VEGA3311_CAP_Config(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, &ctx->ch_init_param);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to cap config\n");
                        return -1;
                }

                printf(MODULE_PREFIX "Configuring ANC Interface\n");

                capret = VEGA3311_CAP_RegisterAncdCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, callback__anc_capture_cb_func, opts);
                if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                        fprintf(stderr, MODULE_PREFIX "ERROR: failed to anc register callback\n");
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
