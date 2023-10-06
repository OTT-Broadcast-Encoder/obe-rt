/*****************************************************************************
 * obecli.h : Open Broadcast Encoder CLI headers
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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

#ifndef OBECLI_SHARED_H
#define OBECLI_SHARED_H

#include <common/common.h>

typedef struct
{
    obe_t *h;
    obe_input_t input;
    obe_input_program_t program;

    /* Configuration params from the command line configure these output streams.
     * before they're finally cloned into the obe_t struct as 'output_streams'.
     * See obe_setup_streams() for the cloning action.
     */
    int num_output_streams;
    obe_output_stream_t *output_streams;
    obe_mux_opts_t mux_opts;
    obe_output_opts_t output;
    int avc_profile;
} obecli_ctx_t;

extern obecli_ctx_t cli; 

#endif /* OBECLI_SHARED_H */
