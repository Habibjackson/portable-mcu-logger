/** @file
 * @brief Queue, fault, and journal state-machine implementation.
 *
 * Task calls enqueue bounded RAM entries. elog_process() selects priority,
 * writes page and record bodies, verifies them, then programs commit units.
 * Storage failures latch diagnostics; recovery rescans without erasing data.
 */
#include "log_internal.h"
#include "log_codec.h"

#include <limits.h>
#include <string.h>

/** @brief Read the optional task clock; zero means no clock was configured. */
static uint64_t now_ms(const elog_t *logger)
{
    return logger->config.timestamp != NULL ?
        logger->config.timestamp(logger->config.timestamp_context) : 0u;
}

/** @brief Freeze the recent RAM ring and arm the configured post-fault count.
 * Repeated fault requests do not replace a snapshot already being drained.
 */
static void freeze_recent(elog_t *logger)
{
    if (logger->pre_frozen != 0u) {
        return;
    }
    logger->pre_frozen = 1u;
    logger->pre_dump_count = logger->pre_count;
    logger->post_remaining = logger->config.post_fault_depth;
    if (logger->config.pre_fault_depth != 0u) {
        logger->pre_dump_index = (uint8_t)((logger->pre_head +
             logger->config.pre_fault_depth - logger->pre_count) %
             logger->config.pre_fault_depth);
    }
}

/** @brief Consume the ISR's atomic request in the serialized task context. */
static void check_freeze_request(elog_t *logger)
{
    if (atomic_exchange_explicit(&logger->freeze_request, false,
                                 memory_order_acq_rel)) {
        freeze_recent(logger);
    }
}

/** @brief Copy eligible telemetry into bounded pre/post-fault RAM snapshots.
 * Only accepted TELEMETRY entries of at most 24 bytes are eligible. This
 * snapshot path does not itself commit data to flash.
 */
static void remember_telemetry(elog_t *logger, const elog_entry_t *entry)
{
    if (entry->type != (uint8_t)ELOG_TYPE_TELEMETRY || entry->length > 24u) {
        return;
    }
    elog_telemetry_sample_t sample;
    memset(&sample, 0, sizeof sample);
    sample.timestamp = entry->timestamp;
    sample.event_id = entry->event_id;
    sample.length = (uint8_t)entry->length;
    if (entry->length != 0u) {
        memcpy(sample.payload, entry->payload, entry->length);
    }
    if (logger->pre_frozen != 0u) {
        if (logger->post_remaining != 0u &&
            logger->post_count < logger->config.post_fault_depth) {
            logger->post_fault[logger->post_count] = sample;
            ++logger->post_count;
            --logger->post_remaining;
        }
    } else if (logger->config.pre_fault_depth != 0u) {
        logger->pre_fault[logger->pre_head] = sample;
        logger->pre_head = (uint8_t)((logger->pre_head + 1u) %
                                     logger->config.pre_fault_depth);
        if (logger->pre_count < logger->config.pre_fault_depth) {
            ++logger->pre_count;
        }
    }
}

/** @brief Reject unsupported policies before any logger state is initialized.
 * Storage geometry and partition coverage are checked by elog_build_pages().
 */
static elog_result_t validate_config(const elog_config_t *config,
                                      const elog_storage_t *storage)
{
    if (config == NULL || storage == NULL || storage->ops == NULL ||
        storage->context == NULL) {
        return ELOG_INVALID_CONFIG;
    }
    const elog_storage_ops_t *ops = storage->ops;
    if (ops->capacity == NULL || ops->program_granularity == NULL ||
        ops->max_program_size == NULL || ops->erase_region == NULL ||
        ops->read == NULL || ops->program == NULL || ops->erase == NULL ||
        ops->is_busy == NULL || ops->sync == NULL) {
        return ELOG_INVALID_CONFIG;
    }
    if (config->critical_queue_depth == 0u ||
        config->critical_queue_depth > ELOG_MAX_CRITICAL_QUEUE ||
        config->event_queue_depth == 0u ||
        config->event_queue_depth > ELOG_MAX_EVENT_QUEUE ||
        config->routine_queue_depth == 0u ||
        config->routine_queue_depth > ELOG_MAX_ROUTINE_QUEUE ||
        config->isr_queue_depth < 2u ||
        config->isr_queue_depth > ELOG_MAX_ISR_QUEUE ||
        config->pre_fault_depth > ELOG_MAX_PRE_FAULT ||
        config->post_fault_depth > ELOG_MAX_POST_FAULT ||
        config->max_payload < 12u || config->max_payload > ELOG_MAX_PAYLOAD ||
        config->fault_table_depth == 0u ||
        config->fault_table_depth > ELOG_MAX_FAULTS ||
        config->storage_timeout_ms == 0u ||
        config->recover_on_init != 1u || config->require_crc != 1u ||
        (config->codec_id > (uint8_t)ELOG_CODEC_VARINT &&
         config->codec_id < ELOG_CODEC_CUSTOM_MIN) ||
        (config->codec_id >= ELOG_CODEC_CUSTOM_MIN &&
         (config->custom_codec == NULL ||
          config->custom_codec->id != config->codec_id ||
          config->custom_codec->encode == NULL ||
          config->custom_codec->decode == NULL)) ||
        (config->codec_id < ELOG_CODEC_CUSTOM_MIN &&
         config->custom_codec != NULL) ||
        config->block_size != 0u ||
        (config->codec_id == (uint8_t)ELOG_CODEC_DELTA &&
         config->snapshot_interval == 0u) ||
        (config->codec_id != (uint8_t)ELOG_CODEC_DELTA &&
         config->snapshot_interval != 0u) ||
        (config->routine_interval_ms != 0u && config->timestamp == NULL) ||
        (config->fault_repeat_interval_ms != 0u && config->timestamp == NULL)) {
        return ELOG_INVALID_CONFIG;
    }
    return ELOG_OK;
}

