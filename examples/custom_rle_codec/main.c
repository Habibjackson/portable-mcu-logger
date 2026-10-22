/** @file
 * @brief Register a per-record run-length codec and decode its stored bytes.
 *
 * The custom codec uses ID 128 and represents each run as [count, value],
 * preceded by the original byte length. Three runs in a 24-byte telemetry
 * payload compress to seven bytes. A payload whose bytes all differ returns
 * ELOG_TOO_LARGE from the encoder, so the logger stores the original bytes
 * under codec NONE. An EVENT with repeated bytes also stays raw because the
 * core compresses only ROUTINE TELEMETRY records.
 *
 * This deliberately small codec is for learning, not a general-purpose RLE
 * format. Each record is independently decodable, which is required by the
 * custom codec API. A real export tool must know ID 128 and this byte format;
 * elog_iterate_records() provides the encoded payload, not decoded bytes.
 */
#include "log.h"
#include "log_codec.h"
#include "log_storage_ram.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 256u
#define PAGE_COUNT 5u
#define PAYLOAD_SIZE 24u
#define RECORD_COUNT 3u
#define CUSTOM_RLE_ID ELOG_CODEC_CUSTOM_MIN
#define RUNS_ID UINT32_C(0x6201)
#define MIXED_ID UINT32_C(0x6202)
#define EVENT_ID UINT32_C(0x6203)

/** @brief Encode runs as one length byte followed by count/value pairs.
 * @param context Unused; reserved for a device-specific codec state.
 * @param source Original payload bytes.
 * @param source_length Number of source bytes; this format supports 1..255.
 * @param destination Caller-owned encoded output.
 * @param destination_capacity Available output bytes.
 * @param written Set only on ELOG_OK to the exact encoded length.
 * @return ELOG_OK if output is shorter, ELOG_TOO_LARGE when RLE is not useful
 *         or cannot fit, or ELOG_INVALID_ARGUMENT for a bad call.
 */
static elog_result_t rle_encode(void *context, const uint8_t *source,
                                size_t source_length, uint8_t *destination,
                                size_t destination_capacity, size_t *written)
{
    (void)context;
    if (source == NULL || destination == NULL || written == NULL ||
        source_length == 0u || source_length > UINT8_MAX) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (source_length < 4u || destination_capacity == 0u) {
        return ELOG_TOO_LARGE;
    }
    destination[0] = (uint8_t)source_length;
    size_t output = 1u;
    for (size_t input = 0u; input < source_length;) {
        size_t run = 1u;
        while (run < UINT8_MAX && input + run < source_length &&
               source[input + run] == source[input]) {
            ++run;
        }
        if (output + 2u >= source_length ||
            output + 2u > destination_capacity) {
            return ELOG_TOO_LARGE;
        }
        destination[output++] = (uint8_t)run;
        destination[output++] = source[input];
        input += run;
    }
    *written = output;
    return ELOG_OK;
}

/** @brief Decode and validate one independent RLE record.
 * @param context Unused; included to match elog_codec_t.
 * @param source Complete encoded record payload.
 * @param source_length Encoded byte count, including the length prefix.
 * @param destination Caller-owned decoded output.
 * @param destination_capacity Available output bytes.
 * @param written Set only on ELOG_OK to the decoded byte count.
 * @return ELOG_OK, ELOG_TOO_LARGE for insufficient output space,
 *         ELOG_CORRUPT for a malformed stream, or ELOG_INVALID_ARGUMENT.
 */
