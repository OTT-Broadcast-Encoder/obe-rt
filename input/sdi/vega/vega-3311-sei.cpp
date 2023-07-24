#if HAVE_VEGA3311_CAP_TYPES_H

/*****************************************************************************
 * vega-vanc.cpp: VANC processing from Vega encoding cards
 *****************************************************************************
 * Copyright (C) 2023 LTN
 *
 * Authors: Steven Toth <steven.toth@ltnglobal.com>
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

#define LOCAL_DEBUG 1

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define MODULE_PREFIX "[vega-sei]: "

void vega_sei_init(vega_ctx_t *ctx)
{
        pthread_mutex_init(&ctx->seiLock, NULL);
}

void vega_sei_lock(vega_ctx_t *ctx)
{
        pthread_mutex_lock(&ctx->seiLock);
}

void vega_sei_unlock(vega_ctx_t *ctx)
{
        pthread_mutex_unlock(&ctx->seiLock);
}

int vega_sei_append(vega_ctx_t *ctx, API_VEGA_BQB_SEI_PARAM_T *item)
{
        if (ctx->seiCount == API_MAX_SEI_NUM)
                return -1;

        vega_sei_lock(ctx);
        memcpy(&ctx->sei[ctx->seiCount], item, sizeof(*item));
        ctx->seiCount++;
        vega_sei_unlock(ctx);

        return 0; /* Success */
}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
