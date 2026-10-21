/** @file
 * @brief Simulate a torn flash write and show read-only reboot recovery.
 *
 * The simulated flash wrapper changes a few bytes of one program call, then
 * reports an error. A new logger instance scans the retained image without
 * erasing it. Real power-cut behavior still needs hardware testing.
 */
#include "log.h"
#include "log_storage_ram.h"
#include "log_storage_sim.h"

#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u

static uint8_t image[PAGE_COUNT * PAGE_SIZE];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t ram_storage;
static elog_sim_flash_t simulated_flash;
static elog_storage_t storage;
static elog_t before_reset;
static elog_t after_reset;

typedef struct {
    uint32_t ids[3];
    size_t count;
} seen_records_t;

static int collect_event(void *context, const elog_record_t *record)
{
    seen_records_t *seen = (seen_records_t *)context;
    if (record->type != ELOG_TYPE_EVENT || seen->count >= 3u) {
        return 1;
    }
    seen->ids[seen->count++] = record->event_id;
    return 0;
}

static elog_config_t configuration(void)
{
    elog_config_t config = {0};
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, PAGE_SIZE};
    config.partition[ELOG_EVENT] =
        (elog_region_t){PAGE_SIZE, 2u * PAGE_SIZE};
    config.partition[ELOG_ROUTINE] =
        (elog_region_t){3u * PAGE_SIZE, 2u * PAGE_SIZE};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 3u;
    config.routine_queue_depth = 2u;
    config.isr_queue_depth = 2u;
    config.max_payload = 24u;
    config.fault_table_depth = 2u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_NONE;
    return config;
}

int main(void)
{
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        pages[i] = (elog_erase_region_t){i * PAGE_SIZE, PAGE_SIZE};
    }
    /* Format this brand-new host fixture once. Neither backend init nor
     * logger init will format it on the simulated reboot below. */
    if (elog_ram_storage_init(&ram, image, sizeof image, pages, PAGE_COUNT,
                              2u, 64u, &ram_storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK ||
        elog_sim_flash_init(&simulated_flash, &ram, &ram_storage, &storage)
            != ELOG_OK) {
        fputs("storage setup failed\n", stderr);
        return 1;
    }
    const elog_config_t config = configuration();
    if (elog_init(&before_reset, &config, &storage) != ELOG_OK ||
        elog_event(&before_reset, 1001u, "boot", 4u) != ELOG_OK ||
        elog_flush(&before_reset, 100u) != ELOG_OK) {
        fputs("first commit failed\n", stderr);
        return 1;
    }

    /* Fail the next program operation after it has changed ten bytes. Its
     * record cannot be committed, even though the enqueue returned ELOG_OK. */
    elog_sim_flash_fail_program(&simulated_flash,
                                simulated_flash.program_calls + 1u, 10u,
                                ELOG_STORAGE_TIMEOUT);
    if (elog_event(&before_reset, 1002u, "interrupted", 11u) != ELOG_OK ||
        elog_process(&before_reset) != ELOG_STORAGE_TIMEOUT) {
        fputs("fault injection did not interrupt a write\n", stderr);
        return 1;
    }

    elog_sim_flash_power_cycle(&simulated_flash); /* Image bytes survive. */
    const elog_result_t recovery = elog_init(&after_reset, &config, &storage);
    if (recovery != ELOG_OK) {
        fprintf(stderr, "reboot scan failed: %d\n", recovery);
        return 1;
    }
    seen_records_t seen = {0};
    if (elog_iterate_records(&after_reset, collect_event, &seen) != ELOG_OK ||
        seen.count != 1u || seen.ids[0] != 1001u) {
        fputs("torn event was incorrectly recovered\n", stderr);
        return 1;
    }
    printf("after reboot: committed event %lu; interrupted event absent\n",
           (unsigned long)seen.ids[0]);

    /* The torn tail is sealed in RAM metadata; a second event page lets the
     * logger commit new records without erasing the retained first event. */
    if (elog_event(&after_reset, 1003u, "resumed", 7u) != ELOG_OK ||
        elog_flush(&after_reset, 100u) != ELOG_OK) {
        fputs("post-reboot commit failed\n", stderr);
        return 1;
    }
    seen = (seen_records_t){0};
    if (elog_iterate_records(&after_reset, collect_event, &seen) != ELOG_OK ||
        seen.count != 2u || seen.ids[0] != 1001u || seen.ids[1] != 1003u) {
        fputs("unexpected recovered event set\n", stderr);
        return 1;
    }
    printf("after new commit: event IDs %lu, %lu\n",
           (unsigned long)seen.ids[0], (unsigned long)seen.ids[1]);
    return 0;
}
