#include "log_internal.h"

#include <limits.h>
#include <string.h>

static uint32_t read32(const uint8_t *p)
{
    uint32_t value = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        value |= (uint32_t)p[i] << (8u * i);
    }
    return value;
}

static uint64_t read64(const uint8_t *p)
{
    uint64_t value = 0u;
    for (unsigned int i = 0u; i < 8u; ++i) {
        value |= (uint64_t)p[i] << (8u * i);
    }
    return value;
}

static void write32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static void write64(uint8_t *p, uint64_t value)
{
    for (unsigned int i = 0u; i < 8u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static int all_erased(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0u; i < length; ++i) {
        if (bytes[i] != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

static elog_result_t check_erased_range(elog_t *logger,
                                        const elog_page_t *page,
                                        uint32_t start_offset,
                                        uint8_t *is_erased)
{
    const elog_storage_ops_t *ops = logger->storage.ops;
    *is_erased = 1u;
    for (uint32_t offset = start_offset; offset < page->length;) {
        const uint32_t remaining = page->length - offset;
        const size_t count = remaining < ELOG_MAX_RECORD ?
                             (size_t)remaining : ELOG_MAX_RECORD;
        const elog_result_t result = ops->read(logger->storage.context,
            page->start + offset, logger->verify_image, count);
        if (result != ELOG_OK) {
            return result;
        }
        if (!all_erased(logger->verify_image, count)) {
            *is_erased = 0u;
            return ELOG_OK;
        }
        offset += (uint32_t)count;
    }
    return ELOG_OK;
}

int elog_sequence_newer(uint64_t a, uint64_t b)
{
    const uint64_t distance = a - b;
    if (distance == UINT64_C(0x8000000000000000)) {
        return -1;
    }
    return distance != 0u && distance < UINT64_C(0x8000000000000000) ? 1 : 0;
}

void elog_make_page_header(uint8_t *image, uint32_t granularity,
                           uint8_t priority, uint64_t generation)
{
    memset(image, 0xFF, ELOG_PAGE_HEADER_SIZE);
    memcpy(image, "ELPG", 4u);
    image[4] = ELOG_FORMAT_VERSION;
    image[5] = priority;
    image[6] = ELOG_PAGE_HEADER_SIZE;
    image[7] = 0u;
    write64(image + 8u, generation);
    write32(image + 16u, elog_crc32(image, 16u));
    memset(image + ELOG_PAGE_HEADER_SIZE - granularity, ELOG_COMMIT_BYTE,
           granularity);
}

elog_result_t elog_validate_page_header(const uint8_t *image,
                                        uint32_t granularity,
                                        uint8_t priority,
                                        uint64_t *generation)
{
    if (image == NULL || generation == NULL || granularity == 0u ||
        granularity > ELOG_MAX_PROGRAM_GRANULARITY) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (memcmp(image, "ELPG", 4u) != 0) {
        return ELOG_CORRUPT;
    }
    if (image[4] != ELOG_FORMAT_VERSION) {
        return ELOG_UNSUPPORTED;
    }
    if (image[5] != priority || image[6] != ELOG_PAGE_HEADER_SIZE ||
        image[7] != 0u || read32(image + 16u) != elog_crc32(image, 16u)) {
        return ELOG_CORRUPT;
    }
    for (size_t i = 20u; i < ELOG_PAGE_HEADER_SIZE - granularity; ++i) {
        if (image[i] != 0xFFu) {
            return ELOG_CORRUPT;
        }
    }
    for (size_t i = ELOG_PAGE_HEADER_SIZE - granularity;
         i < ELOG_PAGE_HEADER_SIZE; ++i) {
        if (image[i] != ELOG_COMMIT_BYTE) {
            return ELOG_CORRUPT;
        }
    }
    *generation = read64(image + 8u);
    return ELOG_OK;
}

elog_result_t elog_build_pages(elog_t *logger)
{
    const elog_storage_ops_t *ops = logger->storage.ops;
    const uint32_t capacity = ops->capacity(logger->storage.context);
    const uint32_t granularity = ops->program_granularity(logger->storage.context);
    const uint32_t max_program =
        ops->max_program_size(logger->storage.context);
    if (granularity == 0u || granularity > ELOG_MAX_PROGRAM_GRANULARITY ||
        ELOG_PAGE_HEADER_SIZE % granularity != 0u ||
        max_program < granularity || max_program % granularity != 0u) {
        return ELOG_INVALID_CONFIG;
    }
    logger->page_count = 0u;
    for (uint8_t lane = 0u; lane < 3u; ++lane) {
        const elog_region_t p = logger->config.partition[lane];
        if (p.length == 0u || p.offset > capacity ||
            p.length > capacity - p.offset || p.offset % granularity != 0u) {
            return ELOG_INVALID_CONFIG;
        }
        for (uint8_t other = 0u; other < lane; ++other) {
            const elog_region_t q = logger->config.partition[other];
            if (p.offset < q.offset + q.length && q.offset < p.offset + p.length) {
                return ELOG_INVALID_CONFIG;
            }
        }
        const uint32_t end = p.offset + p.length;
        uint32_t cursor = p.offset;
        uint8_t lane_pages = 0u;
        while (cursor < end) {
            elog_erase_region_t region;
            elog_result_t result = ops->erase_region(logger->storage.context,
                                                      cursor, &region);
            if (result != ELOG_OK || region.start != cursor ||
                region.length < ELOG_PAGE_HEADER_SIZE + ELOG_MAX_RECORD ||
                region.length > end - cursor ||
                region.length % granularity != 0u ||
                logger->page_count >= ELOG_MAX_PAGES) {
                return ELOG_INVALID_CONFIG;
            }
            elog_page_t *page = &logger->pages[logger->page_count];
            page->start = cursor;
            page->length = region.length;
            page->used = 0u;
            page->valid_end = 0u;
            page->generation = 0u;
            page->priority = lane;
            page->state = ELOG_PAGE_ERASED;
            ++logger->page_count;
            ++lane_pages;
            cursor += region.length;
        }
        if (lane_pages < (lane == (uint8_t)ELOG_CRITICAL ? 1u : 2u)) {
            return ELOG_INVALID_CONFIG;
        }
    }
    return ELOG_OK;
}

elog_result_t elog_scan_pages(elog_t *logger)
{
    const elog_storage_ops_t *ops = logger->storage.ops;
    const uint32_t g = ops->program_granularity(logger->storage.context);
    uint8_t have_sequence = 0u;
    logger->next_sequence = 0u;
    logger->recovered_pending_commit = 0u;
    for (uint8_t lane = 0u; lane < 3u; ++lane) {
        logger->active_page[lane] = -1;
        logger->next_generation[lane] = 0u;
    }
    for (uint8_t i = 0u; i < logger->page_count; ++i) {
        elog_page_t *page = &logger->pages[i];
        elog_result_t result = ops->read(logger->storage.context,
                           page->start, logger->page_image, ELOG_PAGE_HEADER_SIZE);
        if (result != ELOG_OK) {
            return result;
        }
        if (all_erased(logger->page_image, ELOG_PAGE_HEADER_SIZE)) {
            uint8_t fully_erased;
            result = check_erased_range(logger, page,
                                        ELOG_PAGE_HEADER_SIZE, &fully_erased);
            if (result != ELOG_OK) {
                return result;
            }
            page->state = fully_erased != 0u ? ELOG_PAGE_ERASED : ELOG_PAGE_TORN;
            page->used = fully_erased != 0u ? 0u : page->length;
            page->valid_end = 0u;
            if (fully_erased == 0u) {
                ++logger->statistics.corrupt_records;
            }
            continue;
        }
        result = elog_validate_page_header(logger->page_image, g,
                                           page->priority, &page->generation);
        if (result == ELOG_UNSUPPORTED) {
            return result;
        }
        if (result != ELOG_OK) {
            page->state = ELOG_PAGE_TORN;
            page->used = page->length;
            page->valid_end = 0u;
            ++logger->statistics.corrupt_records;
            continue;
        }
        page->state = ELOG_PAGE_VALID;
        page->used = ELOG_PAGE_HEADER_SIZE;
        page->valid_end = ELOG_PAGE_HEADER_SIZE;
        while (page->used < page->length) {
            const uint32_t remaining = page->length - page->used;
            const size_t header_bytes = remaining < ELOG_RECORD_HEADER_SIZE ?
                                        (size_t)remaining : ELOG_RECORD_HEADER_SIZE;
            result = ops->read(logger->storage.context,
                               page->start + page->used,
                               logger->verify_image, header_bytes);
            if (result != ELOG_OK) {
                return result;
            }
            if (all_erased(logger->verify_image, header_bytes)) {
                uint8_t tail_erased;
                result = check_erased_range(logger, page,
                                           page->used, &tail_erased);
                if (result != ELOG_OK) {
                    return result;
                }
                if (tail_erased == 0u) {
                    page->used = page->length;
                    ++logger->statistics.corrupt_records;
                }
                break;
            }
            elog_record_layout_t layout;
            if (header_bytes < ELOG_RECORD_HEADER_SIZE) {
                result = ELOG_CORRUPT;
            } else {
                result = elog_record_layout_from_header(logger->verify_image,
                                     ELOG_RECORD_HEADER_SIZE, g, &layout);
            }
            if (result == ELOG_UNSUPPORTED) {
                return result;
            }
            if (result != ELOG_OK || layout.total_length > remaining) {
                page->used = page->length;
                ++logger->statistics.corrupt_records;
                break;
            }
            result = ops->read(logger->storage.context,
                               page->start + page->used,
                               logger->verify_image, layout.total_length);
            if (result != ELOG_OK) {
                return result;
            }
            elog_record_t record;
            result = elog_record_decode(logger->verify_image,
                                        layout.total_length, g, &record, &layout);
            if (result != ELOG_OK || record.priority != page->priority) {
                page->used = page->length;
                ++logger->statistics.corrupt_records;
                break;
            }
            if (logger->has_pending != 0u &&
                record.sequence == logger->pending_sequence &&
                record.event_id == logger->pending.event_id &&
                record.type == logger->pending.type) {
                logger->recovered_pending_commit = 1u;
            }
            if (have_sequence == 0u) {
                logger->next_sequence = record.sequence + 1u;
                have_sequence = 1u;
            } else {
                const int newer = elog_sequence_newer(record.sequence,
                                                     logger->next_sequence - 1u);
                if (newer < 0) {
                    return ELOG_CORRUPT;
                }
                if (newer > 0) {
                    logger->next_sequence = record.sequence + 1u;
                }
            }
            page->used += (uint32_t)layout.total_length;
            page->valid_end = page->used;
        }
        const int16_t old_index = logger->active_page[page->priority];
        if (old_index < 0) {
            logger->active_page[page->priority] = (int16_t)i;
            logger->next_generation[page->priority] = page->generation + 1u;
        } else {
            const int newer = elog_sequence_newer(page->generation,
                                     logger->pages[old_index].generation);
            if (newer < 0) {
                return ELOG_CORRUPT;
            }
            if (newer > 0) {
                logger->active_page[page->priority] = (int16_t)i;
                logger->next_generation[page->priority] = page->generation + 1u;
            }
        }
    }
    return ELOG_OK;
}
