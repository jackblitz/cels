cmake_minimum_required(VERSION 3.20...4.3)

# Single source of truth for version code and release string
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/cels/version.h" CELS_VER_LINE REGEX "^#define CELS_VERSION_STRING \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
string(REGEX REPLACE "^#define CELS_VERSION_STRING \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" "\\1" CELS_VERSION "${CELS_VER_LINE}")

project(cels VERSION ${CELS_VERSION} LANGUAGES C)

# Detect if CELS is being built as the root project or consumed as a subproject
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CELS_IS_TOP_LEVEL TRUE)
else()
    set(CELS_IS_TOP_LEVEL FALSE)
endif()

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
    src/engine.c
    src/module.c
)
target_include_directories(cels_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(cels_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# Modern CMake target aliases
add_library(cels::core ALIAS cels_core)
add_library(cels::cels ALIAS cels_core)

# Include CELS developer CMake helper functions (cels_add_application)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Cels.cmake")

# Build options: Enabled by default for top-level repo, disabled when consumed via FetchContent / add_subdirectory
option(CELS_BUILD_EXAMPLES "Build CELS canonical example host & app" ${CELS_IS_TOP_LEVEL})
option(CELS_BUILD_TESTS "Build CELS test suite" ${CELS_IS_TOP_LEVEL})
option(CELS_BUILD_BENCHMARKS "Build CELS benchmarks" ${CELS_IS_TOP_LEVEL})

# Global hot-reload option (defaults: Debug=ON, Release=OFF)
if(CMAKE_BUILD_TYPE MATCHES "^[Rr]el")
    set(CELS_HOT_RELOAD_DEFAULT OFF)
else()
    set(CELS_HOT_RELOAD_DEFAULT ON)
endif()
option(CELS_HOT_RELOAD "Enable dynamic hot-reload host (.exe + .dll). When OFF, builds a single monolithic .exe" ${CELS_HOT_RELOAD_DEFAULT})

# =============================================================================
# Canonical Example Application (cel_host + cel_app) via cels_add_application
# =============================================================================
if(CELS_BUILD_EXAMPLES)
    cels_add_application(
        HOST cel_host
        APP cel_app
        HOST_SOURCES examples/host.c
        APP_SOURCES
            examples/app.c
            examples/window.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples
    )

    # Application Rebuild Script Runner (Triggered by CLion 'Play' Button on cel_app)
    add_executable(build_app
        scripts/build_app.c
    )
    target_include_directories(build_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
    set_target_properties(build_app PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# =============================================================================
# Test Suite: Single unified CLI executable test_cli.exe
# =============================================================================
if(CELS_BUILD_TESTS)
    enable_testing()

    set(TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # Test fixture dynamic library for hot-reload tests
    add_library(test_fixture_app SHARED
        test/hot_reload/fixture_app.c
    )
    target_include_directories(test_fixture_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/hot_reload
    )
    target_link_libraries(test_fixture_app PRIVATE cels_core)
    set_target_properties(test_fixture_app PROPERTIES EXCLUDE_FROM_ALL TRUE)

    add_executable(test_cli
        test/cli/test_suite.c
        test/slot_table/test_slot_table.c
        test/slab/test_slab.c
        test/lifecycle/test_lifecycle.c
        test/state_lifetime/test_state_lifetime.c
        test/tree/test_tree.c
        test/benchmark/test_benchmark.c
        test/hot_reload/test_hot_reload.c
    )
    target_include_directories(test_cli PRIVATE
        ${TEST_COMMON_DIR}
        ${TEST_COMMON_DIR}/benchmark
    )
    target_link_libraries(test_cli PRIVATE cels_core)
    set_target_properties(test_cli PROPERTIES OUTPUT_NAME test_cli)
    add_dependencies(test_cli test_fixture_app)
    add_test(NAME test_cli COMMAND test_cli)
endif()

# =============================================================================
# Benchmark Executable: benchmark.exe
# =============================================================================
if(CELS_BUILD_BENCHMARKS)
    add_executable(benchmark test/benchmark/benchmark.c)
    target_include_directories(benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/test/benchmark
    )
    target_link_libraries(benchmark PRIVATE cels_core)
    set_target_properties(benchmark PROPERTIES OUTPUT_NAME benchmark)
endif()

# =============================================================================
# Installation and CMake Package Export Rules
# =============================================================================
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

install(TARGETS cels_core
    EXPORT CelsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/Cels.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cels)

install(EXPORT CelsTargets
    FILE CelsTargets.cmake
    NAMESPACE cels::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cels
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/CelsConfigVersion.cmake"
    VERSION ${CELS_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CelsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CelsConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cels
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/CelsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CelsConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cels
)