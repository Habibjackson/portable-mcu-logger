#ifndef ELOG_STORAGE_SIM_H
#define ELOG_STORAGE_SIM_H

#include <stddef.h>
#include <stdint.h>
#include "log_storage_ram.h"

/** @brief Host-only fault-injection layer over the RAM flash model.
 *
 * A selected program or erase call can change a prefix and then report an
 * error. This models an interruption, not the analog behavior of a real chip.
 */
typedef struct {
    elog_ram_storage_t *ram;
    elog_storage_t inner;
    uint32_t program_calls;
    uint32_t erase_calls;
    uint32_t fail_program_call;
    uint32_t fail_erase_call;
    size_t partial_bytes;
    elog_result_t injected_result;
    uint8_t busy;
} elog_sim_flash_t;

/** @brief Wrap an initialized RAM backend without formatting its bytes.
 *
 * ram and ram_storage must refer to the same model and outlive sim_storage.
 * @return ELOG_OK or ELOG_INVALID_ARGUMENT.
 */
elog_result_t elog_sim_flash_init(elog_sim_flash_t *sim,
                                  elog_ram_storage_t *ram,
                                  const elog_storage_t *ram_storage,
                                  elog_storage_t *sim_storage);
/** @brief Fail the numbered future program call after writing partial_bytes.
 *
 * Call numbers are absolute, starting at 1 after init or
 * elog_sim_flash_power_cycle(). The
 * injected result is normally ELOG_STORAGE_TIMEOUT or ELOG_STORAGE_IO.
 */
void elog_sim_flash_fail_program(elog_sim_flash_t *sim, uint32_t call_number,
                                 size_t partial_bytes,
                                 elog_result_t error_result);
/** @brief Fail the numbered future erase after erasing a byte prefix. */
void elog_sim_flash_fail_erase(elog_sim_flash_t *sim, uint32_t call_number,
                               size_t partial_bytes,
                               elog_result_t error_result);
/** @brief Reset injection counters and busy state while retaining flash bytes. */
void elog_sim_flash_power_cycle(elog_sim_flash_t *sim);

/** @brief Overwrite one simulated byte to test corruption detection.
 *
 * This deliberately bypasses flash programming rules.
 * @return ELOG_OK or ELOG_INVALID_ARGUMENT for an out-of-range address.
 */
elog_result_t elog_sim_flash_corrupt(elog_sim_flash_t *sim,
                                     uint32_t address, uint8_t value);

#endif
