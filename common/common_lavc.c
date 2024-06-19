/*****************************************************************************
 * lavc.c: libavcodec common functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 * Copyright (C) 2019 LTN
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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
 *****************************************************************************/

#include "common/common.h"
#include <libavcodec/avcodec.h>

static void _buffer_default_free(void *opaque, uint8_t *data)
{
//	printf("%s() %p\n", __func__, opaque);
}

int obe_get_buffer2(AVCodecContext *codec, AVFrame *pic, int flags)
{
	int w = codec->width;
	int h = codec->height;
	int stride[4];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
	avcodec_align_dimensions2(codec, &w, &h, stride);
#pragma GCC diagnostic pop

	pic->linesize[0] = w * 2;
	pic->linesize[1] = pic->linesize[0] / 4;
	pic->linesize[2] = pic->linesize[0] / 4;

	/* Only EDGE_EMU codecs are used
	 * Allocate an extra line so that SIMD can modify the entire stride for every active line */
	if (av_image_alloc(pic->data, pic->linesize, w, h, codec->pix_fmt, 32) < 0) {
		return -1;
	}

	pic->buf[0] = av_buffer_create(pic->data[0], pic->linesize[0] * h, _buffer_default_free, NULL, 0);
	pic->buf[1] = av_buffer_create(pic->data[1], pic->linesize[1] * h / 4, _buffer_default_free, NULL, 0);
	pic->buf[2] = av_buffer_create(pic->data[2], pic->linesize[2] * h / 4, _buffer_default_free, NULL, 0);

	pic->reordered_opaque = codec->reordered_opaque;
	pic->pts = AV_NOPTS_VALUE;

	return 0;
}

