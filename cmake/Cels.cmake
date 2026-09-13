# =============================================================================
# CELS: C99 Composition & State Management Library
# CMake Integration Module & Application Helper
# =============================================================================

include_guard(GLOBAL)

#[=======================================================================[.rst:
cels_add_application
--------------------

Declares a CELS application composed of a host engine executable and an
application logic target, supporting both dynamic hot-reload and single-binary
monolithic builds.

Usage:
  cels_add_application(
      HOST <host_target>                  # Name of host executable target
      APP <app_target>                    # Name of app module target
      HOST_SOURCES <sources...>           # Host engine entry point sources
      APP_SOURCES <sources...>            # Composable and application sources
      [MODE <AUTO|HOT_RELOAD|SINGLE_BINARY|MONOLITHIC>]
      [HOT_RELOAD]                        # Convenience flag for MODE HOT_RELOAD
      [SINGLE_BINARY]                     # Convenience flag for MODE SINGLE_BINARY
      [MONOLITHIC]                        # Synonym for SINGLE_BINARY
      [INCLUDES <dirs...>]                # Common include directories
      [HOST_INCLUDES <dirs...>]           # Host-only include directories
      [APP_INCLUDES <dirs...>]            # App-only include directories
      [LIBRARIES <libs...>]               # Common link libraries (cels::core added automatically)
      [HOST_LIBRARIES <libs...>]          # Host-only link libraries
      [APP_LIBRARIES <libs...>]           # App-only link libraries
      [DEFINES <defs...>]                 # Preprocessor compile definitions
      [OUTPUT_DIR <dir>]                  # Directory to co-locate .exe and .dll
  )

Generated Targets:
- <HOST>: The primary engine executable.
- <APP>: The application target (shared library in HOT_RELOAD, alias in SINGLE_BINARY).
- <APP>_rebuild: Executable runner target for CLion/IDE 'Play' button to rebuild and hot-reload.

Modes:
- HOT_RELOAD: Builds HOST as an executable and APP as a shared library (.dll / .so / .dylib).
  Attaches target dependencies, creates <APP>_rebuild runner, and sets CELS_HOT_RELOAD=1.
- SINGLE_BINARY / MONOLITHIC: Compiles HOST and APP together into a single standalone executable.
  Creates compatibility custom targets for APP and <APP>_rebuild, and sets CELS_HOT_RELOAD=0.
