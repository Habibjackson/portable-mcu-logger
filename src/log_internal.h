#ifndef ELOG_INTERNAL_H
#define ELOG_INTERNAL_H

#include "log.h"

enum {
    ELOG_PAGE_ERASED = 0,
    ELOG_PAGE_VALID = 1,
    ELOG_PAGE_TORN = 2
};

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

void elog_queue_setup(elog_queue_t *queue, elog_entry_t *slots,
                      uint8_t capacity);
elog_result_t elog_queue_push(elog_queue_t *queue, const elog_entry_t *entry,
                              uint8_t drop_oldest);
elog_result_t elog_queue_pop(elog_queue_t *queue, elog_entry_t *entry);

elog_result_t elog_build_pages(elog_t *logger);
elog_result_t elog_scan_pages(elog_t *logger);
elog_result_t elog_choose_page(elog_t *logger, uint8_t priority,
                               int16_t *index, uint8_t *needs_erase);
void elog_make_page_header(uint8_t *image, uint32_t granularity,
                           uint8_t priority, uint64_t generation);
elog_result_t elog_validate_page_header(const uint8_t *image,
                                        uint32_t granularity,
                                        uint8_t priority,
                                        uint64_t *generation);
int elog_sequence_newer(uint64_t a, uint64_t b);

#endif
