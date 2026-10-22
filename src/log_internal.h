/** @file
 * @brief Private journal page states and cross-module helpers.
 *
 * Applications use log.h. These declarations coordinate the task-side core,
 * byte-format parser, and read-only recovery scanner. Page indexes and
 * process states belong to elog_t and must not be edited by applications.
 */
#ifndef ELOG_INTERNAL_H
#define ELOG_INTERNAL_H

#include "log.h"

/** @brief Scanner classification of one physical erase page. */
enum {
    ELOG_PAGE_ERASED = 0,
    ELOG_PAGE_VALID = 1,
    ELOG_PAGE_TORN = 2
};

/** @brief One-step journal writer phases, including separate commit writes. */
enum {
    ELOG_PROCESS_IDLE = 0,
    ELOG_PROCESS_ERASE_PAGE = 1,
    ELOG_PROCESS_PAGE_BODY = 2,
    ELOG_PROCESS_PAGE_VERIFY_BODY = 3,
    ELOG_PROCESS_PAGE_COMMIT = 4,
    ELOG_PROCESS_PAGE_VERIFY_COMMIT = 5,
    ELOG_PROCESS_RECORD_BODY = 6,
    ELOG_PROCESS_RECORD_VERIFY_BODY = 7,
    ELOG_PROCESS_RECORD_COMMIT = 8,
    ELOG_PROCESS_RECORD_VERIFY_COMMIT = 9,
    ELOG_PROCESS_FAILED = 10
};

/** @brief Bind a caller-owned fixed array as an empty pending-record queue.
 * @param queue Queue metadata to initialize.
 * @param slots Backing entries retained for the queue lifetime.
 * @param capacity Number of usable slots in that array.
 */
void elog_queue_setup(elog_queue_t *queue, elog_entry_t *slots,
                      uint8_t capacity);
/** @brief Copy one entry into a queue with lane-specific overflow policy.
 * @param queue Initialized queue.
 * @param entry Entry copied before return.
 * @param drop_oldest Nonzero replaces the oldest entry when full.
 * @return ELOG_OK, ELOG_QUEUED_WITH_DROP, ELOG_QUEUE_FULL, or an argument error.
 */
elog_result_t elog_queue_push(elog_queue_t *queue, const elog_entry_t *entry,
                              uint8_t drop_oldest);
/** @brief Remove the oldest pending entry without touching storage.
 * @return ELOG_OK, ELOG_EMPTY, or ELOG_INVALID_ARGUMENT.
 */
elog_result_t elog_queue_pop(elog_queue_t *queue, elog_entry_t *entry);

/** @brief Validate partitions against backend erase geometry and build pages.
 * @return ELOG_OK or an invalid-configuration/storage error.
 * @note This only queries geometry; it does not format or erase flash.
 */
elog_result_t elog_build_pages(elog_t *logger);
/** @brief Reconstruct page generations and committed tails from storage.
 * @return ELOG_OK, ELOG_CORRUPT/UNSUPPORTED, or a storage error.
 * @note Scanning is read-only; uncertain tails are sealed in RAM metadata.
 */
elog_result_t elog_scan_pages(elog_t *logger);
/** @brief Select a free page or an eligible page for rotation.
 * @param priority CRITICAL, EVENT, or ROUTINE lane.
 * @param index Receives the chosen index in logger.pages on ELOG_OK.
 * @param needs_erase Receives whether that page must first be erased.
 * @return ELOG_STORAGE_FULL if no page can be selected.
 * @note A critical page is never selected for automatic erase.
 */
elog_result_t elog_choose_page(elog_t *logger, uint8_t priority,
                               int16_t *index, uint8_t *needs_erase);
/** @brief Build a 32-byte page header image with CRC and commit pattern.
 * @note The caller must write and verify the body before the final commit unit.
 */
void elog_make_page_header(uint8_t *image, uint32_t granularity,
                           uint8_t priority, uint64_t generation);
/** @brief Validate a fully committed page header for the expected lane.
 * @param generation Receives the retained generation on ELOG_OK.
 * @return ELOG_OK, ELOG_CORRUPT, ELOG_UNSUPPORTED, or argument error.
 */
elog_result_t elog_validate_page_header(const uint8_t *image,
                                        uint32_t granularity,
                                        uint8_t priority,
                                        uint64_t *generation);
/** @brief Compare wrapping sequence or generation numbers by half range.
 * @return 1 if a is newer, 0 if not, or -1 for ambiguous half-range distance.
 */
int elog_sequence_newer(uint64_t a, uint64_t b);

#endif
