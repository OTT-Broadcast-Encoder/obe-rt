#include "common/common.h"
#include "common/bitstream.h"
#include "ltn_ws.h"
#include "analyze_fp.h"

#if LTN_WS_ENABLE
#include "/storage/dev/CRYSTALCC/cbtp/tavf/tavf.h"
#include "/storage/dev/CRYSTALCC/cbtp/tavf/tavf.cpp"
#endif

#define MODULE_PREFIX "[filter_analyze_fp]: "

struct filter_analyze_fp_ctx
{
#if LTN_WS_ENABLE
	TAVFProcessor *processor;
#endif
};

void filter_analyze_fp_free(struct filter_analyze_fp_ctx *ctx)
{
	if (!ctx)
		return;

#if LTN_WS_ENABLE
	if (ctx->processor) {
		delete ctx->processor;
		ctx->processor = NULL;
	}
#endif

	free(ctx);
}

int filter_analyze_fp_alloc(struct filter_analyze_fp_ctx **p)
{
	struct filter_analyze_fp_ctx *ctx = (filter_analyze_fp_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	*p = ctx;
	return 0;
}

int filter_analyze_fp_process(struct filter_analyze_fp_ctx *ctx, obe_raw_frame_t *rf)
{
	return 0; /* Disabled for performance testing */

	if (!ctx || !rf)
		return -1;

	if (rf->img.csp != PIX_FMT_YUV420P) {
		return -1; /* Unsupported input colorspace */
	}

	//shared_ptr<TAVFFingerPrint> processFrame(const uint8_t *data, size_t size, int64_t frameId = -1);
	//  memcpy(frame->data[0], rf->img.plane[0], c->height * c->width);
	//  memcpy(frame->data[1], rf->img.plane[1], (c->height * c->width) / 4);
	//  memcpy(frame->data[2], rf->img.plane[2], (c->height * c->width) / 4);
	//
#if LTN_WS_ENABLE
	if (!ctx->processor) {
		printf(MODULE_PREFIX "Allocating new processor(%d, %d, pfYUV420p)\n", rf->img.width, rf->img.height);
		ctx->processor = new TAVFProcessor(rf->img.width, rf->img.height, TAVFFrame::PixelFormat::pfYUV420p);
		if (!ctx)
			return -1;
	}
	size_t sizeBytes = rf->img.width * rf->img.height; /* Luma */
	sizeBytes += (rf->img.width * rf->img.height) / 4; /* Chroma U */
	sizeBytes += (rf->img.width * rf->img.height) / 4; /* Chroma V */

	auto fp = ctx->processor->processFrame(rf->img.plane[0], sizeBytes);
	if (fp) {
		char ts[64];
		obe_getTimestamp(ts, 0);
		printf(MODULE_PREFIX "%s fingerprint = %s\n", ts, fp->asString().c_str());
		ltn_ws_set_property_current_fingerprint(g_ltn_ws_handle, fp->asString().c_str());
	}
#endif

	return 0;
}
