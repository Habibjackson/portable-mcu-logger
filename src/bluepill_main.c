/** @file
 * @brief STM32F103C8T6 hardware exercise using only synthetic signals.
 *
 * The linker reserves 0x0800E000..0x08010000 exclusively for this logger.
 * This test application has no motor outputs; PC13 indicates a latched safe
 * state. Flash processing still blocks the CPU and must be measured on board.
 */
#include "stm32f1xx_hal.h"

#include "log.h"
#include "log_storage_stm32f103.h"
#include "robot_example_ids.h"

#include <stdint.h>
#include <string.h>

#define LOG_START_OFFSET UINT32_C(0xE000)
#define LOG_END_OFFSET UINT32_C(0x10000)
#define SAMPLE_PERIOD_MS 100u
#define FAULT_CURRENT 1u
#define FAULT_PRESSURE 2u
#define FAULT_SENSOR 4u

static UART_HandleTypeDef uart;
static elog_t logger;
static elog_storage_t storage;
static elog_stm32f103_flash_t flash_adapter;
static uint8_t logger_ready;
static uint8_t running;
static uint8_t safe_state;
static uint8_t active_faults;
static uint8_t waveform; /* 0 triangle, 1 saw, 2 step */
static uint32_t sample_index;
static uint32_t last_sample_tick;
static int32_t position_ticks;
static uint16_t last_current_ma = 120u;
static uint16_t last_pressure_deci_kpa = 180u;
static uint32_t app_errors;
static uint32_t max_process_cycles;
static volatile uint32_t max_systick_gap_cycles;
static volatile uint32_t previous_systick_cycle;
static uint64_t uptime_high;
static uint32_t previous_tick;

static void tx(const char *message)
{
    (void)HAL_UART_Transmit(&uart, (uint8_t *)(uintptr_t)message,
                            (uint16_t)strlen(message), 1000u);
}

static void tx_u64(uint64_t value)
{
    char digits[21];
    unsigned int count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u) {
        char out[2] = {digits[--count], '\0'};
        tx(out);
    }
}

static void tx_i32(int32_t value)
{
    if (value < 0) {
        tx("-");
        tx_u64((uint64_t)(-(int64_t)value));
    } else {
        tx_u64((uint32_t)value);
    }
}

static void enter_safe_state(void)
{
    safe_state = 1u;
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); /* LED on. */
}

static void note(elog_result_t result)
{
    if (result < ELOG_OK || result == ELOG_QUEUED_WITH_DROP) {
        ++app_errors;
        enter_safe_state();
    }
}

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

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int i = 0u; i < 4u; ++i) {
        bytes[i] = (uint8_t)(value >> (8u * i));
    }
}

static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void set_fault(uint8_t bit, uint32_t id, uint8_t should_be_active)
{
    if (should_be_active != 0u && (active_faults & bit) == 0u) {
        uint8_t snapshot[4];
        put16(snapshot, last_current_ma);
        put16(snapshot + 2u, last_pressure_deci_kpa);
        /* Safe state is entered before the log call can fail or block. */
        enter_safe_state();
        const elog_result_t result = elog_fault(&logger, id, snapshot,
                                                sizeof snapshot);
        note(result);
        if (result == ELOG_OK) {
            active_faults |= bit;
        }
    } else if (should_be_active == 0u && (active_faults & bit) != 0u) {
        const elog_result_t result = elog_fault_recovered(&logger, id,
                                                           NULL, 0u);
        note(result);
        if (result == ELOG_OK) {
            active_faults &= (uint8_t)~bit;
        }
    }
}

