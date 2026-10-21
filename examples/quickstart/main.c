/** @file
 * @brief Small, self-contained example of enqueue, commit, and readback.
 *
 * This RAM image models erased flash. It exists only for this process and is
 * formatted once when the example starts. Real nonvolatile storage must not
 * be formatted on every boot.
 */
#include "log.h"
#include "log_storage_ram.h"

#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u
#define TELEMETRY_ID UINT32_C(0x1001)
#define STARTED_ID UINT32_C(0x2001)

static uint8_t image[PAGE_COUNT * PAGE_SIZE];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t storage;
static elog_t logger;
static uint64_t now_ms;

static uint64_t timestamp(void *context)
{
    return *(const uint64_t *)context;
}

static void put_u32(uint8_t *target, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        target[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint32_t get_u32(const uint8_t *source)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)source[i] << (8u * i);
    }
    return value;
}

static int show_record(void *context, const elog_record_t *record)
{
    size_t *count = (size_t *)context;
    ++*count;
    printf("seq=%llu type=%u id=0x%04lx time=%llu ms",
           (unsigned long long)record->sequence, (unsigned int)record->type,
           (unsigned long)record->event_id,
           (unsigned long long)record->timestamp);
    /* This example uses NONE. With DELTA/VARINT, decode the encoded payload
     * using the codec or tools/decode_logs.py before interpreting its fields. */
    if (record->event_id == TELEMETRY_ID &&
        record->codec_id == ELOG_CODEC_NONE && record->stored_length == 8u) {
        printf(" rpm=%lu pressure_deci_kpa=%lu",
               (unsigned long)get_u32(record->payload),
               (unsigned long)get_u32(record->payload + 4u));
    }
    putchar('\n');
    return 0;
}

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
    config.fault_table_depth = 2u;
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

    uint8_t telemetry[8];
    put_u32(telemetry, 1250u);      /* motor speed, rpm */
    put_u32(telemetry + 4u, 218u); /* pressure, 0.1 kPa */
    now_ms = 100u;
    if (elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                   TELEMETRY_ID, telemetry, sizeof telemetry) != ELOG_OK) {
        fputs("telemetry queue failed\n", stderr);
        return 1;
    }
    now_ms = 110u;
    if (elog_event(&logger, STARTED_ID, "started", sizeof "started" - 1u)
        != ELOG_OK) {
        fputs("event queue failed\n", stderr);
        return 1;
    }
    /* Queue acceptance only copies bytes into RAM. Flush is what commits
     * records to the storage model. In firmware, budget process calls in a
     * task that is safe to block on the chosen backend. */
    if (elog_flush(&logger, 100u) != ELOG_OK) {
        fputs("commit failed\n", stderr);
        return 1;
    }

    size_t count = 0u;
    if (elog_iterate_records(&logger, show_record, &count) != ELOG_OK ||
        count != 2u) {
        fputs("readback failed\n", stderr);
        return 1;
    }
    elog_statistics_t statistics;
    if (elog_get_statistics(&logger, &statistics) != ELOG_OK ||
        statistics.written[ELOG_EVENT] != 1u ||
        statistics.written[ELOG_ROUTINE] != 1u) {
        fputs("unexpected commit counts\n", stderr);
        return 1;
    }
    printf("committed=%zu (event is sequenced before pending routine data)\n",
           count);
    return 0;
}
