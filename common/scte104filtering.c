#include "common.h"
#include "scte104filtering.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_PREFIX "[scte104filtering] "

#define LOCAL_DEBUG 0

struct scte104_filtering_syntax_s g_scte104_filtering_context;

void scte104filter_init(struct scte104_filtering_syntax_s *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

int scte104filter_count(struct scte104_filtering_syntax_s *ctx)
{
	return ctx->count;
}

/* In any scte35 encoder configuration we need to have atleast the default rule in place to allow
 * all messages to be forwarded to the single pid.
 * If nexessary, we'll override this later when we establish that multiple output SCTE35 streams
 * need to be used, in a more complex filter environment.
 */
int scte104filter_add_filter(struct scte104_filtering_syntax_s *ctx, uint16_t pid, int32_t AS_index, int32_t DPI_PID_index)
{
	if (!ctx)
		return -1; /* Error */

	if (ctx->count >= MAX_SCTE104_FILTERS) {
		return -1; /* Error */
	}

	ctx->array[ctx->count].pid = pid;
	ctx->array[ctx->count].AS_index = AS_index;
	ctx->array[ctx->count].DPI_PID_index = DPI_PID_index;
	ctx->array[ctx->count].messageCount = 0;

	ctx->count++;

	return 0; /* Success */
}

int scte104filter_lookup_scte35_pids(struct scte104_filtering_syntax_s *ctx, const struct klvanc_multiple_operation_message *msg, uint16_t **array, uint8_t *count)
{
	if ((!ctx) || (!msg) || (!array) || (!count))
		return -1; /* Error */

	int cnt = 0;
	uint16_t *arr = NULL;

	int append = 0;
	for (int i = 0; i < ctx->count; i++) {
		struct scte104_filter_s *f = &ctx->array[i];
		append = 0;

		if (f->AS_index == -1 /* all */) {
			if (f->DPI_PID_index == -1 /* all */) {
				append = 1; 
			} else
			if (f->DPI_PID_index == msg->DPI_PID_index) {
				append = 1;
			}
		} else
		if (f->AS_index == msg->AS_index) {
			if (f->DPI_PID_index == -1 /* all */) {
				append = 1; 
			} else
			if (f->DPI_PID_index == msg->DPI_PID_index) {
				append = 1;
			}
		}

		if (append) {
			arr = realloc(arr, ((cnt + 1) * sizeof(uint16_t)));
			if (!arr) {
				return -1;
			}

			arr[cnt] = f->pid;
			cnt++;
		}
	}
	
	*array = arr;
	*count = cnt;
	return 0; /* Success */
}

