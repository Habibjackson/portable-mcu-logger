#ifndef ELOG_CONFIG_H
#define ELOG_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include "log_types.h"
#include "log_codec.h"

/** Compile-time storage and RAM bounds; runtime settings may be smaller. */
#define ELOG_MAX_PAYLOAD 80u
#define ELOG_MAX_RECORD 128u
#define ELOG_MAX_PROGRAM_GRANULARITY 8u
#define ELOG_PAGE_HEADER_SIZE 32u
#define ELOG_MAX_PAGES 64u
#define ELOG_MAX_CRITICAL_QUEUE 8u
#define ELOG_MAX_EVENT_QUEUE 8u
#define ELOG_MAX_ROUTINE_QUEUE 16u
#define ELOG_MAX_ISR_QUEUE 4u
#define ELOG_MAX_FAULTS 8u
#define ELOG_MAX_PRE_FAULT 32u
#define ELOG_MAX_POST_FAULT 8u

/** @brief Device-relative byte range, with an exclusive end. */
typedef struct {
    uint32_t offset; /**< First byte in the storage device. */
    uint32_t length; /**< Number of bytes; must cover whole erase regions. */
} elog_region_t;

/** @brief Return monotonic time in milliseconds from the ordinary task.
 *
 * The callback must be bounded and must not call back into the same logger.
 */
typedef uint64_t (*elog_timestamp_fn)(void *context);

/** @brief Return nonzero when starting one erase is safe for the application.
 *
 * This callback does not gate program operations. It must be bounded and must
 * not call back into the same logger.
 */
typedef int (*elog_safe_erase_fn)(void *context);

/** @brief Runtime policy validated by elog_init().
 *
 * Zero-initialize before setting fields. Arrays are indexed by
 * elog_priority_t. CRC and recovery are mandatory in format version 1.
 * Backend geometry determines valid partition boundaries.
 */
typedef struct {
    elog_region_t partition[3]; /**< Disjoint critical/event/routine regions. */
    uint8_t critical_queue_depth; /**< Reject new records when full. */
    uint8_t event_queue_depth; /**< Reject new records when full. */
    uint8_t routine_queue_depth; /**< Replace oldest queued record when full. */
    uint8_t isr_queue_depth; /**< Capacity for one ISR producer, minimum 2. */
    uint8_t pre_fault_depth; /**< Recent telemetry samples to freeze. */
    uint8_t post_fault_depth; /**< Telemetry samples after a fault. */
    uint8_t max_payload; /**< Runtime payload limit, 12..ELOG_MAX_PAYLOAD. */
    uint8_t fault_table_depth; /**< Simultaneously active fault IDs. */
    uint16_t snapshot_interval; /**< Required for DELTA; zero otherwise. */
    uint16_t block_size; /**< Reserved; must be zero in this version. */
    uint32_t routine_interval_ms; /**< Minimum time between routine enqueues. */
    uint32_t fault_repeat_interval_ms; /**< Interval between repeat summaries. */
    uint32_t storage_timeout_ms; /**< Requested bound for backend operations. */
    uint8_t recover_on_init; /**< Must be 1; startup scans existing records. */
    uint8_t require_crc; /**< Must be 1 in format version 1. */
    uint8_t codec_id; /**< NONE, DELTA, VARINT, or custom ID 128..255. */
    const elog_codec_t *custom_codec; /**< Required for custom ID; else NULL. */
    elog_timestamp_fn timestamp; /**< Required when a time interval is set. */
    void *timestamp_context; /**< Passed unchanged to timestamp. */
    elog_safe_erase_fn may_erase; /**< NULL allows erases at any task call. */
    void *may_erase_context; /**< Passed unchanged to may_erase. */
} elog_config_t;

#endif
