#include "log_storage_spi_nor.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    uint8_t memory[1536];
    uint32_t address;
    uint32_t tick;
    uint8_t opcode;
    uint8_t selected;
    uint8_t write_enable;
    uint8_t busy_polls;
    uint8_t fail_status;
} mock_t;

static elog_result_t select_chip(void *context, int active)
{
    mock_t *mock = (mock_t *)context;
    if (active != 0) {
        if (mock->selected != 0u) {
            return ELOG_STORAGE_IO;
        }
        mock->selected = 1u;
        mock->opcode = 0u;
        return ELOG_OK;
    }
    if (mock->selected == 0u) {
        return ELOG_STORAGE_IO;
    }
    if (mock->opcode == 0x06u) {
        mock->write_enable = 1u;
    } else if (mock->opcode == 0x02u) {
        mock->write_enable = 0u;
        mock->busy_polls = 1u;
    } else if (mock->opcode == 0x20u || mock->opcode == 0xD8u) {
        if (mock->write_enable == 0u) {
            return ELOG_STORAGE_IO;
        }
        const uint32_t size = mock->opcode == 0x20u ? 256u : 512u;
        const uint32_t start = mock->address - mock->address % size;
        if (start + size > sizeof mock->memory) {
            return ELOG_STORAGE_IO;
        }
        memset(mock->memory + start, 0xFF, size);
        mock->write_enable = 0u;
        mock->busy_polls = 1u;
    }
    mock->selected = 0u;
    return ELOG_OK;
}

static elog_result_t transfer(void *context, const uint8_t *tx,
                              uint8_t *rx, size_t count)
{
    mock_t *mock = (mock_t *)context;
    if (mock->selected == 0u || count == 0u) {
        return ELOG_STORAGE_IO;
    }
    if (mock->opcode == 0u) {
        if (tx == NULL) {
            return ELOG_STORAGE_IO;
        }
        mock->opcode = tx[0];
        if (count == 4u) {
            mock->address = ((uint32_t)tx[1] << 16) |
                            ((uint32_t)tx[2] << 8) | tx[3];
        }
        return ELOG_OK;
    }
    if (mock->opcode == 0x05u && rx != NULL && count == 1u) {
        if (mock->fail_status != 0u) {
            mock->fail_status = 0u;
            return ELOG_STORAGE_IO;
        }
        rx[0] = (uint8_t)((mock->busy_polls != 0u ? 1u : 0u) |
                          (mock->write_enable != 0u ? 2u : 0u));
        if (mock->busy_polls != 0u) {
            --mock->busy_polls;
        }
        return ELOG_OK;
    }
    if (mock->address > sizeof mock->memory ||
        count > sizeof mock->memory - mock->address) {
        return ELOG_STORAGE_IO;
    }
    if (mock->opcode == 0x03u && rx != NULL) {
        memcpy(rx, mock->memory + mock->address, count);
        mock->address += (uint32_t)count;
        return ELOG_OK;
    }
    if (mock->opcode == 0x02u && tx != NULL && mock->write_enable != 0u) {
        for (size_t i = 0u; i < count; ++i) {
            if (mock->memory[mock->address + i] != 0xFFu) {
                return ELOG_STORAGE_IO;
            }
        }
        memcpy(mock->memory + mock->address, tx, count);
        mock->address += (uint32_t)count;
        return ELOG_OK;
    }
    return ELOG_STORAGE_IO;
}

static uint32_t now_ms(void *context)
{
    mock_t *mock = (mock_t *)context;
    return mock->tick++;
}

int main(void)
{
    mock_t mock;
    memset(&mock, 0, sizeof mock);
    memset(mock.memory, 0xFF, sizeof mock.memory);
    const elog_spi_nor_erase_range_t ranges[] = {
        {0u, 512u, 256u, 0x20u},
        {512u, 1024u, 512u, 0xD8u}
    };
    const elog_spi_nor_descriptor_t descriptor = {
        1536u, 0u, 1536u, 256u, 3u, 1u,
        0x03u, 0x02u, 0x06u, 0x05u, 1u, 2u, 1u, 100u,
        ranges, 2u
    };
    const elog_spi_nor_bus_t bus = {select_chip, transfer, now_ms, &mock};
    elog_spi_nor_t nor;
    elog_storage_t storage;
    CHECK(elog_spi_nor_init(&nor, &descriptor, &bus, &storage) == ELOG_OK);
    bool busy = false;
    mock.fail_status = 1u;
    CHECK(storage.ops->is_busy(storage.context, &busy) == ELOG_STORAGE_IO);
    CHECK(storage.ops->is_busy(storage.context, &busy) == ELOG_OK);
    CHECK(busy == false);
    elog_erase_region_t region;
    CHECK(storage.ops->erase_region(storage.context, 700u, &region) == ELOG_OK);
    CHECK(region.start == 512u && region.length == 512u);
    const uint8_t source[16] = {1u, 2u, 3u, 4u};
    uint8_t copy[16];
    CHECK(storage.ops->program(storage.context, 250u, source,
                                sizeof source, 50u) == ELOG_OK);
    CHECK(storage.ops->read(storage.context, 250u, copy,
                             sizeof copy) == ELOG_OK);
    CHECK(memcmp(copy, source, sizeof source) == 0);
    CHECK(storage.ops->program(storage.context, 250u, source,
                                sizeof source, 50u) == ELOG_STORAGE_IO);
    CHECK(storage.ops->erase(storage.context, 512u, 512u, 50u) == ELOG_OK);
    CHECK(storage.ops->erase(storage.context, 512u, 256u, 50u) == ELOG_INVALID_ARGUMENT);
    elog_spi_nor_descriptor_t invalid = descriptor;
    invalid.supports_separate_commit = 0u;
    CHECK(elog_spi_nor_init(&nor, &invalid, &bus, &storage) == ELOG_INVALID_CONFIG);
    puts("SPI NOR descriptor and geometry: OK");
    return 0;
}
