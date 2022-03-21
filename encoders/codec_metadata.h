#ifndef CODEC_METADATA_H
#define CODEC_METADATA_H

#include "common/common.h"

#include <stdint.h>
#include <sys/time.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Struct, single allocation guaranteed, no pointers.
 * Often passed to codecs as opaque side data, and returned
 * with compressed frames.
 */
struct opaque_ctx_s
{
	struct avfm_s avfm;
	struct avmetadata_s metadata;
};

struct opaque_ctx_s *codec_metadata_alloc();

void codec_metadata_free(struct opaque_ctx_s *md);
void codec_metadata_set_avfm(struct opaque_ctx_s *md, struct avfm_s *avfm);
void codec_metadata_set_avmetadata(struct opaque_ctx_s *md, struct avmetadata_s *metadata);

#if defined(__cplusplus)
};
#endif

#endif /* CODEC_METADATA_H */
