/* Standalone test: cc -std=c11 -Wall -Wextra -Werror -pthread -I. \
 * tests/statistics.c obe/statistics.c -o /tmp/obe-statistics-test */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "obe/statistics.h"

enum { THREADS = 4, ITERATIONS = 50000 };

static void *add_frames(void *unused)
{
    (void)unused;
    for( int i = 0; i < ITERATIONS; i++ )
    {
        obe_statistic_add( OBE_STATISTIC_VIDEO_FRAMES, 1 );
        obe_statistic_add( OBE_STATISTIC_VIDEO_BYTES, 1316 );
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    obe_statistics_reset();
    for( int i = 0; i < THREADS; i++ )
        assert( pthread_create( &threads[i], NULL, add_frames, NULL ) == 0 );
    uint64_t previous = 0;
    for( int i = 0; i < ITERATIONS; i++ )
    {
        uint64_t current = obe_statistic_get( OBE_STATISTIC_VIDEO_FRAMES );
        assert( current >= previous );
        previous = current;
    }
    for( int i = 0; i < THREADS; i++ )
        assert( pthread_join( threads[i], NULL ) == 0 );
    assert( obe_statistic_get( OBE_STATISTIC_VIDEO_FRAMES ) == THREADS * ITERATIONS );
    assert( obe_statistic_get( OBE_STATISTIC_VIDEO_BYTES ) == (uint64_t)THREADS * ITERATIONS * 1316 );

    obe_statistic_set( OBE_STATISTIC_OUTPUT_BYTES, UINT64_MAX - 1 );
    obe_statistic_add( OBE_STATISTIC_OUTPUT_BYTES, 1 );
    assert( obe_statistic_get( OBE_STATISTIC_OUTPUT_BYTES ) == UINT64_MAX );
    enum obe_statistic_e invalid[] = { -1, OBE_STATISTIC__UNDEFINED, OBE_STATISTIC__MAX };
    for( unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++ )
    {
        obe_statistic_set( invalid[i], 9 );
        obe_statistic_add( invalid[i], 1 );
        assert( obe_statistic_get( invalid[i] ) == 0 );
    }
    assert( obe_statistic_get( OBE_STATISTIC_OUTPUT_BYTES ) == UINT64_MAX );
    obe_statistics_reset();
    for( int i = OBE_STATISTIC__UNDEFINED + 1; i < OBE_STATISTIC__MAX; i++ )
        assert( obe_statistic_get( i ) == 0 );
    puts( "statistics: concurrent increments, unsigned range and reset passed" );
    return 0;
}
