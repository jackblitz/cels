#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes terminal modes for ANSI escape codes and non-blocking input.
 *
 * @param interactive True to enable VT processing, hide cursor, and position screen.
 */
void CelsTerminalInit(bool interactive);

/**
 * Restores original terminal modes and restores the cursor.
 */
void CelsTerminalRestore(void);

/**
 * Polls for a key press without blocking.
 *
 * @return Key character code, or 0 if no key available.
 */
int CelsTerminalPollKey(void);

/**
 * Suspends thread execution for ms milliseconds.
 */
void CelsTerminalSleepMs(int ms);

/**
 * Begins a TUI window frame, repositioning the cursor to the top-left (1,1).
 */
void CelsTerminalBeginFrame(void);

/**
 * Completes a TUI window frame, erasing any stale content and flushing stdout.
 */
void CelsTerminalEndFrame(void);

/**
 * Draws the top border and title header of the window box.
 */
void CelsTerminalDrawHeader(const char *title, const char *profile);

/**
 * Draws a formatted content line padded to the box width with borders.
 */
void CelsTerminalDrawLine(const char *fmt, ...);

/**
 * Draws an empty content line with left and right borders.
 */
void CelsTerminalDrawEmptyLine(void);

/**
 * Draws a horizontal divider line inside the window box.
 */
void CelsTerminalDrawDivider(void);

/**
 * Draws an ASCII/ANSI progress bar line inside the window box.
 *
 * @param label   Field name (e.g. "Health" or "Progress").
 * @param current Current value.
 * @param max     Maximum value.
 * @param extra   Optional trailing label (e.g. curve name or latency).
 */
void CelsTerminalDrawProgressBar(const char *label, float current, float max, const char *extra);

/**
 * Draws the bottom footer line with control hints and closes the window box.
 */
void CelsTerminalDrawFooter(const char *controls);

#ifdef __cplusplus
}
#endif
