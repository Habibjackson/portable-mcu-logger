/** @file
 * @brief Keep recent healthy readings in RAM and persist them after a fault.
 *
 * The six healthy readings below never enter the logger, so they cause no
 * flash writes. A four-entry application ring retains the latest context.
 * When a fault occurs, the application enters a safe state, queues the fault,
 * then copies the ring and fault-window readings into the logger. This is an
 * optional alternative to continuous routine logging; it does not change the
 * library or its on-storage format.
 *
 * Pre-trigger records get the time of enqueue in their record header. Their
 * original sample time is therefore included in the 16-byte payload:
 * little-endian uint64_t sample_ms, uint32_t rpm, uint32_t pressure_deci_kpa.
 * The ring and queued records remain vulnerable to power loss until commit.
 * This host program formats only a new RAM flash fixture, never MCU flash.
 */
#include "log.h"
#include "log_storage_ram.h"
#include "log_storage_sim.h"

#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 6u
#define PRETRIGGER_COUNT 4u
#define FAULT_ID UINT32_C(0x4101)
#define SENSOR_ID UINT32_C(0x5101)

typedef struct {
    uint64_t sample_ms;
    uint32_t rpm;
    uint32_t pressure_deci_kpa;
} sensor_sample_t;

typedef struct {
    size_t faults;
    size_t recoveries;
    size_t before;
    size_t during;
    uint8_t before_mask;
    uint8_t during_mask;
} record_summary_t;

static uint8_t image[PAGE_COUNT * PAGE_SIZE];
static elog_erase_region_t pages[PAGE_COUNT];
static elog_ram_storage_t ram;
static elog_storage_t ram_storage;
static elog_sim_flash_t sim;
static elog_storage_t storage;
static elog_t logger;
static sensor_sample_t recent[PRETRIGGER_COUNT];
static uint8_t recent_next;
static uint8_t recent_count;
static uint64_t now_ms;
static uint8_t safe_state;

/** @brief Supply monotonically increasing sample time to task-side calls.
 * @param context Address of the caller-owned millisecond clock.
 * @return Current uptime in milliseconds; no hardware clock is used here.
 */
static uint64_t timestamp(void *context)
{
    return *(const uint64_t *)context;
}

/** @brief Encode one unsigned value in explicit little-endian byte order.
 * @param target At least four writable bytes.
 * @param value Value to encode independently of host CPU endianness.
 */
