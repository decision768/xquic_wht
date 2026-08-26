#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_stream_reorder.h"

static mini_stream_reorder_chunk_t *
make_chunk(mini_stream_reorder_t *reorder, uint64_t offset,
    const char *data)
{
    size_t length = strlen(data);
    int error_code = 0;
    mini_stream_reorder_chunk_t *chunk =
        mini_stream_reorder_allocate(reorder, offset, length, &error_code);
    assert(chunk != NULL);
    assert(error_code == 0);
    memcpy(chunk->data, data, length);
    chunk->received = length;
    return chunk;
}

int
main(void)
{
    mini_stream_reorder_t reorder;
    mini_stream_reorder_init(&reorder, 64);

    mini_stream_reorder_chunk_t *middle = make_chunk(&reorder, 4, "EFGH");
    mini_stream_reorder_chunk_t *last = make_chunk(&reorder, 8, "IJKL");
    mini_stream_reorder_chunk_t *first = make_chunk(&reorder, 0, "ABCD");
    assert(mini_stream_reorder_insert(&reorder, middle) == 0);
    assert(mini_stream_reorder_insert(&reorder, last) == 0);
    assert(mini_stream_reorder_pop_ready(&reorder) == NULL);
    assert(mini_stream_reorder_insert(&reorder, first) == 0);

    const char *expected[] = { "ABCD", "EFGH", "IJKL" };
    size_t i;
    for (i = 0; i < 3; i++) {
        mini_stream_reorder_chunk_t *ready =
            mini_stream_reorder_pop_ready(&reorder);
        assert(ready != NULL);
        assert(memcmp(ready->data, expected[i], 4) == 0);
        free(ready);
    }
    assert(reorder.next_offset == 12);
    assert(reorder.buffered_bytes == 0);

    mini_stream_reorder_chunk_t *duplicate =
        make_chunk(&reorder, 8, "IJKL");
    assert(mini_stream_reorder_insert(&reorder, duplicate) == EPROTO);
    mini_stream_reorder_discard(&reorder, duplicate);

    mini_stream_reorder_chunk_t *one = make_chunk(&reorder, 12, "12345678");
    mini_stream_reorder_chunk_t *overlap = make_chunk(&reorder, 16, "overlap");
    assert(mini_stream_reorder_insert(&reorder, one) == 0);
    assert(mini_stream_reorder_insert(&reorder, overlap) == EPROTO);
    mini_stream_reorder_discard(&reorder, overlap);

    int error_code = 0;
    mini_stream_reorder_chunk_t *too_large =
        mini_stream_reorder_allocate(&reorder, 100, 60, &error_code);
    assert(too_large == NULL);
    assert(error_code == ENOBUFS);
    mini_stream_reorder_release(&reorder);

    /* Simulate 64 records arriving from eight lanes in shuffled order. */
    enum { RECORD_COUNT = 64, RECORD_SIZE = 17 };
    mini_stream_reorder_init(&reorder, RECORD_COUNT * RECORD_SIZE);
    for (i = 0; i < RECORD_COUNT; i++) {
        size_t record_index = (i * 37 + 23) % RECORD_COUNT;
        mini_stream_reorder_chunk_t *chunk = mini_stream_reorder_allocate(
            &reorder, record_index * RECORD_SIZE, RECORD_SIZE, &error_code);
        assert(chunk != NULL);
        memset(chunk->data, (int)record_index, RECORD_SIZE);
        chunk->received = RECORD_SIZE;
        assert(mini_stream_reorder_insert(&reorder, chunk) == 0);
    }
    for (i = 0; i < RECORD_COUNT; i++) {
        mini_stream_reorder_chunk_t *ready =
            mini_stream_reorder_pop_ready(&reorder);
        assert(ready != NULL);
        assert(ready->offset == i * RECORD_SIZE);
        size_t j;
        for (j = 0; j < RECORD_SIZE; j++) {
            assert(ready->data[j] == (unsigned char)i);
        }
        free(ready);
    }
    assert(reorder.next_offset == RECORD_COUNT * RECORD_SIZE);
    assert(reorder.buffered_bytes == 0);
    mini_stream_reorder_release(&reorder);

    puts("mini stream reorder tests passed");
    return 0;
}
