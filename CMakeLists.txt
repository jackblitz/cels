cmake_minimum_required(VERSION 4.2.3)
project(cels C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_C_STANDARD 99)

if(WIN32)
    set(CELS_PLATFORM_DIR "window")
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

# Composition engine library. No third-party dependencies: CELS owns a slab and
# the diffing state over it, and nothing else.
add_library(cels_composition STATIC
    src/composition/slottable/slot_table.c
    src/composition/composer.c
    src/composition/session.c
    src/composition/state.c
)
target_include_directories(cels_composition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(cels main.c)
target_link_libraries(cels PRIVATE cels_composition)

# Test suites
foreach(suite slot_table composer session state)
    add_executable(test_${suite} test/test_${suite}.c)
    target_link_libraries(test_${suite} PRIVATE cels_composition)
    set_target_properties(test_${suite} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
    )
endforeach()
