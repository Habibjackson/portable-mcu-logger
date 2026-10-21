#ifndef ELOG_CODEC_H
#define ELOG_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "log_types.h"

#define ELOG_CODEC_CUSTOM_MIN 128u

/** @brief Per-record lossless codec extension for IDs 128..255.
 *
 * Custom codecs use IDs 128..255 and must encode each record independently.
 * The callback/context must remain valid for the logger lifetime and be
 * bounded in time and memory. Decoders need the matching implementation.
 * Callbacks operate on caller-owned buffers and set written only on ELOG_OK.
 * The logger stores the original payload when encoding returns ELOG_TOO_LARGE
 * or would not shrink it. Other encoder errors are returned to the caller.
 * The core requires a decode callback but leaves decoding to the record
 * consumer.
 */
typedef struct {
    uint8_t id; /**< Persisted codec ID, 128..255. */
    void *context; /**< Passed unchanged to encode and decode. */
    /** Encode one independent record; output must be losslessly reversible. */
    elog_result_t (*encode)(void *context, const uint8_t *source,
                            size_t source_length, uint8_t *destination,
                            size_t destination_capacity, size_t *written);
    /** Decode one complete record into caller-owned destination memory. */
    elog_result_t (*decode)(void *context, const uint8_t *source,
                            size_t source_length, uint8_t *destination,
                            size_t destination_capacity, size_t *written);
} elog_codec_t;

/** @brief Encode signed little-endian 32-bit words as ZigZag varints.
 *
 * @param source Input bytes; length must be a nonzero multiple of four and at
 *               most ELOG_MAX_PAYLOAD.
 * @param length Number of readable source bytes.
 * @param destination Caller-owned output buffer of capacity bytes.
 * @param capacity Size of destination in bytes.
 * @param written Exact encoded byte count on ELOG_OK only.
 * @return ELOG_OK, ELOG_TOO_LARGE for insufficient output space, or an
 *         argument error. Encoding is byte-for-byte reversible.
 */
elog_result_t elog_varint_encode(const uint8_t *source, size_t length,
                                 uint8_t *destination, size_t capacity,
                                 size_t *written);

/** @brief Decode one complete VARINT payload into signed 32-bit word bytes.
 *
 * @param source Encoded bytes with a leading word count.
 * @param length Exact encoded length; trailing bytes are rejected.
 * @param destination Caller-owned output buffer of capacity bytes.
 * @param capacity Size of destination in bytes.
 * @param written Decoded byte count on ELOG_OK only.
 * @return ELOG_OK, ELOG_CORRUPT for malformed input, or an argument error.
 */
elog_result_t elog_varint_decode(const uint8_t *source, size_t length,
                                 uint8_t *destination, size_t capacity,
                                 size_t *written);

/** @brief Encode signed word differences from a validated prior sample.
 *
 * @param source Current little-endian signed 32-bit words.
 * @param previous Prior words with exactly the same length and schema.
 * @param length Byte length of both buffers, a nonzero multiple of four and at
 *               most ELOG_MAX_PAYLOAD.
 * @param base_sequence Persisted sequence number of previous.
 * @param destination Caller-owned output buffer of capacity bytes.
 * @param capacity Size of destination in bytes.
 * @param written Encoded byte count on ELOG_OK only.
 * @return ELOG_OK, ELOG_TOO_LARGE for insufficient space, or an argument error.
 */
elog_result_t elog_delta_encode(const uint8_t *source,
                                const uint8_t *previous, size_t length,
                                uint64_t base_sequence, uint8_t *destination,
                                size_t capacity, size_t *written);

/** @brief Decode DELTA after checking the expected base sequence.
 *
 * @param source Encoded DELTA bytes, including the base sequence and count.
 * @param length Exact encoded byte length; trailing bytes are rejected.
 * @param previous Decoded prior sample, with previous_length bytes.
 * @param previous_length Number of readable bytes in previous.
 * @param expected_base_sequence Sequence number of that prior sample.
 * @param destination Caller-owned output buffer of capacity bytes.
 * @param capacity Size of destination in bytes.
 * @param written Decoded byte count on ELOG_OK only.
 * @return ELOG_OK, ELOG_CORRUPT for a broken chain, malformed stream, or
 *         out-of-range reconstructed signed value; argument error otherwise.
 */
elog_result_t elog_delta_decode(const uint8_t *source, size_t length,
                                const uint8_t *previous,
                                size_t previous_length,
                                uint64_t expected_base_sequence,
                                uint8_t *destination, size_t capacity,
                                size_t *written);

#endif
