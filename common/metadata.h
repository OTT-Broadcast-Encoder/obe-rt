#ifndef OBE_METADATA_H
#define OBE_METADATA_H

#include <stdio.h>
#include <pthread.h>

enum avmetadata_item_type_e { AVMETADATA_UNDEFINED, AVMETADATA_SECTION_SCTE35, AVMETADATA_VANC_SCTE104 };
struct avmetadata_item_s
{
	enum  avmetadata_item_type_e item_type;
	int   dataLengthAlloc;
	int   dataLengthBytes;
	unsigned char *data;

	/* SCTE35 */
	/* We need this for the codec to be able to hand SCTE to the muxer, only the decklink device knows this. */
	int outputStreamId;

	/* VANC */
	int lineNr;
};

struct avmetadata_s
{
#define MAX_RAW_FRAME_METADATA_ITEMS 16
	struct avmetadata_item_s *array[MAX_RAW_FRAME_METADATA_ITEMS];
	int                       count;
};

void avmetadata_init(struct avmetadata_s *);  /* One time call during startup / initialization */
void avmetadata_reset(struct avmetadata_s *); /* Erase any prior items */
void avmetadata_clone(struct avmetadata_s *dst, struct avmetadata_s *src);

/* Count the number of metadata items in the array matching item_type 'type' */
int  avmetadata_count_by_type(struct avmetadata_s *src, enum avmetadata_item_type_e item_type);

struct avmetadata_item_s *avmetadata_item_alloc(int lengthBytes, enum avmetadata_item_type_e item_type);
struct avmetadata_item_s *avmetadata_item_clone(struct avmetadata_item_s *src);
void avmetadata_item_dprintf(int fd, struct avmetadata_item_s *src);
const char *avmetadata_item_name(struct avmetadata_item_s *src);

int avmetadata_item_data_write(struct avmetadata_item_s *dst, const unsigned char *buf, int lengthBytes);
int avmetadata_item_data_realloc(struct avmetadata_item_s *src, int lengthBytes);
int avmetadata_item_data_set_linenr(struct avmetadata_item_s *src, int lineNr);
int avmetadata_item_data_get_linenr(struct avmetadata_item_s *src);
int avmetadata_item_data_set_outputstreamid(struct avmetadata_item_s *src, int outputstreamid);
int avmetadata_item_data_get_outputstreamid(struct avmetadata_item_s *src);

void avmetadata_item_free(struct avmetadata_item_s *);

#endif /* OBE_METADATA_H */
