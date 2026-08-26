#include "mini_stream_reorder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void
mini_stream_reorder_init(mini_stream_reorder_t *reorder,
    size_t buffer_limit)
{
    if (reorder == NULL) {
        return;
    }
    memset(reorder, 0, sizeof(*reorder));
    reorder->buffer_limit = buffer_limit;
}

mini_stream_reorder_chunk_t *
mini_stream_reorder_allocate(mini_stream_reorder_t *reorder,
    uint64_t offset, size_t length, int *error_code)
{
    if (error_code != NULL) {
        *error_code = 0;
    }
    if (reorder == NULL || length == 0
        || offset > UINT64_MAX - length
        || sizeof(mini_stream_reorder_chunk_t) + length < length) {
        if (error_code != NULL) {
            *error_code = EINVAL;
        }
        return NULL;
    }
    if (reorder->buffered_bytes > reorder->buffer_limit
        || length > reorder->buffer_limit - reorder->buffered_bytes)
    {
        if (error_code != NULL) {
            *error_code = ENOBUFS;
        }
        return NULL;
    }
    mini_stream_reorder_chunk_t *chunk =
        calloc(1, sizeof(*chunk) + length);
    if (chunk == NULL) {
        if (error_code != NULL) {
            *error_code = ENOMEM;
        }
        return NULL;
    }
    chunk->offset = offset;
    chunk->length = length;
    reorder->buffered_bytes += length;
    return chunk;
}

int
mini_stream_reorder_insert(mini_stream_reorder_t *reorder,
    mini_stream_reorder_chunk_t *chunk)
{
    if (reorder == NULL || chunk == NULL || chunk->length == 0
        || chunk->received != chunk->length
        || chunk->offset > UINT64_MAX - chunk->length
        || chunk->offset < reorder->next_offset)
    {
        return EPROTO;
    }
    mini_stream_reorder_chunk_t **cursor = &reorder->head;
    mini_stream_reorder_chunk_t *previous = NULL;
    while (*cursor != NULL && (*cursor)->offset < chunk->offset) {
        previous = *cursor;
        cursor = &(*cursor)->next;
    }
    if ((previous != NULL
            && previous->offset + previous->length > chunk->offset)
        || (*cursor != NULL
            && chunk->offset + chunk->length > (*cursor)->offset))
    {
        return EPROTO;
    }
    chunk->next = *cursor;
    *cursor = chunk;
    return 0;
}

mini_stream_reorder_chunk_t *
mini_stream_reorder_pop_ready(mini_stream_reorder_t *reorder)
{
    if (reorder == NULL || reorder->head == NULL
        || reorder->head->offset != reorder->next_offset)
    {
        return NULL;
    }
    mini_stream_reorder_chunk_t *chunk = reorder->head;
    reorder->head = chunk->next;
    chunk->next = NULL;
    reorder->next_offset += chunk->length;
    reorder->buffered_bytes -= chunk->length;
    return chunk;
}

void
mini_stream_reorder_discard(mini_stream_reorder_t *reorder,
    mini_stream_reorder_chunk_t *chunk)
{
    if (chunk == NULL) {
        return;
    }
    if (reorder != NULL && reorder->buffered_bytes >= chunk->length) {
        reorder->buffered_bytes -= chunk->length;
    }
    free(chunk);
}

void
mini_stream_reorder_release(mini_stream_reorder_t *reorder)
{
    if (reorder == NULL) {
        return;
    }
    mini_stream_reorder_chunk_t *chunk = reorder->head;
    while (chunk != NULL) {
        mini_stream_reorder_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    reorder->head = NULL;
    reorder->buffered_bytes = 0;
}

int
mini_stream_reorder_has_pending(const mini_stream_reorder_t *reorder)
{
    return reorder != NULL && reorder->head != NULL;
}
