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

# Core Slot Table module (C99, zero heap allocation)
add_library(cels_slot_table STATIC
    src/slot_table.c
)
target_include_directories(cels_slot_table PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test Suite
enable_testing()

add_executable(test_slot_table test/test_slot_table.c)
target_link_libraries(test_slot_table PRIVATE cels_slot_table)
add_test(NAME test_slot_table COMMAND test_slot_table)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/cels/cels.h")
    add_executable(cels main.c)
    target_link_libraries(cels PRIVATE cels_slot_table)
endif()