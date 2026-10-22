/** @file
 * @brief Sample sensors continuously but persist only a fault window.
 *
 * Simulated flash program-call counts prove that healthy sampling does not
 * write flash. The first fault reading, later fault readings, and one
 * recovery-boundary reading are committed. No pre-fault history is kept.
 * The RAM image is formatted once as a new host fixture.
 */
#include "log.h"
#include "log_storage_ram.h"
#include "log_storage_sim.h"

#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u
#define FAULT_ID UINT32_C(0x1001)
#define TELEMETRY_ID UINT32_C(1)

static uint8_t image[PAGE_SIZE * PAGE_COUNT];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t ram_storage;
static elog_sim_flash_t sim;
static elog_storage_t storage;
static elog_t logger;
static uint64_t now_ms;
static uint8_t fault_active;

typedef struct {
    size_t faults;
    size_t recoveries;
    size_t telemetry;
    uint64_t first_sensor_time;
    uint64_t last_sensor_time;
    uint8_t sensor_mask;
} record_counts_t;

/** @brief Return the simulated monotonic clock for task-side logger calls.
 * @param context Address of the caller-owned now_ms counter.
 * @return Millisecond uptime at enqueue time.
 */
static uint64_t timestamp(void *context)
{
    return *(const uint64_t *)context;
}

/** @brief Encode a sensor field as little-endian bytes.
 * @param target At least four writable bytes in the record payload.
 * @param value Unsigned sensor value to serialize without struct padding.
 */
static void put_u32(uint8_t *target, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        target[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Decode an uncompressed sensor field during record readback.
 * @param source At least four payload bytes valid for this visitor call.
 * @return Unsigned little-endian value.
 */
static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) |
           ((uint32_t)source[3] << 24u);
}

/** @brief Advance one 100 ms sample and queue records only in a fault window.
 * @param rpm Measured motor speed for this synthetic sample.
 * @param pressure_deci_kpa Measured pressure in tenths of a kPa.
 * @param fault_present Nonzero while the application detects a fault.
 * @return ELOG_OK when no record is needed or all needed records are queued;
 *         otherwise the first enqueue failure.
 * @note This function does not commit flash. A real app must shut down outputs
 *       before calling elog_fault() and must process queued work later.
 */
static elog_result_t sample_sensor(uint32_t rpm, uint32_t pressure_deci_kpa,
                                   uint8_t fault_present)
{
    now_ms += 100u;
    const uint8_t was_active = fault_active;
    uint8_t payload[8];
    put_u32(payload, rpm);
    put_u32(payload + 4u, pressure_deci_kpa);

    /* Real actuator shutdown belongs before the first elog_fault() call.
     * Logging success must never control whether the application is safe. */
    if (fault_present != 0u && was_active == 0u) {
        const elog_result_t result = elog_fault(&logger, FAULT_ID,
                                                payload, sizeof payload);
        if (result != ELOG_OK) {
            return result;
        }
        fault_active = 1u;
    } else if (fault_present == 0u && was_active != 0u) {
        const elog_result_t result = elog_fault_recovered(&logger, FAULT_ID,
                                                           NULL, 0u);
        if (result != ELOG_OK) {
            return result;
        }
        fault_active = 0u;
    }

    /* Save the first fault sample and one recovery-boundary sample. Samples
     * observed while healthy never enter the logger's routine queue. */
    if (fault_present != 0u || was_active != 0u) {
        return elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                          TELEMETRY_ID, payload, sizeof payload);
    }
    return ELOG_OK;
}

/** @brief Verify and count the committed fault-window payloads.
 * @param context Caller-owned counts, timestamps, and uniqueness mask.
 * @param record Borrowed validated record; do not keep record.payload.
 * @return Zero for an expected record, nonzero to stop on unexpected data.
 */
