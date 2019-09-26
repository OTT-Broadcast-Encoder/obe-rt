#ifndef OBE_FILTER_VIDEO_ANALYZE_FP_H
#define OBE_FILTER_VIDEO_ANALYZE_FP_H

#ifdef __cplusplus
extern "C" {
#endif

struct filter_analyze_fp_ctx;

int  filter_analyze_fp_alloc(struct filter_analyze_fp_ctx **ctx);
void filter_analyze_fp_free(struct filter_analyze_fp_ctx *ctx);
int  filter_analyze_fp_process(struct filter_analyze_fp_ctx *ctx, obe_raw_frame_t *rf);

#ifdef __cplusplus
};
#endif

#endif /* OBE_FILTER_VIDEO_ANALYZE_FP_H */
