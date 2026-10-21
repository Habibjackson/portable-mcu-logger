cmake_minimum_required(VERSION 3.16)
project(embedded_logger LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(embedded_logger STATIC
    src/log_record.c
    src/log_compression.c
    src/log_queue.c
    src/log_recovery.c
    src/log_rotation.c
    src/log_core.c
    src/backends/ram/log_storage_ram.c
    src/backends/simulated_flash/log_storage_sim.c
    src/backends/spi_nor/log_storage_spi_nor.c
)
target_include_directories(embedded_logger PUBLIC include)

add_executable(elog_host_demo examples/host_demo/main.c)
target_link_libraries(elog_host_demo PRIVATE embedded_logger)

add_executable(elog_quickstart examples/quickstart/main.c)
target_link_libraries(elog_quickstart PRIVATE embedded_logger)

add_executable(elog_reboot_recovery examples/reboot_recovery/main.c)
target_link_libraries(elog_reboot_recovery PRIVATE embedded_logger)

add_executable(elog_isr_safe_state examples/isr_safe_state/main.c)
target_link_libraries(elog_isr_safe_state PRIVATE embedded_logger)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(example IN ITEMS elog_quickstart elog_reboot_recovery
                             elog_isr_safe_state)
        target_compile_options(${example} PRIVATE -Wall -Wextra -Wconversion
                               -Wsign-conversion -Wpedantic -Werror)
    endforeach()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(embedded_logger PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME example_quickstart COMMAND elog_quickstart)
    add_test(NAME example_reboot_recovery COMMAND elog_reboot_recovery)
    add_test(NAME example_isr_safe_state COMMAND elog_isr_safe_state)
    add_executable(elog_format_test tests/unit/test_format.c)
    target_link_libraries(elog_format_test PRIVATE embedded_logger)
    add_test(NAME format_and_ram COMMAND elog_format_test)
    add_executable(elog_core_test tests/unit/test_core.c)
    target_link_libraries(elog_core_test PRIVATE embedded_logger)
    add_test(NAME core_and_recovery COMMAND elog_core_test)
    add_executable(elog_fault_test tests/fault_injection/test_fault.c)
    target_link_libraries(elog_fault_test PRIVATE embedded_logger)
    add_test(NAME injected_power_cuts COMMAND elog_fault_test)
    add_executable(elog_codec_test tests/unit/test_codec.c)
    target_link_libraries(elog_codec_test PRIVATE embedded_logger)
    add_test(NAME codec_roundtrip COMMAND elog_codec_test)
    add_executable(elog_spi_nor_test tests/unit/test_spi_nor.c)
    target_link_libraries(elog_spi_nor_test PRIVATE embedded_logger)
    add_test(NAME spi_nor_descriptor COMMAND elog_spi_nor_test)
    add_executable(elog_c8_reservation_test tests/unit/test_c8_reservation.c)
    target_link_libraries(elog_c8_reservation_test PRIVATE embedded_logger)
    add_test(NAME c8_reserved_range COMMAND elog_c8_reservation_test)
    if(WIN32)
        add_executable(elog_isr_concurrent_test tests/unit/test_isr_concurrent_win.c)
        target_link_libraries(elog_isr_concurrent_test PRIVATE embedded_logger)
        add_test(NAME isr_concurrent_spsc COMMAND elog_isr_concurrent_test)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(elog_format_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        target_compile_options(elog_core_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        target_compile_options(elog_fault_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        target_compile_options(elog_codec_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        target_compile_options(elog_spi_nor_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        target_compile_options(elog_c8_reservation_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        if(WIN32)
            target_compile_options(elog_isr_concurrent_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror)
        endif()
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME host_tools
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_tools.py
                    $<TARGET_FILE:elog_host_demo> ${CMAKE_SOURCE_DIR})
        add_test(NAME flash_layout_verifier
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/tools/test_verify_flash_layout.py)
    endif()
endif()