static int count_record(void *context, const elog_record_t *record)
{
    record_counts_t *counts = (record_counts_t *)context;
    if (record->type == ELOG_TYPE_FAULT && record->event_id == FAULT_ID) {
        ++counts->faults;
    } else if (record->type == ELOG_TYPE_FAULT_RECOVERY &&
               record->event_id == FAULT_ID) {
        ++counts->recoveries;
    } else if (record->type == ELOG_TYPE_TELEMETRY &&
               record->event_id == TELEMETRY_ID &&
               record->codec_id == ELOG_CODEC_NONE &&
               record->stored_length == 8u &&
               record->original_length == 8u) {
        if (record->timestamp < 1100u || record->timestamp > 1400u ||
            (record->timestamp - 1100u) % 100u != 0u) {
            return 1;
        }
        const unsigned int index =
            (unsigned int)((record->timestamp - 1100u) / 100u);
        const uint8_t bit = (uint8_t)(1u << index);
        const uint32_t expected_rpm = index < 3u ? 1100u + index : 1000u;
        const uint32_t expected_pressure = index < 3u ? 230u + index : 210u;
        if ((counts->sensor_mask & bit) != 0u ||
            get_u32(record->payload) != expected_rpm ||
            get_u32(record->payload + 4u) != expected_pressure) {
            return 1;
        }
        counts->sensor_mask |= bit;
        ++counts->telemetry;
        if (counts->first_sensor_time == 0u ||
            record->timestamp < counts->first_sensor_time) {
            counts->first_sensor_time = record->timestamp;
        }
        if (record->timestamp > counts->last_sensor_time) {
            counts->last_sensor_time = record->timestamp;
        }
    } else {
        return 1;
    }
    return 0;
}

/** @brief Prove healthy samples do not write the simulated flash image.
 * @return Zero when exactly one fault, four sensor readings, and one recovery
 *         commit, with zero writes in healthy intervals; nonzero otherwise.
 * @note The configured codec is NONE so visitor payload checks are direct.
 */
int main(void)
{
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        pages[i] = (elog_erase_region_t){i * PAGE_SIZE, PAGE_SIZE};
    }
    if (elog_ram_storage_init(&ram, image, sizeof image, pages, PAGE_COUNT,
                              2u, 64u, &ram_storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK ||
        elog_sim_flash_init(&sim, &ram, &ram_storage, &storage) != ELOG_OK) {
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

    for (unsigned int i = 0u; i < 10u; ++i) {
        if (sample_sensor(1000u, 200u, 0u) != ELOG_OK ||
            elog_process(&logger) != ELOG_EMPTY) {
            fputs("healthy sampling queued a log\n", stderr);
            return 1;
        }
    }
    if (sim.program_calls != 0u || sim.erase_calls != 0u) {
        fputs("healthy sampling wrote flash\n", stderr);
        return 1;
    }
    puts("10 healthy samples: 0 flash writes");

    for (unsigned int i = 0u; i < 3u; ++i) {
        if (sample_sensor(1100u + i, 230u + i, 1u) != ELOG_OK ||
            elog_flush(&logger, 100u) != ELOG_OK) {
            fputs("fault-window commit failed\n", stderr);
            return 1;
        }
    }
    if (sample_sensor(1000u, 210u, 0u) != ELOG_OK ||
        elog_flush(&logger, 100u) != ELOG_OK) {
        fputs("recovery-boundary commit failed\n", stderr);
        return 1;
    }
    const uint32_t writes_after_fault = sim.program_calls;
    if (writes_after_fault == 0u) {
        fputs("fault window wrote no flash\n", stderr);
        return 1;
    }
    for (unsigned int i = 0u; i < 10u; ++i) {
        if (sample_sensor(1000u, 200u, 0u) != ELOG_OK ||
            elog_process(&logger) != ELOG_EMPTY) {
            fputs("post-recovery sampling queued a log\n", stderr);
            return 1;
        }
    }
    record_counts_t counts = {0};
    if (sim.program_calls != writes_after_fault ||
        elog_iterate_records(&logger, count_record, &counts) != ELOG_OK ||
        counts.faults != 1u || counts.recoveries != 1u ||
        counts.telemetry != 4u || counts.first_sensor_time != 1100u ||
        counts.last_sensor_time != 1400u || counts.sensor_mask != 0x0Fu) {
        fputs("unexpected fault-window records\n", stderr);
        return 1;
    }
    printf("fault window: %zu fault, %zu sensor samples, %zu recovery; "
           "10 later healthy samples: 0 new flash writes\n",
           counts.faults, counts.telemetry, counts.recoveries);
    return 0;
}
