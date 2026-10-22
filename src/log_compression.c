/** @file
 * @brief Reversible VARINT and DELTA codecs for 32-bit telemetry words.
 *
 * The byte streams use ZigZag LEB128 values. DELTA stores a base sequence
 * so a decoder can reject an unavailable or mismatched prior sample.
 */
#include "log_codec.h"
#include "log_config.h"

#include <limits.h>

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)p[i] << (8u * i);
    }
    return value;
}

static int64_t read_i32(const uint8_t *p)
{
    const uint32_t raw = read_u32(p);
    return raw <= INT32_MAX ? (int64_t)raw :
           (int64_t)raw - INT64_C(4294967296);
}

static void write_i32(uint8_t *p, int64_t value)
{
    const uint32_t raw = (uint32_t)(value < 0 ?
                         value + INT64_C(4294967296) : value);
    for (unsigned int i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(raw >> (8u * i));
    }
}

/** @brief Map a signed difference to an unsigned value for LEB128 coding. */
static uint64_t zigzag(int64_t value)
{
    return value >= 0 ? (uint64_t)value << 1 :
           (((uint64_t)(-value) << 1) - 1u);
}

/** @brief Reverse ZigZag coding after varint bounds have been checked. */
static int64_t unzigzag(uint64_t value)
{
    return (value & 1u) == 0u ? (int64_t)(value >> 1) :
           -(int64_t)((value >> 1) + 1u);
}

/** @brief Append one bounded unsigned LEB128 value to an output buffer.
 * position advances only as bytes are written; callers report size errors.
 */
static elog_result_t put_varint(uint64_t value, uint8_t *destination,
                                size_t capacity, size_t *position)
{
    do {
        if (*position >= capacity) {
            return ELOG_TOO_LARGE;
        }
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value != 0u) {
            byte |= 0x80u;
        }
        destination[(*position)++] = byte;
    } while (value != 0u);
    return ELOG_OK;
}

/** @brief Parse one bounded LEB128 value, rejecting overflow and truncation.
 * At most ten bytes can represent a uint64_t, and the tenth byte has only
 * one usable payload bit.
 */
static elog_result_t get_varint(const uint8_t *source, size_t length,
                                size_t *position, uint64_t *value)
{
    uint64_t result = 0u;
    for (unsigned int i = 0u; i < 10u; ++i) {
        if (*position >= length) {
            return ELOG_CORRUPT;
        }
        const uint8_t byte = source[(*position)++];
        if (i == 9u && (byte & 0xFEu) != 0u) {
            return ELOG_CORRUPT;
        }
        result |= (uint64_t)(byte & 0x7Fu) << (7u * i);
        if ((byte & 0x80u) == 0u) {
            *value = result;
            return ELOG_OK;
        }
    }
    return ELOG_CORRUPT;
}

static int valid_numeric_input(const uint8_t *source, size_t length,
                                const uint8_t *destination, size_t *written)
{
    return source != NULL && destination != NULL && written != NULL &&
           length >= 4u && length <= ELOG_MAX_PAYLOAD && length % 4u == 0u;
}

elog_result_t elog_varint_encode(const uint8_t *source, size_t length,
                                 uint8_t *destination, size_t capacity,
                                 size_t *written)
{
    if (!valid_numeric_input(source, length, destination, written)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (capacity == 0u) {
        return ELOG_TOO_LARGE;
    }
    destination[0] = (uint8_t)(length / 4u);
    size_t position = 1u;
    for (size_t i = 0u; i < length; i += 4u) {
        const elog_result_t result = put_varint(zigzag(read_i32(source + i)),
                                          destination, capacity, &position);
        if (result != ELOG_OK) {
            return result;
        }
    }
    *written = position;
    return ELOG_OK;
}

elog_result_t elog_varint_decode(const uint8_t *source, size_t length,
                                 uint8_t *destination, size_t capacity,
                                 size_t *written)
{
    if (source == NULL || destination == NULL || written == NULL || length == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    const size_t words = source[0];
    if (words == 0u || words > ELOG_MAX_PAYLOAD / 4u ||
        capacity < words * 4u) {
        return ELOG_CORRUPT;
    }
    size_t position = 1u;
    for (size_t i = 0u; i < words; ++i) {
        uint64_t encoded;
        elog_result_t result = get_varint(source, length, &position, &encoded);
        if (result != ELOG_OK || encoded > UINT32_MAX) {
            return ELOG_CORRUPT;
        }
        write_i32(destination + i * 4u, unzigzag(encoded));
    }
    if (position != length) {
        return ELOG_CORRUPT;
    }
    *written = words * 4u;
    return ELOG_OK;
}

elog_result_t elog_delta_encode(const uint8_t *source,
                                const uint8_t *previous, size_t length,
                                uint64_t base_sequence, uint8_t *destination,
                                size_t capacity, size_t *written)
{
    if (!valid_numeric_input(source, length, destination, written) ||
        previous == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (capacity < 9u) {
        return ELOG_TOO_LARGE;
    }
    for (unsigned int i = 0u; i < 8u; ++i) {
        destination[i] = (uint8_t)(base_sequence >> (8u * i));
    }
    destination[8] = (uint8_t)(length / 4u);
    size_t position = 9u;
    for (size_t i = 0u; i < length; i += 4u) {
        const int64_t difference = read_i32(source + i) -
                                   read_i32(previous + i);
        const elog_result_t result = put_varint(zigzag(difference),
                                          destination, capacity, &position);
        if (result != ELOG_OK) {
            return result;
        }
    }
    *written = position;
    return ELOG_OK;
}

elog_result_t elog_delta_decode(const uint8_t *source, size_t length,
                                const uint8_t *previous,
                                size_t previous_length,
                                uint64_t expected_base_sequence,
                                uint8_t *destination, size_t capacity,
                                size_t *written)
{
    if (source == NULL || previous == NULL || destination == NULL ||
        written == NULL || length < 9u) {
        return ELOG_INVALID_ARGUMENT;
    }
    uint64_t base = 0u;
    for (unsigned int i = 0u; i < 8u; ++i) {
        base |= (uint64_t)source[i] << (8u * i);
    }
    const size_t words = source[8];
    if (base != expected_base_sequence || words == 0u ||
        words > ELOG_MAX_PAYLOAD / 4u || previous_length != words * 4u ||
        capacity < previous_length) {
        return ELOG_CORRUPT;
    }
    size_t position = 9u;
    for (size_t i = 0u; i < words; ++i) {
        uint64_t encoded;
        elog_result_t result = get_varint(source, length, &position, &encoded);
        if (result != ELOG_OK || encoded > UINT64_C(0x1FFFFFFFF)) {
            return ELOG_CORRUPT;
        }
        const int64_t value = read_i32(previous + i * 4u) +
                              unzigzag(encoded);
        if (value < INT32_MIN || value > INT32_MAX) {
            return ELOG_CORRUPT;
        }
        write_i32(destination + i * 4u, value);
    }
    if (position != length) {
        return ELOG_CORRUPT;
    }
    *written = previous_length;
    return ELOG_OK;
}
