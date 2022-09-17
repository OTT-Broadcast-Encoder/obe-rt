#ifndef LTN_VEGA_H
#define LTN_VEGA_H

#include <apiVENC_types.h>
#include <apiVENC.h>
#include <VEGA3311_cap_types.h>
#include <VEGA3311_capture.h>
#include <VEGA_BQB_types.h>
#include <VEGA_BQB_encoder.h>
#include <VEGA_BQB_version.h>

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

struct obe_to_vega_video
{
    int progressive;            /* Boolean - Progressive or interlaced. */
    int obe_name;
    int width;                  /* Visual pixel width / height */
    int height;
    int vegaCaptureResolution;  /* SDK specific enum */
    int vegaEncodingResolution;
    int timebase_num;           /* Eg.  1001 */
    int timebase_den;           /* Eg. 60000 */
    API_VEGA_BQB_FPS_E vegaFramerate;          /* SDK specific enum */
};

const char *vega_lookupFrameType(API_VEGA_BQB_FRAME_TYPE_E type);
const char *lookupVegaSDILevelName(int v);
const char *lookupVegaPixelFormatName(int v);
const char *lookupVegaInputSourceName(int v);
const char *lookupVegaInputModeName(int v);
const char *lookupVegaBitDepthName(int v);
const char *lookupVegaEncodingChromaName(API_VEGA_BQB_CHROMA_FORMAT_E v);
const char *lookupVegaEncodingResolutionName(int v);
const char *lookupVegaAudioLayoutName(API_VEGA3311_CAP_AUDIO_LAYOUT_E v);

const struct obe_to_vega_video *lookupVegaCaptureResolution(int std, int framerate, int interlaced);
const struct obe_to_vega_video *lookupVegaStandardByResolution(int width, int height, int framerate);
int lookupVegaFramerate(int num, int den, API_VEGA_BQB_FPS_E *fps);

#endif /* LTN_VEGA_H */
