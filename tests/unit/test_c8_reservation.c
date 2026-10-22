/** @file
 * @brief Exercise the Blue Pill's 1 KiB page geometry and flash reservation.
 *
 * A patterned application prefix models the firmware region. Logger writes
 * and page rotations must leave every byte of that prefix unchanged.
 */
#include "log.h"
#include "log_storage_ram.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static uint8_t image[64u * 1024u];
static elog_erase_region_t pages[64];
static elog_ram_storage_t ram;
static elog_storage_t storage;
static elog_t logger;
static elog_t rebooted;

/** @brief Describe 1 KiB erase pages and the final 8 KiB C8 logger region. */
static elog_config_t configuration(void)
{
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0xE000u, 2048u};
    config.partition[ELOG_EVENT] = (elog_region_t){0xE800u, 2048u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){0xF000u, 4096u};
    config.critical_queue_depth = 4u;
    config.event_queue_depth = 4u;
    config.routine_queue_depth = 8u;
    config.isr_queue_depth = 4u;
    config.max_payload = 80u;
    config.fault_table_depth = 4u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    return config;
}

int main(void)
{
    for (uint32_t i = 0u; i < 64u; ++i) {
        pages[i] = (elog_erase_region_t){i * 1024u, 1024u};
    }
    CHECK(elog_ram_storage_init(&ram, image, sizeof image,
          pages, 64u, 2u, 64u, &storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    /* Any out-of-range erase or program changes this firmware stand-in. */
    memset(image, 0xA5, 0xE000u);
    const elog_config_t config = configuration();
    CHECK(elog_init(&logger, &config, &storage) == ELOG_OK);
    for (uint32_t i = 0u; i < 240u; ++i) {
        uint8_t sample[24];
        memset(sample, (int)(i & 0xFFu), sizeof sample);
        CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                         1u, sample, sizeof sample) == ELOG_OK);
        CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    }
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.pages_erased > 0u);
    for (uint32_t i = 0u; i < 0xE000u; ++i) {
        CHECK(image[i] == 0xA5u);
    }
    CHECK(elog_init(&rebooted, &config, &storage) == ELOG_OK);
    CHECK(rebooted.next_sequence == 240u);
    puts("C8 geometry and reserved-range rotation: OK");
    return 0;
}
