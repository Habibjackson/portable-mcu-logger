/** @file
 * @brief Inject torn program and erase results over RAM flash.
 *
 * A configured operation changes only a prefix before returning an error.
 * A simulated power cycle resets injection state while keeping image bytes.
 */
#include "log_storage_sim.h"

#include <string.h>

static uint32_t sim_capacity(void *context)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    return sim->inner.ops->capacity(sim->inner.context);
}

static uint32_t sim_granularity(void *context)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    return sim->inner.ops->program_granularity(sim->inner.context);
}

static uint32_t sim_max_program(void *context)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    return sim->inner.ops->max_program_size(sim->inner.context);
}

static elog_result_t sim_region(void *context, uint32_t address,
                                elog_erase_region_t *region)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    return sim->inner.ops->erase_region(sim->inner.context, address, region);
}

static elog_result_t sim_read(void *context, uint32_t address,
                              void *destination, size_t length)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    if (sim->busy != 0u) {
        return ELOG_BUSY;
    }
    return sim->inner.ops->read(sim->inner.context,
                                address, destination, length);
}

/** @brief Count each program call and optionally inject a torn write.
 * The selected call changes at most partial_bytes, then returns the injected
 * error without writing a commit unit unless that unit was selected itself.
 */
static elog_result_t sim_program(void *context, uint32_t address,
                                 const void *source, size_t length,
                                 uint32_t timeout_ms)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    if (sim->busy != 0u) {
        return ELOG_BUSY;
    }
    ++sim->program_calls;
    if (sim->program_calls == sim->fail_program_call) {
        const size_t count = sim->partial_bytes < length ?
                             sim->partial_bytes : length;
        if (source == NULL || address > sim->ram->capacity ||
            count > sim->ram->capacity - address) {
            return ELOG_INVALID_ARGUMENT;
        }
        const uint8_t *input = (const uint8_t *)source;
        for (size_t i = 0u; i < count; ++i) {
            if (sim->ram->bytes[address + i] != 0xFFu) {
                return ELOG_STORAGE_IO;
            }
        }
        memcpy(sim->ram->bytes + address, input, count);
        sim->fail_program_call = 0u;
        return sim->injected_result;
    }
    return sim->inner.ops->program(sim->inner.context, address, source,
                                    length, timeout_ms);
}

/** @brief Count erases and optionally erase only a prefix before failing. */
static elog_result_t sim_erase(void *context, uint32_t address,
                               uint32_t length, uint32_t timeout_ms)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    if (sim->busy != 0u) {
        return ELOG_BUSY;
    }
    ++sim->erase_calls;
    if (sim->erase_calls == sim->fail_erase_call) {
        elog_erase_region_t region;
        if (sim_region(context, address, &region) != ELOG_OK ||
            region.start != address || region.length != length) {
            return ELOG_INVALID_ARGUMENT;
        }
        const size_t count = sim->partial_bytes < length ?
                             sim->partial_bytes : length;
        memset(sim->ram->bytes + address, 0xFF, count);
        sim->fail_erase_call = 0u;
        return sim->injected_result;
    }
    return sim->inner.ops->erase(sim->inner.context, address,
                                  length, timeout_ms);
}

static elog_result_t sim_busy(void *context, bool *busy)
{
    if (busy == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    *busy = ((const elog_sim_flash_t *)context)->busy != 0u;
    return ELOG_OK;
}

static elog_result_t sim_sync(void *context, uint32_t timeout_ms)
{
    elog_sim_flash_t *sim = (elog_sim_flash_t *)context;
    return sim->busy != 0u ? ELOG_STORAGE_TIMEOUT :
        sim->inner.ops->sync(sim->inner.context, timeout_ms);
}

static const elog_storage_ops_t SIM_OPS = {
    sim_capacity, sim_granularity, sim_max_program, sim_region,
    sim_read, sim_program, sim_erase, sim_busy, sim_sync
};

elog_result_t elog_sim_flash_init(elog_sim_flash_t *sim,
                                  elog_ram_storage_t *ram,
                                  const elog_storage_t *ram_storage,
                                  elog_storage_t *sim_storage)
{
    if (sim == NULL || ram == NULL || ram_storage == NULL ||
        ram_storage->ops == NULL || sim_storage == NULL ||
        ram_storage->context != ram) {
        return ELOG_INVALID_ARGUMENT;
    }
    memset(sim, 0, sizeof *sim);
    sim->ram = ram;
    sim->inner = *ram_storage;
    sim_storage->ops = &SIM_OPS;
    sim_storage->context = sim;
    return ELOG_OK;
}

void elog_sim_flash_fail_program(elog_sim_flash_t *sim, uint32_t call_number,
                                 size_t partial_bytes,
                                 elog_result_t error_result)
{
    if (sim != NULL) {
        sim->fail_program_call = call_number;
        sim->partial_bytes = partial_bytes;
        sim->injected_result = error_result;
    }
}

void elog_sim_flash_fail_erase(elog_sim_flash_t *sim, uint32_t call_number,
                               size_t partial_bytes,
                               elog_result_t error_result)
{
    if (sim != NULL) {
        sim->fail_erase_call = call_number;
        sim->partial_bytes = partial_bytes;
        sim->injected_result = error_result;
    }
}

void elog_sim_flash_power_cycle(elog_sim_flash_t *sim)
{
    if (sim != NULL) {
        sim->program_calls = 0u;
        sim->erase_calls = 0u;
        sim->fail_program_call = 0u;
        sim->fail_erase_call = 0u;
        sim->partial_bytes = 0u;
        sim->busy = 0u;
    }
}

elog_result_t elog_sim_flash_corrupt(elog_sim_flash_t *sim,
                                     uint32_t address, uint8_t value)
{
    if (sim == NULL || address >= sim->ram->capacity) {
        return ELOG_INVALID_ARGUMENT;
    }
    sim->ram->bytes[address] = value;
    return ELOG_OK;
}
