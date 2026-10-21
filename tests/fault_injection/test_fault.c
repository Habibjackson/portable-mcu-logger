#include "log.h"
#include "log_storage_ram.h"
#include "log_storage_sim.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    uint32_t ids[16];
    size_t count;
} observed_t;

static int observe(void *context, const elog_record_t *record)
{
    observed_t *observed = (observed_t *)context;
    if (observed->count >= 16u) {
        return 1;
    }
    observed->ids[observed->count++] = record->event_id;
    return 0;
}

static elog_config_t config(void)
{
    elog_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.partition[ELOG_CRITICAL] = (elog_region_t){0u, 512u};
    cfg.partition[ELOG_EVENT] = (elog_region_t){512u, 512u};
    cfg.partition[ELOG_ROUTINE] = (elog_region_t){1024u, 512u};
    cfg.critical_queue_depth = 2u;
    cfg.event_queue_depth = 2u;
    cfg.routine_queue_depth = 2u;
    cfg.isr_queue_depth = 2u;
    cfg.max_payload = 80u;
    cfg.fault_table_depth = 2u;
    cfg.storage_timeout_ms = 1u;
    cfg.recover_on_init = 1u;
    cfg.require_crc = 1u;
    return cfg;
}

static int one_cut(uint32_t call, size_t partial)
{
    uint8_t memory[6u * 256u];
    elog_erase_region_t regions[6];
    elog_ram_storage_t ram;
    elog_storage_t ram_storage;
    elog_sim_flash_t sim;
    elog_storage_t storage;
    static elog_t before;
    static elog_t after;
    for (uint32_t i = 0u; i < 6u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 6u, 2u, 64u, &ram_storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(elog_sim_flash_init(&sim, &ram, &ram_storage, &storage) == ELOG_OK);
    elog_config_t cfg = config();
    CHECK(elog_init(&before, &cfg, &storage) == ELOG_OK);
    const uint8_t payload[16] = {0u};
    CHECK(elog_event(&before, 41u, payload, sizeof payload) == ELOG_OK);
    elog_sim_flash_fail_program(&sim, call, partial, ELOG_STORAGE_TIMEOUT);
    CHECK(elog_flush(&before, 100u) == ELOG_STORAGE_TIMEOUT);
    elog_sim_flash_power_cycle(&sim);
    CHECK(elog_init(&after, &cfg, &storage) == ELOG_OK);
    observed_t observed;
    memset(&observed, 0, sizeof observed);
    CHECK(elog_iterate_records(&after, observe, &observed) == ELOG_OK);
    CHECK(observed.count == (call == 4u && partial == 2u ? 1u : 0u));
    if (observed.count != 0u) {
        CHECK(observed.ids[0] == 41u);
    }
    CHECK(elog_event(&after, 42u, payload, sizeof payload) == ELOG_OK);
    CHECK(elog_flush(&after, 100u) == ELOG_OK);
    memset(&observed, 0, sizeof observed);
    CHECK(elog_iterate_records(&after, observe, &observed) == ELOG_OK);
    CHECK(observed.ids[observed.count - 1u] == 42u);
    return 0;
}

static int test_crc_corruption(void)
{
    uint8_t memory[6u * 256u];
    elog_erase_region_t regions[6];
    elog_ram_storage_t ram;
    elog_storage_t ram_storage;
    elog_sim_flash_t sim;
    elog_storage_t storage;
    static elog_t before;
    static elog_t after;
    for (uint32_t i = 0u; i < 6u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 6u, 2u, 64u, &ram_storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(elog_sim_flash_init(&sim, &ram, &ram_storage, &storage) == ELOG_OK);
    elog_config_t cfg = config();
    CHECK(elog_init(&before, &cfg, &storage) == ELOG_OK);
    CHECK(elog_event(&before, 51u, "abc", 3u) == ELOG_OK);
    CHECK(elog_flush(&before, 100u) == ELOG_OK);
    CHECK(elog_sim_flash_corrupt(&sim, 512u + 32u + 36u, 0u) == ELOG_OK);
    CHECK(elog_init(&after, &cfg, &storage) == ELOG_OK);
    observed_t observed;
    memset(&observed, 0, sizeof observed);
    CHECK(elog_iterate_records(&after, observe, &observed) == ELOG_OK);
    CHECK(observed.count == 0u);
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&after, &stats) == ELOG_OK);
    CHECK(stats.corrupt_records >= 1u);
    return 0;
}

