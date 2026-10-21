#ifndef ELOG_H
#define ELOG_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "log_config.h"
#include "log_format.h"
#include "log_storage.h"

/** @brief Session counters. Arrays are indexed by elog_priority_t. */
typedef struct {
    uint64_t written[3]; /**< Committed records by priority this session. */
    uint64_t dropped_queue[3]; /**< Queue rejections/replacements; includes ISR. */
    uint64_t dropped_storage[3]; /**< Pending records discarded on storage full. */
    uint64_t storage_errors; /**< Failed storage operations this session. */
    uint64_t corrupt_records; /**< Invalid tails/headers observed during scans. */
    uint64_t pages_erased; /**< Successful erase operations this session. */
    uint64_t fault_repeats_suppressed; /**< Repeats without a summary record. */
    uint64_t routine_rate_limited; /**< Routine writes rejected by interval. */
} elog_statistics_t;

/** @brief Current partition usage, queue occupancy, and latched errors.
 *
 * used_bytes includes sealed or torn space that cannot accept another record.
 * busy describes an active journal operation or pending record; queued[] is
 * reported separately.
 */
typedef struct {
    elog_result_t last_error; /**< Last journal storage error or ELOG_OK. */
    uint32_t total_bytes[3]; /**< Configured partition bytes by priority. */
    uint32_t used_bytes[3]; /**< Programmed or sealed bytes by priority. */
    uint8_t queued[3]; /**< Ordinary RAM queue entries by priority. */
    uint8_t queued_isr; /**< ISR ingress entries awaiting task processing. */
    uint8_t storage_full[3]; /**< Latched capacity exhaustion by priority. */
    uint8_t busy; /**< Active journal state or pending selected record. */
} elog_status_t;

typedef struct {
    uint64_t timestamp;
    uint32_t event_id;
    uint16_t length;
    uint8_t type;
    uint8_t priority;
    uint8_t payload[ELOG_MAX_PAYLOAD];
} elog_entry_t;

typedef struct {
    elog_entry_t *slots;
    uint8_t capacity;
    uint8_t head;
    uint8_t count;
} elog_queue_t;

typedef struct {
    uint32_t start;
    uint32_t length;
    uint32_t used;
    uint32_t valid_end;
    uint64_t generation;
    uint8_t priority;
    uint8_t state;
} elog_page_t;

typedef struct {
    uint32_t id;
    uint32_t repeats;
    uint64_t first_timestamp;
    uint64_t last_summary_timestamp;
    uint8_t active;
} elog_fault_state_t;

typedef struct {
    uint64_t timestamp;
    uint32_t event_id;
    uint8_t length;
    uint8_t payload[24];
} elog_telemetry_sample_t;

/** @brief Caller-owned logger workspace.
 *
 * Allocate for the logger lifetime, preferably as static storage. Treat every
 * field as private even though the type is visible for allocation; never copy
 * or modify an initialized instance.
 */
typedef struct {
    elog_config_t config;
    elog_storage_t storage;
    elog_page_t pages[ELOG_MAX_PAGES];
    uint8_t page_count;
    int16_t active_page[3];
    uint64_t next_generation[3];
    uint64_t next_sequence;
    elog_queue_t queue[3];
    elog_entry_t critical_slots[ELOG_MAX_CRITICAL_QUEUE];
    elog_entry_t event_slots[ELOG_MAX_EVENT_QUEUE];
    elog_entry_t routine_slots[ELOG_MAX_ROUTINE_QUEUE];
    elog_entry_t isr_slots[ELOG_MAX_ISR_QUEUE];
    atomic_uint isr_head;
    atomic_uint isr_tail;
    atomic_uint isr_dropped;
    atomic_bool freeze_request;
    elog_fault_state_t faults[ELOG_MAX_FAULTS];
    elog_telemetry_sample_t pre_fault[ELOG_MAX_PRE_FAULT];
    elog_telemetry_sample_t post_fault[ELOG_MAX_POST_FAULT];
    uint8_t pre_head;
    uint8_t pre_count;
    uint8_t post_count;
    uint8_t post_remaining;
    uint8_t pre_frozen;
    uint8_t pre_dump_index;
    uint8_t pre_dump_count;
    elog_entry_t pending;
    uint8_t has_pending;
    uint8_t pending_source;
    uint64_t pending_sequence;
    uint8_t recovered_pending_commit;
    uint8_t storage_full[3];
    uint8_t record_image[ELOG_MAX_RECORD];
    uint8_t codec_image[ELOG_MAX_PAYLOAD];
    uint8_t previous_numeric[ELOG_MAX_PAYLOAD];
    uint32_t previous_numeric_id;
    uint64_t previous_numeric_sequence;
    uint16_t previous_numeric_length;
    uint16_t since_snapshot;
    uint8_t previous_numeric_valid;
    uint8_t verify_image[ELOG_MAX_RECORD];
    uint8_t page_image[ELOG_PAGE_HEADER_SIZE];
    elog_record_layout_t record_layout;
    size_t body_progress;
    int16_t target_page;
    uint8_t process_state;
    uint8_t initialized;
    uint64_t last_routine_timestamp;
    uint8_t has_last_routine_timestamp;
    elog_result_t last_error;
    elog_statistics_t statistics;
} elog_t;

