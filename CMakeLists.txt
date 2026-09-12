cmake_minimum_required(VERSION 4.2.3)
project(cels C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_C_STANDARD 99)

if(WIN32)
    set(CELS_PLATFORM_DIR "windows")
elseif(APPLE)
    set(CELS_PLATFORM_DIR "macos")
elseif(UNIX)
    set(CELS_PLATFORM_DIR "linux")
else()
    message(FATAL_ERROR "cels: unsupported platform '${CMAKE_SYSTEM_NAME}'")
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" CELS_CONFIG_DIR)
set(CELS_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/${CELS_CONFIG_DIR}/${CELS_PLATFORM_DIR}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/libs")

# Compiler warning flags per Section 1 of coding-standards-c99.md
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Core CELS library (C99 modular implementation)
add_library(cels_core STATIC
    src/slot_table.c
    src/session.c
    src/state.c
)
target_include_directories(cels_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Main Executable
add_executable(CELS main.c)
target_link_libraries(CELS PRIVATE cels_core)

# Test Suite: Single unified executable test_suite.exe
enable_testing()

set(TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)

add_executable(test_suite
    test/cli/test_suite.c
    test/slot_table/test_slot_table.c
    test/slab/test_slab.c
    test/lifecycle/test_lifecycle.c
    test/state_lifetime/test_state_lifetime.c
    test/tree/test_tree.c
)
target_include_directories(test_suite PRIVATE ${TEST_COMMON_DIR})
target_link_libraries(test_suite PRIVATE cels_core)
set_target_properties(test_suite PROPERTIES OUTPUT_NAME test_suite)
add_test(NAME test_suite COMMAND test_suite)