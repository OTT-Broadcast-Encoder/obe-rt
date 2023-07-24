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

/* Craft a LTN SEI timing message for use by a lter video frame. */
void vega_sei_append_ltn_timing(vega_ctx_t *ctx)
{
        /* Create the SEI for the LTN latency tracking. */
        //img.u32SeiNum = 1;
        //img.bSeiPassThrough = true;

        API_VEGA_BQB_SEI_PARAM_T s;
        memset(&s, 0, sizeof(s));

        s.ePayloadLoc   = API_VEGA_BQB_SEI_PAYLOAD_LOC_PICTURE;
        s.ePayloadType  = API_VEGA_BQB_SEI_PAYLOAD_TYPE_USER_DATA_UNREGISTERED;
        s.u8PayloadSize = SEI_TIMESTAMP_PAYLOAD_LENGTH;

        struct timeval tv;
        gettimeofday(&tv, NULL);

        unsigned char *p = &s.u8PayloadData[0];

        if (sei_timestamp_init(p, SEI_TIMESTAMP_PAYLOAD_LENGTH) < 0) {
                /* This should never happen unless the vega SDK sei header shrinks drastically. */
                fprintf(stderr, MODULE_PREFIX "SEI space too small\n");
                return;
        }

        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 1, ctx->framecount);
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, tv.tv_sec);  /* Rx'd from hw/ */
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 3, tv.tv_usec);
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, tv.tv_sec);  /* Tx'd to codec */
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, tv.tv_usec);
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 0); /* time exit from compressor seconds/useconds. */
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 0); /* time exit from compressor seconds/useconds. */
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 8, 0); /* time transmit to udp seconds/useconds. */
        sei_timestamp_field_set(p, SEI_TIMESTAMP_PAYLOAD_LENGTH, 9, 0); /* time transmit to udp seconds/useconds. */

        if (vega_sei_append(ctx, &s) < 0) {
                fprintf(stderr, MODULE_PREFIX "Unable to add SEI timing message, skipping\n");
        }

}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
