/** @file
 * @brief Encode and validate version-1 records and CRC-32 checksums.
 *
 * Fields are serialized explicitly in little-endian order. Decoding checks
 * the header, length, CRC, erased padding, and final commit unit.
 */
#include "log_format.h"

#include <limits.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned int i = 0u; i < 8u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)p[i] << (8u * i);
    }
    return value;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0u;
    for (unsigned int i = 0u; i < 8u; ++i) {
        value |= (uint64_t)p[i] << (8u * i);
    }
    return value;
}

static int valid_type(uint8_t type)
{
    return type >= (uint8_t)ELOG_TYPE_TELEMETRY &&
           type <= (uint8_t)ELOG_TYPE_POST_FAULT;
}

static int valid_codec(uint8_t codec)
{
    return codec <= (uint8_t)ELOG_CODEC_BLOCK ||
           codec >= ELOG_CODEC_CUSTOM_MIN;
}

static int valid_granularity(uint32_t granularity)
{
    return granularity > 0u && granularity <= ELOG_MAX_PROGRAM_GRANULARITY;
}

uint32_t elog_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    if (bytes == NULL && length != 0u) {
        return 0u;
    }
    for (size_t i = 0u; i < length; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (unsigned int bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) != 0u ? UINT32_C(0xEDB88320) : 0u);
        }
    }
    return ~crc;
}

/** @brief Reserve aligned body bytes and a separate final commit unit.
 * Padding remains erased so the decoder can distinguish an intact body from
 * a torn or unexpectedly programmed image.
 */
static elog_result_t make_layout(uint16_t stored_length,
                                  uint32_t granularity,
                                  elog_record_layout_t *layout)
{
    if (layout == NULL || !valid_granularity(granularity)) {
        return ELOG_INVALID_ARGUMENT;
    }
    const size_t base = ELOG_RECORD_HEADER_SIZE + (size_t)stored_length +
                        ELOG_RECORD_CRC_SIZE;
    const size_t g = (size_t)granularity;
    const size_t padding = (g - (base % g)) % g;
    if (base > SIZE_MAX - padding - g) {
        return ELOG_TOO_LARGE;
    }
    layout->body_length = base + padding;
    layout->commit_offset = layout->body_length;
    layout->commit_length = g;
    layout->total_length = layout->body_length + g;
    return layout->total_length <= ELOG_MAX_RECORD ? ELOG_OK : ELOG_TOO_LARGE;
}

/** @brief Reject unsupported versions and inconsistent fixed header fields.
 * Payload, CRC, padding, and commit verification happen after layout parsing.
 */
static elog_result_t check_header(const uint8_t *header)
{
    if (memcmp(header, "ELOG", 4u) != 0) {
        return ELOG_CORRUPT;
    }
    if (header[4] != ELOG_FORMAT_VERSION) {
        return ELOG_UNSUPPORTED;
    }
    if (header[5] != ELOG_RECORD_HEADER_SIZE || !valid_type(header[6]) ||
        header[7] > (uint8_t)ELOG_ROUTINE || !valid_codec(header[8]) ||
        (header[9] & (uint8_t)~ELOG_FLAG_ABSOLUTE_TIME) != 0u ||
        header[10] != 0u || header[11] != 0u) {
        return ELOG_CORRUPT;
    }
    if (header[8] == (uint8_t)ELOG_CODEC_NONE &&
        get16(header + 32u) != get16(header + 34u)) {
        return ELOG_CORRUPT;
    }
    return ELOG_OK;
}

elog_result_t elog_record_layout_from_header(const uint8_t *header,
                                              size_t header_length,
                                              uint32_t granularity,
                                              elog_record_layout_t *layout)
{
    if (header == NULL || layout == NULL || header_length < ELOG_RECORD_HEADER_SIZE) {
        return ELOG_INVALID_ARGUMENT;
    }
    const elog_result_t result = check_header(header);
    return result == ELOG_OK ? make_layout(get16(header + 34u), granularity, layout)
                             : result;
}

