/** @file
 * @brief Exercise queue priority, fault transitions, rotation, and recovery.
 *
 * These host checks use RAM flash geometry and assert retained records and
 * counters after normal operation, overflow, and capacity boundaries.
 */
#include "log.h"
#include "log_storage_ram.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    uint8_t bytes[8u * 256u];
    elog_erase_region_t regions[8];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    uint64_t time;
} fixture_t;

typedef struct {
    uint8_t type[32];
    uint8_t priority[32];
    uint64_t sequence[32];
    uint32_t event_id[32];
    size_t count;
} records_t;

static uint64_t clock_ms(void *context)
{
    return *(uint64_t *)context;
}

static elog_config_t configuration(fixture_t *fixture)
{
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 512u};
    config.partition[ELOG_EVENT] = (elog_region_t){512u, 512u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){1024u, 1024u};
    config.critical_queue_depth = 4u;
    config.event_queue_depth = 4u;
    config.routine_queue_depth = 4u;
    config.isr_queue_depth = 4u;
    config.max_payload = ELOG_MAX_PAYLOAD;
    config.fault_table_depth = 4u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.timestamp = clock_ms;
    config.timestamp_context = &fixture->time;
    config.fault_repeat_interval_ms = 100u;
    return config;
}

static int fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    for (uint32_t i = 0u; i < 8u; ++i) {
        fixture->regions[i].start = i * 256u;
        fixture->regions[i].length = 256u;
    }
    if (elog_ram_storage_init(&fixture->ram, fixture->bytes,
         sizeof fixture->bytes, fixture->regions, 8u, 2u, 64u,
         &fixture->storage) != ELOG_OK) {
        return 1;
    }
    return elog_ram_storage_format(&fixture->ram) == ELOG_OK ? 0 : 1;
}

static int visit(void *context, const elog_record_t *record)
{
    records_t *records = (records_t *)context;
    if (records->count >= 32u) {
        return 1;
    }
    const size_t i = records->count++;
    records->type[i] = record->type;
    records->priority[i] = record->priority;
    records->sequence[i] = record->sequence;
    records->event_id[i] = record->event_id;
    return 0;
}

/** @brief Confirm critical/event scheduling and read-only reboot scanning. */
static int test_priority_and_reboot(void)
{
    fixture_t fixture;
    static elog_t logger;
    static elog_t rebooted;
    records_t records;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    const uint8_t telemetry[] = {1u, 2u, 3u, 4u};
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     11u, telemetry, sizeof telemetry) == ELOG_OK);
    CHECK(elog_fault(&logger, 99u, telemetry, sizeof telemetry) == ELOG_OK);
    CHECK(elog_event(&logger, 21u, NULL, 0u) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    elog_statistics_t statistics;
    CHECK(elog_get_statistics(&logger, &statistics) == ELOG_OK);
    CHECK(statistics.written[ELOG_CRITICAL] == 1u);
    CHECK(statistics.written[ELOG_EVENT] == 1u);
    CHECK(statistics.written[ELOG_ROUTINE] == 1u);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&logger, visit, &records) == ELOG_OK);
    CHECK(records.count == 3u);
    /* Physical iteration is partition order; sequences show scheduling. */
    CHECK(records.sequence[0] == 0u && records.type[0] == ELOG_TYPE_FAULT);
    CHECK(records.sequence[1] == 1u && records.type[1] == ELOG_TYPE_EVENT);
    CHECK(records.sequence[2] == 2u && records.type[2] == ELOG_TYPE_TELEMETRY);
    CHECK(elog_init(&rebooted, &config, &fixture.storage) == ELOG_OK);
    CHECK(rebooted.next_sequence == 3u);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&rebooted, visit, &records) == ELOG_OK);
    CHECK(records.count == 3u);
    return 0;
}

/** @brief Check bounded fault and ISR queues plus repeated-fault accounting. */
static int test_overflow_faults_and_isr(void)
{
    fixture_t fixture;
    static elog_t logger;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    config.critical_queue_depth = 1u;
    config.routine_queue_depth = 2u;
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    CHECK(elog_fault(&logger, 7u, NULL, 0u) == ELOG_OK);
    CHECK(elog_fault(&logger, 8u, NULL, 0u) == ELOG_QUEUE_FULL);
    fixture.time = 10u;
    CHECK(elog_fault(&logger, 7u, NULL, 0u) == ELOG_OK);
    fixture.time = 120u;
    CHECK(elog_fault(&logger, 7u, NULL, 0u) == ELOG_QUEUE_FULL);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_fault(&logger, 7u, NULL, 0u) == ELOG_OK);
    CHECK(elog_fault_recovered(&logger, 7u, NULL, 0u) == ELOG_QUEUE_FULL);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_fault_recovered(&logger, 7u, NULL, 0u) == ELOG_OK);
    CHECK(elog_capture_fault_isr(&logger, 10u, 123u, NULL, 0u) == ELOG_OK);
    CHECK(elog_capture_fault_isr(&logger, 11u, 124u, NULL, 0u) == ELOG_OK);
    CHECK(elog_capture_fault_isr(&logger, 12u, 125u, NULL, 0u) == ELOG_OK);
    CHECK(elog_capture_fault_isr(&logger, 13u, 126u, NULL, 0u) == ELOG_OK);
    CHECK(elog_capture_fault_isr(&logger, 14u, 127u, NULL, 0u) == ELOG_QUEUE_FULL);
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.dropped_queue[ELOG_CRITICAL] >= 3u);
    return 0;
}

