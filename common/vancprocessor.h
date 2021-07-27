#ifndef OBE_VANCPROCESSOR_H
#define OBE_VANCPROCESSOR_H

/* Helper logic that is connected to each codec, designed to
 * take scte104 (or other) metadata attached to each video frame
 * and do 'useful' things with it.
 *
 * First use case:
 * SCTE104, we'll convert it to scte35 and pass it to the muxer.
 */

#include <stdio.h>
#include <pthread.h>
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>

struct vanc_processor_s
{
	struct klvanc_context_s *vh; /* Handle to the vanc library */

	int outputStreamId;          /* Needed for when we pass things to the muxer (SCTE35) */
	obe_queue_t *mux_queue;      /* Muxer object, SCTE35. */

	int lineNr;
	obe_coded_frame_t *cf;
};

int  vancprocessor_alloc(struct vanc_processor_s **ctx, obe_queue_t *mux_queue, int outputStreamId, obe_coded_frame_t *cf);
int  vancprocessor_write(struct vanc_processor_s *ctx, unsigned short arrayLengthWords, unsigned short *array, int lineNr);
void vancprocessor_free(struct vanc_processor_s *ctx);

#endif /* OBE_VANCPROCESSOR_H */
