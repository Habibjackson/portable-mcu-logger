/** @file
 * @brief Compare NONE, VARINT, and DELTA using the same sensor samples.
 *
 * Each mode gets a newly formatted RAM-backed journal. The program writes
 * three six-field telemetry payloads, commits each one, reads the stored
 * records, decodes them according to their persisted codec IDs, and compares
 * every reconstructed byte with the original payload. This is a host example;
 * the RAM image models flash and does not access an MCU.
 *
 * The sample words are signed 32-bit little-endian values. VARINT stores each
 * sample independently. DELTA starts with a raw snapshot, stores a difference
 * from that sample, then writes another raw snapshot because the configured
 * snapshot interval is one compressed record. Payload byte totals exclude
 * record headers, CRC, commit marker, page headers, and alignment padding.
 */
#include "log.h"
#include "log_codec.h"
#include "log_storage_ram.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u
#define SAMPLE_COUNT 3u
#define FIELD_COUNT 6u
#define SAMPLE_BYTES (FIELD_COUNT * 4u)
#define TELEMETRY_ID UINT32_C(0x6101)

/** @brief Copy the encoded bytes while the visitor's payload is valid. */
typedef struct {
    uint8_t codec_id;
    uint16_t original_length;
    uint16_t stored_length;
    uint8_t payload[SAMPLE_BYTES];
} saved_sample_t;

/** @brief Collect by sequence because journal iteration is physical order. */
typedef struct {
    saved_sample_t sample[SAMPLE_COUNT];
    uint8_t seen;
} collected_samples_t;

static const int32_t readings[SAMPLE_COUNT][FIELD_COUNT] = {
    {1000, 200, -3, 12000, 0, 42},
    {1001, 201, -2, 12001, 1, 43},
    {1002, 202, -1, 12002, 2, 44}
};

/** @brief Serialize one signed field without relying on host struct layout.
 * @param target Four writable bytes.
 * @param value Sensor value; its two's-complement bits go to little-endian
 *              storage, as required by the built-in numeric codecs.
 */
static void put_i32(uint8_t *target, int32_t value)
{
    const uint32_t bits = (uint32_t)value;
    for (unsigned int i = 0u; i < 4u; ++i) {
        target[i] = (uint8_t)(bits >> (8u * i));
    }
}

/** @brief Produce the original byte payload for a chosen sample.
 * @param index Sample number, less than SAMPLE_COUNT.
 * @param payload SAMPLE_BYTES of caller-owned output space.
 */
static void make_sample(size_t index, uint8_t payload[SAMPLE_BYTES])
{
    for (size_t field = 0u; field < FIELD_COUNT; ++field) {
        put_i32(payload + field * 4u, readings[index][field]);
    }
}

/** @brief Save one validated record's encoded payload for later decoding.
 * @param context Caller-owned collection, indexed by record sequence.
 * @param record Borrowed record whose payload expires after this callback.
 * @return Zero to continue; nonzero for an unexpected or duplicate record.
 */
static int collect_sample(void *context, const elog_record_t *record)
{
    collected_samples_t *collected = (collected_samples_t *)context;
    if (record->sequence >= SAMPLE_COUNT ||
        record->priority != ELOG_ROUTINE ||
        record->type != ELOG_TYPE_TELEMETRY ||
        record->event_id != TELEMETRY_ID ||
        record->original_length != SAMPLE_BYTES ||
        record->stored_length > SAMPLE_BYTES) {
        return 1;
    }
    const size_t index = (size_t)record->sequence;
    const uint8_t bit = (uint8_t)(1u << index);
    if ((collected->seen & bit) != 0u) {
        return 1;
    }
    saved_sample_t *saved = &collected->sample[index];
    saved->codec_id = record->codec_id;
    saved->original_length = record->original_length;
    saved->stored_length = record->stored_length;
    memcpy(saved->payload, record->payload, record->stored_length);
    collected->seen |= bit;
    return 0;
}

/** @brief Decode a stored sample and check its bytes against the input.
 * @param saved Stored bytes and codec ID copied from one committed record.
 * @param previous Last decoded sample, needed only for DELTA.
 * @param previous_sequence Sequence number of that prior sample.
 * @param index Which original reading to compare.
 * @param decoded Receives the verified uncompressed sample.
 * @return Zero when decoding and comparison both succeed.
 */