/** @brief Seal an interrupted tail and continue on a different erase page. */
static int test_torn_tail_and_rotation(void)
{
    fixture_t fixture;
    static elog_t logger;
    static elog_t rebooted;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    const uint8_t body[16] = {0u};
    CHECK(elog_event(&logger, 1u, body, sizeof body) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_event(&logger, 2u, body, sizeof body) == ELOG_OK);
    /* One process step programs the body; stop before the commit. */
    CHECK(elog_process(&logger) == ELOG_OK);
    CHECK(elog_init(&rebooted, &config, &fixture.storage) == ELOG_OK);
    records_t records;
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&rebooted, visit, &records) == ELOG_OK);
    CHECK(records.count == 1u);
    CHECK(rebooted.pages[rebooted.active_page[ELOG_EVENT]].used == 256u);
    CHECK(elog_event(&rebooted, 3u, body, sizeof body) == ELOG_OK);
    CHECK(elog_flush(&rebooted, 100u) == ELOG_OK);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&rebooted, visit, &records) == ELOG_OK);
    CHECK(records.count == 2u);
    return 0;
}

/** @brief Reject unsupported or internally inconsistent runtime policies. */
static int test_bad_config(void)
{
    fixture_t fixture;
    static elog_t logger;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    config.partition[ELOG_CRITICAL].offset = 1u;
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_INVALID_CONFIG);
    config = configuration(&fixture);
    config.max_payload = 255u;
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_INVALID_CONFIG);
    return 0;
}

/** @brief Verify routine replacement reports and counts the displaced entry. */
static int test_routine_queue_drop(void)
{
    fixture_t fixture;
    static elog_t logger;
    records_t records;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    config.routine_queue_depth = 2u;
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     1u, NULL, 0u) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     2u, NULL, 0u) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     3u, NULL, 0u) == ELOG_QUEUED_WITH_DROP);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&logger, visit, &records) == ELOG_OK);
    CHECK(records.count == 2u);
    CHECK(records.event_id[0] == 2u && records.event_id[1] == 3u);
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.dropped_queue[ELOG_ROUTINE] == 1u);
    return 0;
}

/** @brief Check the optional bounded telemetry snapshot around an ISR fault. */
static int test_isr_fault_telemetry_window(void)
{
    fixture_t fixture;
    static elog_t logger;
    records_t records;
    const uint8_t sample[4] = {1u, 0u, 0u, 0u};
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    config.routine_queue_depth = 8u;
    config.pre_fault_depth = 2u;
    config.post_fault_depth = 2u;
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    for (uint32_t id = 1u; id <= 3u; ++id) {
        CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                         id, sample, sizeof sample) == ELOG_OK);
    }
    CHECK(elog_capture_fault_isr(&logger, 99u, 10u, NULL, 0u) == ELOG_OK);
    CHECK(elog_process(&logger) == ELOG_OK);
    CHECK(logger.pre_frozen == 1u && logger.pre_dump_count == 2u);
    for (uint32_t id = 4u; id <= 5u; ++id) {
        CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                         id, sample, sizeof sample) == ELOG_OK);
    }
    CHECK(elog_flush(&logger, 1000u) == ELOG_OK);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&logger, visit, &records) == ELOG_OK);
    CHECK(records.count == 10u);
    uint8_t pre = 0u;
    uint8_t post = 0u;
    for (size_t i = 0u; i < records.count; ++i) {
        if (records.type[i] == ELOG_TYPE_PRE_FAULT) {
            CHECK(records.event_id[i] == 2u + pre);
            ++pre;
        }
        if (records.type[i] == ELOG_TYPE_POST_FAULT) {
            CHECK(records.event_id[i] == 4u + post);
            ++post;
        }
    }
    CHECK(pre == 2u && post == 2u);
    return 0;
}

