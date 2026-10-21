#include "log_storage_stm32f103.h"

#include "stm32f1xx_hal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ELOG_F103_FLASH_BASE UINT32_C(0x08000000)
#if defined(STM32F103xB) && !defined(STM32F103xE)
/* The Blue Pill build is explicitly limited to the C8's nominal 64 KiB. */
#define ELOG_F103_FLASH_BYTES UINT32_C(65536)
#define ELOG_F103_PAGE_BYTES UINT32_C(1024)
#elif defined(STM32F103xE) && !defined(STM32F103xB)
/* The production RD build retains its existing 384 KiB / 2 KiB layout. */
#define ELOG_F103_FLASH_BYTES UINT32_C(393216)
#define ELOG_F103_PAGE_BYTES UINT32_C(2048)
#else
#error "Select exactly one supported STM32F103 density family"
#endif
#define ELOG_F103_PROGRAM_MAX UINT32_C(64)

#if FLASH_PAGE_SIZE != ELOG_F103_PAGE_BYTES
#error "HAL flash page geometry does not match the selected MCU"
#endif

extern uint8_t __elog_start[];
extern uint8_t __elog_end[];

static int allowed(const elog_stm32f103_flash_t *adapter,
                   uint32_t address, size_t length)
{
    return address >= adapter->allowed_start &&
           address <= adapter->allowed_end &&
           length <= (size_t)(adapter->allowed_end - address);
}

static elog_result_t hal_result(HAL_StatusTypeDef result)
{
    if (result == HAL_OK) {
        return ELOG_OK;
    }
    if (result == HAL_TIMEOUT) {
        return ELOG_STORAGE_TIMEOUT;
    }
    if (result == HAL_BUSY) {
        return ELOG_BUSY;
    }
    return ELOG_STORAGE_IO;
}

static uint32_t f103_capacity(void *context)
{
    (void)context;
    return ELOG_F103_FLASH_BYTES;
}

static uint32_t f103_granularity(void *context)
{
    (void)context;
    return 2u;
}

static uint32_t f103_max_program(void *context)
{
    (void)context;
    return ELOG_F103_PROGRAM_MAX;
}

static elog_result_t f103_region(void *context, uint32_t address,
                                  elog_erase_region_t *region)
{
    const elog_stm32f103_flash_t *adapter =
        (const elog_stm32f103_flash_t *)context;
    if (region == NULL || !allowed(adapter, address, 1u)) {
        return ELOG_INVALID_ARGUMENT;
    }
    const uint32_t start = address - address % ELOG_F103_PAGE_BYTES;
    if (!allowed(adapter, start, ELOG_F103_PAGE_BYTES)) {
        return ELOG_INVALID_ARGUMENT;
    }
    region->start = start;
    region->length = ELOG_F103_PAGE_BYTES;
    return ELOG_OK;
}

