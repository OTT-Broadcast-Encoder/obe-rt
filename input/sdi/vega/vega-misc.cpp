#if HAVE_VEGA330X_H

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

/* include vega330x_encoder */
#include <VEGA330X_types.h>
#include <VEGA330X_encoder.h>
#include <vega330x_config.h>

/* include sdk/vega_api */
#include <VEGA3301_cap_types.h>
#include <VEGA3301_capture.h>

const char *lookupVegaEncodingResolutionName(int v)
{
        switch (v) {
        case API_VEGA330X_RESOLUTION_720x480:           return "API_VEGA330X_RESOLUTION_720x480";
        case API_VEGA330X_RESOLUTION_720x576:           return "API_VEGA330X_RESOLUTION_720x576";
        case API_VEGA330X_RESOLUTION_1280x720:          return "API_VEGA330X_RESOLUTION_1280x720";
        case API_VEGA330X_RESOLUTION_1920x1080:         return "API_VEGA330X_RESOLUTION_1920x1080";
        case API_VEGA330X_RESOLUTION_3840x2160:         return "API_VEGA330X_RESOLUTION_3840x2160";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaChromaName(int v)
{
        switch (v) {
        case API_VEGA330X_CHROMA_FORMAT_MONO:           return "API_VEGA330X_CHROMA_FORMAT_MONO";
        case API_VEGA330X_CHROMA_FORMAT_420:            return "API_VEGA330X_CHROMA_FORMAT_420";
        case API_VEGA330X_CHROMA_FORMAT_422:            return "API_VEGA330X_CHROMA_FORMAT_422";
        case API_VEGA330X_CHROMA_FORMAT_422_TO_420:     return "API_VEGA330X_CHROMA_FORMAT_422_TO_420";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaBitDepthName(int v)
{
        switch (v) {
        case API_VEGA330X_BIT_DEPTH_8:                  return "API_VEGA330X_BIT_DEPTH_8";
        case API_VEGA330X_BIT_DEPTH_10:                 return "API_VEGA330X_BIT_DEPTH_10";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaInputModeName(int v)
{
        switch (v) {
        case API_VEGA3301_CAP_INPUT_MODE_1CHN_QFHD:     return "API_VEGA3301_CAP_INPUT_MODE_1CHN_QFHD";
        case API_VEGA3301_CAP_INPUT_MODE_4CHN_FHD:      return "API_VEGA3301_CAP_INPUT_MODE_4CHN_FHD";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaInputSourceName(int v)
{
        switch (v) {
        case API_VEGA3301_CAP_INPUT_SOURCE_SDI:         return "API_VEGA3301_CAP_INPUT_SOURCE_SDI";
        case API_VEGA3301_CAP_INPUT_SOURCE_HDMI:        return "API_VEGA3301_CAP_INPUT_SOURCE_HDMI";
        case API_VEGA3301_CAP_INPUT_SOURCE_DP2:         return "API_VEGA3301_CAP_INPUT_SOURCE_DP2";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaSDILevelName(int v)
{
        switch (v) {
        case API_VEGA3301_CAP_SDI_LEVEL_A:              return "API_VEGA3301_CAP_SDI_LEVEL_A";
        case API_VEGA3301_CAP_SDI_LEVEL_B:              return "API_VEGA3301_CAP_SDI_LEVEL_B";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaPixelFormatName(int v)
{
        switch (v) {
        case API_VEGA3301_CAP_IMAGE_FORMAT_P210:        return "API_VEGA3301_CAP_IMAGE_FORMAT_P210";
        case API_VEGA3301_CAP_IMAGE_FORMAT_P010:        return "API_VEGA3301_CAP_IMAGE_FORMAT_P010";
        case API_VEGA3301_CAP_IMAGE_FORMAT_NV12:        return "API_VEGA3301_CAP_IMAGE_FORMAT_NV12";
        case API_VEGA3301_CAP_IMAGE_FORMAT_NV16:        return "API_VEGA3301_CAP_IMAGE_FORMAT_NV16";
        case API_VEGA3301_CAP_IMAGE_FORMAT_YV12:        return "API_VEGA3301_CAP_IMAGE_FORMAT_YV12";
        case API_VEGA3301_CAP_IMAGE_FORMAT_I420:        return "API_VEGA3301_CAP_IMAGE_FORMAT_I420";
        case API_VEGA3301_CAP_IMAGE_FORMAT_YV16:        return "API_VEGA3301_CAP_IMAGE_FORMAT_YV16";
        case API_VEGA3301_CAP_IMAGE_FORMAT_YUY2:        return "API_VEGA3301_CAP_IMAGE_FORMAT_YUY2";
        default:                                        return "UNDEFINED";
        }
}

const char *vega_lookupFrameType(API_VEGA330X_FRAME_TYPE_E type)
{
        switch (type) {
        case API_VEGA330X_FRAME_TYPE_I: return "I";
        case API_VEGA330X_FRAME_TYPE_P: return "P";
        case API_VEGA330X_FRAME_TYPE_B: return "B";
        default:
                return "U";
        }
}

#endif /* #if HAVE_VEGA330X_H */
