#ifndef ELOG_TYPES_H
#define ELOG_TYPES_H

#include <stdint.h>

/** @brief Result of an API or storage operation.
 *
 * Positive values are nonfatal flow results; negative values are errors.
 */
typedef enum {
    ELOG_OK = 0,
    ELOG_QUEUED_WITH_DROP = 1,
    ELOG_EMPTY = 2,
    ELOG_WOULD_BLOCK = 3,
    ELOG_INVALID_ARGUMENT = -1,
    ELOG_INVALID_CONFIG = -2,
    ELOG_TOO_LARGE = -3,
    ELOG_QUEUE_FULL = -4,
    ELOG_STORAGE_FULL = -5,
    ELOG_STORAGE_IO = -6,
    ELOG_STORAGE_TIMEOUT = -7,
    ELOG_CORRUPT = -8,
    ELOG_UNSUPPORTED = -9,
    ELOG_BUSY = -10
} elog_result_t;

/** @brief Priority partition and scheduler order. */
typedef enum {
    ELOG_CRITICAL = 0,
    ELOG_EVENT = 1,
    ELOG_ROUTINE = 2
} elog_priority_t;

/** @brief Persisted record kind in format version 1. */
typedef enum {
    ELOG_TYPE_TELEMETRY = 1,
    ELOG_TYPE_EVENT = 2,
    ELOG_TYPE_FAULT = 3,
    ELOG_TYPE_FAULT_RECOVERY = 4,
    ELOG_TYPE_FAULT_REPEAT = 5,
    ELOG_TYPE_RESET = 6,
    ELOG_TYPE_PRE_FAULT = 7,
    ELOG_TYPE_POST_FAULT = 8
} elog_record_type_t;

/** @brief Persisted compression mode; BLOCK is reserved. */
typedef enum {
    ELOG_CODEC_NONE = 0,
    ELOG_CODEC_DELTA = 1,
    ELOG_CODEC_VARINT = 2,
    ELOG_CODEC_BLOCK = 3
} elog_codec_id_t;

#endif
