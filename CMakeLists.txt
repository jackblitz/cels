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

add_executable(cels main.c)
target_link_libraries(cels PRIVATE
    cels_slottable
)

# Compiler warning flags per Section 1 of coding-standards-c99.md
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Third-party: Flecs ECS amalgamation
add_library(flecs STATIC
    vendor/flecs/flecs.c
)
target_include_directories(flecs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/flecs
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(flecs PRIVATE -w)
endif()
if(WIN32)
    target_link_libraries(flecs PUBLIC ws2_32 dbghelp)
endif()

# Composition engine library
add_library(cels_slottable STATIC
    src/composition/slottable/slot_table.c
    src/composition/composer.c
    src/composition/recomposition_dispatcher.c
    src/composition/state.c
)
target_include_directories(cels_slottable PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/flecs
)
target_link_libraries(cels_slottable PUBLIC
    flecs
)

# Test suite: Slot Table
add_executable(test_slot_table
    test/test_slot_table.c
)
target_link_libraries(test_slot_table PRIVATE
    cels_slottable
)
set_target_properties(test_slot_table PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
)

# Test suite: Composer & Diffing Engine
add_executable(test_composer
    test/test_composer.c
)
target_link_libraries(test_composer PRIVATE
    cels_slottable
)
set_target_properties(test_composer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
)

# Test suite: Multi-Threaded Recomposition Dispatcher
add_executable(test_recomposition_threading
    test/test_recomposition_threading.c
)
target_link_libraries(test_recomposition_threading PRIVATE
    cels_slottable
    flecs
)
set_target_properties(test_recomposition_threading PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
)

# Test suite: Cels Session & Flecs Entity Lifecycle
add_executable(test_cels_session
    test/test_cels_session.c
)
target_link_libraries(test_cels_session PRIVATE
    cels_slottable
    flecs
)
set_target_properties(test_cels_session PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
)

# Test suite: Reactive State Cells & Invalidation
add_executable(test_state
    test/test_state.c
)
target_link_libraries(test_state PRIVATE
    cels_slottable
    flecs
)
set_target_properties(test_state PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CELS_OUTPUT_DIR}/test"
)

# Copy the MinGW pthread runtime DLL next to the test binaries if the
# toolchain links it dynamically.
if(CELS_PLATFORM_DIR STREQUAL "window")
    get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    if(EXISTS "${MINGW_BIN_DIR}/libwinpthread-1.dll")
        add_custom_command(TARGET test_composer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MINGW_BIN_DIR}/libwinpthread-1.dll"
                "${CELS_OUTPUT_DIR}/test/"
            COMMENT "Copying libwinpthread-1.dll to ${CELS_OUTPUT_DIR}/test"
        )
        add_custom_command(TARGET test_recomposition_threading POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MINGW_BIN_DIR}/libwinpthread-1.dll"
                "${CELS_OUTPUT_DIR}/test/"
            COMMENT "Copying libwinpthread-1.dll to ${CELS_OUTPUT_DIR}/test"
        )
        add_custom_command(TARGET test_cels_session POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MINGW_BIN_DIR}/libwinpthread-1.dll"
                "${CELS_OUTPUT_DIR}/test/"
            COMMENT "Copying libwinpthread-1.dll to ${CELS_OUTPUT_DIR}/test"
        )
        add_custom_command(TARGET test_state POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MINGW_BIN_DIR}/libwinpthread-1.dll"
                "${CELS_OUTPUT_DIR}/test/"
            COMMENT "Copying libwinpthread-1.dll to ${CELS_OUTPUT_DIR}/test"
        )
    endif()
endif()