static int check_sample(const saved_sample_t *saved, const uint8_t *previous,
                        uint64_t previous_sequence, size_t index,
                        uint8_t decoded[SAMPLE_BYTES])
{
    size_t length = 0u;
    elog_result_t result = ELOG_UNSUPPORTED;
    switch (saved->codec_id) {
    case ELOG_CODEC_NONE:
        if (saved->stored_length != SAMPLE_BYTES) {
            return 1;
        }
        memcpy(decoded, saved->payload, SAMPLE_BYTES);
        length = SAMPLE_BYTES;
        result = ELOG_OK;
        break;
    case ELOG_CODEC_VARINT:
        result = elog_varint_decode(saved->payload, saved->stored_length,
                                    decoded, SAMPLE_BYTES, &length);
        break;
    case ELOG_CODEC_DELTA:
        if (index == 0u) {
            return 1; /* No earlier decoded sample is available. */
        }
        result = elog_delta_decode(saved->payload, saved->stored_length,
                                   previous, SAMPLE_BYTES, previous_sequence,
                                   decoded, SAMPLE_BYTES, &length);
        break;
    default:
        return 1;
    }
    uint8_t original[SAMPLE_BYTES];
    make_sample(index, original);
    return result == ELOG_OK && length == SAMPLE_BYTES &&
           saved->original_length == SAMPLE_BYTES &&
           memcmp(decoded, original, SAMPLE_BYTES) == 0 ? 0 : 1;
}

/** @brief Format a fresh fixture and run one built-in compression mode.
 * @param mode NONE, VARINT, or DELTA.
 * @param name Short label printed with per-record and total payload bytes.
 * @return Zero only after all three records commit and decode exactly.
 * @note Reformatting is appropriate for this new host fixture, not for a real
 *       device whose prior committed logs must survive reboot.
 */
static int run_mode(uint8_t mode, const char *name)
{
    uint8_t image[PAGE_COUNT * PAGE_SIZE];
    elog_erase_region_t pages[PAGE_COUNT];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    static elog_t logger;
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        pages[i] = (elog_erase_region_t){i * PAGE_SIZE, PAGE_SIZE};
    }
    if (elog_ram_storage_init(&ram, image, sizeof image, pages, PAGE_COUNT,
                              2u, 64u, &storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK) {
        fprintf(stderr, "%s: RAM storage setup failed\n", name);
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
    config.routine_queue_depth = 3u;
    config.isr_queue_depth = 2u;
    config.max_payload = SAMPLE_BYTES;
    config.fault_table_depth = 1u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = mode;
    config.snapshot_interval = mode == ELOG_CODEC_DELTA ? 1u : 0u;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        fprintf(stderr, "%s: logger initialization failed\n", name);
        return 1;
    }

    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        uint8_t original[SAMPLE_BYTES];
        make_sample(i, original);
        if (elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                       TELEMETRY_ID, original, sizeof original) != ELOG_OK ||
            elog_flush(&logger, 200u) != ELOG_OK) {
            fprintf(stderr, "%s: sample %zu did not commit\n", name, i);
            return 1;
        }
    }

    collected_samples_t collected = {0};
    if (elog_iterate_records(&logger, collect_sample, &collected) != ELOG_OK ||
        collected.seen != (uint8_t)((1u << SAMPLE_COUNT) - 1u)) {
        fprintf(stderr, "%s: readback did not find every sample\n", name);
        return 1;
    }
    const uint8_t expected_delta[SAMPLE_COUNT] = {
        ELOG_CODEC_NONE, ELOG_CODEC_DELTA, ELOG_CODEC_NONE
    };
    size_t stored_bytes = 0u;
    uint8_t previous[SAMPLE_BYTES] = {0};
    for (size_t i = 0u; i < SAMPLE_COUNT; ++i) {
        uint8_t decoded[SAMPLE_BYTES];
        const saved_sample_t *saved = &collected.sample[i];
        const uint8_t expected_codec = mode == ELOG_CODEC_DELTA ?
            expected_delta[i] : mode;
        if (saved->codec_id != expected_codec ||
            check_sample(saved, previous, i == 0u ? 0u : (uint64_t)(i - 1u),
                         i, decoded) != 0) {
            fprintf(stderr, "%s: sample %zu codec or data mismatch\n",
                    name, i);
            return 1;
        }
        memcpy(previous, decoded, sizeof previous);
        stored_bytes += saved->stored_length;
        printf("%-6s sample=%zu codec=%u payload=%u/%u bytes\n", name, i,
               (unsigned int)saved->codec_id,
               (unsigned int)saved->stored_length,
               (unsigned int)saved->original_length);
    }
    printf("%-6s payload total=%zu/%u bytes\n", name, stored_bytes,
           (unsigned int)(SAMPLE_COUNT * SAMPLE_BYTES));
    return 0;
}

/** @brief Verify all implemented built-in modes using identical readings.
 * @return Zero only when each committed record round-trips exactly.
 */
int main(void)
{
    if (run_mode(ELOG_CODEC_NONE, "NONE") != 0 ||
        run_mode(ELOG_CODEC_VARINT, "VARINT") != 0 ||
        run_mode(ELOG_CODEC_DELTA, "DELTA") != 0) {
        return 1;
    }
    return 0;
}
