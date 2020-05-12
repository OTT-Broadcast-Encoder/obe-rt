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

#include <stdio.h>
#include "statistics.h"

static char lbl[64]; /* in non-typical use cases. */
const char *obe_statistic_name(enum obe_statistic_e type)
{
	switch (type) {
	default:
		{
		sprintf(lbl,  "UNDEFINED id %d", type);
		return lbl;
		}
	}
}

void obe_statistic_set(enum obe_statistic_e type, int64_t value)
{
	if ((type >= OBE_STATISTIC__MAX) || (type == OBE_STATISTIC__UNDEFINED))
		return;
}

int64_t obe_statistic_get(enum obe_statistic_e type)
{
	if ((type >= OBE_STATISTIC__MAX) || (type == OBE_STATISTIC__UNDEFINED))
		return -1;

	return 0;
}