/** @brief Keep event and routine lanes usable after critical pages fill. */
static int test_critical_full_does_not_block_other_lanes(void)
{
    fixture_t fixture;
    static elog_t logger;
    static elog_t rebooted;
    records_t records;
    const uint8_t body[ELOG_MAX_PAYLOAD] = {0u};
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    /* A maximal record occupies more than half of each 256-byte page. */
    for (uint32_t id = 1u; id <= 2u; ++id) {
        CHECK(elog_write(&logger, ELOG_CRITICAL, ELOG_TYPE_FAULT,
                         id, body, sizeof body) == ELOG_OK);
        CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    }
    CHECK(elog_write(&logger, ELOG_CRITICAL, ELOG_TYPE_FAULT,
                     3u, body, sizeof body) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_STORAGE_FULL);
    CHECK(elog_write(&logger, ELOG_CRITICAL, ELOG_TYPE_FAULT,
                     4u, body, sizeof body) == ELOG_STORAGE_FULL);
    CHECK(elog_fault(&logger, 6u, NULL, 0u) == ELOG_STORAGE_FULL);
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.dropped_storage[ELOG_CRITICAL] == 1u);
    elog_status_t status;
    CHECK(elog_get_status(&logger, &status) == ELOG_OK);
    CHECK(status.storage_full[ELOG_CRITICAL] == 1u);
    CHECK(elog_event(&logger, 5u, NULL, 0u) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_init(&rebooted, &config, &fixture.storage) == ELOG_OK);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&rebooted, visit, &records) == ELOG_OK);
    CHECK(records.count == 3u);
    CHECK(records.event_id[0] == 1u && records.event_id[1] == 2u);
    CHECK(records.event_id[2] == 5u);
    return 0;
}

/** @brief Check oldest-page rotation and wrapping sequence comparisons. */
static int test_rotation_and_sequence_wrap(void)
{
    fixture_t fixture;
    static elog_t logger;
    static elog_t rebooted;
    records_t records;
    const uint8_t body[16] = {0u};
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    CHECK(elog_init(&logger, &config, &fixture.storage) == ELOG_OK);
    logger.next_sequence = UINT64_MAX - 2u;
    for (uint32_t id = 0u; id < 60u; ++id) {
        CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                         id, body, sizeof body) == ELOG_OK);
        CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    }
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.pages_erased >= 4u);
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0u;
    for (uint8_t i = 0u; i < logger.page_count; ++i) {
        const elog_page_t *page = &logger.pages[i];
        if (page->priority == (uint8_t)ELOG_ROUTINE) {
            CHECK(page->state == 1u); /* ELOG_PAGE_VALID */
            if (page->generation < lowest) {
                lowest = page->generation;
            }
            if (page->generation > highest) {
                highest = page->generation;
            }
        }
    }
    CHECK(highest - lowest <= 3u);
    CHECK(elog_init(&rebooted, &config, &fixture.storage) == ELOG_OK);
    CHECK(rebooted.next_sequence == UINT64_MAX - 2u + 60u);
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&rebooted, visit, &records) == ELOG_OK);
    CHECK(records.count > 0u && records.count <= 12u);
    uint32_t newest_id = 0u;
    for (size_t i = 0u; i < records.count; ++i) {
        if (records.event_id[i] > newest_id) {
            newest_id = records.event_id[i];
        }
    }
    CHECK(newest_id == 59u);
    return 0;
}

static uint32_t odd_program_size(void *context)
{
    (void)context;
    return 3u;
}

/** @brief Reject backend program geometry the journal cannot safely use. */
static int test_bad_storage_geometry(void)
{
    fixture_t fixture;
    static elog_t logger;
    CHECK(fixture_init(&fixture) == 0);
    elog_config_t config = configuration(&fixture);
    elog_storage_ops_t bad_ops = *fixture.storage.ops;
    bad_ops.max_program_size = odd_program_size;
    elog_storage_t bad_storage = {&bad_ops, fixture.storage.context};
    CHECK(elog_init(&logger, &config, &bad_storage) == ELOG_INVALID_CONFIG);
    return 0;
}

int main(void)
{
    if (test_priority_and_reboot() != 0 ||
        test_overflow_faults_and_isr() != 0 ||
        test_torn_tail_and_rotation() != 0 ||
        test_routine_queue_drop() != 0 ||
        test_isr_fault_telemetry_window() != 0 ||
        test_critical_full_does_not_block_other_lanes() != 0 ||
        test_rotation_and_sequence_wrap() != 0 ||
        test_bad_storage_geometry() != 0 ||
        test_bad_config() != 0) {
        return 1;
    }
    printf("core and recovery: OK (elog_t=%zu bytes)\n", sizeof(elog_t));
    return 0;
}
