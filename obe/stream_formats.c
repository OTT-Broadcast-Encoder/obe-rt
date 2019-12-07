/*****************************************************************************
 * stream_formats.c : Stream format type definitions and utility functions
 *****************************************************************************
 * Copyright (C) 2019 LiveTimeNet Inc. All Rights Reserved.
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 ******************************************************************************/

#include "stream_formats.h"

const char *stream_format_name(enum stream_formats_e id)
{
	switch(id) {
	case AUDIO_MP2: return "AUDIO_MP2";
	case VIDEO_HEVC_X265: return "VIDEO_HEVC_X265";
	default: return "UNDEFINED";
	}
}