static void emit_sample(uint8_t inject_faults)
{
    const uint32_t speed_phase = sample_index % 40u;
    const uint32_t pressure_phase = sample_index % 60u;
    uint32_t rpm;
    if (waveform == 1u) {
        rpm = 600u + 20u * speed_phase;
    } else if (waveform == 2u) {
        rpm = speed_phase < 20u ? 600u : 1400u;
    } else {
        rpm = 600u + 40u * (speed_phase < 20u ?
                             speed_phase : 40u - speed_phase);
    }
    last_current_ma = (uint16_t)(120u + rpm / 20u);
    if (sample_index % 60u >= 12u && sample_index % 60u < 17u) {
        last_current_ma = 1500u;
    }
    last_pressure_deci_kpa = (uint16_t)(180u + 2u *
        (pressure_phase < 30u ? pressure_phase : 60u - pressure_phase));
    position_ticks += pressure_phase < 30u ? 13 : -11;
    const uint8_t current_high = last_current_ma >= 1000u;
    const uint8_t pressure_high = last_pressure_deci_kpa >= 230u;
    const uint8_t sensor_missing =
        pressure_phase >= 42u && pressure_phase < 47u;
    const uint32_t warnings = (uint32_t)current_high |
        ((uint32_t)pressure_high << 1) | ((uint32_t)sensor_missing << 2);
    uint8_t telemetry[24];
    put32(telemetry, rpm);
    put32(telemetry + 4u, last_current_ma);
    put32(telemetry + 8u, last_pressure_deci_kpa);
    put32(telemetry + 12u, (uint32_t)position_ticks);
    put32(telemetry + 16u, warnings != 0u);
    put32(telemetry + 20u, warnings);
    note(elog_write(&logger, ELOG_ROUTINE, ELOG_TYPE_TELEMETRY,
                    ROBOT_EXAMPLE_TELEMETRY_ID, telemetry,
                    sizeof telemetry));
    if (inject_faults != 0u) {
        set_fault(FAULT_CURRENT, ROBOT_EXAMPLE_MOTOR_FAULT_ID, current_high);
        set_fault(FAULT_PRESSURE, ROBOT_EXAMPLE_PRESSURE_FAULT_ID,
                  pressure_high);
        set_fault(FAULT_SENSOR, ROBOT_EXAMPLE_SENSOR_FAULT_ID,
                  sensor_missing);
    }
    ++sample_index;
}

typedef struct {
    uint32_t count;
    uint32_t faults;
    uint32_t recoveries;
    uint32_t events;
    uint32_t resets;
    uint32_t routine;
    uint32_t original_bytes;
    uint32_t stored_bytes;
} record_summary_t;

static int count_record(void *context, const elog_record_t *record)
{
    record_summary_t *summary = (record_summary_t *)context;
    ++summary->count;
    summary->faults += record->type == (uint8_t)ELOG_TYPE_FAULT;
    summary->recoveries += record->type == (uint8_t)ELOG_TYPE_FAULT_RECOVERY;
    summary->events += record->type == (uint8_t)ELOG_TYPE_EVENT;
    summary->resets += record->type == (uint8_t)ELOG_TYPE_RESET;
    summary->routine += record->priority == (uint8_t)ELOG_ROUTINE;
    summary->original_bytes += record->original_length;
    summary->stored_bytes += record->stored_length;
    return 0;
}

