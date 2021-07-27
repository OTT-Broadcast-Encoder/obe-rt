#include "common.h"
#include "vancprocessor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_PREFIX "[vancprocessor] "

#define LOCAL_DEBUG 0

static int transmit_scte35_section_to_muxer(struct vanc_processor_s *ctx, uint8_t *section, uint32_t section_length)
{
	if (ctx->outputStreamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(ctx->outputStreamId, section_length);
	if (!coded_frame) {
		return -1;
	}

	coded_frame->pts                      = ctx->cf->pts;
	coded_frame->real_pts                 = ctx->cf->real_pts;
	coded_frame->real_dts                 = ctx->cf->real_dts;
	coded_frame->cpb_initial_arrival_time = ctx->cf->cpb_initial_arrival_time;
	coded_frame->cpb_final_arrival_time   = ctx->cf->cpb_final_arrival_time;
	coded_frame->random_access            = 1;
	memcpy(coded_frame->data, section, section_length);

        const char *s = obe_ascii_datetime();

	/* TODO: TOne this down in future builds */
        printf(MODULE_PREFIX "%s - Sending SCTE35 with PCR %" PRIi64 " / PTS %" PRIi64 " / Mux-PTS is %" PRIi64 "\n",
                s,
                coded_frame->real_pts,
                coded_frame->real_pts / 300,
                (coded_frame->real_pts / 300) + (10 * 90000));

        printf(MODULE_PREFIX "%s -                                   / DTS %" PRIi64 " / Mux-DTS is %" PRIi64 "\n",
                s,
                coded_frame->real_dts / 300,
                (coded_frame->real_dts / 300) + (10 * 90000));

        free((char *)s);

	add_to_queue(ctx->mux_queue, coded_frame);

	return 0;
}

static int vancprocessor_cb_SCTE_104(void *callback_context, struct klvanc_context_s *vh, struct klvanc_packet_scte_104_s *pkt)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif

	struct vanc_processor_s *ctx = (struct vanc_processor_s *)callback_context;

	if (klvanc_packetType1(&pkt->hdr)) {
		/* Silently discard type 1 SCTE104 packets, as per SMPTE 291 section 6.3 */
		return 0;
	}

	struct klvanc_single_operation_message *m = &pkt->so_msg;

	if (m->opID == 0xFFFF /* Multiple Operation Message */) {
		struct splice_entries results;
		/* Note, we add 10 second to the PTS to compensate for TS_START added by libmpegts */
		int r = scte35_generate_from_scte104(pkt, &results,
						     (ctx->cf->real_pts / 300) + (10 * 90000));
		if (r != 0) {
			fprintf(stderr, "Generation of SCTE-35 sections failed\n");
		}

		for (size_t i = 0; i < results.num_splices; i++) {
                        /* Send the message right to the muxer, which isn't frame accurate. */
			transmit_scte35_section_to_muxer(ctx, results.splice_entry[i],
							 results.splice_size[i]);
			free(results.splice_entry[i]);
		}
	} else {
		/* Unsupported single_operation_message type */
		fprintf(stderr, MODULE_PREFIX "Unsupported single_operation SCTE35 message type, ignoring.\n");
	}

	return 0;
}

static struct klvanc_callbacks_s callbacks =
{
	.scte_104 = vancprocessor_cb_SCTE_104,
};

int vancprocessor_alloc(struct vanc_processor_s **p, obe_queue_t *mux_queue, int outputStreamId, obe_coded_frame_t *cf)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif

	*p = NULL;

	struct vanc_processor_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	
	if (klvanc_context_create(&ctx->vh) < 0) {
		fprintf(stderr, MODULE_PREFIX "Error initializing VANC library context\n");
		free(ctx);
		return -1;
	}

	ctx->vh->verbose = 0;
	ctx->vh->callbacks = &callbacks;
	ctx->vh->callback_context = ctx;
	ctx->vh->allow_bad_checksums = 1;

	ctx->mux_queue = mux_queue;
	ctx->outputStreamId = outputStreamId;
	ctx->cf = cf;

	*p = ctx;

	return 0;
}

void vancprocessor_free(struct vanc_processor_s *ctx)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif
	if (ctx->vh) {
		klvanc_context_destroy(ctx->vh);
		ctx->vh = NULL;
	}

	free(ctx);
}

int vancprocessor_write(struct vanc_processor_s *ctx, unsigned short arrayLengthWords, unsigned short *array, int lineNr)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s() lineNr %d, %d words\n", __func__, lineNr, arrayLengthWords);
#endif
	if (!ctx || !array || arrayLengthWords <= 0)
		return -1;

	ctx->lineNr = lineNr;

	int ret = klvanc_packet_parse(ctx->vh, lineNr, array, arrayLengthWords);
	if (ret != 0) {
		fprintf(stderr, MODULE_PREFIX "write returns %d\n", ret);
	}

	return 0;
}
