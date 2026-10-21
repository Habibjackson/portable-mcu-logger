#include "log.h"
#include "log_codec.h"
#include "log_storage_ram.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static int test_codec_functions(void)
{
    uint8_t source[24];
    uint8_t previous[24];
    uint8_t encoded[80];
    uint8_t decoded[80];
    size_t written;
    size_t decoded_length;
    const uint32_t extremes[6] = {
        0u, 1u, UINT32_MAX, UINT32_C(0x80000000),
        UINT32_C(0x7FFFFFFF), UINT32_C(0xDEADBEEF)
    };
    for (size_t i = 0u; i < 6u; ++i) {
        put32(source + i * 4u, extremes[i]);
        put32(previous + i * 4u, (uint32_t)(extremes[i] + 1u));
    }
    CHECK(elog_varint_encode(source, sizeof source, encoded,
                             sizeof encoded, &written) == ELOG_OK);
    CHECK(elog_varint_decode(encoded, written, decoded,
                             sizeof decoded, &decoded_length) == ELOG_OK);
    CHECK(decoded_length == sizeof source);
    CHECK(memcmp(decoded, source, sizeof source) == 0);
    CHECK(elog_delta_encode(source, previous, sizeof source, 77u,
                            encoded, sizeof encoded, &written) == ELOG_OK);
    CHECK(elog_delta_decode(encoded, written, previous, sizeof previous,
                            77u, decoded, sizeof decoded,
                            &decoded_length) == ELOG_OK);
    CHECK(written < sizeof source && decoded_length == sizeof source);
    CHECK(memcmp(decoded, source, sizeof source) == 0);
    CHECK(elog_delta_decode(encoded, written, previous, sizeof previous,
                            78u, decoded, sizeof decoded,
                            &decoded_length) == ELOG_CORRUPT);
    encoded[written - 1u] = 0x80u;
    CHECK(elog_delta_decode(encoded, written, previous, sizeof previous,
                            77u, decoded, sizeof decoded,
                            &decoded_length) == ELOG_CORRUPT);
    return 0;
}

typedef struct {
    uint8_t codecs[4];
    uint8_t payloads[4][80];
    uint16_t lengths[4];
    size_t count;
} compressed_records_t;

static int collect(void *context, const elog_record_t *record)
{
    compressed_records_t *records = (compressed_records_t *)context;
    if (records->count >= 4u || record->stored_length > 80u) {
        return 1;
    }
    const size_t index = records->count++;
    records->codecs[index] = record->codec_id;
    records->lengths[index] = record->stored_length;
    memcpy(records->payloads[index], record->payload, record->stored_length);
    return 0;
}

static int test_core_delta_and_fallback(void)
{
    uint8_t memory[8u * 256u];
    elog_erase_region_t regions[8];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    static elog_t logger;
    for (uint32_t i = 0u; i < 8u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 8u, 2u, 64u, &storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 512u};
    config.partition[ELOG_EVENT] = (elog_region_t){512u, 512u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){1024u, 1024u};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 4u;
    config.isr_queue_depth = 2u;
    config.max_payload = 80u;
    config.fault_table_depth = 2u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_DELTA;
    config.snapshot_interval = 2u;
    CHECK(elog_init(&logger, &config, &storage) == ELOG_OK);
    uint8_t a[24];
    uint8_t b[24];
    uint8_t c[24];
    for (size_t i = 0u; i < 6u; ++i) {
        put32(a + i * 4u, (uint32_t)(100u + i));
        put32(b + i * 4u, (uint32_t)(101u + i));
        put32(c + i * 4u, (uint32_t)(500000u + i * 100000u));
    }
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     1u, a, sizeof a) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     1u, b, sizeof b) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     1u, c, sizeof c) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    compressed_records_t records;
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&logger, collect, &records) == ELOG_OK);
    CHECK(records.count == 3u);
    CHECK(records.codecs[0] == ELOG_CODEC_NONE);
    CHECK(records.codecs[1] == ELOG_CODEC_DELTA);
    CHECK(records.lengths[1] < sizeof b);
    uint8_t decoded[80];
    size_t decoded_length;
    CHECK(elog_delta_decode(records.payloads[1], records.lengths[1], a,
        sizeof a, 0u, decoded, sizeof decoded, &decoded_length) == ELOG_OK);
    CHECK(memcmp(decoded, b, sizeof b) == 0);
    /* Large deltas or a page boundary force a full snapshot. */
    CHECK(records.codecs[2] == ELOG_CODEC_NONE);
    return 0;
}