/** @brief Initialize a logger and scan existing records without erasing them.
 *
 * @param logger Caller-owned workspace that remains valid until logging ends.
 * @param config Configuration copied into logger; callback targets must remain
 *               valid. CRC and recovery must both be enabled.
 * @param storage Storage handle copied into logger; its context and operations
 *                must remain valid for the logger lifetime.
 * @return ELOG_OK, ELOG_INVALID_CONFIG, ELOG_UNSUPPORTED, or a scan error.
 * @note Call from one serialized task context. This function is not ISR-safe.
 */
elog_result_t elog_init(elog_t *logger, const elog_config_t *config,
                        const elog_storage_t *storage);

/** @brief Copy a record into the selected priority queue.
 *
 * @param logger Initialized logger.
 * @param priority CRITICAL, EVENT, or ROUTINE partition.
 * @param type Version-1 record type.
 * @param event_id Application-defined event or fault identifier.
 * @param payload Bytes copied before return; may be NULL only when length is 0.
 * @param length Payload size, at most config.max_payload.
 * @return ELOG_OK on enqueue, ELOG_QUEUED_WITH_DROP if an old routine entry was
 *         replaced, ELOG_WOULD_BLOCK if routine rate limited, or an error.
 * @note Enqueue success does not mean that flash has been committed. Ordinary
 *       API calls require application serialization and are not ISR-safe.
 */
elog_result_t elog_write(elog_t *logger, elog_priority_t priority,
                         elog_record_type_t type, uint32_t event_id,
                         const void *payload, size_t length);

/** @brief Queue an EVENT record using the same copy and durability rules.
 *
 * @param logger Initialized logger.
 * @param event_id Application-defined event identifier.
 * @param payload Bytes copied before return; NULL requires length 0.
 * @param length Payload byte count, at most config.max_payload.
 * @return ELOG_OK on enqueue or an explicit size, queue, or argument error.
 */
elog_result_t elog_event(elog_t *logger, uint32_t event_id,
                         const void *payload, size_t length);

/** @brief Queue a CRITICAL fault occurrence or rate-limited repeat summary.
 *
 * @param logger Initialized logger.
 * @param fault_id Application-defined fault identifier.
 * @param snapshot Fault-time bytes copied on first occurrence.
 * @param length Snapshot size, at most config.max_payload.
 * @return ELOG_OK for an enqueued or intentionally suppressed repeat; an error
 *         if the queue, fault table, or critical storage cannot accept it.
 * @note The active-fault table is RAM-only and resets on reboot.
 */
elog_result_t elog_fault(elog_t *logger, uint32_t fault_id,
                         const void *snapshot, size_t length);

/** @brief Queue recovery of a currently active fault as a CRITICAL record.
 *
 * @param logger Initialized logger.
 * @param fault_id Must match an active fault in the RAM fault table.
 * @param payload Optional application bytes copied after a 12-byte repeat
 *                count and duration prefix.
 * @param length At most config.max_payload minus 12 bytes.
 * @return ELOG_OK on enqueue, ELOG_INVALID_ARGUMENT for an unknown/inactive
 *         fault, or another explicit queue/storage/size error.
 */
elog_result_t elog_fault_recovered(elog_t *logger, uint32_t fault_id,
                                   const void *payload, size_t length);

