#ifndef OBE_COMMON_FFMPEG_H
#define OBE_COMMON_FFMPEG_H

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

/* OBE keeps channel masks in its public structures. Convert only at the
 * libavutil boundary; FFmpeg 9 no longer accepts integer layout options. */
static inline int obe_set_channel_layout(void *obj, const char *name, uint64_t mask)
{
    AVChannelLayout layout = {AV_CHANNEL_ORDER_UNSPEC};
    int ret = av_channel_layout_from_mask(&layout, mask);
    if (ret >= 0)
        ret = av_opt_set_chlayout(obj, name, &layout, 0);
    av_channel_layout_uninit(&layout);
    return ret;
}

#endif