static elog_result_t rle_decode(void *context, const uint8_t *source,
                                size_t source_length, uint8_t *destination,
                                size_t destination_capacity, size_t *written)
{
    (void)context;
    if (source == NULL || destination == NULL || written == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (source_length < 3u || (source_length & 1u) == 0u || source[0] == 0u) {
        return ELOG_CORRUPT;
    }
    const size_t expected = source[0];
    if (expected > destination_capacity) {
        return ELOG_TOO_LARGE;
    }
    size_t output = 0u;
    for (size_t input = 1u; input < source_length; input += 2u) {
        const size_t run = source[input];
        if (run == 0u || run > expected - output) {
            return ELOG_CORRUPT;
        }
        memset(destination + output, source[input + 1u], run);
        output += run;
    }
    if (output != expected) {
        return ELOG_CORRUPT;
    }
    *written = output;
    return ELOG_OK;
}

/** @brief Copy stored bytes during the readback callback's lifetime. */
typedef struct {
    uint8_t codec_id;
    uint8_t type;
    uint32_t event_id;
    uint16_t original_length;
    uint16_t stored_length;
    uint8_t payload[PAYLOAD_SIZE];
} saved_record_t;

/** @brief Keep physical readback independent from chronological order. */
typedef struct {
    saved_record_t record[RECORD_COUNT];
    uint8_t seen;
} collected_records_t;

/** @brief Collect each committed record under its sequence number.
 * @param context Caller-owned result collection.
 * @param record Borrowed stored record; its encoded payload is copied here.
 * @return Zero to continue, nonzero if metadata is unexpected.
 */
static int collect_record(void *context, const elog_record_t *record)
{
    collected_records_t *collected = (collected_records_t *)context;
    if (record->sequence >= RECORD_COUNT ||
        record->original_length != PAYLOAD_SIZE ||
        record->stored_length > PAYLOAD_SIZE) {
        return 1;
    }
    const size_t index = (size_t)record->sequence;
    const uint8_t bit = (uint8_t)(1u << index);
    if ((collected->seen & bit) != 0u) {
        return 1;
    }
    saved_record_t *saved = &collected->record[index];
    saved->codec_id = record->codec_id;
    saved->type = record->type;
    saved->event_id = record->event_id;
    saved->original_length = record->original_length;
    saved->stored_length = record->stored_length;
    memcpy(saved->payload, record->payload, record->stored_length);
    collected->seen |= bit;
    return 0;
}

/** @brief Decode one record according to its persisted ID and compare bytes.
 * @param saved Committed header fields and stored payload.
 * @param original Application's expected uncompressed bytes.
 * @param codec Registered RLE encoder/decoder, valid for the program lifetime.
 * @return Zero only for an exact 24-byte round trip.
 */
static int check_record(const saved_record_t *saved, const uint8_t *original,
                        const elog_codec_t *codec)
{
    uint8_t decoded[PAYLOAD_SIZE];
    size_t decoded_length = 0u;
    if (saved->codec_id == ELOG_CODEC_NONE) {
        if (saved->stored_length != PAYLOAD_SIZE) {
            return 1;
        }
        memcpy(decoded, saved->payload, PAYLOAD_SIZE);
        decoded_length = PAYLOAD_SIZE;
    } else if (saved->codec_id == codec->id) {
        if (codec->decode(codec->context, saved->payload,
                          saved->stored_length, decoded, sizeof decoded,
                          &decoded_length) != ELOG_OK) {
            return 1;
        }
    } else {
        return 1;
    }
    return saved->original_length == PAYLOAD_SIZE &&
           decoded_length == PAYLOAD_SIZE &&
           memcmp(decoded, original, PAYLOAD_SIZE) == 0 ? 0 : 1;
}

/** @brief Demonstrate compressed, fallback, and ineligible record paths.
 * @return Zero only when all records commit and decode byte for byte.
 * @note A new host RAM fixture is formatted here. Real flash must not be
 *       formatted on each boot or its committed logs would be destroyed.
 */
int main(void)
{
    uint8_t image[PAGE_COUNT * PAGE_SIZE];
    elog_erase_region_t pages[PAGE_COUNT];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    static elog_t logger;
    static const elog_codec_t codec = {
        CUSTOM_RLE_ID, NULL, rle_encode, rle_decode
    };
    for (uint32_t i = 0u; i < PAGE_COUNT; ++i) {
        pages[i] = (elog_erase_region_t){i * PAGE_SIZE, PAGE_SIZE};
    }
    if (elog_ram_storage_init(&ram, image, sizeof image, pages, PAGE_COUNT,
                              2u, 64u, &storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK) {
        fputs("RAM storage setup failed\n", stderr);
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
    config.routine_queue_depth = 2u;
    config.isr_queue_depth = 2u;
    config.max_payload = PAYLOAD_SIZE;
    config.fault_table_depth = 1u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = codec.id;
    config.custom_codec = &codec;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        fputs("logger initialization failed\n", stderr);
        return 1;
    }

    uint8_t repeated[PAYLOAD_SIZE];
    uint8_t mixed[PAYLOAD_SIZE];
    uint8_t event[PAYLOAD_SIZE];
    for (size_t i = 0u; i < PAYLOAD_SIZE; ++i) {
        repeated[i] = i < 8u ? 10u : (i < 16u ? 20u : 30u);
        mixed[i] = (uint8_t)i;
        event[i] = 0xAAu;
    }
    if (elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                   RUNS_ID, repeated, sizeof repeated) != ELOG_OK ||
        elog_flush(&logger, 200u) != ELOG_OK ||
        elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                   MIXED_ID, mixed, sizeof mixed) != ELOG_OK ||
        elog_flush(&logger, 200u) != ELOG_OK ||
        elog_event(&logger, EVENT_ID, event, sizeof event) != ELOG_OK ||
        elog_flush(&logger, 200u) != ELOG_OK) {
        fputs("record enqueue or commit failed\n", stderr);
        return 1;
    }

    collected_records_t collected = {0};
    if (elog_iterate_records(&logger, collect_record, &collected) != ELOG_OK ||
        collected.seen != (uint8_t)((1u << RECORD_COUNT) - 1u)) {
        fputs("readback did not find every record\n", stderr);
        return 1;
    }
    const saved_record_t *records = collected.record;
    if (records[0].type != ELOG_TYPE_TELEMETRY ||
        records[0].event_id != RUNS_ID ||
        records[0].codec_id != CUSTOM_RLE_ID ||
        records[0].stored_length != 7u ||
        check_record(&records[0], repeated, &codec) != 0 ||
        records[1].type != ELOG_TYPE_TELEMETRY ||
        records[1].event_id != MIXED_ID ||
        records[1].codec_id != ELOG_CODEC_NONE ||
        check_record(&records[1], mixed, &codec) != 0 ||
        records[2].type != ELOG_TYPE_EVENT ||
        records[2].event_id != EVENT_ID ||
        records[2].codec_id != ELOG_CODEC_NONE ||
        check_record(&records[2], event, &codec) != 0) {
        fputs("codec choice or decoded payload was unexpected\n", stderr);
        return 1;
    }
    printf("RLE telemetry: codec=%u payload=%u/%u bytes\n",
           (unsigned int)records[0].codec_id,
           (unsigned int)records[0].stored_length,
           (unsigned int)records[0].original_length);
    printf("mixed telemetry: codec=%u payload=%u/%u bytes\n",
           (unsigned int)records[1].codec_id,
           (unsigned int)records[1].stored_length,
           (unsigned int)records[1].original_length);
    printf("event: codec=%u payload=%u/%u bytes\n",
           (unsigned int)records[2].codec_id,
           (unsigned int)records[2].stored_length,
           (unsigned int)records[2].original_length);
    return 0;
}
