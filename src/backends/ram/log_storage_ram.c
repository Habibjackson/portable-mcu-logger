#include "log_storage_ram.h"
#include "log_config.h"

#include <string.h>

static int in_bounds(const elog_ram_storage_t *ram, uint32_t address,
                     size_t length)
{
    return address <= ram->capacity && length <= (size_t)(ram->capacity - address);
}

static uint32_t ram_capacity(void *context)
{
    return ((const elog_ram_storage_t *)context)->capacity;
}

static uint32_t ram_granularity(void *context)
{
    return ((const elog_ram_storage_t *)context)->program_granularity;
}

static uint32_t ram_max_program(void *context)
{
    return ((const elog_ram_storage_t *)context)->max_program_size;
}

static elog_result_t ram_region(void *context, uint32_t address,
                                elog_erase_region_t *region)
{
    const elog_ram_storage_t *ram = (const elog_ram_storage_t *)context;
    if (region == NULL || address >= ram->capacity) {
        return ELOG_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < ram->region_count; ++i) {
        const elog_erase_region_t current = ram->regions[i];
        if (address >= current.start && address - current.start < current.length) {
            *region = current;
            return ELOG_OK;
        }
    }
    return ELOG_INVALID_CONFIG;
}

static elog_result_t ram_read(void *context, uint32_t address,
                              void *destination, size_t length)
{
    const elog_ram_storage_t *ram = (const elog_ram_storage_t *)context;
    if ((destination == NULL && length != 0u) || !in_bounds(ram, address, length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length != 0u) {
        memcpy(destination, ram->bytes + address, length);
    }
    return ELOG_OK;
}

static elog_result_t ram_program(void *context, uint32_t address,
                                 const void *source, size_t length,
                                 uint32_t timeout_ms)
{
    elog_ram_storage_t *ram = (elog_ram_storage_t *)context;
    (void)timeout_ms;
    const uint32_t g = ram->program_granularity;
    if (source == NULL || length == 0u || length > ram->max_program_size ||
        !in_bounds(ram, address, length) || address % g != 0u ||
        length % g != 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    elog_erase_region_t first;
    elog_erase_region_t last;
    if (ram_region(context, address, &first) != ELOG_OK ||
        ram_region(context, address + (uint32_t)length - 1u, &last) != ELOG_OK ||
        first.start != last.start) {
        return ELOG_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < length; ++i) {
        if (ram->bytes[address + i] != 0xFFu) {
            return ELOG_STORAGE_IO;
        }
    }
    memcpy(ram->bytes + address, source, length);
    return ELOG_OK;
}

static elog_result_t ram_erase(void *context, uint32_t address,
                               uint32_t length, uint32_t timeout_ms)
{
    elog_ram_storage_t *ram = (elog_ram_storage_t *)context;
    (void)timeout_ms;
    elog_erase_region_t region;
    if (ram_region(context, address, &region) != ELOG_OK ||
        address != region.start || length != region.length) {
        return ELOG_INVALID_ARGUMENT;
    }
    memset(ram->bytes + address, 0xFF, length);
    return ELOG_OK;
}

static elog_result_t ram_busy(void *context, bool *busy)
{
    (void)context;
    if (busy == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    *busy = false;
    return ELOG_OK;
}

static elog_result_t ram_sync(void *context, uint32_t timeout_ms)
{
    (void)context;
    (void)timeout_ms;
    return ELOG_OK;
}

static const elog_storage_ops_t RAM_OPS = {
    ram_capacity, ram_granularity, ram_max_program, ram_region,
    ram_read, ram_program, ram_erase, ram_busy, ram_sync
};

elog_result_t elog_ram_storage_init(elog_ram_storage_t *ram, uint8_t *bytes,
                                    uint32_t capacity,
                                    const elog_erase_region_t *regions,
                                    size_t region_count,
                                    uint32_t program_granularity,
                                    uint32_t max_program_size,
                                    elog_storage_t *storage)
{
    if (ram == NULL || bytes == NULL || storage == NULL || regions == NULL ||
        capacity == 0u || region_count == 0u || program_granularity == 0u ||
        program_granularity > ELOG_MAX_PROGRAM_GRANULARITY ||
        max_program_size == 0u || max_program_size % program_granularity != 0u) {
        return ELOG_INVALID_CONFIG;
    }
    uint32_t end = 0u;
    for (size_t i = 0u; i < region_count; ++i) {
        if (regions[i].start != end || regions[i].length == 0u ||
            regions[i].length > capacity - end ||
            regions[i].length % program_granularity != 0u) {
            return ELOG_INVALID_CONFIG;
        }
        end += regions[i].length;
    }
    if (end != capacity) {
        return ELOG_INVALID_CONFIG;
    }
    ram->bytes = bytes;
    ram->capacity = capacity;
    ram->regions = regions;
    ram->region_count = region_count;
    ram->program_granularity = program_granularity;
    ram->max_program_size = max_program_size;
    storage->ops = &RAM_OPS;
    storage->context = ram;
    return ELOG_OK;
}

elog_result_t elog_ram_storage_format(elog_ram_storage_t *ram)
{
    if (ram == NULL || ram->bytes == NULL || ram->capacity == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    memset(ram->bytes, 0xFF, ram->capacity);
    return ELOG_OK;
}
