/*****************************************************************************
 * udp.c : UDP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Large Portions of this code originate from FFmpeg
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

#include "common/common.h"
#include "common/network/network.h"
#include "output/output.h"
#include "udp.h"
#include <libltntstools/ltntstools.h>

typedef struct
{
    char hostname[1024];
    int port;
    int ttl;
    int miface;
    int buffer_size;
    int reuse_socket;
    int is_connected;

    int udp_fd;
    int is_multicast;
    int local_port;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;

    time_t bps_last;
    void *throughputHandle;
} obe_udp_ctx;

int g_udp_output_bps = 0;

static int udp_set_multicast_opts( int sockfd, obe_udp_ctx *s )
{
    struct sockaddr *addr = (struct sockaddr *)&s->dest_addr;

    if( addr->sa_family == AF_INET )
    {
#ifdef IP_MULTICAST_TTL
        if( setsockopt( sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &s->ttl, sizeof(s->ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv4 multicast\n" );
            return -1;
        }
#endif

#ifdef IP_MULTICAST_IF
        struct ip_mreqn req = { .imr_ifindex = s->miface };
        if( setsockopt( sockfd, IPPROTO_IP, IP_MULTICAST_IF, &req, sizeof(req) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup multicast interface\n" );
            return -1;
        }
#endif
    }

#ifdef IPPROTO_IPV6
    if( addr->sa_family == AF_INET6 )
    {
#ifdef IPV6_MULTICAST_HOPS
        if( setsockopt( sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &s->ttl, sizeof(s->ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv6 multicast\n" );
            return -1;
        }
#endif
#ifdef IPV6_MULTICAST_IF
        if( setsockopt( sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &s->miface, sizeof(s->miface) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv6 multicast interface\n" );
            return -1;
        }
#endif
    }
#endif
    return 0;
}

static struct addrinfo* udp_resolve_host( const char *hostname, int port, int type, int family, int flags )
{
    struct addrinfo hints, *res = 0;
    int error;
    char sport[16];
    const char *node = 0, *service = "0";

    if( port > 0 )
    {
        snprintf( sport, sizeof(sport), "%d", port );
        service = sport;
    }
    if( (hostname) && (hostname[0] != '\0') && (hostname[0] != '?') )
        node = hostname;

    memset( &hints, 0, sizeof(hints) );
    hints.ai_socktype = type;
    hints.ai_family   = family;
    hints.ai_flags    = flags;
    if( (error = getaddrinfo( node, service, &hints, &res )) )
    {
        res = NULL;
        fprintf( stderr, "[udp] error: %s \n", gai_strerror( error ) );
    }

    return res;
}

static int udp_set_url( struct sockaddr_storage *addr, const char *hostname, int port )
{
    struct addrinfo *res0;
    int addr_len;

    res0 = udp_resolve_host( hostname, port, SOCK_DGRAM, AF_UNSPEC, 0 );
    if( res0 == 0 )
        return -1;
    memcpy( addr, res0->ai_addr, res0->ai_addrlen );
    addr_len = res0->ai_addrlen;
    freeaddrinfo( res0 );

    return addr_len;
}

static int udp_socket_create( obe_udp_ctx *s, struct sockaddr_storage *addr, int *addr_len )
{
    int udp_fd = -1;
    struct addrinfo *res0 = NULL, *res = NULL;
    int family = AF_UNSPEC;

    if( ((struct sockaddr *) &s->dest_addr)->sa_family )
        family = ((struct sockaddr *) &s->dest_addr)->sa_family;
    res0 = udp_resolve_host( 0, s->local_port, SOCK_DGRAM, family, AI_PASSIVE );
    if( res0 == 0 )
        goto fail;
    for( res = res0; res; res=res->ai_next )
    {
        udp_fd = socket( res->ai_family, SOCK_DGRAM, 0 );
        if( udp_fd > 0 )
            break;
        // TODO error
    }

    if( udp_fd < 0 )
        goto fail;

    memcpy( addr, res->ai_addr, res->ai_addrlen );
    *addr_len = res->ai_addrlen;

    freeaddrinfo( res0 );

    return udp_fd;

 fail:
    if( udp_fd >= 0 )
        close( udp_fd );
    if( res0 )
        freeaddrinfo( res0 );
    return -1;
}

static int udp_port( struct sockaddr_storage *addr, int addr_len )
{
    char sbuf[sizeof(int)*3+1];

    if( getnameinfo( (struct sockaddr *)addr, addr_len, NULL, 0, sbuf, sizeof(sbuf), NI_NUMERICSERV ) != 0 )
    {
        fprintf( stderr, "[udp]: getnameinfo failed \n" );
        return -1;
    }

    return strtol( sbuf, NULL, 10 );
}

/**
 * If no filename is given to av_open_input_file because you want to
 * get the local port first, then you must call this function to set
 * the remote server address.
 *
 * url syntax: udp://host:port[?option=val...]
 * option: 'ttl=n'       : set the ttl value (for multicast only)
 *         'localport=n' : set the local port
 *         'pkt_size=n'  : set max packet size
 *         'reuse=1'     : enable reusing the socket
 *
 * @param h media file context
 * @param uri of the remote server
 * @return zero if no error.
 */
