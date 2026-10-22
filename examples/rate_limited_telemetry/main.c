/** @file
 * @brief Limit routine telemetry without delaying important events.
 *
 * Ten sensor readings arrive at 100 ms intervals. A 250 ms routine interval
 * admits only readings at 100, 400, 700, and 1000 ms. The six intervening
 * attempts return ELOG_WOULD_BLOCK and increase routine_rate_limited; they do
 * not enter a RAM queue or write flash. A state-change EVENT at 500 ms is
 * unaffected by the routine interval and commits ahead of pending routine
 * entries because it has a higher-priority partition.
 *
 * The program uses a newly formatted RAM flash fixture. Queue acceptance is
 * not durability: only elog_flush() advances and verifies the commits.
 * Production code should call elog_process() in an allowed task window and
 * check all queue, storage, and deadline results.
 */
#include "log.h"
#include "log_storage_ram.h"

#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u
#define TELEMETRY_ID UINT32_C(0x5201)
#define STATE_CHANGE_ID UINT32_C(0x5202)

typedef struct {
    size_t routine;
    size_t events;
    uint8_t routine_mask;
} record_summary_t;

static uint8_t image[PAGE_COUNT * PAGE_SIZE];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t storage;
static elog_t logger;
static uint64_t now_ms;

/** @brief Return the simulated monotonic clock used for rate-limit decisions.
 * @param context Address of the caller-owned uint64_t millisecond counter.
 * @return Current uptime. Application code must not use a wrapping 32-bit
 *         tick directly when intervals may cross its wrap point.
 */
static uint64_t timestamp(void *context)
{
    return *(const uint64_t *)context;
}

/** @brief Encode one telemetry field independent of host struct layout.
 * @param target At least four writable bytes.
 * @param value Unsigned value serialized little-endian.
 */
static void put_u32(uint8_t *target, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        target[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Decode a field from an uncompressed committed payload.
 * @param source At least four readable bytes borrowed from the record.
 * @return Unsigned value encoded in little-endian byte order.
 */
static uint32_t get_u32(const uint8_t *source)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)source[i] << (8u * i);
    }
    return value;
}

/** @brief Validate the retained event and each admitted sensor reading.
 * @param context Accumulates record counts and a uniqueness mask.
 * @param record Borrowed record; its payload must not escape this callback.
 * @return Zero to continue, nonzero when any value or metadata is unexpected.
 */
static int inspect_record(void *context, const elog_record_t *record)
{
    record_summary_t *summary = (record_summary_t *)context;
    if (record->type == ELOG_TYPE_EVENT &&
        record->event_id == STATE_CHANGE_ID &&
        record->priority == ELOG_EVENT && record->timestamp == 500u &&
        record->sequence == 0u && record->codec_id == ELOG_CODEC_NONE &&
        record->stored_length == 1u && record->payload[0] == 1u) {
        ++summary->events;
        return 0;
    }
    if (record->type != ELOG_TYPE_TELEMETRY ||
        record->event_id != TELEMETRY_ID ||
        record->priority != ELOG_ROUTINE ||
        record->codec_id != ELOG_CODEC_NONE ||
        record->stored_length != 8u || record->original_length != 8u ||
        record->timestamp < 100u || record->timestamp > 1000u ||
        (record->timestamp - 100u) % 300u != 0u) {
        return 1;
    }
    const uint8_t index = (uint8_t)((record->timestamp - 100u) / 300u);
    const uint8_t bit = (uint8_t)(1u << index);
    const uint32_t sample_number = (uint32_t)(1u + 3u * index);
    if ((summary->routine_mask & bit) != 0u ||
        record->sequence != (uint64_t)(index + 1u) ||
        get_u32(record->payload) != 1000u + sample_number ||
        get_u32(record->payload + 4u) != 200u + sample_number) {
        return 1;
    }
    summary->routine_mask |= bit;
    ++summary->routine;
    return 0;
}

/** @brief Exercise routine rate limiting and independent event priority.
 * @return Zero only when six samples are limited, four routine records and
 *         one event are committed, and no queue or storage drops occur.
 * @note ELOG_WOULD_BLOCK from elog_write() here means deliberate rate limiting;
 *       the same result from elog_process() can mean storage must wait.
 */
int main(void)
{
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        pages[i] = (elog_erase_region_t){i * PAGE_SIZE, PAGE_SIZE};
    }
    if (elog_ram_storage_init(&ram, image, sizeof image, pages, PAGE_COUNT,
                              2u, 64u, &storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK) {
        fputs("storage setup failed\n", stderr);
        return 1;
    }
    elog_config_t config = {0};
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, PAGE_SIZE};
    config.partition[ELOG_EVENT] =
        (elog_region_t){PAGE_SIZE, 2u * PAGE_SIZE};
    config.partition[ELOG_ROUTINE] =
        (elog_region_t){3u * PAGE_SIZE, 2u * PAGE_SIZE};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 4u;
    config.isr_queue_depth = 2u;
    config.max_payload = 24u;
    config.fault_table_depth = 1u;
    config.routine_interval_ms = 250u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_NONE;
    config.timestamp = timestamp;
    config.timestamp_context = &now_ms;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        fputs("logger initialization failed\n", stderr);
        return 1;
    }

    size_t admitted = 0u;
    size_t limited = 0u;
    for (uint32_t sample_number = 1u; sample_number <= 10u;
         ++sample_number) {
        now_ms = 100u * sample_number;
        uint8_t telemetry[8];
        put_u32(telemetry, 1000u + sample_number);
        put_u32(telemetry + 4u, 200u + sample_number);
        const elog_result_t result = elog_write(&logger, ELOG_ROUTINE,
            ELOG_TYPE_TELEMETRY, TELEMETRY_ID, telemetry, sizeof telemetry);
        if (result == ELOG_OK) {
            ++admitted;
        } else if (result == ELOG_WOULD_BLOCK) {
            ++limited; /* Intentional sampling reduction, not queue overflow. */
        } else {
            fprintf(stderr, "unexpected telemetry enqueue result: %d\n",
                    result);
            return 1;
        }
        if (now_ms == 500u) {
            const uint8_t state = 1u;
            if (elog_event(&logger, STATE_CHANGE_ID, &state,
                           sizeof state) != ELOG_OK) {
                fputs("state-change event enqueue failed\n", stderr);
                return 1;
            }
        }
    }
    if (admitted != 4u || limited != 6u ||
        elog_flush(&logger, 200u) != ELOG_OK) {
        fputs("unexpected rate limit or commit result\n", stderr);
        return 1;
    }

    elog_statistics_t statistics;
    elog_status_t status;
    record_summary_t summary = {0};
    if (elog_get_statistics(&logger, &statistics) != ELOG_OK ||
        elog_get_status(&logger, &status) != ELOG_OK ||
        elog_iterate_records(&logger, inspect_record, &summary) != ELOG_OK ||
        statistics.routine_rate_limited != 6u ||
        statistics.dropped_queue[ELOG_ROUTINE] != 0u ||
        statistics.dropped_storage[ELOG_ROUTINE] != 0u ||
        statistics.written[ELOG_ROUTINE] != 4u ||
        statistics.written[ELOG_EVENT] != 1u ||
        status.queued[ELOG_ROUTINE] != 0u ||
        status.queued[ELOG_EVENT] != 0u ||
        summary.routine != 4u || summary.events != 1u ||
        summary.routine_mask != 0x0Fu) {
        fputs("retained records or counters were unexpected\n", stderr);
        return 1;
    }
    printf("samples=10 routine_saved=%zu rate_limited=%zu events=%zu\n",
           summary.routine, limited, summary.events);
    return 0;
}
