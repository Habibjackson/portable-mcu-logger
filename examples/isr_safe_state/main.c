/** @file
 * @brief Show an ISR-shaped fault path that is safe before logging begins.
 *
 * This host program calls the ISR-shaped function directly. On a device,
 * replace disable_outputs_now() with immediate application hardware shutdown
 * and call on_fault_isr() from one designated interrupt producer.
 */
#include "log.h"
#include "log_storage_ram.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 6u

static uint8_t image[PAGE_COUNT * PAGE_SIZE];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t storage;
static elog_t logger;
static atomic_bool outputs_disabled;

static void disable_outputs_now(void)
{
    /* Application hardware shutdown belongs here, before any logger call. */
    atomic_store_explicit(&outputs_disabled, 1, memory_order_relaxed);
}

static elog_result_t on_fault_isr(uint32_t fault_id, uint64_t timestamp_ms)
{
    const uint8_t snapshot[4] = {0xDCu, 0x05u, 0u, 0u}; /* 1500 mA */
    disable_outputs_now();
    return elog_capture_fault_isr(&logger, fault_id, timestamp_ms,
                                  snapshot, sizeof snapshot);
}

static int count_fault(void *context, const elog_record_t *record)
{
    size_t *count = (size_t *)context;
    if (record->type == ELOG_TYPE_FAULT) {
        ++*count;
    }
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
    config.partition[ELOG_CRITICAL] =
        (elog_region_t){0u, 2u * PAGE_SIZE};
    config.partition[ELOG_EVENT] =
        (elog_region_t){2u * PAGE_SIZE, 2u * PAGE_SIZE};
    config.partition[ELOG_ROUTINE] =
        (elog_region_t){4u * PAGE_SIZE, 2u * PAGE_SIZE};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 2u;
    config.isr_queue_depth = 2u;
    config.max_payload = 16u;
    config.fault_table_depth = 3u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        fputs("logger initialization failed\n", stderr);
        return 1;
    }

    /* The third capture exceeds this example's two-entry ISR ingress queue.
     * Outputs are already disabled even though that record was not queued. */
    const elog_result_t first = on_fault_isr(101u, 100u);
    const elog_result_t second = on_fault_isr(102u, 101u);
    const elog_result_t third = on_fault_isr(103u, 102u);
    if (first != ELOG_OK || second != ELOG_OK ||
        third != ELOG_QUEUE_FULL ||
        !atomic_load_explicit(&outputs_disabled, memory_order_relaxed)) {
        fputs("ISR capture or safe-state check failed\n", stderr);
        return 1;
    }

    /* Run this from a serialized task, not the ISR. The storage backend may
     * block here; flash processing needs a control-safe scheduling window. */
    if (elog_flush(&logger, 100u) != ELOG_OK) {
        fputs("task-side commit failed\n", stderr);
        return 1;
    }
    size_t committed = 0u;
    elog_statistics_t statistics;
    if (elog_iterate_records(&logger, count_fault, &committed) != ELOG_OK ||
        elog_get_statistics(&logger, &statistics) != ELOG_OK ||
        committed != 2u || statistics.dropped_queue[ELOG_CRITICAL] != 1u) {
        fputs("unexpected fault counters\n", stderr);
        return 1;
    }
    printf("outputs_disabled=1 captured=2 rejected=1 committed=%zu\n",
           committed);
    return 0;
}
