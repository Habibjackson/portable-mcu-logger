#ifndef ELOG_STORAGE_SPI_NOR_H
#define ELOG_STORAGE_SPI_NOR_H

#include <stddef.h>
#include <stdint.h>
#include "log_storage.h"

/** @brief Homogeneous portion of a chip's possibly variable erase geometry. */
typedef struct {
    uint32_t start; /**< First device-relative byte. */
    uint32_t length; /**< Covered bytes, a multiple of erase_size. */
    uint32_t erase_size; /**< Bytes erased by one erase command. */
    uint8_t erase_opcode; /**< Command from the actual chip datasheet. */
} elog_spi_nor_erase_range_t;

/** @brief Device-specific geometry and ordinary 1-1-1 SPI commands.
 *
 * All commands and geometry must come from the selected chip's datasheet.
 * This adapter supports ordinary 1-1-1 SPI command/address/data transfers;
 * chips requiring modes, dummy cycles, or protection setup need a custom
 * descriptor/bus adapter or a different backend. No JEDEC ID probe or address
 * mode negotiation is performed by this generic adapter.
 */
typedef struct {
    uint32_t capacity; /**< Total chip bytes. */
    uint32_t allowed_start; /**< First logger-accessible device byte. */
    uint32_t allowed_end; /**< Exclusive logger-accessible end. */
    uint16_t page_size; /**< Chip page-program boundary in bytes. */
    uint8_t address_bytes; /**< Three or four address bytes per command. */
    uint8_t program_granularity; /**< Minimum program unit, 1..8 bytes. */
    uint8_t read_opcode; /**< Ordinary read command with no dummy cycles. */
    uint8_t program_opcode; /**< Page-program command. */
    uint8_t write_enable_opcode; /**< Set write-enable latch. */
    uint8_t read_status_opcode; /**< Read status byte command. */
    uint8_t status_busy_mask; /**< Nonzero write-in-progress bit mask. */
    uint8_t status_write_enable_mask; /**< WEL bit; zero skips WEL check. */
    uint8_t supports_separate_commit; /**< Must be 1 for this journal. */
    uint32_t max_poll_count; /**< Upper bound on status polls per wait. */
    const elog_spi_nor_erase_range_t *erase_ranges; /**< Full chip geometry. */
    size_t erase_range_count; /**< Length of erase_ranges. */
} elog_spi_nor_descriptor_t;

/** @brief Application-supplied SPI transport and millisecond clock.
 *
 * Callbacks must serialize bus access, complete within bounded time, and
 * return an explicit error. The adapter does not own the bus context.
 */
typedef struct {
    /** Assert CS for active != 0; deassert for active == 0. */
    elog_result_t (*select)(void *context, int active);
    /** Transfer count bytes; NULL transmit emits dummy 0xFF bytes. */
    elog_result_t (*transfer)(void *context, const uint8_t *transmit,
                              uint8_t *receive, size_t count);
    /** Wrapping 32-bit millisecond clock used for timeout polling. */
    uint32_t (*now_ms)(void *context);
    void *context; /**< Passed unchanged to all callbacks. */
} elog_spi_nor_bus_t;

/** @brief Caller-owned SPI NOR backend instance, filled by init. */
typedef struct {
    elog_spi_nor_descriptor_t device;
    elog_spi_nor_bus_t bus;
} elog_spi_nor_t;

/** @brief Validate a chip descriptor and bind the SPI NOR storage adapter.
 *
 * @param adapter Caller-owned state retained for the logger lifetime.
 * @param descriptor Chip facts copied into adapter; erase_ranges stays borrowed.
 * @param bus Callbacks copied into adapter; bus.context stays borrowed.
 * @param storage Output storage handle to pass to elog_init().
 * @return ELOG_OK or ELOG_INVALID_CONFIG for invalid inputs or geometry.
 * @note Initialization validates geometry and commands but does not probe
 *       hardware, erase bytes, or configure chip protection/address mode.
 */
elog_result_t elog_spi_nor_init(elog_spi_nor_t *adapter,
                                const elog_spi_nor_descriptor_t *descriptor,
                                const elog_spi_nor_bus_t *bus,
                                elog_storage_t *storage);

#endif
