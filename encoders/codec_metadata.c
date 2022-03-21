#include <stdio.h>

#include "encoders/codec_metadata.h"

struct opaque_ctx_s *codec_metadata_alloc()
{
    return calloc(1, sizeof(struct opaque_ctx_s));
}

void codec_metadata_free(struct opaque_ctx_s *md)
{
    free(md);
}

void codec_metadata_set_avfm(struct opaque_ctx_s *md, struct avfm_s *avfm)
{
    memcpy(&md->avfm, avfm, sizeof(struct avfm_s));
}

void codec_metadata_set_avmetadata(struct opaque_ctx_s *md, struct avmetadata_s *metadata)
{
    avmetadata_clone(&md->metadata, metadata);
}
