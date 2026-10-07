#pragma once

#include "cels.h"
#include "cels/engine.h"
#include "terminal.h"
#include "common_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs the unified CELS host engine for an application.
 *
 * Handles:
 * - Engine initialization with CELS_PROFILE_1K
 * - Dynamic hot-reloading (CelsAppRuntimeCheck)
 * - TUI terminal setup and teardown
 * - Discrete key signal dispatch via CelsKeySignal
 * - Automated verification (--once)
 *
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return 0 on success, non-zero on failure.
 */
int CelsRunHostEx(int argc, char **argv, const char *appName);

#if defined(CELS_APP_TARGET)
#define CelsRunHost(argc, argv) CelsRunHostEx((argc), (argv), CELS_APP_TARGET)
#else
int CelsRunHost(int argc, char **argv);
#endif

#ifdef __cplusplus
}
#endif