elog_result_t elog_record_encode(const elog_record_t *record,
                                 uint32_t granularity, uint8_t *destination,
                                 size_t destination_capacity,
                                 elog_record_layout_t *layout)
{
    if (record == NULL || destination == NULL || layout == NULL ||
        (record->payload == NULL && record->stored_length != 0u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (!valid_type(record->type) || record->priority > (uint8_t)ELOG_ROUTINE ||
        !valid_codec(record->codec_id) ||
        (record->flags & (uint8_t)~ELOG_FLAG_ABSOLUTE_TIME) != 0u ||
        (record->codec_id == (uint8_t)ELOG_CODEC_NONE &&
         record->original_length != record->stored_length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    elog_result_t result = make_layout(record->stored_length, granularity, layout);
    if (result != ELOG_OK) {
        return result;
    }
    if (destination_capacity < layout->total_length) {
        return ELOG_TOO_LARGE;
    }
    memset(destination, 0xFF, layout->total_length);
    memcpy(destination, "ELOG", 4u);
    destination[4] = ELOG_FORMAT_VERSION;
    destination[5] = ELOG_RECORD_HEADER_SIZE;
    destination[6] = record->type;
    destination[7] = record->priority;
    destination[8] = record->codec_id;
    destination[9] = record->flags;
    destination[10] = 0u;
    destination[11] = 0u;
    put64(destination + 12u, record->sequence);
    put64(destination + 20u, record->timestamp);
    put32(destination + 28u, record->event_id);
    put16(destination + 32u, record->original_length);
    put16(destination + 34u, record->stored_length);
    if (record->stored_length != 0u) {
        memcpy(destination + ELOG_RECORD_HEADER_SIZE, record->payload,
               record->stored_length);
    }
    const size_t crc_offset = ELOG_RECORD_HEADER_SIZE + record->stored_length;
    put32(destination + crc_offset, elog_crc32(destination, crc_offset));
    memset(destination + layout->commit_offset, ELOG_COMMIT_BYTE,
           layout->commit_length);
    return ELOG_OK;
}

elog_result_t elog_record_decode(const uint8_t *image, size_t image_length,
                                 uint32_t granularity, elog_record_t *record,
                                 elog_record_layout_t *layout)
{
    if (image == NULL || record == NULL || layout == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (image_length < ELOG_RECORD_HEADER_SIZE) {
        return ELOG_CORRUPT;
    }
    elog_result_t result = elog_record_layout_from_header(image,
                                    ELOG_RECORD_HEADER_SIZE, granularity, layout);
    if (result != ELOG_OK) {
        return result;
    }
    if (image_length < layout->total_length) {
        return ELOG_CORRUPT;
    }
    const uint16_t length = get16(image + 34u);
    const size_t crc_offset = ELOG_RECORD_HEADER_SIZE + length;
    if (get32(image + crc_offset) != elog_crc32(image, crc_offset)) {
        return ELOG_CORRUPT;
    }
    for (size_t i = crc_offset + ELOG_RECORD_CRC_SIZE;
         i < layout->commit_offset; ++i) {
        if (image[i] != 0xFFu) {
            return ELOG_CORRUPT;
        }
    }
    for (size_t i = layout->commit_offset; i < layout->total_length; ++i) {
        if (image[i] != ELOG_COMMIT_BYTE) {
            return ELOG_CORRUPT;
        }
    }
    record->type = image[6];
    record->priority = image[7];
    record->codec_id = image[8];
    record->flags = image[9];
    record->sequence = get64(image + 12u);
    record->timestamp = get64(image + 20u);
    record->event_id = get32(image + 28u);
    record->original_length = get16(image + 32u);
    record->stored_length = length;
    record->payload = image + ELOG_RECORD_HEADER_SIZE;
    return ELOG_OK;
}
