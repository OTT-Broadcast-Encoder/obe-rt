/*****************************************************************************
 * statistics.c :
 *****************************************************************************
 * Copyright (C) 2020 LiveTimeNet Inc. All Rights Reserved.
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

#include "statistics.h"

static uint64_t counters[OBE_STATISTIC__MAX];

const char *obe_statistic_name(enum obe_statistic_e type)
{
    switch( type )
    {
        case OBE_STATISTIC_VIDEO_FRAMES: return "video_frames";
        case OBE_STATISTIC_VIDEO_BYTES: return "video_bytes";
        case OBE_STATISTIC_OUTPUT_BYTES: return "output_bytes";
        case OBE_STATISTIC_OUTPUT_DATAGRAMS: return "output_datagrams";
        case OBE_STATISTIC_OUTPUT_ERRORS: return "output_errors";
        default: return "undefined";
    }
}

void obe_statistics_reset(void)
{
    for( int type = OBE_STATISTIC__UNDEFINED + 1; type < OBE_STATISTIC__MAX; type++ )
        __atomic_store_n( &counters[type], 0, __ATOMIC_RELAXED );
}

void obe_statistic_set(enum obe_statistic_e type, uint64_t value)
{
    if( type > OBE_STATISTIC__UNDEFINED && type < OBE_STATISTIC__MAX )
        __atomic_store_n( &counters[type], value, __ATOMIC_RELAXED );
}

void obe_statistic_add(enum obe_statistic_e type, uint64_t value)
{
    if( type > OBE_STATISTIC__UNDEFINED && type < OBE_STATISTIC__MAX )
        __atomic_fetch_add( &counters[type], value, __ATOMIC_RELAXED );
}

uint64_t obe_statistic_get(enum obe_statistic_e type)
{
    if( type > OBE_STATISTIC__UNDEFINED && type < OBE_STATISTIC__MAX )
        return __atomic_load_n( &counters[type], __ATOMIC_RELAXED );
    return 0;
}

