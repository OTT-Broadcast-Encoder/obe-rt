/*****************************************************************************
 * statistics.h :
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

#ifndef STATISTICS_H
#define STATISTICS_H

#include <stdint.h>

enum obe_statistic_e
{
        OBE_STATISTIC__UNDEFINED = 0,
        OBE_STATISTIC_VIDEO_FRAMES,
        OBE_STATISTIC_VIDEO_BYTES,
        OBE_STATISTIC_OUTPUT_BYTES,
        OBE_STATISTIC_OUTPUT_DATAGRAMS,
        OBE_STATISTIC_OUTPUT_ERRORS,
        OBE_STATISTIC__MAX
};
const char *obe_statistic_name(enum obe_statistic_e type);

/* Process-wide counters; reset only before starting encoder/output threads. */
void     obe_statistics_reset(void);
void     obe_statistic_set(enum obe_statistic_e type, uint64_t value);
void     obe_statistic_add(enum obe_statistic_e type, uint64_t value);
uint64_t obe_statistic_get(enum obe_statistic_e type);

#endif /* STATISTICS_H */
