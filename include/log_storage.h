/** @file
 * @brief Synchronous device-relative storage contract for the journal.
 *
 * Backends supply erase geometry, bounded reads and writes, and completion
 * status. The application reserves the permitted address range separately.
 */
#ifndef ELOG_STORAGE_H
#define ELOG_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "log_types.h"

/** @brief One device erase region, with exclusive end start + length. */
typedef struct {
    uint32_t start;  /**< Device-relative first byte. */
    uint32_t length; /**< Bytes erased together by one erase() call. */
} elog_erase_region_t;

/** @brief Synchronous storage operations used by the portable journal.
 *
 * Addresses are device-relative byte offsets. An adapter must reject access
 * outside its permitted range. Program addresses and lengths are multiples of
 * program_granularity(), and no program may exceed max_program_size(). Only
 * erased program units may be changed. erase() removes exactly one full region
 * returned by erase_region(); regions may have different lengths.
 *
 * timeout_ms is the caller's requested bound. A backend must return a timeout
 * or explicitly reject a bound it cannot honor. All calls return after the
 * device operation finishes; successful writes and erases are verified.
 * Source and destination buffers remain caller-owned until the call returns.
 * A backend may split a program call at its chip program-page boundary.
 */
typedef struct {
    uint32_t (*capacity)(void *context); /**< Device size in bytes. */
    uint32_t (*program_granularity)(void *context); /**< Minimum write unit. */
    uint32_t (*max_program_size)(void *context); /**< Maximum bytes per call. */
    /** Return the full erase region containing address. */
    elog_result_t (*erase_region)(void *context, uint32_t address,
                                  elog_erase_region_t *region);
    /** Read bytes into destination; no erase or program side effect. */
    elog_result_t (*read)(void *context, uint32_t address,
                          void *destination, size_t length);
    /** Program erased units and verify completion before returning ELOG_OK. */
    elog_result_t (*program)(void *context, uint32_t address,
                             const void *source, size_t length,
                             uint32_t timeout_ms);
    /** Erase and verify exactly the region beginning at address. */
    elog_result_t (*erase)(void *context, uint32_t address,
                           uint32_t length, uint32_t timeout_ms);
    /** Report busy state or a status-read error without hiding the error. */
    elog_result_t (*is_busy)(void *context, bool *busy);
    /** Wait for outstanding device work, returning BUSY/TIMEOUT if needed. */
    elog_result_t (*sync)(void *context, uint32_t timeout_ms);
} elog_storage_ops_t;

/** @brief Storage method table and caller-owned backend instance.
 *
 * Both ops and context must remain valid for the logger lifetime.
 */
typedef struct {
    const elog_storage_ops_t *ops;
    void *context;
} elog_storage_t;

#endif
