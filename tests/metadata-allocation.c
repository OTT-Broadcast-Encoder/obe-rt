/* Build and run with AddressSanitizer; no SDK or encoder dependencies required.
 * cc -g -fsanitize=address,undefined tests/metadata-allocation.c -o /tmp/metadata-allocation
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int fail_after = -1;

static void *test_malloc(size_t size)
{
    if (fail_after == 0) {
        fail_after = -1;
        return NULL;
    }
    if (fail_after > 0)
        fail_after--;
    return malloc(size);
}

#define malloc test_malloc
#include "../common/metadata.c"
#undef malloc

int main(void)
{
    struct avmetadata_item_s *small = avmetadata_item_alloc(1, AVMETADATA_SECTION_SCTE35);
    assert(small);
    small->data[0] = 0xfc;
    small->dataLengthBytes = 1;
    small->lineNr = 10;
    small->outputStreamId = 3;
    struct avmetadata_item_s *copy = avmetadata_item_clone(small);
    assert(copy && copy != small && copy->data != small->data);
    assert(copy->data[0] == 0xfc && copy->dataLengthBytes == 1);
    assert(copy->lineNr == 10 && copy->outputStreamId == 3);
    avmetadata_item_free(copy);

    for (int allocation = 0; allocation < 2; allocation++) {
        fail_after = allocation;
        assert(avmetadata_item_clone(small) == NULL);
    }

    struct avmetadata_s src, dst;
    avmetadata_init(&src);
    avmetadata_init(&dst);
    src.array[src.count++] = small;
    src.array[src.count++] = avmetadata_item_clone(small);
    assert(src.array[1]);
    fail_after = 0;
    avmetadata_clone(&dst, &src);
    assert(dst.count == 1 && dst.array[0] && dst.array[0]->data[0] == 0xfc);
    assert(avmetadata_count_by_type(&dst, AVMETADATA_SECTION_SCTE35) == 1);
    avmetadata_reset(&dst);
    avmetadata_reset(&src);
    assert(src.count == 0 && dst.count == 0);
    return 0;
}
