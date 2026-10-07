cmake_minimum_required(VERSION 3.20...4.3)

# Single source of truth for version code and release string
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/cels/runtime/version.h" CELS_VER_LINE REGEX "^#define CELS_VERSION_STRING \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
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

if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU")
    add_link_options(-static-libgcc)
endif()

# Core CELS library sources
set(CELS_CORE_SOURCES
    src/runtime/slot_table.c
    src/runtime/session.c
    src/runtime/state.c
    src/runtime/task.c
    src/runtime/transition.c
    src/runtime/event.c
    src/host/engine.c
    src/host/module.c
    src/platform/fiber.c
    src/platform/log.c
)

# Core CELS library (C99 modular implementation)
add_library(cels_core STATIC ${CELS_CORE_SOURCES})
target_include_directories(cels_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(UNIX)
    if(NOT APPLE)
        target_link_libraries(cels_core PUBLIC ${CMAKE_DL_LIBS} m pthread)
    else()
        target_link_libraries(cels_core PUBLIC pthread)
    endif()
endif()

# Link specification to statically embed libgcc and libwinpthread on MinGW Windows
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(CELS_CORE_LINK
        -Wl,--whole-archive
        cels_core
        -Wl,--no-whole-archive
        -static-libgcc
        -Wl,-Bstatic,--whole-archive
        winpthread
        -Wl,--no-whole-archive,-Bdynamic
    )
else()
    set(CELS_CORE_LINK cels_core)
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

# Helper runner for IDE hot-reload rebuilds (CLion, VS Code)
add_executable(cels_hot_reload_notifier
    tools/hot_reload_notifier.c
)
set_target_properties(cels_hot_reload_notifier PROPERTIES
    OUTPUT_NAME cels_hot_reload_notifier
)

# =============================================================================
# Canonical Example Applications (**_host + **_dll) via cels_add_application
if(CELS_BUILD_EXAMPLES)
    # PDCurses static library for terminal TUI
    file(GLOB PDCURSES_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdcurses/pdcurses/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdcurses/wincon/*.c
    )
    add_library(pdcurses STATIC ${PDCURSES_SRCS})
    target_compile_options(pdcurses PRIVATE -w)
    target_include_directories(pdcurses PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdcurses
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdcurses/wincon
    )
    target_compile_definitions(pdcurses PUBLIC PDC_WIDE)
    set_target_properties(pdcurses PROPERTIES
        C_STANDARD 99
        ARCHIVE_OUTPUT_DIRECTORY "${CELS_OUTPUT_LIBS_DIR}"
    )

    # Common terminal TUI and host runner library for all examples
    add_library(cels_example_common STATIC
        examples/common/terminal.c
        examples/common/host.c
    )
    target_compile_definitions(cels_example_common PRIVATE CELS_NO_MAIN)
    target_include_directories(cels_example_common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(cels_example_common PUBLIC cels_core pdcurses)

    # 1. Window Example (cel_host + cel_dll)
    cels_add_application(
        HOST cel_host
        DLL cel_dll
        HOST_SOURCES
            examples/window/host.c
        APP_SOURCES
            examples/window/app/app.c
            examples/window/app/composition/window.c
            examples/window/app/composition/window_content.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/window/app
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/window/app/composition
        LIBRARIES
            cels_example_common
    )

    # 2. Task / Network Example (cel_task_host + cel_task_dll)
    cels_add_application(
        HOST cel_task_host
        DLL cel_task_dll
        HOST_SOURCES
            examples/task/host.c
        APP_SOURCES
            examples/task/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/task/app
        LIBRARIES
            cels_example_common
    )

    # 3. Temporal Transition Example (cel_transition_host + cel_transition_dll)
    cels_add_application(
        HOST cel_transition_host
        DLL cel_transition_dll
        HOST_SOURCES
            examples/transition/host.c
        APP_SOURCES
            examples/transition/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition/app
        LIBRARIES
            cels_example_common
    )

    # 4. Events, Signals & Broadcasts Example (cel_event_host + cel_event_dll)
    cels_add_application(
        HOST cel_event_host
        DLL cel_event_dll
        HOST_SOURCES
            examples/event/host.c
        APP_SOURCES
            examples/event/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/event/app
        LIBRARIES
            cels_example_common
    )

    # 5. Declarative Component & ECS Example (cel_component_host + cel_component_dll)
    cels_add_application(
        HOST cel_component_host
        DLL cel_component_dll
        HOST_SOURCES
            examples/component/host.c
        APP_SOURCES
            examples/component/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/component/app
        LIBRARIES
            cels_example_common
    )

    # 6. Ambient Theming Example (cel_theming_host + cel_theming_dll)
    cels_add_application(
        HOST cel_theming_host
        DLL cel_theming_dll
        HOST_SOURCES
            examples/theming/host.c
        APP_SOURCES
            examples/theming/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/theming/app
        LIBRARIES
            cels_example_common
    )

    # 7. Declarative Input & Focus Scoping Example (cel_input_host + cel_input_dll)
    cels_add_application(
        HOST cel_input_host
        DLL cel_input_dll
        HOST_SOURCES
            examples/input/host.c
        APP_SOURCES
            examples/input/app/app.c
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/input/app
        LIBRARIES
            cels_example_common
    )
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
    target_link_libraries(test_fixture_app PRIVATE ${CELS_CORE_LINK})
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
        test/task/test_task.c
        test/transaction/test_transaction.c
        test/transition/test_transition.c
        test/event/test_event.c
        test/input/test_input.c
    )
    target_include_directories(test_cli PRIVATE
        ${TEST_COMMON_DIR}
        ${TEST_COMMON_DIR}/benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
    )
    target_link_libraries(test_cli PRIVATE ${CELS_CORE_LINK})
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
    target_link_libraries(benchmark PRIVATE ${CELS_CORE_LINK})
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