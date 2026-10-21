#ifndef ELOG_FORMAT_H
#define ELOG_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include "log_config.h"

#define ELOG_FORMAT_VERSION 1u
#define ELOG_RECORD_HEADER_SIZE 36u
#define ELOG_RECORD_CRC_SIZE 4u
#define ELOG_COMMIT_BYTE 0xA5u
#define ELOG_FLAG_ABSOLUTE_TIME 0x01u

/** @brief Parsed record values, never copied as a raw C struct to storage.
 *
 * payload refers to encoded bytes in the caller's image or write buffer.
 * original_length is the size after decoding; stored_length is the bytes on
 * flash. All multibyte on-storage fields are explicitly little-endian.
 */
typedef struct {
    uint8_t type;
    uint8_t priority;
    uint8_t codec_id;
    uint8_t flags;
    uint64_t sequence;
    uint64_t timestamp;
    uint32_t event_id;
    uint16_t original_length;
    uint16_t stored_length;
    const uint8_t *payload;
} elog_record_t;

/** @brief Alignment-aware placement of record body and final commit unit. */
typedef struct {
    size_t body_length;   /* Header + payload + CRC + erased alignment bytes. */
    size_t commit_offset; /* Equal to body_length. */
    size_t commit_length;
    size_t total_length;
} elog_record_layout_t;

/** @brief Compute reflected CRC-32/IEEE over length bytes.
 *
 * Uses polynomial 0xEDB88320, initial state 0xFFFFFFFF, and final XOR
 * 0xFFFFFFFF. bytes must remain readable for the call; NULL is allowed only
 * when length is zero.
 */
uint32_t elog_crc32(const uint8_t *bytes, size_t length);

/** @brief Validate a version-1 record header and calculate its physical size.
 *
 * @param header At least ELOG_RECORD_HEADER_SIZE readable bytes.
 * @param header_length Available header bytes.
 * @param granularity Backend program unit in bytes, at most 8.
 * @param layout Use the calculated offsets only on ELOG_OK.
 * @return ELOG_OK, ELOG_UNSUPPORTED for another format version,
 *         ELOG_CORRUPT for invalid fields, or an argument/size error.
 * @note This function has no payload, CRC, or commit bytes to validate.
 */
elog_result_t elog_record_layout_from_header(const uint8_t *header,
                                              size_t header_length,
                                              uint32_t granularity,
                                              elog_record_layout_t *layout);

/** @brief Serialize a record into a caller-owned image without writing flash.
 *
 * @param record Semantic values and stored payload to serialize.
 * @param granularity Backend program unit used for padding and commit.
 * @param destination Receives body, CRC, erased padding, and commit pattern.
 * @param destination_capacity Writable bytes at destination.
 * @param layout Body and commit offsets filled on ELOG_OK.
 * @return ELOG_OK or an argument/size error.
 * @warning The image contains the commit pattern, but a storage writer must
 *          program the body and verify it before programming that final unit.
 */
elog_result_t elog_record_encode(const elog_record_t *record,
                                 uint32_t granularity, uint8_t *destination,
                                 size_t destination_capacity,
                                 elog_record_layout_t *layout);

/** @brief Validate and parse one fully committed physical record image.
 *
 * Checks header, lengths, CRC, erased padding, and commit unit. The returned
 * payload is still encoded; it borrows image memory for as long as the caller
 * keeps that image alive. No codec decompression occurs here.
 * @return ELOG_OK, ELOG_CORRUPT, ELOG_UNSUPPORTED, or an argument/size error.
 */
elog_result_t elog_record_decode(const uint8_t *image, size_t image_length,
                                 uint32_t granularity, elog_record_t *record,
                                 elog_record_layout_t *layout);

#endif