static void show_status(void)
{
    if (logger_ready == 0u) {
        tx("logger unavailable\r\n");
        return;
    }
    const elog_result_t flush_result = elog_flush(&logger, 1000u);
    if (flush_result < ELOG_OK) {
        note(flush_result);
    }
    record_summary_t summary = {0};
    const elog_result_t scan_result = elog_iterate_records(&logger,
                                                            count_record,
                                                            &summary);
    elog_statistics_t statistics;
    elog_status_t status;
    (void)elog_get_statistics(&logger, &statistics);
    (void)elog_get_status(&logger, &status);
    tx("scan="); tx_i32(scan_result);
    tx(" flush="); tx_i32(flush_result);
    tx(" records="); tx_u64(summary.count);
    tx(" routine="); tx_u64(summary.routine);
    tx(" faults="); tx_u64(summary.faults);
    tx(" recoveries="); tx_u64(summary.recoveries);
    tx(" events="); tx_u64(summary.events);
    tx(" resets="); tx_u64(summary.resets);
    tx("\r\nbytes original="); tx_u64(summary.original_bytes);
    tx(" stored="); tx_u64(summary.stored_bytes);
    tx(" ratio_pct=");
    tx_u64(summary.original_bytes == 0u ? 0u :
           (uint64_t)summary.stored_bytes * 100u / summary.original_bytes);
    tx("\r\nqueue_drops critical="); tx_u64(statistics.dropped_queue[0]);
    tx(" event="); tx_u64(statistics.dropped_queue[1]);
    tx(" routine="); tx_u64(statistics.dropped_queue[2]);
    tx(" storage_drops=");
    tx_u64(statistics.dropped_storage[0] + statistics.dropped_storage[1] +
           statistics.dropped_storage[2]);
    tx("\r\nstorage_errors="); tx_u64(statistics.storage_errors);
    tx(" corrupt_on_scan="); tx_u64(statistics.corrupt_records);
    tx(" erases_this_boot="); tx_u64(statistics.pages_erased);
    tx(" last_error="); tx_i32(status.last_error);
    tx(" app_errors="); tx_u64(app_errors);
    tx(" safe_state="); tx_u64(safe_state);
    tx("\r\nfull critical="); tx_u64(status.storage_full[0]);
    tx(" event="); tx_u64(status.storage_full[1]);
    tx(" routine="); tx_u64(status.storage_full[2]);
    tx(" queued critical="); tx_u64(status.queued[0]);
    tx(" event="); tx_u64(status.queued[1]);
    tx(" routine="); tx_u64(status.queued[2]);
    tx("\r\nmax_process_us="); tx_u64(max_process_cycles / 64u);
    tx(" max_systick_gap_us="); tx_u64(max_systick_gap_cycles / 64u);
    tx("\r\n");
}

static char hex_digit(uint8_t nibble)
{
    return (char)(nibble < 10u ? '0' + nibble : 'A' + nibble - 10u);
}

static void dump_image(void)
{
    if (logger_ready == 0u) {
        tx("logger unavailable\r\n");
        return;
    }
    tx("ELOG_HEX_BEGIN 8192\r\n");
    for (uint32_t offset = LOG_START_OFFSET;
         offset < LOG_END_OFFSET; offset += 16u) {
        uint8_t bytes[16];
        if (storage.ops->read(storage.context, offset, bytes,
                              sizeof bytes) != ELOG_OK) {
            tx("ELOG_HEX_ERROR\r\n");
            enter_safe_state();
            return;
        }
        char line[35];
        for (unsigned int i = 0u; i < sizeof bytes; ++i) {
            line[2u * i] = hex_digit((uint8_t)(bytes[i] >> 4));
            line[2u * i + 1u] = hex_digit((uint8_t)(bytes[i] & 15u));
        }
        line[32] = '\r'; line[33] = '\n'; line[34] = '\0';
        tx(line);
    }
    tx("ELOG_HEX_END\r\n");
}

