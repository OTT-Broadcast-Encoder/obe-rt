#ifndef OBE_FILTER_VIDEO_VAPOURSYNTH_VF_H
#define OBE_FILTER_VIDEO_VAPOURSYNTH_VF_H

#include <VapourSynth4.h>
#include <VSConstants4.h>
#include <VSHelper4.h>
#include <VSScript4.h>

#ifdef __cplusplus
extern "C" {
#endif

struct filter_vapoursynth_ctx;

int  filter_vapoursynth_alloc(struct filter_vapoursynth_ctx **ctx, obe_t *h);
int filter_vapoursynth_loaded(struct filter_vapoursynth_ctx *ctx);
void filter_vapoursynth_free(struct filter_vapoursynth_ctx *ctx);
int  filter_vapoursynth_process(struct filter_vapoursynth_ctx *ctx, obe_raw_frame_t *rf);

#ifdef __cplusplus
};
#endif

#endif /* OBE_FILTER_VIDEO_VAPOURSYNTH_VF_H */