static int one_erase_cut(size_t partial)
{
    uint8_t memory[6u * 256u];
    elog_erase_region_t regions[6];
    elog_ram_storage_t ram;
    elog_storage_t ram_storage;
    elog_sim_flash_t sim;
    elog_storage_t storage;
    static elog_t before;
    static elog_t after;
    const uint8_t payload[80] = {0u};
    for (uint32_t i = 0u; i < 6u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 6u, 2u, 64u, &ram_storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(elog_sim_flash_init(&sim, &ram, &ram_storage, &storage) == ELOG_OK);
    elog_config_t cfg = config();
    CHECK(elog_init(&before, &cfg, &storage) == ELOG_OK);
    for (uint32_t id = 1u; id <= 2u; ++id) {
        CHECK(elog_event(&before, id, payload, sizeof payload) == ELOG_OK);
        CHECK(elog_flush(&before, 100u) == ELOG_OK);
    }
    CHECK(elog_event(&before, 3u, payload, sizeof payload) == ELOG_OK);
    elog_sim_flash_fail_erase(&sim, 1u, partial, ELOG_STORAGE_TIMEOUT);
    CHECK(elog_flush(&before, 100u) == ELOG_STORAGE_TIMEOUT);
    elog_sim_flash_power_cycle(&sim);
    CHECK(elog_init(&after, &cfg, &storage) == ELOG_OK);
    observed_t observed;
    memset(&observed, 0, sizeof observed);
    CHECK(elog_iterate_records(&after, observe, &observed) == ELOG_OK);
    CHECK(observed.count == (partial == 0u ? 2u : 1u));
    CHECK(observed.ids[observed.count - 1u] == 2u);
    CHECK(elog_event(&after, 4u, payload, sizeof payload) == ELOG_OK);
    CHECK(elog_flush(&after, 100u) == ELOG_OK);
    memset(&observed, 0, sizeof observed);
    CHECK(elog_iterate_records(&after, observe, &observed) == ELOG_OK);
    CHECK(observed.count == 2u);
    CHECK((observed.ids[0] == 4u || observed.ids[1] == 4u));
    return 0;
}

static int test_unknown_versions(void)
{
    uint8_t memory[6u * 256u];
    elog_erase_region_t regions[6];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    static elog_t before;
    static elog_t after;
    for (uint32_t i = 0u; i < 6u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 6u, 2u, 64u, &storage) == ELOG_OK);
    elog_config_t cfg = config();
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(elog_init(&before, &cfg, &storage) == ELOG_OK);
    CHECK(elog_event(&before, 7u, NULL, 0u) == ELOG_OK);
    CHECK(elog_flush(&before, 100u) == ELOG_OK);
    memory[512u + 4u] = 2u;
    CHECK(elog_init(&after, &cfg, &storage) == ELOG_UNSUPPORTED);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(elog_init(&before, &cfg, &storage) == ELOG_OK);
    CHECK(elog_event(&before, 7u, NULL, 0u) == ELOG_OK);
    CHECK(elog_flush(&before, 100u) == ELOG_OK);
    memory[512u + 32u + 4u] = 2u;
    CHECK(elog_init(&after, &cfg, &storage) == ELOG_UNSUPPORTED);
    return 0;
}

int main(void)
{
    CHECK(one_cut(1u, 2u) == 0);  /* page header body */
    CHECK(one_cut(2u, 1u) == 0);  /* page header commit */
    CHECK(one_cut(3u, 10u) == 0); /* record body */
    CHECK(one_cut(4u, 1u) == 0);  /* record commit, torn */
    CHECK(one_cut(4u, 2u) == 0);  /* record commit, complete but timeout */
    CHECK(test_crc_corruption() == 0);
    CHECK(one_erase_cut(0u) == 0);
    CHECK(one_erase_cut(128u) == 0);
    CHECK(one_erase_cut(256u) == 0);
    CHECK(test_unknown_versions() == 0);
    puts("injected power cuts and CRC corruption: OK");
    return 0;
}
