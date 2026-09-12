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

# Main Application
add_executable(cels
    main.c
    cels.h
)
target_include_directories(cels PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Test Suite
enable_testing()

add_executable(TreeTest
    test/TreeTest.c
    cels.h
)
target_include_directories(TreeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME TreeTest COMMAND TreeTest)

add_executable(LifecycleTest
    test/LifecycleTest.c
    cels.h
)
target_include_directories(LifecycleTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME LifecycleTest COMMAND LifecycleTest)

add_executable(StateLifetimeTest test/StateLifetimeTest.c cels.h)
target_include_directories(StateLifetimeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME StateLifetimeTest COMMAND StateLifetimeTest)
