#ifndef ROBOT_EXAMPLE_IDS_H
#define ROBOT_EXAMPLE_IDS_H

/** @file
 * @brief Stable IDs used only by the example robot applications.
 *
 * Keep these values and examples/robot_schema.json in sync when changing the
 * example payload layout. Production firmware should maintain its own IDs.
 */
#define ROBOT_EXAMPLE_TELEMETRY_ID 1u
#define ROBOT_EXAMPLE_MOTOR_FAULT_ID 0x1001u
#define ROBOT_EXAMPLE_MOTOR_STOPPED_ID 0x2001u
#define ROBOT_EXAMPLE_PRESSURE_FAULT_ID 0x1002u
#define ROBOT_EXAMPLE_SENSOR_FAULT_ID 0x1003u
#define ROBOT_EXAMPLE_SOFTWARE_RESET_ID 0x3001u

#endif
