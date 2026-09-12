#pragma once

/**
 * @file version.h
 * @brief Semantic version definitions and runtime version queries for CELS.
 *
 * This file is the single source of truth for CELS versioning across CMake,
 * the CELS library, test suites, and benchmark release artifacts.
 */

#include <stdint.h>

#define CELS_VERSION_MAJOR 0
#define CELS_VERSION_MINOR 1
#define CELS_VERSION_PATCH 0
#define CELS_VERSION_BUILD 1

/**
 * Packed 32-bit integer version code:
 * Format: 0xMMmmPPbb (Major, Minor, Patch, Build)
 * Example: 0.1.0 (build 1) -> 0x00010001
 */
#define CELS_VERSION_CODE \
    (((uint32_t)CELS_VERSION_MAJOR << 24) | \
     ((uint32_t)CELS_VERSION_MINOR << 16) | \
     ((uint32_t)CELS_VERSION_PATCH << 8)  | \
     ((uint32_t)CELS_VERSION_BUILD))

#define CELS_VERSION_STRING "0.1.0"

/**
 * Returns the human-readable version string (e.g. "0.1.0").
 */
static inline const char *CelsGetVersionString(void) {
    return CELS_VERSION_STRING;
}

/**
 * Returns the packed 32-bit integer version code.
 */
static inline uint32_t CelsGetVersionCode(void) {
    return CELS_VERSION_CODE;
}
