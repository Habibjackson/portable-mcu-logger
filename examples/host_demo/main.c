/** @file
 * @brief Export a compressed telemetry and fault session as a raw log image.
 *
 * This host-only program models sixteen 2 KiB flash erase pages in RAM and
 * formats the new fixture once. It commits six 24-byte robot readings, a
 * motor fault, a motor-stopped event, and recovery. DELTA compression is
 * configured with periodic snapshots. Pre/post-fault capture is enabled, but
 * this session supplies no telemetry after the fault for post-fault capture.
 * The output file contains the entire 32 KiB storage image, including erased
 * bytes. Decode it with tools/decode_logs.py and examples/robot_schema.json.
 * The example does not program an MCU or demonstrate real actuator shutdown.
 */
#include "log.h"
#include "log_storage_ram.h"
#include "robot_example_ids.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash_image[16u * 2048u];
static elog_erase_region_t pages[16];
static elog_t logger;
static uint64_t uptime_ms;

/** @brief Supply a monotonic millisecond timestamp to queued records.
 * @param context Address of uptime_ms, alive until the logger is destroyed.
 * @return Current simulated uptime; it is not a wall-clock date.
 */
static uint64_t timestamp(void *context)
{
    return *(const uint64_t *)context;
}

/** @brief Serialize one robot field as little-endian bytes.
 * @param bytes At least four writable payload bytes.
 * @param value Unsigned sample field; the decoder schema supplies its units.
 */
static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        bytes[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Commit an example session and write the complete RAM flash image.
 * @param argc Must be two: program name and destination path.
 * @param argv argv[1] is the output binary file; the caller chooses its path.
 * @return Zero on successful logger commits and full file write, 2 for usage
 *         error, or 1 for storage, logger, or output failure.
 * @note Queue success is not durability. Each elog_flush() must finish before
 *       the image is exported; real flash needs a reserved region and recovery.
 */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s output.bin\n", argv[0]);
        return 2;
    }
    for (uint32_t i = 0u; i < 16u; ++i) {
        pages[i] = (elog_erase_region_t){i * 2048u, 2048u};
    }
    elog_ram_storage_t ram;
    elog_storage_t storage;
    if (elog_ram_storage_init(&ram, flash_image, sizeof flash_image,
         pages, 16u, 2u, 64u, &storage) != ELOG_OK ||
        elog_ram_storage_format(&ram) != ELOG_OK) {
        return 1;
    }
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] = (elog_region_t){0u, 8192u};
    config.partition[ELOG_EVENT] = (elog_region_t){8192u, 8192u};
    config.partition[ELOG_ROUTINE] = (elog_region_t){16384u, 16384u};
    config.critical_queue_depth = 8u;
    config.event_queue_depth = 8u;
    config.routine_queue_depth = 16u;
    config.isr_queue_depth = 4u;
    config.pre_fault_depth = 4u;
    config.post_fault_depth = 2u;
    config.max_payload = 80u;
    config.fault_table_depth = 8u;
    config.storage_timeout_ms = 100u;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_DELTA;
    config.snapshot_interval = 4u;
    config.timestamp = timestamp;
    config.timestamp_context = &uptime_ms;
    if (elog_init(&logger, &config, &storage) != ELOG_OK) {
        return 1;
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        uint8_t telemetry[24];
        put32(telemetry + 0u, 1000u + i);  /* motor rpm */
        put32(telemetry + 4u, 120u + i);   /* motor current mA */
        put32(telemetry + 8u, 205u + i);   /* pressure kPa x10 */
        put32(telemetry + 12u, 9000u + i); /* encoder ticks */
        put32(telemetry + 16u, 0u);        /* fault_active: no */
        put32(telemetry + 20u, 0u);        /* warning bits */
        uptime_ms += 100u;
        if (elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                       ROBOT_EXAMPLE_TELEMETRY_ID,
                       telemetry, sizeof telemetry) < ELOG_OK ||
            elog_flush(&logger, 200u) != ELOG_OK) {
            return 1;
        }
    }
    const uint8_t snapshot[] = {0xB0u, 0x04u, 0xD2u, 0x00u};
    if (elog_fault(&logger, ROBOT_EXAMPLE_MOTOR_FAULT_ID,
                   snapshot, sizeof snapshot) != ELOG_OK ||
        elog_event(&logger, ROBOT_EXAMPLE_MOTOR_STOPPED_ID,
                   "motor stopped", 13u) != ELOG_OK ||
        elog_flush(&logger, 400u) != ELOG_OK) {
        return 1;
    }
    uptime_ms += 500u;
    if (elog_fault_recovered(&logger, ROBOT_EXAMPLE_MOTOR_FAULT_ID,
                             NULL, 0u) != ELOG_OK ||
        elog_flush(&logger, 200u) != ELOG_OK) {
        return 1;
    }
    FILE *output = fopen(argv[1], "wb");
    if (output == NULL) {
        return 1;
    }
    const size_t count = fwrite(flash_image, 1u, sizeof flash_image, output);
    if (fclose(output) != 0 || count != sizeof flash_image) {
        return 1;
    }
    printf("wrote %zu bytes to %s\n", count, argv[1]);
    return 0;
}
