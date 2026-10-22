/** @file
 * @brief Choose a page for the next journal generation.
 *
 * Critical pages are never selected for automatic erase. Event and routine
 * lanes may reuse a torn or oldest inactive page when their free pages end.
 */
#include "log_internal.h"

elog_result_t elog_choose_page(elog_t *logger, uint8_t priority,
                               int16_t *index, uint8_t *needs_erase)
{
    if (logger == NULL || index == NULL || needs_erase == NULL || priority > 2u) {
        return ELOG_INVALID_ARGUMENT;
    }
    for (uint8_t i = 0u; i < logger->page_count; ++i) {
        if (logger->pages[i].priority == priority &&
            logger->pages[i].state == ELOG_PAGE_ERASED) {
            *index = (int16_t)i;
            *needs_erase = 0u;
            return ELOG_OK;
        }
    }
    if (priority == (uint8_t)ELOG_CRITICAL) {
        /* Fault history requires explicit export and service; never recycle
         * critical pages automatically when they are full. */
        return ELOG_STORAGE_FULL;
    }
    int16_t oldest = -1;
    for (uint8_t i = 0u; i < logger->page_count; ++i) {
        const elog_page_t *page = &logger->pages[i];
        if (page->priority != priority ||
            logger->active_page[priority] == (int16_t)i) {
            continue;
        }
        if (page->state == ELOG_PAGE_TORN) {
            /* Torn pages have no reusable tail. They can only be used after
             * a whole-page erase permitted by the application. */
            *index = (int16_t)i;
            *needs_erase = 1u;
            return ELOG_OK;
        }
        if (page->state != ELOG_PAGE_VALID) {
            continue;
        }
        if (oldest < 0 || elog_sequence_newer(logger->pages[oldest].generation,
                                              page->generation) > 0) {
            oldest = (int16_t)i;
        }
    }
    if (oldest < 0) {
        return ELOG_STORAGE_FULL;
    }
    *index = oldest;
    *needs_erase = 1u;
    return ELOG_OK;
}