static void put_u32(uint8_t *target, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        target[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Encode an original sample timestamp in the application payload.
 * @param target At least eight writable bytes.
 * @param value Monotonic timestamp of the reading, which may predate enqueue.
 */
static void put_u64(uint8_t *target, uint64_t value)
{
    for (unsigned int i = 0u; i < 8u; ++i) {
        target[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Decode little-endian payload bytes during readback verification.
 * @param source At least four readable bytes from one uncompressed record.
 * @return The decoded unsigned value.
 */
static uint32_t get_u32(const uint8_t *source)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)source[i] << (8u * i);
    }
    return value;
}

/** @brief Decode the original sample time embedded in a record payload.
 * @param source At least eight readable bytes.
 * @return The sample timestamp, which may differ from record.timestamp.
 */
static uint64_t get_u64(const uint8_t *source)
{
    uint64_t value = 0u;
    for (unsigned int i = 0u; i < 8u; ++i) {
        value |= (uint64_t)source[i] << (8u * i);
    }
    return value;
}

/** @brief Retain one healthy sensor reading without calling the logger.
 * @param sample Reading copied into the bounded application ring.
 * @note Once full, the oldest reading is replaced. A reset loses this RAM.
 */
static void remember_healthy(sensor_sample_t sample)
{
    recent[recent_next] = sample;
    recent_next = (uint8_t)((recent_next + 1u) % PRETRIGGER_COUNT);
    if (recent_count < PRETRIGGER_COUNT) {
        ++recent_count;
    }
}

/** @brief Queue one sample with an explicit original timestamp in its body.
 * @param sample Values and acquisition time to preserve.
 * @param type PRE_FAULT for buffered history or TELEMETRY for live readings.
 * @return Logger enqueue result; ELOG_OK does not mean a durable commit.
 */
static elog_result_t queue_sensor(sensor_sample_t sample,
                                  elog_record_type_t type)
{
    uint8_t payload[16];
    put_u64(payload, sample.sample_ms);
    put_u32(payload + 8u, sample.rpm);
    put_u32(payload + 12u, sample.pressure_deci_kpa);
    return elog_write(&logger, ELOG_ROUTINE, type, SENSOR_ID,
                      payload, sizeof payload);
}

/** @brief Queue buffered readings oldest first after the critical fault.
 * @return First enqueue failure, or ELOG_OK if all four fit in the queue.
 * @note A real application should report drops and preserve its safe state.
 */
static elog_result_t queue_recent(void)
{
    for (uint8_t i = 0u; i < recent_count; ++i) {
        const uint8_t index = (uint8_t)((recent_next + PRETRIGGER_COUNT -
                                         recent_count + i) % PRETRIGGER_COUNT);
        const elog_result_t result = queue_sensor(recent[index],
                                                   ELOG_TYPE_PRE_FAULT);
        if (result != ELOG_OK) {
            return result;
        }
    }
    return ELOG_OK;
}

/** @brief Validate committed fault and sensor records from the RAM image.
 * @param context Caller-owned counters and bit masks for unique samples.
 * @param record Borrowed record whose payload is valid only in this callback.
 * @return Zero to continue, nonzero to fail the example on unexpected data.
 */
static int inspect_record(void *context, const elog_record_t *record)
{
    record_summary_t *summary = (record_summary_t *)context;
    if (record->type == ELOG_TYPE_FAULT && record->event_id == FAULT_ID &&
        record->priority == ELOG_CRITICAL && record->timestamp == 700u &&
        record->sequence == 0u && record->codec_id == ELOG_CODEC_NONE &&
        record->stored_length == 8u &&
        get_u32(record->payload) == 1500u &&
        get_u32(record->payload + 4u) == 240u) {
        ++summary->faults;
        return 0;
    }
    if (record->type == ELOG_TYPE_FAULT_RECOVERY &&
        record->event_id == FAULT_ID && record->priority == ELOG_CRITICAL &&
        record->timestamp == 900u && record->codec_id == ELOG_CODEC_NONE &&
        record->stored_length == 12u &&
        get_u32(record->payload) == 0u &&
        get_u64(record->payload + 4u) == 200u) {
        ++summary->recoveries;
        return 0;
    }
    if (record->priority != ELOG_ROUTINE || record->event_id != SENSOR_ID ||
        record->codec_id != ELOG_CODEC_NONE ||
        record->stored_length != 16u || record->original_length != 16u) {
        return 1;
    }
    const uint64_t sample_ms = get_u64(record->payload);
    const uint32_t rpm = get_u32(record->payload + 8u);
    const uint32_t pressure = get_u32(record->payload + 12u);
    if (record->type == ELOG_TYPE_PRE_FAULT && record->timestamp == 700u &&
        sample_ms >= 300u && sample_ms <= 600u &&
        (sample_ms - 300u) % 100u == 0u) {
        const uint8_t index = (uint8_t)((sample_ms - 300u) / 100u);
        const uint8_t bit = (uint8_t)(1u << index);
        if ((summary->before_mask & bit) != 0u ||
            rpm != 1020u + 10u * index || pressure != 202u + index) {
            return 1;
        }
        summary->before_mask |= bit;
        ++summary->before;
        return 0;
    }
    if (record->type == ELOG_TYPE_TELEMETRY &&
        sample_ms >= 700u && sample_ms <= 900u &&
        (sample_ms - 700u) % 100u == 0u &&
        record->timestamp == sample_ms) {
        const uint8_t index = (uint8_t)((sample_ms - 700u) / 100u);
        const uint8_t bit = (uint8_t)(1u << index);
        static const uint32_t expected_rpm[3] = {1500u, 1450u, 1100u};
        static const uint32_t expected_pressure[3] = {240u, 238u, 210u};
        if ((summary->during_mask & bit) != 0u ||
            rpm != expected_rpm[index] ||
            pressure != expected_pressure[index]) {
            return 1;
        }
        summary->during_mask |= bit;
        ++summary->during;
        return 0;
    }
    return 1;
}

/** @brief Exercise a healthy interval, fault trigger, and recovery.
 * @return Zero only if no healthy flash calls occur and all seven sampled
 *         readings and both critical transitions survive commit and readback.
 * @note The RAM fixture is formatted once. Actual flash must use a separately
 *       reserved region and must not be reformatted on every boot.
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
        (elog_region_t){3u * PAGE_SIZE, 3u * PAGE_SIZE};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 8u;
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

    for (uint32_t i = 0u; i < 6u; ++i) {
        now_ms += 100u;
        remember_healthy((sensor_sample_t){now_ms, 1000u + 10u * i,
                                           200u + i});
        if (elog_process(&logger) != ELOG_EMPTY) {
            fputs("healthy sample created logger work\n", stderr);
            return 1;
        }
    }
    if (sim.program_calls != 0u || sim.erase_calls != 0u) {
        fputs("healthy sampling wrote flash\n", stderr);
        return 1;
    }

    now_ms = 700u;
    const sensor_sample_t trigger = {now_ms, 1500u, 240u};
    uint8_t fault_snapshot[8];
    put_u32(fault_snapshot, trigger.rpm);
    put_u32(fault_snapshot + 4u, trigger.pressure_deci_kpa);
    safe_state = 1u; /* Shut down real outputs before calling the logger. */
    if (elog_fault(&logger, FAULT_ID, fault_snapshot,
                   sizeof fault_snapshot) != ELOG_OK ||
        queue_recent() != ELOG_OK ||
        queue_sensor(trigger, ELOG_TYPE_TELEMETRY) != ELOG_OK ||
        elog_flush(&logger, 200u) != ELOG_OK) {
        fputs("fault context did not commit\n", stderr);
        return 1;
    }
    now_ms = 800u;
    if (queue_sensor((sensor_sample_t){now_ms, 1450u, 238u},
                     ELOG_TYPE_TELEMETRY) != ELOG_OK ||
        elog_flush(&logger, 100u) != ELOG_OK) {
        fputs("fault sample did not commit\n", stderr);
        return 1;
    }
    now_ms = 900u;
    if (elog_fault_recovered(&logger, FAULT_ID, NULL, 0u) != ELOG_OK ||
        queue_sensor((sensor_sample_t){now_ms, 1100u, 210u},
                     ELOG_TYPE_TELEMETRY) != ELOG_OK ||
        elog_flush(&logger, 100u) != ELOG_OK) {
        fputs("recovery did not commit\n", stderr);
        return 1;
    }

    record_summary_t summary = {0};
    if (safe_state == 0u || sim.program_calls == 0u ||
        elog_iterate_records(&logger, inspect_record, &summary) != ELOG_OK ||
        summary.faults != 1u || summary.recoveries != 1u ||
        summary.before != 4u || summary.before_mask != 0x0Fu ||
        summary.during != 3u || summary.during_mask != 0x07u) {
        fputs("retained sensor context was unexpected\n", stderr);
        return 1;
    }
    puts("6 healthy readings: 0 flash writes; fault retained 4 earlier and "
         "3 fault-window readings");
    return 0;
}
