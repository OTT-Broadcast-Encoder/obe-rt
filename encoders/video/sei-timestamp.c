#include <encoders/video/sei-timestamp.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "common/common.h"

int g_sei_timestamping = 0;

const unsigned char ltn_uuid_sei_timestamp[] =
{
    0x59, 0x96, 0xFF, 0x28, 0x17, 0xCA, 0x41, 0x96, 0x8D, 0xE3, 0xE5, 0x3F, 0xE2, 0xF9, 0x92, 0xAE
};

unsigned char *set_timestamp_alloc()
{
	unsigned char *p = calloc(1, SEI_TIMESTAMP_PAYLOAD_LENGTH);
	if (!p)
		return NULL;

	memcpy(p, &ltn_uuid_sei_timestamp[0], sizeof(ltn_uuid_sei_timestamp));
	return p;
}

int set_timestamp_field_set(unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t value)
{
	if (nr < 1 || nr > SEI_TIMESTAMP_FIELD_COUNT)
		return -1;

	unsigned char *p = buffer;
	p += (sizeof(ltn_uuid_sei_timestamp) + ((nr - 1) * 6));

	if (lengthBytes - (p - buffer) < 6) {
		printf("%s() overflow\n", __func__);
		return -EOVERFLOW;
	}

	*(p++) = (value >> 24) & 0xff;
	*(p++) = (value >> 16) & 0xff;
	*(p++) = SEI_BIT_DELIMITER;
	*(p++) = (value >>  8) & 0xff;
	*(p++) = (value >>  0) & 0xff;
	*(p++) = SEI_BIT_DELIMITER;

	return 0;
}

int ltn_uuid_find(const unsigned char *buf, unsigned int lengthBytes)
{
	if (lengthBytes < sizeof(ltn_uuid_sei_timestamp))
		return -1;

	for (int i = 0; i < lengthBytes - sizeof(ltn_uuid_sei_timestamp); i++) {
		if (memcmp(buf + i, &ltn_uuid_sei_timestamp[0], sizeof(ltn_uuid_sei_timestamp)) == 0) {
			return i;
		}
	}

	return -1;
}

int set_timestamp_field_get(const unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t *value)
{
	if (nr < 1 || nr > SEI_TIMESTAMP_FIELD_COUNT)
		return -1;

	uint32_t v;
	const unsigned char *p = buffer;
	p += (sizeof(ltn_uuid_sei_timestamp) + ((nr - 1) * 6));
	v  = (*(p++) << 24);
	v |= (*(p++) << 16);
	p++;
	v |= (*(p++) <<  8);
	v |= (*(p++) <<  0);

	*value = v;

	return 0;
}

int64_t sei_timestamp_query_codec_latency_ms(const unsigned char *buffer, int lengthBytes)
{
	struct timeval begin, end;
	uint32_t v[8];
	for (int i = 0; i < 8; i++)
		set_timestamp_field_get(buffer, lengthBytes, i, &v[i]);

	begin.tv_sec = v[4];
	begin.tv_usec = v[5];
	end.tv_sec = v[6];
	end.tv_usec = v[7];

	struct timeval diff;
	obe_timeval_subtract(&diff, &end, &begin);

#if 0
	printf("%08d: %d.%d - %d.%d = %d.%d\n",
		v[1], 
		begin.tv_sec, begin.tv_usec,
		end.tv_sec, end.tv_usec,
		diff.tv_sec, diff.tv_usec);
#endif

	return obe_timediff_to_msecs(&diff);
}

void sei_timestamp_hexdump(const unsigned char *buffer, int lengthBytes)
{
	int len = SEI_TIMESTAMP_PAYLOAD_LENGTH;
	if (lengthBytes < len)
		len = lengthBytes;

	int v = 0;
	for (int i = 1; i <= len; i++) {
		printf("%02x ", *(buffer + i - 1));
		if (i == sizeof(ltn_uuid_sei_timestamp))
			printf(" ");
		if (i > sizeof(ltn_uuid_sei_timestamp)) {
			if (v++ == 2) {
				printf(" ");
				v = 0;
			}
		}
	}
	printf("\n");
}
