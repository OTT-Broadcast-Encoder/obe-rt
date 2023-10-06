#ifndef OBE_SCTE104FILTERING_H
#define OBE_SCTE104FILTERING_H

/* Helper logic that connects controller configuration syntax
 * to internal controller mapping state.
 *
 * These strictures track SCTE35 pid values and allow numberic AS/DPI values
 * to be assigned, which are then acted upon to decide which SCTE104 messages
 * get routed to shate SCTE35 streams, based on user configuration.
 */

#include <stdio.h>
#include <pthread.h>
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>

#define MULTIPLE_SCTE35_PIDS 1

/*
     set variable scte104.filter.add = pid = 56, AS_index = all, DPI_PID_index = 1
     set variable scte104.filter.add = pid = 57, AS_index = all, DPI_PID_index = 80
*/

#define MAX_SCTE104_FILTERS 64
struct scte104_filter_s
{
	uint16_t pid;			/* SCTE35 pid */
	int32_t  AS_index;      /* -1: all, else a single index value*/
	int32_t  DPI_PID_index; /* -1: all, else a single index value*/
	uint64_t messageCount;
};

struct scte104_filtering_syntax_s
{
	int count; /* Number of active filters */
	struct scte104_filter_s array[MAX_SCTE104_FILTERS];
};

extern struct scte104_filtering_syntax_s g_scte104_filtering_context;

void scte104filter_init(struct scte104_filtering_syntax_s *ctx);

/* return < 0 on error */
int scte104filter_add_filter(struct scte104_filtering_syntax_s *ctx, uint16_t pid, int32_t AS_index, int32_t DPI_PID_index);

/* For a given 104 multi operations message, build an array of each SCTE35 pid this messages should be routed to.
 * Caller is responsible for freeing the return array else memory leak occurs.
 * return < 0 on error
 */
int scte104filter_lookup_scte35_pids(struct scte104_filtering_syntax_s *ctx, const struct klvanc_multiple_operation_message *msg, uint16_t **array, uint8_t *count);

int scte104filter_count(struct scte104_filtering_syntax_s *ctx);

#endif /* OBE_SCTE104FILTERING_H */
