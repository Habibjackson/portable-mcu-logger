/** @file
 * @brief Linker-bounded STM32F103 internal-flash storage adapter.
 *
 * The selected MCU density determines nominal capacity and erase-page size.
 * Only the separately reserved linker range is available to the logger.
 */
#ifndef ELOG_STORAGE_STM32F103_H
#define ELOG_STORAGE_STM32F103_H

#include <stdint.h>
#include "log_storage.h"

/** @brief Linker-bounded HAL flash adapter instance, set by init. */
typedef struct {
    uint32_t allowed_start; /* Device-relative, derived from linker symbols. */
    uint32_t allowed_end;   /* Exclusive. */
    uint16_t reported_flash_kib; /* Factory size field; not trusted as capacity. */
} elog_stm32f103_flash_t;

/** @brief Initialize the STM32F103C8 or STM32F103RD internal-flash adapter.
 *
 * @param adapter Caller-owned backend instance retained for the logger life.
 * @param storage Output storage handle for elog_init().
 * @return ELOG_OK when linker symbols and the factory size signature are
 *         compatible with the selected MCU's flash limit and page geometry, or
 *         ELOG_INVALID_CONFIG / ELOG_INVALID_ARGUMENT otherwise.
 * @warning Requires linker symbols __elog_start and __elog_end. The adapter
 * refuses accesses outside that region. HAL's fixed 50-second nominal wait is
 * not a hard wall-clock deadline when flash stalls interrupt-based ticks;
 * call elog_process() only in an application-defined safe state. Neither this
 * initializer nor elog_init() erases the region.
 */
elog_result_t elog_stm32f103_flash_init(elog_stm32f103_flash_t *adapter,
                                        elog_storage_t *storage);

#endif