elog_result_t elog_init(elog_t *logger, const elog_config_t *config,
                        const elog_storage_t *storage)
{
    if (logger == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    elog_result_t result = validate_config(config, storage);
    if (result != ELOG_OK) {
        return result;
    }
    memset(logger, 0, sizeof *logger);
    logger->config = *config;
    logger->storage = *storage;
    logger->last_error = ELOG_OK;
    logger->target_page = -1;
    elog_queue_setup(&logger->queue[ELOG_CRITICAL], logger->critical_slots,
                     config->critical_queue_depth);
    elog_queue_setup(&logger->queue[ELOG_EVENT], logger->event_slots,
                     config->event_queue_depth);
    elog_queue_setup(&logger->queue[ELOG_ROUTINE], logger->routine_slots,
                     config->routine_queue_depth);
    atomic_init(&logger->isr_head, 0u);
    atomic_init(&logger->isr_tail, 0u);
    atomic_init(&logger->isr_dropped, 0u);
    atomic_init(&logger->freeze_request, false);
    if (!atomic_is_lock_free(&logger->isr_head) ||
        !atomic_is_lock_free(&logger->isr_tail) ||
        !atomic_is_lock_free(&logger->isr_dropped) ||
        !atomic_is_lock_free(&logger->freeze_request)) {
        return ELOG_UNSUPPORTED;
    }
    result = elog_build_pages(logger);
    if (result != ELOG_OK) {
        return result;
    }
    result = elog_scan_pages(logger);
    if (result != ELOG_OK) {
        return result;
    }
    logger->initialized = 1u;
    return ELOG_OK;
}

elog_result_t elog_write(elog_t *logger, elog_priority_t priority,
                         elog_record_type_t type, uint32_t event_id,
                         const void *payload, size_t length)
{
    if (logger == NULL || logger->initialized == 0u ||
        (unsigned int)priority > (unsigned int)ELOG_ROUTINE ||
        (unsigned int)type < (unsigned int)ELOG_TYPE_TELEMETRY ||
        (unsigned int)type > (unsigned int)ELOG_TYPE_POST_FAULT ||
        (payload == NULL && length != 0u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length > logger->config.max_payload) {
        return ELOG_TOO_LARGE;
    }
    if (priority == ELOG_CRITICAL &&
        logger->storage_full[ELOG_CRITICAL] != 0u) {
        return ELOG_STORAGE_FULL;
    }
    check_freeze_request(logger);
    elog_entry_t entry;
    memset(&entry, 0, sizeof entry);
    entry.timestamp = now_ms(logger);
    entry.event_id = event_id;
    entry.length = (uint16_t)length;
    entry.type = (uint8_t)type;
    entry.priority = (uint8_t)priority;
    if (length != 0u) {
        memcpy(entry.payload, payload, length);
    }
    if (priority == ELOG_ROUTINE && logger->config.routine_interval_ms != 0u &&
        logger->has_last_routine_timestamp != 0u &&
        entry.timestamp - logger->last_routine_timestamp <
            logger->config.routine_interval_ms) {
        ++logger->statistics.routine_rate_limited;
        return ELOG_WOULD_BLOCK;
    }
    const elog_result_t result = elog_queue_push(&logger->queue[priority],
                                  &entry, priority == ELOG_ROUTINE ? 1u : 0u);
    if (result == ELOG_QUEUE_FULL || result == ELOG_QUEUED_WITH_DROP) {
        ++logger->statistics.dropped_queue[priority];
    }
    if (result == ELOG_OK || result == ELOG_QUEUED_WITH_DROP) {
        if (priority == ELOG_ROUTINE) {
            logger->last_routine_timestamp = entry.timestamp;
            logger->has_last_routine_timestamp = 1u;
            remember_telemetry(logger, &entry);
        }
    }
    return result;
}

elog_result_t elog_event(elog_t *logger, uint32_t event_id,
                         const void *payload, size_t length)
{
    return elog_write(logger, ELOG_EVENT, ELOG_TYPE_EVENT,
                      event_id, payload, length);
}

elog_result_t elog_fault(elog_t *logger, uint32_t fault_id,
                         const void *snapshot, size_t length)
{
    if (logger == NULL || logger->initialized == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (logger->storage_full[ELOG_CRITICAL] != 0u) {
        return ELOG_STORAGE_FULL;
    }
    elog_fault_state_t *free_slot = NULL;
    for (uint8_t i = 0u; i < logger->config.fault_table_depth; ++i) {
        elog_fault_state_t *fault = &logger->faults[i];
        if (fault->active != 0u && fault->id == fault_id) {
            if (fault->repeats != UINT32_MAX) {
                ++fault->repeats;
            }
            const uint64_t timestamp = now_ms(logger);
            if (logger->config.fault_repeat_interval_ms != 0u &&
                timestamp - fault->last_summary_timestamp >=
                    logger->config.fault_repeat_interval_ms) {
                uint8_t count[4];
                for (unsigned int j = 0u; j < 4u; ++j) {
                    count[j] = (uint8_t)(fault->repeats >> (8u * j));
                }
                const elog_result_t result = elog_write(logger, ELOG_CRITICAL,
                    ELOG_TYPE_FAULT_REPEAT, fault_id, count, sizeof count);
                if (result == ELOG_OK) {
                    fault->repeats = 0u;
                    fault->last_summary_timestamp = timestamp;
                }
                return result;
            }
            ++logger->statistics.fault_repeats_suppressed;
            return ELOG_OK;
        }
        if (fault->active == 0u && free_slot == NULL) {
            free_slot = fault;
        }
    }
    if (free_slot == NULL) {
        return ELOG_QUEUE_FULL;
    }
    const uint64_t timestamp = now_ms(logger);
    const elog_result_t result = elog_write(logger, ELOG_CRITICAL,
        ELOG_TYPE_FAULT, fault_id, snapshot, length);
    if (result == ELOG_OK) {
        free_slot->id = fault_id;
        free_slot->repeats = 0u;
        free_slot->first_timestamp = timestamp;
        free_slot->last_summary_timestamp = timestamp;
        free_slot->active = 1u;
        freeze_recent(logger);
    }
    return result;
}

elog_result_t elog_fault_recovered(elog_t *logger, uint32_t fault_id,
                                   const void *payload, size_t length)
{
    if (logger == NULL || logger->initialized == 0u ||
        (payload == NULL && length != 0u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length > logger->config.max_payload - 12u) {
        return ELOG_TOO_LARGE;
    }
    for (uint8_t i = 0u; i < logger->config.fault_table_depth; ++i) {
        elog_fault_state_t *fault = &logger->faults[i];
        if (fault->active != 0u && fault->id == fault_id) {
            const uint64_t duration = now_ms(logger) - fault->first_timestamp;
            uint8_t body[ELOG_MAX_PAYLOAD];
            for (unsigned int j = 0u; j < 4u; ++j) {
                body[j] = (uint8_t)(fault->repeats >> (8u * j));
            }
            for (unsigned int j = 0u; j < 8u; ++j) {
                body[4u + j] = (uint8_t)(duration >> (8u * j));
            }
            if (length != 0u) {
                memcpy(body + 12u, payload, length);
            }
            const elog_result_t result = elog_write(logger, ELOG_CRITICAL,
                ELOG_TYPE_FAULT_RECOVERY, fault_id, body, length + 12u);
            if (result == ELOG_OK) {
                fault->active = 0u;
                logger->post_remaining = 0u;
            }
            return result;
        }
    }
    return ELOG_INVALID_ARGUMENT;
}

elog_result_t elog_capture_fault_isr(elog_t *logger, uint32_t fault_id,
                                     uint64_t timestamp,
                                     const void *snapshot, size_t length)
{
    if (logger == NULL || logger->initialized == 0u ||
        (snapshot == NULL && length != 0u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length > logger->config.max_payload) {
        return ELOG_TOO_LARGE;
    }
    const unsigned int head = atomic_load_explicit(&logger->isr_head,
                                                   memory_order_relaxed);
    const unsigned int tail = atomic_load_explicit(&logger->isr_tail,
                                                   memory_order_acquire);
    if (head - tail >= logger->config.isr_queue_depth) {
        atomic_fetch_add_explicit(&logger->isr_dropped, 1u,
                                  memory_order_relaxed);
        return ELOG_QUEUE_FULL;
    }
    elog_entry_t *entry = &logger->isr_slots[head % logger->config.isr_queue_depth];
    entry->timestamp = timestamp;
    entry->event_id = fault_id;
    entry->length = (uint16_t)length;
    entry->type = (uint8_t)ELOG_TYPE_FAULT;
    entry->priority = (uint8_t)ELOG_CRITICAL;
    if (length != 0u) {
        memcpy(entry->payload, snapshot, length);
    }
    atomic_store_explicit(&logger->freeze_request, true, memory_order_release);
    atomic_store_explicit(&logger->isr_head, head + 1u, memory_order_release);
    return ELOG_OK;
}

elog_result_t elog_get_status(const elog_t *logger, elog_status_t *status)
{
    if (logger == NULL || logger->initialized == 0u || status == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    memset(status, 0, sizeof *status);
    status->last_error = logger->last_error;
    for (uint8_t lane = 0u; lane < 3u; ++lane) {
        status->total_bytes[lane] = logger->config.partition[lane].length;
        status->queued[lane] = logger->queue[lane].count;
    }
    for (uint8_t i = 0u; i < logger->page_count; ++i) {
        const elog_page_t *page = &logger->pages[i];
        status->used_bytes[page->priority] += page->used;
    }
    const unsigned int head = atomic_load_explicit(&logger->isr_head,
                                                   memory_order_acquire);
    const unsigned int tail = atomic_load_explicit(&logger->isr_tail,
                                                   memory_order_acquire);
    status->queued_isr = (uint8_t)(head - tail);
    status->busy = (uint8_t)(logger->process_state != ELOG_PROCESS_IDLE ||
                             logger->has_pending != 0u);
    memcpy(status->storage_full, logger->storage_full,
           sizeof status->storage_full);
    return ELOG_OK;
}

elog_result_t elog_get_statistics(const elog_t *logger,
                                   elog_statistics_t *statistics)
{
    if (logger == NULL || logger->initialized == 0u || statistics == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    *statistics = logger->statistics;
    statistics->dropped_queue[ELOG_CRITICAL] +=
        atomic_load_explicit(&logger->isr_dropped, memory_order_relaxed);
    return ELOG_OK;
}

elog_result_t elog_flush(elog_t *logger, uint32_t max_steps)
{
    if (logger == NULL || logger->initialized == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0u; i < max_steps; ++i) {
        const elog_result_t result = elog_process(logger);
        if (result == ELOG_EMPTY) {
            return ELOG_OK;
        }
        if (result != ELOG_OK) {
            return result;
        }
    }
    return ELOG_WOULD_BLOCK;
}

/** @brief Select the next RAM entry in journal priority order.
 * ISR ingress precedes task critical and event queues. Frozen pre/post-fault
 * samples precede ordinary routine data. ELOG_EMPTY means no pending work.
 */
static elog_result_t take_pending(elog_t *logger)
{
    check_freeze_request(logger);
    const unsigned int tail = atomic_load_explicit(&logger->isr_tail,
                                                   memory_order_relaxed);
    const unsigned int head = atomic_load_explicit(&logger->isr_head,
                                                   memory_order_acquire);
    if (head != tail) {
        /* Publication of head also publishes the ISR freeze request. */
        check_freeze_request(logger);
        logger->pending = logger->isr_slots[tail % logger->config.isr_queue_depth];
        atomic_store_explicit(&logger->isr_tail, tail + 1u, memory_order_release);
        logger->has_pending = 1u;
        return ELOG_OK;
    }
    for (uint8_t lane = 0u; lane < 2u; ++lane) {
        if (elog_queue_pop(&logger->queue[lane], &logger->pending) == ELOG_OK) {
            logger->has_pending = 1u;
            return ELOG_OK;
        }
    }
    if (logger->pre_dump_count != 0u || logger->post_count != 0u) {
        const uint8_t pre = (uint8_t)(logger->pre_dump_count != 0u);
        elog_telemetry_sample_t sample;
        if (pre != 0u) {
            sample = logger->pre_fault[logger->pre_dump_index];
            logger->pre_dump_index = (uint8_t)((logger->pre_dump_index + 1u) %
                                                logger->config.pre_fault_depth);
            --logger->pre_dump_count;
            if (logger->pre_dump_count == 0u &&
                logger->post_remaining == 0u) {
                logger->pre_frozen = 0u;
                logger->pre_count = 0u;
                logger->pre_head = 0u;
            }
        } else {
            sample = logger->post_fault[0];
            --logger->post_count;
            if (logger->post_count != 0u) {
                memmove(logger->post_fault, logger->post_fault + 1u,
                        (size_t)logger->post_count * sizeof logger->post_fault[0]);
            }
        }
        memset(&logger->pending, 0, sizeof logger->pending);
        logger->pending.timestamp = sample.timestamp;
        logger->pending.event_id = sample.event_id;
        logger->pending.length = sample.length;
        logger->pending.type = pre != 0u ? (uint8_t)ELOG_TYPE_PRE_FAULT :
                                           (uint8_t)ELOG_TYPE_POST_FAULT;
        logger->pending.priority = (uint8_t)ELOG_ROUTINE;
        memcpy(logger->pending.payload, sample.payload, sample.length);
        logger->has_pending = 1u;
        return ELOG_OK;
    }
    if (elog_queue_pop(&logger->queue[ELOG_ROUTINE], &logger->pending) == ELOG_OK) {
        logger->has_pending = 1u;
        return ELOG_OK;
    }
    if (logger->pre_frozen != 0u && logger->pre_dump_count == 0u &&
        logger->post_remaining == 0u) {
        logger->pre_frozen = 0u;
        logger->pre_count = 0u;
        logger->pre_head = 0u;
    }
    return ELOG_EMPTY;
}

/** @brief Build one record image, using compression only when it saves bytes.
 * force_snapshot disables DELTA when a new page would break its base chain.
 * The image includes commit bytes, but program_chunk() writes them separately.
 */
static elog_result_t encode_pending(elog_t *logger, uint8_t force_snapshot)
{
    const uint32_t g = logger->storage.ops->program_granularity(
                                                  logger->storage.context);
    logger->pending_sequence = logger->next_sequence;
    uint8_t codec = (uint8_t)ELOG_CODEC_NONE;
    const uint8_t *payload = logger->pending.payload;
    size_t stored_length = logger->pending.length;
    if (force_snapshot == 0u &&
        logger->pending.priority == (uint8_t)ELOG_ROUTINE &&
        logger->pending.type == (uint8_t)ELOG_TYPE_TELEMETRY) {
        size_t compressed_length = 0u;
        elog_result_t compressed = ELOG_UNSUPPORTED;
        if (logger->config.codec_id >= ELOG_CODEC_CUSTOM_MIN) {
            compressed = logger->config.custom_codec->encode(
                logger->config.custom_codec->context, logger->pending.payload,
                logger->pending.length, logger->codec_image,
                sizeof logger->codec_image, &compressed_length);
            codec = logger->config.codec_id;
            if (compressed != ELOG_OK && compressed != ELOG_TOO_LARGE) {
                return compressed;
            }
        } else if (logger->pending.length >= 4u &&
                   logger->pending.length % 4u == 0u &&
                   logger->config.codec_id == (uint8_t)ELOG_CODEC_VARINT) {
            compressed = elog_varint_encode(logger->pending.payload,
                logger->pending.length, logger->codec_image,
                sizeof logger->codec_image, &compressed_length);
            codec = (uint8_t)ELOG_CODEC_VARINT;
        } else if (logger->pending.length >= 4u &&
            logger->pending.length % 4u == 0u &&
            logger->config.codec_id == (uint8_t)ELOG_CODEC_DELTA &&
            logger->previous_numeric_valid != 0u &&
            logger->previous_numeric_id == logger->pending.event_id &&
            logger->previous_numeric_length == logger->pending.length &&
            logger->since_snapshot < logger->config.snapshot_interval) {
            compressed = elog_delta_encode(logger->pending.payload,
                logger->previous_numeric, logger->pending.length,
                logger->previous_numeric_sequence, logger->codec_image,
                sizeof logger->codec_image, &compressed_length);
            codec = (uint8_t)ELOG_CODEC_DELTA;
        }
        if (compressed == ELOG_OK &&
            compressed_length < logger->pending.length) {
            payload = logger->codec_image;
            stored_length = compressed_length;
        } else {
            codec = (uint8_t)ELOG_CODEC_NONE;
        }
    }
    const elog_record_t record = {
        logger->pending.type, logger->pending.priority, codec, 0u,
        logger->pending_sequence, logger->pending.timestamp,
        logger->pending.event_id, logger->pending.length,
        (uint16_t)stored_length, payload
    };
    return elog_record_encode(&record, g,
        logger->record_image, sizeof logger->record_image, &logger->record_layout);
}

/** @brief Bind the pending record to an active or newly selected erase page.
 * If an active page cannot fit it, the first record on the next page is made
 * self-contained before page creation or rotation begins.
 */
static elog_result_t prepare_record(elog_t *logger)
{
    const uint32_t g = logger->storage.ops->program_granularity(
                                                  logger->storage.context);
    if (logger->has_pending == 0u) {
        const elog_result_t take = take_pending(logger);
        if (take != ELOG_OK) {
            return take;
        }
    }
    elog_result_t result = encode_pending(logger, 0u);
    if (result != ELOG_OK) {
        return result;
    }
    const uint8_t lane = logger->pending.priority;
    const int16_t active = logger->active_page[lane];
    if (active >= 0) {
        const elog_page_t *page = &logger->pages[active];
        if (page->state == ELOG_PAGE_VALID &&
            logger->record_layout.total_length <= page->length - page->used) {
            logger->target_page = active;
            logger->body_progress = 0u;
            logger->process_state = ELOG_PROCESS_RECORD_BODY;
            return ELOG_OK;
        }
    }
    if (logger->record_image[8] == (uint8_t)ELOG_CODEC_DELTA) {
        result = encode_pending(logger, 1u);
        if (result != ELOG_OK) {
            return result;
        }
    }
    uint8_t needs_erase;
    result = elog_choose_page(logger, lane, &logger->target_page, &needs_erase);
    if (result != ELOG_OK) {
        if (result == ELOG_STORAGE_FULL) {
            logger->storage_full[lane] = 1u;
        }
        return result;
    }
    elog_make_page_header(logger->page_image, g, lane,
                          logger->next_generation[lane]);
    logger->body_progress = 0u;
    logger->process_state = needs_erase != 0u ? ELOG_PROCESS_ERASE_PAGE :
                                                 ELOG_PROCESS_PAGE_BODY;
    return ELOG_OK;
}

/** @brief Latch one failed journal operation for explicit recovery. */
static elog_result_t storage_failure(elog_t *logger, elog_result_t result)
{
    logger->last_error = result;
    logger->process_state = ELOG_PROCESS_FAILED;
    ++logger->statistics.storage_errors;
    return result;
}

/** @brief Program one backend-sized piece of a body or commit unit.
 * The process state advances only after the complete requested image part is
 * programmed. A backend failure leaves the writer in ELOG_PROCESS_FAILED.
 */
static elog_result_t program_chunk(elog_t *logger, uint32_t address,
                                    const uint8_t *source, size_t total,
                                    uint8_t next_state)
{
    const uint32_t max_size = logger->storage.ops->max_program_size(
                                                  logger->storage.context);
    const size_t remaining = total - logger->body_progress;
    const size_t chunk = remaining < max_size ? remaining : max_size;
    if (chunk == 0u) {
        return storage_failure(logger, ELOG_INVALID_CONFIG);
    }
    const elog_result_t result = logger->storage.ops->program(
        logger->storage.context, address + (uint32_t)logger->body_progress,
        source + logger->body_progress, chunk,
        logger->config.storage_timeout_ms);
    if (result != ELOG_OK) {
        return storage_failure(logger, result);
    }
    logger->body_progress += chunk;
    if (logger->body_progress == total) {
        logger->body_progress = 0u;
        logger->process_state = next_state;
    }
    return ELOG_OK;
}

elog_result_t elog_process(elog_t *logger)
{
    if (logger == NULL || logger->initialized == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (logger->process_state == ELOG_PROCESS_FAILED) {
        return logger->last_error;
    }
    bool busy = false;
    elog_result_t result = logger->storage.ops->is_busy(
        logger->storage.context, &busy);
    if (result != ELOG_OK) {
        return storage_failure(logger, result);
    }
    if (busy) {
        return ELOG_WOULD_BLOCK;
    }
    if (logger->process_state == ELOG_PROCESS_IDLE) {
        const elog_result_t result = prepare_record(logger);
        if (result != ELOG_OK) {
            if (result == ELOG_STORAGE_FULL && logger->has_pending != 0u) {
                ++logger->statistics.dropped_storage[logger->pending.priority];
                logger->has_pending = 0u;
                logger->last_error = result;
            }
            return result;
        }
    }
    elog_page_t *page = &logger->pages[logger->target_page];
    const elog_storage_ops_t *ops = logger->storage.ops;
    void *storage_context = logger->storage.context;
    const uint32_t g = ops->program_granularity(storage_context);
    switch (logger->process_state) {
    case ELOG_PROCESS_ERASE_PAGE:
        if (logger->config.may_erase != NULL &&
            logger->config.may_erase(logger->config.may_erase_context) == 0) {
            return ELOG_WOULD_BLOCK;
        }
        result = ops->erase(storage_context, page->start, page->length,
                            logger->config.storage_timeout_ms);
        if (result != ELOG_OK) {
            return storage_failure(logger, result);
        }
        page->state = ELOG_PAGE_ERASED;
        page->used = 0u;
        page->valid_end = 0u;
        ++logger->statistics.pages_erased;
        logger->process_state = ELOG_PROCESS_PAGE_BODY;
        return ELOG_OK;

    case ELOG_PROCESS_PAGE_BODY:
        return program_chunk(logger, page->start, logger->page_image,
                             ELOG_PAGE_HEADER_SIZE - g,
                             ELOG_PROCESS_PAGE_VERIFY_BODY);

    case ELOG_PROCESS_PAGE_VERIFY_BODY:
        result = ops->read(storage_context, page->start,
                           logger->verify_image, ELOG_PAGE_HEADER_SIZE - g);
        if (result != ELOG_OK || memcmp(logger->verify_image,
             logger->page_image, ELOG_PAGE_HEADER_SIZE - g) != 0) {
            return storage_failure(logger, result != ELOG_OK ? result :
                                                   ELOG_STORAGE_IO);
        }
        logger->process_state = ELOG_PROCESS_PAGE_COMMIT;
        return ELOG_OK;

    case ELOG_PROCESS_PAGE_COMMIT:
        result = ops->program(storage_context, page->start +
            ELOG_PAGE_HEADER_SIZE - g,
            logger->page_image + ELOG_PAGE_HEADER_SIZE - g, g,
            logger->config.storage_timeout_ms);
        if (result != ELOG_OK) {
            return storage_failure(logger, result);
        }
        logger->process_state = ELOG_PROCESS_PAGE_VERIFY_COMMIT;
        return ELOG_OK;

    case ELOG_PROCESS_PAGE_VERIFY_COMMIT: {
        uint64_t generation;
        result = ops->read(storage_context, page->start,
                           logger->verify_image, ELOG_PAGE_HEADER_SIZE);
        if (result != ELOG_OK) {
            return storage_failure(logger, result);
        }
        result = elog_validate_page_header(logger->verify_image, g,
                                            page->priority, &generation);
        if (result != ELOG_OK || generation !=
            logger->next_generation[page->priority]) {
            return storage_failure(logger, result != ELOG_OK ? result :
                                                   ELOG_STORAGE_IO);
        }
        page->state = ELOG_PAGE_VALID;
        page->generation = generation;
        page->used = ELOG_PAGE_HEADER_SIZE;
        page->valid_end = ELOG_PAGE_HEADER_SIZE;
        logger->active_page[page->priority] = logger->target_page;
        ++logger->next_generation[page->priority];
        logger->storage_full[page->priority] = 0u;
        logger->process_state = ELOG_PROCESS_RECORD_BODY;
        return ELOG_OK;
    }

    case ELOG_PROCESS_RECORD_BODY:
        return program_chunk(logger, page->start + page->used,
                             logger->record_image,
                             logger->record_layout.body_length,
                             ELOG_PROCESS_RECORD_VERIFY_BODY);

    case ELOG_PROCESS_RECORD_VERIFY_BODY:
        result = ops->read(storage_context, page->start + page->used,
                   logger->verify_image, logger->record_layout.body_length);
        if (result != ELOG_OK || memcmp(logger->verify_image,
            logger->record_image, logger->record_layout.body_length) != 0) {
            return storage_failure(logger, result != ELOG_OK ? result :
                                                   ELOG_STORAGE_IO);
        }
        logger->process_state = ELOG_PROCESS_RECORD_COMMIT;
        return ELOG_OK;

    case ELOG_PROCESS_RECORD_COMMIT:
        result = ops->program(storage_context, page->start + page->used +
            (uint32_t)logger->record_layout.commit_offset,
            logger->record_image + logger->record_layout.commit_offset,
            logger->record_layout.commit_length,
            logger->config.storage_timeout_ms);
        if (result != ELOG_OK) {
            return storage_failure(logger, result);
        }
        logger->process_state = ELOG_PROCESS_RECORD_VERIFY_COMMIT;
        return ELOG_OK;

    case ELOG_PROCESS_RECORD_VERIFY_COMMIT: {
        elog_record_t record;
        elog_record_layout_t layout;
        result = ops->read(storage_context, page->start + page->used,
                   logger->verify_image, logger->record_layout.total_length);
        if (result != ELOG_OK) {
            return storage_failure(logger, result);
        }
        result = elog_record_decode(logger->verify_image,
              logger->record_layout.total_length, g, &record, &layout);
        if (result != ELOG_OK || record.sequence != logger->pending_sequence ||
            record.event_id != logger->pending.event_id) {
            return storage_failure(logger, result != ELOG_OK ? result :
                                                   ELOG_STORAGE_IO);
        }
        page->used += (uint32_t)layout.total_length;
        page->valid_end = page->used;
        logger->next_sequence = logger->pending_sequence + 1u;
        ++logger->statistics.written[page->priority];
        if (logger->pending.priority == (uint8_t)ELOG_ROUTINE &&
            logger->pending.type == (uint8_t)ELOG_TYPE_TELEMETRY &&
            logger->pending.length >= 4u &&
            logger->pending.length % 4u == 0u) {
            memcpy(logger->previous_numeric, logger->pending.payload,
                   logger->pending.length);
            logger->previous_numeric_id = logger->pending.event_id;
            logger->previous_numeric_length = logger->pending.length;
            logger->previous_numeric_sequence = logger->pending_sequence;
            logger->previous_numeric_valid = 1u;
            if (record.codec_id == (uint8_t)ELOG_CODEC_DELTA) {
                ++logger->since_snapshot;
            } else {
                logger->since_snapshot = 0u;
            }
        }
        logger->has_pending = 0u;
        logger->process_state = ELOG_PROCESS_IDLE;
        logger->last_error = ELOG_OK;
        return ELOG_OK;
    }

    default:
        return storage_failure(logger, ELOG_INVALID_CONFIG);
    }
}

elog_result_t elog_recover(elog_t *logger)
{
    if (logger == NULL || logger->initialized == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    const elog_result_t result = elog_scan_pages(logger);
    if (result != ELOG_OK) {
        logger->last_error = result;
        return result;
    }
    if (logger->recovered_pending_commit != 0u) {
        logger->has_pending = 0u;
    }
    logger->previous_numeric_valid = 0u;
    logger->since_snapshot = 0u;
    memset(logger->storage_full, 0, sizeof logger->storage_full);
    logger->process_state = ELOG_PROCESS_IDLE;
    logger->target_page = -1;
    logger->body_progress = 0u;
    logger->last_error = ELOG_OK;
    return ELOG_OK;
}

elog_result_t elog_iterate_records(elog_t *logger,
                                   elog_record_visitor_t visitor,
                                   void *visitor_context)
{
    if (logger == NULL || logger->initialized == 0u || visitor == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (logger->process_state != ELOG_PROCESS_IDLE) {
        return ELOG_BUSY;
    }
    const elog_storage_ops_t *ops = logger->storage.ops;
    const uint32_t g = ops->program_granularity(logger->storage.context);
    for (uint8_t i = 0u; i < logger->page_count; ++i) {
        const elog_page_t *page = &logger->pages[i];
        if (page->state != ELOG_PAGE_VALID) {
            continue;
        }
        uint32_t position = ELOG_PAGE_HEADER_SIZE;
        while (position < page->valid_end) {
            elog_result_t result = ops->read(logger->storage.context,
                page->start + position, logger->verify_image,
                ELOG_RECORD_HEADER_SIZE);
            if (result != ELOG_OK) {
                return result;
            }
            elog_record_layout_t layout;
            result = elog_record_layout_from_header(logger->verify_image,
                ELOG_RECORD_HEADER_SIZE, g, &layout);
            if (result != ELOG_OK || layout.total_length > page->valid_end - position) {
                return ELOG_CORRUPT;
            }
            result = ops->read(logger->storage.context, page->start + position,
                               logger->verify_image, layout.total_length);
            if (result != ELOG_OK) {
                return result;
            }
            elog_record_t record;
            result = elog_record_decode(logger->verify_image,
                layout.total_length, g, &record, &layout);
            if (result != ELOG_OK) {
                return result;
            }
            if (visitor(visitor_context, &record) != 0) {
                return ELOG_WOULD_BLOCK;
            }
            position += (uint32_t)layout.total_length;
        }
    }
    return ELOG_OK;
}
