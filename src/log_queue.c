/** @file
 * @brief Fixed-capacity task-side queues for pending records.
 *
 * Critical and event queues reject a new entry when full. The routine
 * queue replaces its oldest pending entry and reports the replacement.
 */
#include "log_internal.h"

#include <string.h>

void elog_queue_setup(elog_queue_t *queue, elog_entry_t *slots,
                      uint8_t capacity)
{
    queue->slots = slots;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->count = 0u;
}

elog_result_t elog_queue_push(elog_queue_t *queue, const elog_entry_t *entry,
                              uint8_t drop_oldest)
{
    if (queue == NULL || entry == NULL || queue->capacity == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    elog_result_t result = ELOG_OK;
    if (queue->count == queue->capacity) {
        if (drop_oldest == 0u) {
            return ELOG_QUEUE_FULL;
        }
        queue->head = (uint8_t)((queue->head + 1u) % queue->capacity);
        --queue->count;
        result = ELOG_QUEUED_WITH_DROP;
    }
    const uint8_t tail = (uint8_t)((queue->head + queue->count) % queue->capacity);
    queue->slots[tail] = *entry;
    ++queue->count;
    return result;
}

elog_result_t elog_queue_pop(elog_queue_t *queue, elog_entry_t *entry)
{
    if (queue == NULL || entry == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (queue->count == 0u) {
        return ELOG_EMPTY;
    }
    *entry = queue->slots[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % queue->capacity);
    --queue->count;
    return ELOG_OK;
}