static void command(const char *line)
{
    if (strcmp(line, "help") == 0) {
        tx("run stop wave triangle|saw|step routine burst fault current|pressure|sensor\r\n");
        tx("recover current|pressure|sensor status dump reset clear fill critical help\r\n");
    } else if (strcmp(line, "status") == 0) {
        show_status();
    } else if (strcmp(line, "dump") == 0) {
        dump_image();
    } else if (strcmp(line, "run") == 0) {
        running = 1u; last_sample_tick = HAL_GetTick(); tx("running\r\n");
    } else if (strcmp(line, "stop") == 0) {
        running = 0u; tx("stopped\r\n");
    } else if (strcmp(line, "wave triangle") == 0 ||
               strcmp(line, "wave saw") == 0 ||
               strcmp(line, "wave step") == 0) {
        waveform = strcmp(line, "wave triangle") == 0 ? 0u :
                   strcmp(line, "wave saw") == 0 ? 1u : 2u;
        tx("wave set\r\n");
    } else if (logger_ready != 0u && strcmp(line, "routine") == 0) {
        emit_sample(0u); tx("routine queued\r\n");
    } else if (logger_ready != 0u && strcmp(line, "burst") == 0) {
        for (unsigned int i = 0u; i < 12u; ++i) {
            emit_sample(0u); /* Deliberately overflow the eight-entry queue. */
        }
        tx("routine burst queued; inspect drops with status\r\n");
    } else if (logger_ready != 0u && strcmp(line, "fill critical") == 0) {
        enter_safe_state();
        for (uint32_t i = 0u; i < 64u; ++i) {
            const uint8_t snapshot[4] = {0u, 0u, 0u, 0u};
            elog_result_t result = elog_write(&logger, ELOG_CRITICAL,
                ELOG_TYPE_FAULT, 0x4000u + i, snapshot, sizeof snapshot);
            if (result == ELOG_OK) {
                result = elog_flush(&logger, 100u);
            }
            if (result != ELOG_OK) {
                note(result);
                tx("critical fill stopped, result="); tx_i32(result);
                tx("\r\n");
                break;
            }
        }
    } else if (logger_ready != 0u && strcmp(line, "fault current") == 0) {
        set_fault(FAULT_CURRENT, ROBOT_EXAMPLE_MOTOR_FAULT_ID, 1u);
        tx("current fault requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "fault pressure") == 0) {
        set_fault(FAULT_PRESSURE, ROBOT_EXAMPLE_PRESSURE_FAULT_ID, 1u);
        tx("pressure fault requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "fault sensor") == 0) {
        set_fault(FAULT_SENSOR, ROBOT_EXAMPLE_SENSOR_FAULT_ID, 1u);
        tx("sensor fault requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "recover current") == 0) {
        set_fault(FAULT_CURRENT, ROBOT_EXAMPLE_MOTOR_FAULT_ID, 0u);
        tx("current recovery requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "recover pressure") == 0) {
        set_fault(FAULT_PRESSURE, ROBOT_EXAMPLE_PRESSURE_FAULT_ID, 0u);
        tx("pressure recovery requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "recover sensor") == 0) {
        set_fault(FAULT_SENSOR, ROBOT_EXAMPLE_SENSOR_FAULT_ID, 0u);
        tx("sensor recovery requested\r\n");
    } else if (logger_ready != 0u && strcmp(line, "reset") == 0) {
        const uint8_t reason = 1u; /* Simulated software reset. */
        note(elog_write(&logger, ELOG_EVENT, ELOG_TYPE_RESET,
                        ROBOT_EXAMPLE_SOFTWARE_RESET_ID, &reason, 1u));
        const elog_result_t result = elog_flush(&logger, 1000u);
        if (result == ELOG_OK) {
            tx("reset record committed; rebooting\r\n");
            HAL_Delay(10u);
            NVIC_SystemReset();
        }
        note(result);
        tx("reset aborted: record not committed\r\n");
    } else if (strcmp(line, "clear") == 0 && logger_ready != 0u &&
               active_faults == 0u && app_errors == 0u) {
        safe_state = 0u;
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
        tx("safe state cleared (test LED only)\r\n");
    } else {
        tx("unknown or unavailable command; type help\r\n");
    }
}

static void poll_console(void)
{
    static char input[40];
    static unsigned int used;
    if (__HAL_UART_GET_FLAG(&uart, UART_FLAG_RXNE) == RESET) {
        return;
    }
    const char received = (char)(uart.Instance->DR & 0xFFu);
    if (received == '\r' || received == '\n') {
        if (used != 0u) {
            input[used] = '\0';
            tx("\r\n");
            command(input);
            used = 0u;
        }
    } else if (received == '\b' || received == 127) {
        if (used != 0u) {
            --used;
        }
    } else if (received >= 32 && received < 127 &&
               used < sizeof input - 1u) {
        input[used++] = received;
        char echo[2] = {received, '\0'};
        tx(echo);
    }
}

void SysTick_Handler(void)
{
    const uint32_t now = DWT->CYCCNT;
    if (previous_systick_cycle != 0u) {
        const uint32_t gap = now - previous_systick_cycle;
        if (gap > max_systick_gap_cycles) {
            max_systick_gap_cycles = gap;
        }
    }
    previous_systick_cycle = now;
    HAL_IncTick();
}

static void init_clock(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL16; /* 8 MHz HSI / 2 * 16 = 64 MHz. */
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        for (;;) { }
    }
    clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV2;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK) {
        for (;;) { }
    }
}

static void init_console(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
    uart.Instance = USART1;
    uart.Init.BaudRate = 115200u;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.Mode = UART_MODE_TX_RX;
    uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    if (HAL_UART_Init(&uart) != HAL_OK) {
        enter_safe_state();
        for (;;) { }
    }
}

static void print_reset_cause(uint32_t flags)
{
    tx("reset_cause=");
    if (flags & RCC_CSR_IWDGRSTF) { tx("iwdg "); }
    if (flags & RCC_CSR_WWDGRSTF) { tx("wwdg "); }
    if (flags & RCC_CSR_SFTRSTF) { tx("software "); }
    if (flags & RCC_CSR_PORRSTF) { tx("power_on "); }
    if (flags & RCC_CSR_PINRSTF) { tx("reset_pin "); }
    if (flags & RCC_CSR_LPWRRSTF) { tx("low_power "); }
    tx("\r\n");
}

static elog_result_t init_logger(void)
{
    elog_result_t result = elog_stm32f103_flash_init(&flash_adapter,
                                                      &storage);
    if (result != ELOG_OK || flash_adapter.allowed_start != LOG_START_OFFSET ||
        flash_adapter.allowed_end != LOG_END_OFFSET) {
        return result != ELOG_OK ? result : ELOG_INVALID_CONFIG;
    }
    elog_config_t config;
    memset(&config, 0, sizeof config);
    config.partition[ELOG_CRITICAL] =
        (elog_region_t){LOG_START_OFFSET, 2048u};
    config.partition[ELOG_EVENT] =
        (elog_region_t){LOG_START_OFFSET + 2048u, 2048u};
    config.partition[ELOG_ROUTINE] =
        (elog_region_t){LOG_START_OFFSET + 4096u, 4096u};
    config.critical_queue_depth = 4u;
    config.event_queue_depth = 4u;
    config.routine_queue_depth = 8u;
    config.isr_queue_depth = 4u;
    config.pre_fault_depth = 4u;
    config.post_fault_depth = 2u;
    config.max_payload = 80u;
    config.fault_table_depth = 4u;
    config.storage_timeout_ms = FLASH_TIMEOUT_VALUE;
    config.recover_on_init = 1u;
    config.require_crc = 1u;
    config.codec_id = ELOG_CODEC_DELTA;
    config.snapshot_interval = 4u;
    config.timestamp = uptime_ms;
    return elog_init(&logger, &config, &storage);
}

int main(void)
{
    HAL_Init();
    init_clock();
    init_console();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t reset_flags = RCC->CSR;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    const elog_result_t init_result = init_logger();
    logger_ready = init_result == ELOG_OK;
    tx("Blue Pill ELOG test; HSI PLL 64 MHz; USART1 115200 8N1\r\n");
    tx("reset_flags="); tx_u64(reset_flags);
    tx(" logger_init="); tx_i32(init_result); tx("\r\n");
    if (logger_ready != 0u) {
        tx("flash_reported_kib="); tx_u64(flash_adapter.reported_flash_kib);
        tx(" address_limit_kib=64\r\n");
    }
    print_reset_cause(reset_flags);
    if (logger_ready == 0u) {
        enter_safe_state();
    } else {
        show_status(); /* Recovery scan result and retained record count. */
    }
    tx("type help for commands\r\n");
    for (;;) {
        poll_console();
        const uint32_t now = HAL_GetTick();
        if (logger_ready != 0u) {
            if (running != 0u && now - last_sample_tick >= SAMPLE_PERIOD_MS) {
                last_sample_tick = now;
                emit_sample(1u);
            }
            const uint32_t before = DWT->CYCCNT;
            const elog_result_t result = elog_process(&logger);
            const uint32_t cycles = DWT->CYCCNT - before;
            if (cycles > max_process_cycles) {
                max_process_cycles = cycles;
            }
            if (result < ELOG_OK) {
                note(result);
            }
        }
        HAL_Delay(1u);
    }
}
