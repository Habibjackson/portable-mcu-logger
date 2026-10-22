/** @file
 * @brief RAM-backed flash model for host examples and tests.
 *
 * Programming requires erased bytes and respects caller-supplied geometry.
 * Formatting is explicit and destroys the image; initialization preserves it.
 */
#ifndef ELOG_STORAGE_RAM_H
#define ELOG_STORAGE_RAM_H

#include <stddef.h>
#include <stdint.h>
#include "log_storage.h"

/** @brief Caller-owned RAM flash model; fields are set by init. */
typedef struct {
    uint8_t *bytes;
    uint32_t capacity;
    const elog_erase_region_t *regions;
    size_t region_count;
    uint32_t program_granularity;
    uint32_t max_program_size;
} elog_ram_storage_t;

/** @brief Bind a byte array and erase geometry to the storage interface.
 *
 * @param ram Caller-owned backend instance.
 * @param bytes Existing capacity-byte array; init does not erase it.
 * @param capacity Size of bytes and total geometry in bytes.
 * @param regions Ordered, gap-free erase-region cover of [0, capacity).
 * @param region_count Number of entries in regions.
 * @param program_granularity Required program address/length alignment.
 * @param max_program_size Largest accepted program() call, a multiple of
 *                         program_granularity.
 * @param storage Output handle to pass to elog_init().
 * @return ELOG_OK or ELOG_INVALID_CONFIG for invalid geometry/arguments.
 * @note bytes, regions, and ram must outlive storage and the logger. This
 *       backend enforces erase-before-program and variable region sizes.
 */
elog_result_t elog_ram_storage_init(elog_ram_storage_t *ram, uint8_t *bytes,
                                    uint32_t capacity,
                                    const elog_erase_region_t *regions,
                                    size_t region_count,
                                    uint32_t program_granularity,
                                    uint32_t max_program_size,
                                    elog_storage_t *storage);

/** @brief Set the entire RAM image to erased 0xFF bytes.
 *
 * Useful when creating a new host fixture. It destroys existing records;
 * neither storage initialization nor logger initialization calls it.
 * @return ELOG_OK or ELOG_INVALID_ARGUMENT for an uninitialized backend.
 */
elog_result_t elog_ram_storage_format(elog_ram_storage_t *ram);

#endif