static elog_result_t f103_read(void *context, uint32_t address,
                                void *destination, size_t length)
{
    const elog_stm32f103_flash_t *adapter =
        (const elog_stm32f103_flash_t *)context;
    if ((destination == NULL && length != 0u) ||
        !allowed(adapter, address, length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if ((FLASH->SR & FLASH_SR_BSY) != 0u) {
        return ELOG_BUSY;
    }
    if (length != 0u) {
        memcpy(destination,
               (const void *)(uintptr_t)(ELOG_F103_FLASH_BASE + address),
               length);
    }
    return ELOG_OK;
}

static elog_result_t f103_program(void *context, uint32_t address,
                                   const void *source, size_t length,
                                   uint32_t timeout_ms)
{
    const elog_stm32f103_flash_t *adapter =
        (const elog_stm32f103_flash_t *)context;
    if (source == NULL || length == 0u || length > ELOG_F103_PROGRAM_MAX ||
        (address & 1u) != 0u || (length & 1u) != 0u ||
        !allowed(adapter, address, length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    /* HAL_FLASH_Program uses FLASH_TIMEOUT_VALUE internally, not timeout_ms.
     * Reject a stricter requested bound instead of silently ignoring it. */
    if (timeout_ms < FLASH_TIMEOUT_VALUE) {
        return ELOG_UNSUPPORTED;
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_HSIRDY) == RESET) {
        return ELOG_WOULD_BLOCK;
    }
    const uint8_t *bytes = (const uint8_t *)source;
    for (size_t i = 0u; i < length; i += 2u) {
        const volatile uint16_t *target = (const volatile uint16_t *)(uintptr_t)
            (ELOG_F103_FLASH_BASE + address + (uint32_t)i);
        if (*target != UINT16_C(0xFFFF)) {
            return ELOG_STORAGE_IO;
        }
    }
    elog_result_t result = hal_result(HAL_FLASH_Unlock());
    if (result != ELOG_OK) {
        return result;
    }
    for (size_t i = 0u; i < length; i += 2u) {
        const uint16_t value = (uint16_t)((uint16_t)bytes[i] |
                                ((uint16_t)bytes[i + 1u] << 8));
        const uint32_t physical = ELOG_F103_FLASH_BASE + address + (uint32_t)i;
        result = hal_result(HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                             physical, value));
        if (result != ELOG_OK) {
            break;
        }
        const volatile uint16_t *target =
            (const volatile uint16_t *)(uintptr_t)physical;
        if (*target != value) {
            result = ELOG_STORAGE_IO;
            break;
        }
    }
    const elog_result_t lock_result = hal_result(HAL_FLASH_Lock());
    return result != ELOG_OK ? result : lock_result;
}

static elog_result_t f103_erase(void *context, uint32_t address,
                                 uint32_t length, uint32_t timeout_ms)
{
    elog_stm32f103_flash_t *adapter = (elog_stm32f103_flash_t *)context;
    if (length != ELOG_F103_PAGE_BYTES ||
        address % ELOG_F103_PAGE_BYTES != 0u ||
        !allowed(adapter, address, length)) {
        return ELOG_INVALID_ARGUMENT;
    }
    if (timeout_ms < FLASH_TIMEOUT_VALUE) {
        return ELOG_UNSUPPORTED;
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_HSIRDY) == RESET) {
        return ELOG_WOULD_BLOCK;
    }
    elog_result_t result = hal_result(HAL_FLASH_Unlock());
    if (result != ELOG_OK) {
        return result;
    }
    FLASH_EraseInitTypeDef init;
    init.TypeErase = FLASH_TYPEERASE_PAGES;
    init.PageAddress = ELOG_F103_FLASH_BASE + address;
    init.NbPages = 1u;
    uint32_t page_error = UINT32_MAX;
    result = hal_result(HAL_FLASHEx_Erase(&init, &page_error));
    if (result == ELOG_OK && page_error != UINT32_MAX) {
        result = ELOG_STORAGE_IO;
    }
    if (result == ELOG_OK) {
        for (uint32_t offset = 0u; offset < length; offset += 2u) {
            const volatile uint16_t *value =
                (const volatile uint16_t *)(uintptr_t)
                    (ELOG_F103_FLASH_BASE + address + offset);
            if (*value != UINT16_C(0xFFFF)) {
                result = ELOG_STORAGE_IO;
                break;
            }
        }
    }
    const elog_result_t lock_result = hal_result(HAL_FLASH_Lock());
    return result != ELOG_OK ? result : lock_result;
}

static elog_result_t f103_busy(void *context, bool *busy)
{
    (void)context;
    if (busy == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    *busy = (FLASH->SR & FLASH_SR_BSY) != 0u;
    return ELOG_OK;
}

static elog_result_t f103_sync(void *context, uint32_t timeout_ms)
{
    (void)timeout_ms;
    bool busy;
    const elog_result_t result = f103_busy(context, &busy);
    return result != ELOG_OK ? result : (busy ? ELOG_BUSY : ELOG_OK);
}

static const elog_storage_ops_t F103_OPS = {
    f103_capacity, f103_granularity, f103_max_program, f103_region,
    f103_read, f103_program, f103_erase, f103_busy, f103_sync
};

elog_result_t elog_stm32f103_flash_init(elog_stm32f103_flash_t *adapter,
                                        elog_storage_t *storage)
{
    if (adapter == NULL || storage == NULL) {
        return ELOG_INVALID_ARGUMENT;
    }
    const uintptr_t start = (uintptr_t)__elog_start;
    const uintptr_t end = (uintptr_t)__elog_end;
    /* Reject a mismatched factory size signature. This is a consistency
     * check, not proof that an unofficial clone has genuine ST flash. */
    const uint16_t reported_kib =
        *(const volatile uint16_t *)(uintptr_t)FLASHSIZE_BASE;
#if defined(STM32F103xB)
    /* Some C8-marked Blue Pills report 128 KiB. Accept the signature, but
     * continue to address only the C8's nominal first 64 KiB. */
    const int signature_compatible = reported_kib == 64u ||
                                     reported_kib == 128u;
#else
    const int signature_compatible = reported_kib == 384u;
#endif
    if (start < ELOG_F103_FLASH_BASE ||
        end > (uintptr_t)ELOG_F103_FLASH_BASE + ELOG_F103_FLASH_BYTES ||
        end <= start ||
        !signature_compatible ||
        (start - ELOG_F103_FLASH_BASE) % ELOG_F103_PAGE_BYTES != 0u ||
        (end - start) % ELOG_F103_PAGE_BYTES != 0u) {
        return ELOG_INVALID_CONFIG;
    }
    adapter->allowed_start = (uint32_t)(start - ELOG_F103_FLASH_BASE);
    adapter->allowed_end = (uint32_t)(end - ELOG_F103_FLASH_BASE);
    adapter->reported_flash_kib = reported_kib;
    storage->ops = &F103_OPS;
    storage->context = adapter;
    return ELOG_OK;
}