/** @brief Copy a fault into the bounded single-producer ISR ingress queue.
 *
 * @param logger Initialized logger. The designated ISR may interrupt ordinary
 *               task calls, but must not race with init or recovery.
 * @param fault_id Application-defined fault identifier.
 * @param timestamp Caller-supplied timestamp; no clock callback runs in ISR.
 * @param snapshot Bytes copied before return; NULL is valid only for length 0.
 * @param length At most config.max_payload.
 * @return ELOG_OK if captured in RAM, ELOG_QUEUE_FULL if ingress is full, or
 *         an argument/size error. ELOG_OK does not mean nonvolatile durability.
 * @warning Only one designated ISR producer is supported. No flash operation,
 *          erase, compression, or blocking call occurs here. C11 atomics must
 *          be lock-free on the target, as checked by elog_init().
 */
elog_result_t elog_capture_fault_isr(elog_t *logger, uint32_t fault_id,
                                     uint64_t timestamp,
                                     const void *snapshot, size_t length);

/** @brief Advance one journal state from a serialized task context.
 *
 * @param logger Initialized logger.
 * @return ELOG_OK when a step completes, ELOG_EMPTY when no work remains,
 *         ELOG_WOULD_BLOCK if storage is busy or erase is disallowed, or an
 *         explicit storage/configuration error. On ELOG_STORAGE_FULL the
 *         current pending entry is counted as dropped so other lanes can run.
 * @warning A synchronous backend can block for one device operation. Gate
 *          internal-flash processing with the application's control-safe state.
 */
elog_result_t elog_process(elog_t *logger);

/** @brief Run up to max_steps journal steps, stopping on empty or first error.
 *
 * @param logger Initialized logger.
 * @param max_steps Maximum number of calls to elog_process(); zero performs
 *                  no work and returns ELOG_WOULD_BLOCK.
 * @return ELOG_OK when drained, ELOG_WOULD_BLOCK when the step budget expires
 *         or processing must wait, otherwise the first error from elog_process.
 * @note This is a task API and may block for several storage operations.
 */
elog_result_t elog_flush(elog_t *logger, uint32_t max_steps);

/** @brief Rescan storage after a failed or interrupted operation.
 *
 * Recovery is read-only. It seals torn tails in RAM metadata and resets the
 * journal state so pending work can be retried when safe.
 * @param logger Initialized logger.
 * @return ELOG_OK or the first scan/format/storage error.
 * @note Call only while ordinary API access and the ISR producer are quiescent.
 */
elog_result_t elog_recover(elog_t *logger);

/** @brief Visitor for a validated stored record.
 *
 * @param context Opaque visitor_context from elog_iterate_records().
 * @param record Borrowed decoded header and encoded payload.
 * The record and its encoded payload borrow logger scratch memory only during
 * the callback. Return zero to continue or nonzero to stop iteration.
 */
typedef int (*elog_record_visitor_t)(void *context, const elog_record_t *record);

/** @brief Visit committed records in physical partition/page order.
 *
 * @param logger Initialized logger with no active journal operation.
 * @param visitor Called for each CRC- and commit-validated record.
 * @param visitor_context Passed unchanged to visitor.
 * @return ELOG_OK after all records, ELOG_WOULD_BLOCK if visitor stops, ELOG_BUSY
 *         while the journal has an active operation, or a read/format error.
 * @note Sort by record.sequence if chronological order is needed. Iteration
 *       is read-only and requires a quiescent logger; payloads stay encoded.
 */
elog_result_t elog_iterate_records(elog_t *logger,
                                   elog_record_visitor_t visitor,
                                   void *visitor_context);

/** @brief Copy current usage, queues, and latched error flags into status.
 *
 * @param logger Initialized logger.
 * @param status Caller-owned output filled on ELOG_OK.
 * @return ELOG_OK or ELOG_INVALID_ARGUMENT.
 */
elog_result_t elog_get_status(const elog_t *logger, elog_status_t *status);

/** @brief Copy session counters, including ISR queue drops, into statistics.
 *
 * @param logger Initialized logger.
 * @param statistics Caller-owned output filled on ELOG_OK.
 * @return ELOG_OK or ELOG_INVALID_ARGUMENT.
 */
elog_result_t elog_get_statistics(const elog_t *logger,
                                   elog_statistics_t *statistics);

#endif
