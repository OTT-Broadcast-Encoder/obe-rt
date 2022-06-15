#if HAVE_VEGA3311_CAP_TYPES_H

/*****************************************************************************
 * vega-misc.cpp: Audio/Video from the VEGA encoder cards
 *****************************************************************************
 * Copyright (C) 2021 LTN
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
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

#include "vega-3311.h"

const char *lookupVegaEncodingResolutionName(int v)
{
        switch (v) {
        case API_VENC_RESOLUTION_720x480:           return "API_VENC_RESOLUTION_720x480";
        case API_VENC_RESOLUTION_720x576:           return "API_VENC_RESOLUTION_720x576";
        case API_VENC_RESOLUTION_1280x720:          return "API_VENC_RESOLUTION_1280x720";
        case API_VENC_RESOLUTION_1920x1080:         return "API_VENC_RESOLUTION_1920x1080";
        case API_VENC_RESOLUTION_3840x2160:         return "API_VENC_RESOLUTION_3840x2160";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaChromaName(int v)
{
        switch (v) {
        case API_VENC_CHROMA_FORMAT_MONO:           return "API_VENC_CHROMA_FORMAT_MONO";
        case API_VENC_CHROMA_FORMAT_420:            return "API_VENC_CHROMA_FORMAT_420";
        case API_VENC_CHROMA_FORMAT_422:            return "API_VENC_CHROMA_FORMAT_422";
        case API_VENC_CHROMA_FORMAT_422_TO_420:     return "API_VENC_CHROMA_FORMAT_422_TO_420";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaBitDepthName(int v)
{
        switch (v) {
        case API_VENC_BIT_DEPTH_8:                  return "API_VENC_BIT_DEPTH_8";
        case API_VENC_BIT_DEPTH_10:                 return "API_VENC_BIT_DEPTH_10";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaInputModeName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI:  return "API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI";
        case API_VEGA3311_CAP_INPUT_MODE_4CHN:      return "API_VEGA3311_CAP_INPUT_MODE_4CHN";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaInputSourceName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_INPUT_SOURCE_SDI:         return "API_VEGA3311_CAP_INPUT_SOURCE_SDI";
        case API_VEGA3311_CAP_INPUT_SOURCE_HDMI:        return "API_VEGA3311_CAP_INPUT_SOURCE_HDMI";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaSDILevelName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_SDI_LEVEL_A:              return "API_VEGA3311_CAP_SDI_LEVEL_A";
        case API_VEGA3311_CAP_SDI_LEVEL_B:              return "API_VEGA3311_CAP_SDI_LEVEL_B";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaPixelFormatName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_IMAGE_FORMAT_P210:        return "API_VEGA3311_CAP_IMAGE_FORMAT_P210";
        case API_VEGA3311_CAP_IMAGE_FORMAT_P010:        return "API_VEGA3311_CAP_IMAGE_FORMAT_P010";
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV12:        return "API_VEGA3311_CAP_IMAGE_FORMAT_NV12";
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV16:        return "API_VEGA3311_CAP_IMAGE_FORMAT_NV16";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV12:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YV12";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV16:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YV16";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YUY2:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YUY2";
        case API_VEGA3311_CAP_IMAGE_FORMAT_V210:        return "API_VEGA3311_CAP_IMAGE_FORMAT_V210";
        default:                                        return "UNDEFINED";
        }
}

const char *vega_lookupFrameType(API_VEGA_BQB_FRAME_TYPE_E type)
{
        switch (type) {
        case API_VEGA_BQB_FRAME_TYPE_I: return "I";
        case API_VEGA_BQB_FRAME_TYPE_P: return "P";
        case API_VEGA_BQB_FRAME_TYPE_B: return "B";
        default:
                return "U";
        }
}

const static struct obe_to_vega_video video_format_tab[] =
{
    { 0, INPUT_VIDEO_FORMAT_PAL,          720,  576,  API_VEGA3311_CAP_RESOLUTION_720x576,   API_VENC_RESOLUTION_720x576,      1,    25, API_VEGA_BQB_FPS_25 },
    { 0, INPUT_VIDEO_FORMAT_NTSC,         720,  480,  API_VEGA3311_CAP_RESOLUTION_720x480,   API_VENC_RESOLUTION_720x480,   1001, 30000, API_VEGA_BQB_FPS_29_97 },
    //
    { 1, INPUT_VIDEO_FORMAT_720P_50,     1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_720P_5994,   1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1001, 60000, API_VEGA_BQB_FPS_59_94 },
    { 1, INPUT_VIDEO_FORMAT_720P_60,     1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1000, 60000, API_VEGA_BQB_FPS_60 },
    //
    { 0, INPUT_VIDEO_FORMAT_1080I_50,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 0, INPUT_VIDEO_FORMAT_1080I_5994,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1001, 60000, API_VEGA_BQB_FPS_59_94 },
    //
    { 1, INPUT_VIDEO_FORMAT_1080P_24,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 24000, API_VEGA_BQB_FPS_24 },
    { 1, INPUT_VIDEO_FORMAT_1080P_25,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 25000, API_VEGA_BQB_FPS_25 },
    { 1, INPUT_VIDEO_FORMAT_1080P_2997,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 30000, API_VEGA_BQB_FPS_29_97 },
    { 1, INPUT_VIDEO_FORMAT_1080P_30,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 30000, API_VEGA_BQB_FPS_30 },
    { 1, INPUT_VIDEO_FORMAT_1080P_50,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_1080P_5994,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1001, 60000, API_VEGA_BQB_FPS_59_94 },
    { 1, INPUT_VIDEO_FORMAT_1080P_60,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 60000, API_VEGA_BQB_FPS_60 },
    //
    { 1, INPUT_VIDEO_FORMAT_2160P_25,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_1920x1080, 1000, 25000, API_VEGA_BQB_FPS_25 },
    { 1, INPUT_VIDEO_FORMAT_2160P_2997,  3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_1920x1080, 1001, 30000, API_VEGA_BQB_FPS_29_97 },
    { 1, INPUT_VIDEO_FORMAT_2160P_30,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_1920x1080, 1000, 30000, API_VEGA_BQB_FPS_30 },
    { 1, INPUT_VIDEO_FORMAT_2160P_50,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_1920x1080, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_2160P_60,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_1920x1080, 1000, 60000, API_VEGA_BQB_FPS_60 },
};

/* For a given vegas format and framerate, return a record with translations into OBE speak. */
const struct obe_to_vega_video *lookupVegaCaptureResolution(int std, int framerate, int interlaced)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->vegaCaptureResolution == std && fmt->vegaFramerate == framerate && fmt->progressive == !interlaced) {
			return fmt;
		}
	}

	return NULL;
}

const struct obe_to_vega_video *lookupVegaStandardByResolution(int width, int height, int framerate)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->width == width && fmt->height == height && fmt->vegaFramerate == framerate) {
			return fmt;
		}
	}

	return NULL;
}

int lookupVegaFramerate(int num, int den, API_VEGA_BQB_FPS_E *fps)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->timebase_num == num && fmt->timebase_den == den) {
			*fps = fmt->vegaFramerate;
                        return 0; /* Success */
		}
	}

	return -1; /* Error */
}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
