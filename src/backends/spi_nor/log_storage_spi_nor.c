/** @file
 * @brief Implement descriptor-driven ordinary 1-1-1 SPI NOR storage.
 *
 * Bus callbacks perform chip select and transfers. The adapter bounds
 * every access, polls status with a finite budget, and verifies program and
 * erase results. Device identification and protection setup are external.
 */
#include "log_storage_spi_nor.h"

#include <string.h>

/** @brief Check a device-relative range against the logger reservation. */
static int allowed(const elog_spi_nor_t *nor, uint32_t address, size_t length)
{
    return address >= nor->device.allowed_start &&
           address <= nor->device.allowed_end &&
           length <= nor->device.allowed_end - address;
}

/** @brief Assert CS and send an opcode/address prefix, releasing CS on error. */
static elog_result_t start_command(elog_spi_nor_t *nor,
                                    const uint8_t *command, size_t length)
{
    elog_result_t result = nor->bus.select(nor->bus.context, 1);
    if (result != ELOG_OK) {
        return result;
    }
    result = nor->bus.transfer(nor->bus.context, command, NULL, length);
    if (result != ELOG_OK) {
        (void)nor->bus.select(nor->bus.context, 0);
    }
    return result;
}

/** @brief Release CS while preserving an earlier transfer failure. */
static elog_result_t finish_command(elog_spi_nor_t *nor,
                                     elog_result_t transfer_result)
{
    const elog_result_t deselect = nor->bus.select(nor->bus.context, 0);
    return transfer_result != ELOG_OK ? transfer_result : deselect;
}

/** @brief Encode a three- or four-byte big-endian SPI command address. */
static size_t address_command(const elog_spi_nor_t *nor, uint8_t opcode,
                               uint32_t address, uint8_t command[5])
{
    command[0] = opcode;
    for (uint8_t i = 0u; i < nor->device.address_bytes; ++i) {
        const unsigned int shift = 8u * (unsigned int)
            (nor->device.address_bytes - i - 1u);
        command[i + 1u] = (uint8_t)(address >> shift);
    }
    return 1u + nor->device.address_bytes;
}

static elog_result_t status_byte(elog_spi_nor_t *nor, uint8_t *status)
{
    const uint8_t opcode = nor->device.read_status_opcode;
    elog_result_t result = start_command(nor, &opcode, 1u);
    if (result != ELOG_OK) {
        return result;
    }
    result = nor->bus.transfer(nor->bus.context, NULL, status, 1u);
    return finish_command(nor, result);
}

/** @brief Poll write-in-progress with both clock and poll-count bounds.
 * The bus clock may wrap; unsigned subtraction keeps elapsed time valid over
 * intervals shorter than one full counter cycle.
 */
static elog_result_t wait_ready(elog_spi_nor_t *nor, uint32_t timeout_ms)
{
    const uint32_t start = nor->bus.now_ms(nor->bus.context);
    for (uint32_t polls = 0u; polls < nor->device.max_poll_count; ++polls) {
        uint8_t status;
        const elog_result_t result = status_byte(nor, &status);
        if (result != ELOG_OK) {
            return result;
        }
        if ((status & nor->device.status_busy_mask) == 0u) {
            return ELOG_OK;
        }
        if ((uint32_t)(nor->bus.now_ms(nor->bus.context) - start) >= timeout_ms) {
            return ELOG_STORAGE_TIMEOUT;
        }
    }
    return ELOG_STORAGE_TIMEOUT;
}

/** @brief Set the write-enable latch and verify it when the chip exposes it. */
static elog_result_t write_enable(elog_spi_nor_t *nor)
{
    const uint8_t opcode = nor->device.write_enable_opcode;
    elog_result_t result = start_command(nor, &opcode, 1u);
    if (result != ELOG_OK) {
        return result;
    }
    result = finish_command(nor, ELOG_OK);
    if (result != ELOG_OK) {
        return result;
    }
    if (nor->device.status_write_enable_mask != 0u) {
        uint8_t status;
        result = status_byte(nor, &status);
        if (result != ELOG_OK) {
            return result;
        }
        if ((status & nor->device.status_write_enable_mask) == 0u) {
            return ELOG_STORAGE_IO;
        }
    }
    return ELOG_OK;
}

static uint32_t nor_capacity(void *context)
{
    return ((const elog_spi_nor_t *)context)->device.capacity;
}

static uint32_t nor_granularity(void *context)
{
    return ((const elog_spi_nor_t *)context)->device.program_granularity;
}

static uint32_t nor_max_program(void *context)
{
    const elog_spi_nor_t *nor = (const elog_spi_nor_t *)context;
    return nor->device.page_size < 128u ? nor->device.page_size : 128u;
}