static int udp_set_remote_url( obe_udp_ctx *s )
{
    /* set the destination address */
    s->dest_addr_len = udp_set_url( &s->dest_addr, s->hostname, s->port );
    if( s->dest_addr_len < 0 )
        return -1;

    s->is_multicast = is_multicast_address( (struct sockaddr*) &s->dest_addr );

    return 0;
}

void udp_populate_opts( obe_udp_opts_t *udp_opts, char *uri )
{
    char buf[256];
    const char *p = strchr( uri, '?' );

    memset( udp_opts, 0, sizeof(*udp_opts) );

    if( p )
    {
        if( av_find_info_tag( buf, sizeof(buf), "reuse", p ) )
        {
            const char *endptr = NULL;
            udp_opts->reuse_socket = strtol( buf, (char **)&endptr, 10 );
            /* assume if no digits were found it is a request to enable it */
            if( buf == endptr )
                udp_opts->reuse_socket = 1;
        }
        if( av_find_info_tag( buf, sizeof(buf), "ttl", p ) )
            udp_opts->ttl = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "localport", p ) )
            udp_opts->local_port = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "buffer_size", p ) )
            udp_opts->buffer_size = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "miface", p ) )
            udp_opts->miface = if_nametoindex( buf );
    }

    /* fill the dest addr */
    av_url_split( NULL, 0, NULL, 0, udp_opts->hostname, sizeof(udp_opts->hostname), &udp_opts->port, NULL, 0, uri );
}

int udp_open( hnd_t *p_handle, obe_udp_opts_t *udp_opts )
{
    int udp_fd = -1, tmp, bind_ret = -1;
    struct sockaddr_storage my_addr;
    int len;

    obe_udp_ctx *s = calloc( 1, sizeof(*s) );
    *p_handle = NULL;
    if( !s )
        return -1;

    strncpy( s->hostname, udp_opts->hostname, sizeof(s->hostname) );
    s->port = udp_opts->port;
    s->local_port = udp_opts->local_port;
    s->reuse_socket = udp_opts->reuse_socket;
    s->ttl = udp_opts->ttl;
    s->buffer_size = udp_opts->buffer_size;
    s->miface = udp_opts->miface;

    if( udp_set_remote_url( s ) < 0 )
        goto fail;

    udp_fd = udp_socket_create( s, &my_addr, &len );
    if( udp_fd < 0 )
        goto fail;

    if( s->reuse_socket || s->is_multicast )
    {
        s->reuse_socket = 1;
        if( setsockopt( udp_fd, SOL_SOCKET, SO_REUSEADDR, &(s->reuse_socket), sizeof(s->reuse_socket) ) != 0)
            goto fail;
    }

    /* bind to the local address if not multicast or if the multicast
     * bind failed */
    if( bind_ret < 0 && bind( udp_fd, (struct sockaddr *)&my_addr, len ) < 0 )
        goto fail;

    len = sizeof(my_addr);
    getsockname( udp_fd, (struct sockaddr *)&my_addr, (socklen_t *) &len );
    s->local_port = udp_port( &my_addr, len );

    /* set output multicast ttl */
    if( s->is_multicast && udp_set_multicast_opts( udp_fd, s ) < 0 )
        goto fail;

    /* limit the tx buf size to limit latency */
    tmp = s->buffer_size;
#if defined(__APPLE__)
    /* Mac goes nuts if the default value of 0, s->buffer_size, is used. */
    if (s->buffer_size) {
        if( setsockopt( udp_fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp) ) < 0 )
            goto fail;
    }