static elog_result_t repeat_encode(void *context, const uint8_t *source,
                                   size_t length, uint8_t *destination,
                                   size_t capacity, size_t *written)
{
    (void)context;
    if (source == NULL || destination == NULL || written == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length < 3u || capacity < 2u) {
        return ELOG_TOO_LARGE;
    }
    for (size_t i = 1u; i < length; ++i) {
        if (source[i] != source[0]) {
            return ELOG_TOO_LARGE;
        }
    }
    destination[0] = (uint8_t)length;
    destination[1] = source[0];
    *written = 2u;
    return ELOG_OK;
}

static elog_result_t repeat_decode(void *context, const uint8_t *source,
                                   size_t length, uint8_t *destination,
                                   size_t capacity, size_t *written)
{
    (void)context;
    if (source == NULL || destination == NULL || written == NULL ||
        length != 2u || source[0] > capacity) {
        return ELOG_INVALID_ARGUMENT;
    }
    memset(destination, source[1], source[0]);
    *written = source[0];
    return ELOG_OK;
}

static int test_custom_codec_integration(void)
{
    uint8_t memory[6u * 256u];
    elog_erase_region_t regions[6];
    elog_ram_storage_t ram;
    elog_storage_t storage;
    static elog_t logger;
    for (uint32_t i = 0u; i < 6u; ++i) {
        regions[i] = (elog_erase_region_t){i * 256u, 256u};
    }
    CHECK(elog_ram_storage_init(&ram, memory, sizeof memory,
         regions, 6u, 2u, 64u, &storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 512u};
    config.partition[ELOG_EVENT] = (elog_region_t){512u, 512u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){1024u, 512u};
    config.critical_queue_depth = 2u;
    config.event_queue_depth = 2u;
    config.routine_queue_depth = 2u;
    config.isr_queue_depth = 2u;
    config.max_payload = 80u;
    config.fault_table_depth = 2u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_CUSTOM_MIN;
    CHECK(elog_init(&logger, &config, &storage) == ELOG_INVALID_CONFIG);
    const elog_codec_t codec = {
        ELOG_CODEC_CUSTOM_MIN, NULL, repeat_encode, repeat_decode
    };
    config.custom_codec = &codec;
    CHECK(elog_init(&logger, &config, &storage) == ELOG_OK);
    const uint8_t repeated[12] = {7u, 7u, 7u, 7u, 7u, 7u,
                                  7u, 7u, 7u, 7u, 7u, 7u};
    const uint8_t varied[3] = {1u, 2u, 3u};
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     1u, repeated, sizeof repeated) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    CHECK(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                     2u, varied, sizeof varied) == ELOG_OK);
    CHECK(elog_flush(&logger, 100u) == ELOG_OK);
    compressed_records_t records;
    memset(&records, 0, sizeof records);
    CHECK(elog_iterate_records(&logger, collect, &records) == ELOG_OK);
    CHECK(records.count == 2u);
    CHECK(records.codecs[0] == ELOG_CODEC_CUSTOM_MIN);
    CHECK(records.lengths[0] == 2u);
    CHECK(records.codecs[1] == ELOG_CODEC_NONE);
    uint8_t decoded[80];
    size_t decoded_length;
    CHECK(codec.decode(codec.context, records.payloads[0], records.lengths[0],
                       decoded, sizeof decoded, &decoded_length) == ELOG_OK);
    CHECK(decoded_length == sizeof repeated);
    CHECK(memcmp(decoded, repeated, sizeof repeated) == 0);
    return 0;
}

int main(void)
{
    if (test_codec_functions() != 0 || test_core_delta_and_fallback() != 0 ||
        test_custom_codec_integration() != 0) {
        return 1;
    }
    puts("numeric codec round trips and fallback: OK");
    return 0;
}