- AUTO (default): Uses HOT_RELOAD for Debug builds, and SINGLE_BINARY for Release builds.
#]=======================================================================]
function(cels_add_application)
    set(options HOT_RELOAD SINGLE_BINARY MONOLITHIC)
    set(oneValueArgs HOST APP MODE OUTPUT_DIR)
    set(multiValueArgs HOST_SOURCES APP_SOURCES INCLUDES HOST_INCLUDES APP_INCLUDES LIBRARIES HOST_LIBRARIES APP_LIBRARIES DEFINES)

    cmake_parse_arguments(PARSE_ARGV 0 PARSED "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT PARSED_HOST)
        message(FATAL_ERROR "cels_add_application: Required argument 'HOST' (host executable target name) is missing.")
    endif()

    if(NOT PARSED_APP)
        message(FATAL_ERROR "cels_add_application: Required argument 'APP' (application module target name) is missing.")
    endif()

    if(NOT PARSED_HOST_SOURCES)
        message(FATAL_ERROR "cels_add_application: Required argument 'HOST_SOURCES' is missing for target '${PARSED_HOST}'.")
    endif()

    if(NOT PARSED_APP_SOURCES)
        message(FATAL_ERROR "cels_add_application: Required argument 'APP_SOURCES' is missing for target '${PARSED_APP}'.")
    endif()

    # 1. Resolve core CELS library target
    if(TARGET cels::core)
        set(CELS_CORE_LIB cels::core)
    elseif(TARGET cels_core)
        set(CELS_CORE_LIB cels_core)
    else()
        message(FATAL_ERROR "cels_add_application: Neither 'cels::core' nor 'cels_core' target found. Ensure CELS is added before defining applications.")
    endif()

    # 2. Determine build mode (HOT_RELOAD vs SINGLE_BINARY)
    set(RESOLVED_MODE "")

    if(PARSED_SINGLE_BINARY OR PARSED_MONOLITHIC)
        set(RESOLVED_MODE "SINGLE_BINARY")
    elseif(PARSED_HOT_RELOAD)
        set(RESOLVED_MODE "HOT_RELOAD")
    elseif(PARSED_MODE)
        string(TOUPPER "${PARSED_MODE}" PARSED_MODE_UPPER)
        if(PARSED_MODE_UPPER STREQUAL "SINGLE_BINARY" OR PARSED_MODE_UPPER STREQUAL "MONOLITHIC")
            set(RESOLVED_MODE "SINGLE_BINARY")
        elseif(PARSED_MODE_UPPER STREQUAL "HOT_RELOAD")
            set(RESOLVED_MODE "HOT_RELOAD")
        elseif(PARSED_MODE_UPPER STREQUAL "AUTO")
            set(RESOLVED_MODE "AUTO")
        else()
            message(FATAL_ERROR "cels_add_application: Invalid MODE '${PARSED_MODE}'. Valid options are AUTO, HOT_RELOAD, SINGLE_BINARY, MONOLITHIC.")
        endif()
    else()
        set(RESOLVED_MODE "AUTO")
    endif()

    # If AUTO, inspect global CELS_HOT_RELOAD option or CMAKE_BUILD_TYPE
    if(RESOLVED_MODE STREQUAL "AUTO")
        if(DEFINED CELS_HOT_RELOAD)
            if(CELS_HOT_RELOAD)
                set(RESOLVED_MODE "HOT_RELOAD")
            else()
                set(RESOLVED_MODE "SINGLE_BINARY")
            endif()
        elseif(CMAKE_BUILD_TYPE MATCHES "^[Rr]el")
            set(RESOLVED_MODE "SINGLE_BINARY")
        else()
            set(RESOLVED_MODE "HOT_RELOAD")
        endif()
    endif()

    # 3. Resolve output directory
    set(RESOLVED_OUTPUT_DIR "")
    if(PARSED_OUTPUT_DIR)
        set(RESOLVED_OUTPUT_DIR "${PARSED_OUTPUT_DIR}")
    elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(RESOLVED_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endif()

    # 4. Generate targets based on mode
    if(RESOLVED_MODE STREQUAL "HOT_RELOAD")
        message(STATUS "cels_add_application: Configuring '${PARSED_HOST}' and '${PARSED_APP}' in HOT-RELOAD mode (.exe + .dll)")

        # Target 1: Dynamic Application Library (.dll / .so / .dylib)
        add_library(${PARSED_APP} SHARED ${PARSED_APP_SOURCES})
        set_target_properties(${PARSED_APP} PROPERTIES
            PREFIX ""
            OUTPUT_NAME "${PARSED_APP}"
        )
        target_link_libraries(${PARSED_APP} PRIVATE ${CELS_CORE_LIB} ${PARSED_LIBRARIES} ${PARSED_APP_LIBRARIES})
        if(PARSED_INCLUDES OR PARSED_APP_INCLUDES)
            target_include_directories(${PARSED_APP} PRIVATE ${PARSED_INCLUDES} ${PARSED_APP_INCLUDES})
        endif()
        if(PARSED_DEFINES)
            target_compile_definitions(${PARSED_APP} PRIVATE ${PARSED_DEFINES})
        endif()

        # Target 2: Host Engine Executable (.exe)
        add_executable(${PARSED_HOST} ${PARSED_HOST_SOURCES})
        target_link_libraries(${PARSED_HOST} PRIVATE ${CELS_CORE_LIB} ${PARSED_LIBRARIES} ${PARSED_HOST_LIBRARIES})
        if(PARSED_INCLUDES OR PARSED_HOST_INCLUDES)
            target_include_directories(${PARSED_HOST} PRIVATE ${PARSED_INCLUDES} ${PARSED_HOST_INCLUDES})
        endif()
        target_compile_definitions(${PARSED_HOST} PRIVATE
            CELS_HOT_RELOAD=1
            "CELS_CMAKE_COMMAND=\"${CMAKE_COMMAND}\""
            "CELS_BINARY_DIR=\"${CMAKE_BINARY_DIR}\""
            "CELS_APP_TARGET=\"${PARSED_APP}\""
            ${PARSED_DEFINES}
        )

        # Host depends on App: Rebuilding/launching Host automatically ensures App is up-to-date
        add_dependencies(${PARSED_HOST} ${PARSED_APP})

        # Ensure co-located binary placement
        if(RESOLVED_OUTPUT_DIR)
            set_target_properties(${PARSED_APP} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${RESOLVED_OUTPUT_DIR}"
                LIBRARY_OUTPUT_DIRECTORY "${RESOLVED_OUTPUT_DIR}"
            )
            set_target_properties(${PARSED_HOST} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${RESOLVED_OUTPUT_DIR}"
            )
        endif()

        # Target 3: Rebuild Runner Executable (${PARSED_APP}_rebuild)
        # Enables CLion and IDE users to select this target and click the green 'Play' button
        set(RUNNER_SRC "${CMAKE_CURRENT_BINARY_DIR}/${PARSED_APP}_rebuild_runner.c")
        file(WRITE "${RUNNER_SRC}"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"int main(void) {\n"
"    printf(\"\\n======================================================================\\n\");\n"
"    printf(\"  [CELS] Rebuilding application library '${PARSED_APP}'...\\n\");\n"
"    printf(\"======================================================================\\n\");\n"
"#if defined(_WIN32)\n"
"    int res = system(\"\\\"\\\"${CMAKE_COMMAND}\\\" --build \\\"${CMAKE_BINARY_DIR}\\\" --target ${PARSED_APP}\\\"\");\n"
"#else\n"
"    int res = system(\"\\\"${CMAKE_COMMAND}\\\" --build \\\"${CMAKE_BINARY_DIR}\\\" --target ${PARSED_APP}\");\n"
"#endif\n"
"    if (res == 0) {\n"
"        printf(\"\\n[CELS] Rebuild successful! If '${PARSED_HOST}' is running, it will reload in <50ms.\\n\\n\");\n"
"    } else {\n"
"        printf(\"\\n[CELS] Rebuild failed with exit code %d.\\n\\n\", res);\n"
"    }\n"
"    return res;\n"
"}\n"
        )
        add_executable(${PARSED_APP}_rebuild "${RUNNER_SRC}")
        set_target_properties(${PARSED_APP}_rebuild PROPERTIES EXCLUDE_FROM_ALL TRUE)
        if(RESOLVED_OUTPUT_DIR)
            set_target_properties(${PARSED_APP}_rebuild PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${RESOLVED_OUTPUT_DIR}"
            )
        endif()

        # Generate convenience scripts in build directory for terminal users
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rebuild_${PARSED_APP}.bat"
"@echo off\r\n\"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target ${PARSED_APP}\r\n"
        )
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rebuild_${PARSED_APP}.sh"
"#!/bin/sh\n\"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target ${PARSED_APP}\n"
        )

    else()
        message(STATUS "cels_add_application: Configuring '${PARSED_HOST}' in SINGLE-BINARY mode (monolithic standalone .exe)")

        # Single Monolithic Executable Target
        add_executable(${PARSED_HOST} ${PARSED_HOST_SOURCES} ${PARSED_APP_SOURCES})
        target_link_libraries(${PARSED_HOST} PRIVATE
            ${CELS_CORE_LIB}
            ${PARSED_LIBRARIES}
            ${PARSED_HOST_LIBRARIES}
            ${PARSED_APP_LIBRARIES}
        )
        if(PARSED_INCLUDES OR PARSED_HOST_INCLUDES OR PARSED_APP_INCLUDES)
            target_include_directories(${PARSED_HOST} PRIVATE
                ${PARSED_INCLUDES}
                ${PARSED_HOST_INCLUDES}
                ${PARSED_APP_INCLUDES}
            )
        endif()
        target_compile_definitions(${PARSED_HOST} PRIVATE
            CELS_HOT_RELOAD=0
            ${PARSED_DEFINES}
        )

        # Compatibility target: If IDE or script targets APP, build HOST seamlessly
        if(NOT TARGET ${PARSED_APP})
            add_custom_target(${PARSED_APP} DEPENDS ${PARSED_HOST})
        endif()

        if(NOT TARGET ${PARSED_APP}_rebuild)
            add_custom_target(${PARSED_APP}_rebuild DEPENDS ${PARSED_HOST})
        endif()

        if(RESOLVED_OUTPUT_DIR)
            set_target_properties(${PARSED_HOST} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${RESOLVED_OUTPUT_DIR}"
            )
        endif()
    endif()
endfunction()