#else
    /* Linux */
    if( setsockopt( udp_fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp) ) < 0 )
        goto fail;
#endif

    if( s->is_connected && connect( udp_fd, (struct sockaddr *)&s->dest_addr, s->dest_addr_len ) )
        goto fail;

    /* Allocate a hires throughput timer for measuring accurate bitrates.
     * 4000 writes per second as an upper ballpark, beyond this the solution
     * self adapts by allocating more slots. Calculate for 40mbps, a reasonable
     * default.
     */
    throughput_hires_alloc(&s->throughputHandle, ((40 * 1e6) / 8 ) / 1316);

    s->udp_fd = udp_fd;
    *p_handle = s;
    return 0;

 fail:
    if( udp_fd >= 0 )
        close( udp_fd );

    free( s );
    return -1;
}

#include <encoders/video/sei-timestamp.h>

int udp_write( hnd_t handle, uint8_t *buf, int size )
{
    obe_udp_ctx *s = handle;
    int ret;

    /* Measure throughput in bits per second. Store this bitrate with now (NULL). */
    throughput_hires_write_i64(s->throughputHandle, 0, size * 8, NULL);

    /* If its been a second size we last ran the bitrate calculate, run it again. */
    time_t now;
    time(&now);
    if (now != s->bps_last) {
        s->bps_last = now;
        g_udp_output_bps = throughput_hires_sumtotal_i64(s->throughputHandle, 0, NULL, NULL);

        /* Purge data older than 2 seconds */
        throughput_hires_expire(s->throughputHandle, NULL);
    }

    if (!s->is_connected) {
        if (g_sei_timestamping > 1) {
            while (size == 1316) {
                //printf("%s() %d bytes\n", __func__, size);
                int offset = ltn_uuid_find(buf, 1316);
                if (offset < 0)
                    break;

                struct timeval now;
                gettimeofday(&now, NULL);

                if (((offset % 188) + SEI_TIMESTAMP_PAYLOAD_LENGTH) < 188) {
                    /* Ensure we don't pass over the end of the TS packet into the beginning of
                     * the next packet.
                     */
                    if (sei_timestamp_field_set(buf + offset, size - offset, 8, now.tv_sec) >= 0) {
                        sei_timestamp_field_set(buf +  offset, size - offset, 9, now.tv_usec);
                    }
                }

                if (g_sei_timestamping > 2) {
                    sei_timestamp_hexdump(buf + offset, size - offset);
                }
                break;
            }
        } /* (g_sei_timestamping) */
        ret = sendto( s->udp_fd, buf, size, 0, (struct sockaddr *)&s->dest_addr, s->dest_addr_len );
    } else {
        /* !s->is_connected */
        ret = send( s->udp_fd, buf, size, 0 );
    }

    if( ret < 0 )
    {
        syslog( LOG_WARNING, "UDP packet failed to send \n" );
        return -1;
    }

    return size;
}

void udp_close( hnd_t handle )
{
    obe_udp_ctx *s = handle;

    close( s->udp_fd );
    throughput_hires_free(s->throughputHandle);
    free( s );
}
