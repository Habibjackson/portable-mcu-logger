/** @file
 * @brief PlatformIO compile example with placeholder robot signals.
 *
 * Uses RAM storage until a production flash layout, bootloader, and
 * motor-control safe window are verified on hardware. This example formats
 * its volatile RAM image on every boot and does not persist to MCU flash.
 */
#include "stm32f1xx_hal.h"

#include "log.h"
#include "log_storage_ram.h"
#include "robot_example_ids.h"

#include <stdint.h>
#include <string.h>

static elog_t logger;
static uint8_t ram_image[8u * 256u];
static elog_erase_region_t ram_pages[8];
static elog_ram_storage_t ram_backend;
static elog_storage_t storage;
static volatile elog_result_t last_logger_error;
static uint64_t uptime_high;
static uint32_t previous_tick;

/** @brief Extend HAL's 32-bit millisecond tick for the logger task.
 *
 * Must be called from one serialized task at least once per 32-bit tick wrap;
 * context is unused. The ISR fault API receives its timestamp directly.
 */
static uint64_t uptime_ms(void *context)
{
    (void)context;
    const uint32_t tick = HAL_GetTick();
    if (tick < previous_tick) {
        uptime_high += UINT64_C(1) << 32;
    }
    previous_tick = tick;
    return uptime_high | tick;
}

/** @brief Retain the most recent logger error or routine drop for inspection.
 *
 * Production firmware should report this to its diagnostics and choose a
 * safety response appropriate to the robot, especially for critical faults.
 */
static void note_logger_result(elog_result_t result)
{
    if (result < ELOG_OK || result == ELOG_QUEUED_WITH_DROP) {
        last_logger_error = result; /* Inspect in a debugger or report upstream. */
    }
}

/** @brief Serialize one 32-bit sensor field in little-endian byte order.
 *
 * p must point to at least four writable bytes in the telemetry payload.
 */
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Forward the MCU SysTick interrupt to the STM32 HAL tick counter. */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/** @brief Initialize a RAM-backed logger and emit sample robot telemetry.
 *
 * The timed fault transitions and sensor values are placeholders. Replace
 * them with actual safety signals and keep flash processing out of unsafe
 * motor-control intervals when changing to nonvolatile storage.
 */
int main(void)
{
    HAL_Init();
    for (uint32_t i = 0u; i < 8u; ++i) {
        ram_pages[i].start = i * 256u;
        ram_pages[i].length = 256u;
    }
    if (elog_ram_storage_init(&ram_backend, ram_image, sizeof ram_image,
          ram_pages, 8u, 2u, 64u, &storage) != ELOG_OK ||
        elog_ram_storage_format(&ram_backend) != ELOG_OK) {
        for (;;) { }
    }
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 512u};
    config.partition[ELOG_EVENT] = (elog_region_t){512u, 512u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){1024u, 1024u};
    config.critical_queue_depth = 4u;
    config.event_queue_depth = 4u;
    config.routine_queue_depth = 8u;
    config.isr_queue_depth = 4u;
    config.pre_fault_depth = 4u;
    config.post_fault_depth = 2u;
    config.max_payload = 80u;
    config.fault_table_depth = 4u;
    config.storage_timeout_ms = 1u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.timestamp = uptime_ms;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        for (;;) { }
    }
    uint32_t last_sample = 0u;
    uint8_t fault_active = 0u;
    for (;;) {
        const uint32_t now = HAL_GetTick();
        if (now - last_sample >= 100u) {
            last_sample = now;
            uint8_t telemetry[24];
            put32(telemetry + 0u, 1000u); /* placeholder motor rpm */
            put32(telemetry + 4u, 120u);  /* placeholder current mA */
            put32(telemetry + 8u, 205u);  /* placeholder pressure x10 kPa */
            put32(telemetry + 12u, now);  /* placeholder encoder ticks */
            put32(telemetry + 16u, fault_active);
            put32(telemetry + 20u, 0u);
            note_logger_result(elog_write(&logger, ELOG_ROUTINE,
                ELOG_TYPE_TELEMETRY, ROBOT_EXAMPLE_TELEMETRY_ID,
                telemetry, sizeof telemetry));
        }
        /* Replace these timed transitions with the robot's safety signals. */
        if (now >= 1000u && now < 1500u && fault_active == 0u) {
            fault_active = 1u;
            note_logger_result(elog_fault(&logger,
                ROBOT_EXAMPLE_MOTOR_FAULT_ID, NULL, 0u));
        }
        if (now >= 1500u && fault_active != 0u) {
            fault_active = 0u;
            note_logger_result(elog_fault_recovered(&logger,
                                                    ROBOT_EXAMPLE_MOTOR_FAULT_ID,
                                                    NULL, 0u));
        }
        note_logger_result(elog_process(&logger));
        HAL_Delay(1u);
    }
}
