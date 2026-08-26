#ifndef XQC_MINI_STREAM_REORDER_H
#define XQC_MINI_STREAM_REORDER_H

#include <stddef.h>
#include <stdint.h>

typedef struct mini_stream_reorder_chunk_s mini_stream_reorder_chunk_t;

struct mini_stream_reorder_chunk_s {
    mini_stream_reorder_chunk_t *next;
    uint64_t offset;
    size_t length;
    size_t received;
    unsigned char data[];
};

typedef struct mini_stream_reorder_s {
    mini_stream_reorder_chunk_t *head;
    uint64_t next_offset;
    size_t buffered_bytes;
    size_t buffer_limit;
} mini_stream_reorder_t;

void mini_stream_reorder_init(mini_stream_reorder_t *reorder,
    size_t buffer_limit);
mini_stream_reorder_chunk_t *mini_stream_reorder_allocate(
    mini_stream_reorder_t *reorder, uint64_t offset, size_t length,
    int *error_code);
int mini_stream_reorder_insert(mini_stream_reorder_t *reorder,
    mini_stream_reorder_chunk_t *chunk);
mini_stream_reorder_chunk_t *mini_stream_reorder_pop_ready(
    mini_stream_reorder_t *reorder);
void mini_stream_reorder_discard(mini_stream_reorder_t *reorder,
    mini_stream_reorder_chunk_t *chunk);
void mini_stream_reorder_release(mini_stream_reorder_t *reorder);
int mini_stream_reorder_has_pending(const mini_stream_reorder_t *reorder);

#endif