static elog_result_t nor_region(void *context, uint32_t address,
                                elog_erase_region_t *region)
{
    const elog_spi_nor_t *nor = (const elog_spi_nor_t *)context;
    if (region == NULL || !allowed(nor, address, 1u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < nor->device.erase_range_count; ++i) {
        const elog_spi_nor_erase_range_t *range =
            &nor->device.erase_ranges[i];
        if (address >= range->start && address - range->start < range->length) {
            const uint32_t start = range->start +
                ((address - range->start) / range->erase_size) *
                    range->erase_size;
            if (!allowed(nor, start, range->erase_size)) {
                return ELOG_INVALID_ARGUMENT;
            }
            region->start = start;
            region->length = range->erase_size;
            return ELOG_OK;
        }
    }
    return ELOG_INVALID_CONFIG;
}

static elog_result_t nor_read(void *context, uint32_t address,
                              void *destination, size_t length)
{
    elog_spi_nor_t *nor = (elog_spi_nor_t *)context;
    if ((destination == NULL && length != 0u) ||
        !allowed(nor, address, length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (length == 0u) {
        return ELOG_OK;
    }
    uint8_t command[5];
    const size_t command_length = address_command(nor,
        nor->device.read_opcode, address, command);
    elog_result_t result = start_command(nor, command, command_length);
    if (result != ELOG_OK) {
        return result;
    }
    result = nor->bus.transfer(nor->bus.context, NULL,
                                (uint8_t *)destination, length);
    return finish_command(nor, result);
}

/** @brief Program erased data in chip-page pieces and verify each piece.
 * A command never crosses the configured page-program boundary. Readback
 * failure is reported before the caller may write its separate commit unit.
 */
static elog_result_t nor_program(void *context, uint32_t address,
                                 const void *source, size_t length,
                                 uint32_t timeout_ms)
{
    elog_spi_nor_t *nor = (elog_spi_nor_t *)context;
    const uint32_t g = nor->device.program_granularity;
    if (source == NULL || length == 0u || length > nor_max_program(context) ||
        address % g != 0u || length % g != 0u ||
        !allowed(nor, address, length) || timeout_ms == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    const uint8_t *bytes = (const uint8_t *)source;
    size_t progress = 0u;
    uint8_t verify[128];
    while (progress < length) {
        const uint32_t position = address + (uint32_t)progress;
        const size_t page_left = nor->device.page_size -
                                 position % nor->device.page_size;
        const size_t count = length - progress < page_left ?
                             length - progress : page_left;
        elog_result_t result = nor_read(context, position, verify, count);
        if (result != ELOG_OK) {
            return result;
        }
        for (size_t i = 0u; i < count; ++i) {
            if (verify[i] != 0xFFu) {
                return ELOG_STORAGE_IO;
            }
        }
        result = wait_ready(nor, timeout_ms);
        if (result != ELOG_OK) {
            return result;
        }
        result = write_enable(nor);
        if (result != ELOG_OK) {
            return result;
        }
        uint8_t command[5];
        const size_t command_length = address_command(nor,
            nor->device.program_opcode, position, command);
        result = start_command(nor, command, command_length);
        if (result != ELOG_OK) {
            return result;
        }
        result = nor->bus.transfer(nor->bus.context, bytes + progress,
                                    NULL, count);
        result = finish_command(nor, result);
        if (result != ELOG_OK) {
            return result;
        }
        result = wait_ready(nor, timeout_ms);
        if (result != ELOG_OK) {
            return result;
        }
        result = nor_read(context, position, verify, count);
        if (result != ELOG_OK || memcmp(verify, bytes + progress, count) != 0) {
            return result != ELOG_OK ? result : ELOG_STORAGE_IO;
        }
        progress += count;
    }
    return ELOG_OK;
}

/** @brief Erase one declared region with its descriptor opcode and verify it.
 * The routine never chooses an erase size or opcode from an address alone;
 * the validated chip descriptor supplies both.
 */
static elog_result_t nor_erase(void *context, uint32_t address,
                               uint32_t length, uint32_t timeout_ms)
{
    elog_spi_nor_t *nor = (elog_spi_nor_t *)context;
    elog_erase_region_t region;
    if (nor_region(context, address, &region) != ELOG_OK ||
        region.start != address || region.length != length || timeout_ms == 0u) {
        return ELOG_INVALID_ARGUMENT;
    }
    uint8_t opcode = 0u;
    for (size_t i = 0u; i < nor->device.erase_range_count; ++i) {
        const elog_spi_nor_erase_range_t *range =
            &nor->device.erase_ranges[i];
        if (address >= range->start && address - range->start < range->length) {
            opcode = range->erase_opcode;
            break;
        }
    }
    elog_result_t result = wait_ready(nor, timeout_ms);
    if (result != ELOG_OK) {
        return result;
    }
    result = write_enable(nor);
    if (result != ELOG_OK) {
        return result;
    }
    uint8_t command[5];
    const size_t command_length = address_command(nor, opcode,
                                                   address, command);
    result = start_command(nor, command, command_length);
    if (result != ELOG_OK) {
        return result;
    }
    result = finish_command(nor, ELOG_OK);
    if (result != ELOG_OK) {
        return result;
    }
    result = wait_ready(nor, timeout_ms);
    if (result != ELOG_OK) {
        return result;
    }
    uint8_t verify[64];
    for (uint32_t offset = 0u; offset < length; offset += sizeof verify) {
        const size_t count = length - offset < sizeof verify ?
                             length - offset : sizeof verify;
        result = nor_read(context, address + offset, verify, count);
        if (result != ELOG_OK) {
            return result;
        }
        for (size_t i = 0u; i < count; ++i) {
            if (verify[i] != 0xFFu) {
                return ELOG_STORAGE_IO;
            }
        }
    }
    return ELOG_OK;
}

static elog_result_t nor_busy(void *context, bool *busy)
{
    if (busy == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    elog_spi_nor_t *nor = (elog_spi_nor_t *)context;
    uint8_t status = 0u;
    const elog_result_t result = status_byte(nor, &status);
    if (result != ELOG_OK) {
        return result;
    }
    *busy = (status & nor->device.status_busy_mask) != 0u;
    return ELOG_OK;
}

static elog_result_t nor_sync(void *context, uint32_t timeout_ms)
{
    return wait_ready((elog_spi_nor_t *)context, timeout_ms);
}

static const elog_storage_ops_t NOR_OPS = {
    nor_capacity, nor_granularity, nor_max_program, nor_region,
    nor_read, nor_program, nor_erase, nor_busy, nor_sync
};

elog_result_t elog_spi_nor_init(elog_spi_nor_t *adapter,
                                const elog_spi_nor_descriptor_t *descriptor,
                                const elog_spi_nor_bus_t *bus,
                                elog_storage_t *storage)
{
    if (adapter == NULL || descriptor == NULL || bus == NULL || storage == NULL ||
        bus->select == NULL || bus->transfer == NULL || bus->now_ms == NULL ||
        descriptor->capacity == 0u || descriptor->page_size == 0u ||
        (descriptor->address_bytes != 3u && descriptor->address_bytes != 4u) ||
        (descriptor->address_bytes == 3u && descriptor->capacity > 0x1000000u) ||
        descriptor->program_granularity == 0u ||
        descriptor->program_granularity > 8u ||
        descriptor->page_size % descriptor->program_granularity != 0u ||
        descriptor->allowed_end > descriptor->capacity ||
        descriptor->allowed_start >= descriptor->allowed_end ||
        descriptor->read_opcode == 0u || descriptor->program_opcode == 0u ||
        descriptor->write_enable_opcode == 0u ||
        descriptor->read_status_opcode == 0u ||
        descriptor->status_busy_mask == 0u ||
        descriptor->supports_separate_commit != 1u ||
        descriptor->max_poll_count == 0u ||
        descriptor->erase_ranges == NULL || descriptor->erase_range_count == 0u) {
        return ELOG_INVALID_CONFIG;
    }
    uint32_t end = 0u;
    for (size_t i = 0u; i < descriptor->erase_range_count; ++i) {
        const elog_spi_nor_erase_range_t *range =
            &descriptor->erase_ranges[i];
        if (range->start != end || range->erase_size < 160u ||
            range->erase_size % descriptor->program_granularity != 0u ||
            range->length == 0u || range->length % range->erase_size != 0u ||
            range->length > descriptor->capacity - end ||
            range->erase_opcode == 0u) {
            return ELOG_INVALID_CONFIG;
        }
        end += range->length;
    }
    if (end != descriptor->capacity) {
        return ELOG_INVALID_CONFIG;
    }
    adapter->device = *descriptor;
    adapter->bus = *bus;
    elog_erase_region_t first;
    elog_erase_region_t last;
    if (nor_region(adapter, descriptor->allowed_start, &first) != ELOG_OK ||
        first.start != descriptor->allowed_start ||
        nor_region(adapter, descriptor->allowed_end - 1u, &last) != ELOG_OK ||
        last.start + last.length != descriptor->allowed_end) {
        return ELOG_INVALID_CONFIG;
    }
    storage->ops = &NOR_OPS;
    storage->context = adapter;
    return ELOG_OK;
}
