#include "log.h"
#include "log_storage_ram.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

#define PAGE_COUNT 64u
#define SAMPLE_COUNT 500u

static uint8_t memory[PAGE_COUNT * 2048u];
static elog_erase_region_t regions[PAGE_COUNT];
static elog_t logger;
static atomic_bool producer_done;
static atomic_bool producer_error;

static DWORD WINAPI produce_faults(LPVOID context)
{
    (void)context;
    for (uint32_t id = 0u; id < SAMPLE_COUNT; ++id) {
        for (;;) {
            const elog_result_t result = elog_capture_fault_isr(
                &logger, id, id, NULL, 0u);
            if (result == ELOG_OK) {
                break;
            }
            if (result != ELOG_QUEUE_FULL) {
                atomic_store(&producer_error, true);
                atomic_store(&producer_done, true);
                return 1u;
            }
            Sleep(0u);
        }
    }
    atomic_store(&producer_done, true);
    return 0u;
}

static int count_records(void *context, const elog_record_t *record)
{
    uint32_t *next = (uint32_t *)context;
    if (record->priority != (uint8_t)ELOG_CRITICAL ||
        record->event_id != *next || record->sequence != *next) {
        return 1;
    }
    ++*next;
    return 0;
}

int main(void)
{
    elog_ram_storage_t ram;
    elog_storage_t storage;
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        regions[i] = (elog_erase_region_t){i * 2048u, 2048u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, PAGE_COUNT, 2u, 64u, &storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 60u * 2048u};
    config.partition[ELOG_EVENT] = (elog_region_t){60u * 2048u, 2u * 2048u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){62u * 2048u, 2u * 2048u};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 2u;
    config.isr_queue_depth = 4u;
    config.max_payload = 80u;
    config.fault_table_depth = 2u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    CHECK(elog_init(&logger, &config, &storage) == ELOG_OK);
    atomic_init(&producer_done, false);
    atomic_init(&producer_error, false);
    HANDLE thread = CreateThread(NULL, 0u, produce_faults, NULL, 0u, NULL);
    CHECK(thread != NULL);
    const ULONGLONG deadline = GetTickCount64() + 15000u;
    while (!atomic_load(&producer_done) ||
           atomic_load(&logger.isr_head) != atomic_load(&logger.isr_tail) ||
           logger.has_pending != 0u || logger.process_state != 0u) {
        const elog_result_t result = elog_process(&logger);
        if (result != ELOG_OK && result != ELOG_EMPTY) {
            atomic_store(&producer_error, true);
            break;
        }
        if (GetTickCount64() > deadline) {
            atomic_store(&producer_error, true);
            break;
        }
    }
    CHECK(WaitForSingleObject(thread, 15000u) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CHECK(!atomic_load(&producer_error));
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    elog_statistics_t stats;
    CHECK(elog_get_statistics(&logger, &stats) == ELOG_OK);
    CHECK(stats.written[ELOG_CRITICAL] == SAMPLE_COUNT);
    uint32_t next = 0u;
    CHECK(elog_iterate_records(&logger, count_records, &next) == ELOG_OK);
    CHECK(next == SAMPLE_COUNT);
    puts("concurrent ISR producer/task consumer: OK");
    return 0;
}
