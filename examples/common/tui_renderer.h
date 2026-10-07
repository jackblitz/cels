#pragma once

#include "cels.h"
#include "terminal.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Native Curses / Terminal Display Lifecycle.
 *
 * Demonstrates how to bridge external render and windowing backends
 * (ncurses, PDCurses, Raylib, SDL, Vulkan) into CELS using cel_lifecycle:
 * - Mount: Invoked when the TUI renderer enters the active composable tree.
 * - Unmount: Invoked when the root composition unmounts on shutdown.
 */
CEL_Lifecycle(TuiRendererLifecycle, void*, unused) {
    (void)unused;
    mount {
        CelsTerminalInit(true);
    }
    unmount {
        CelsTerminalRestore();
    }
}

/**
 * Declarative TUI Composables:
 */

/**
 * Top-level TUI Window Composable.
 *
 * Mounts the display backend lifecycle, begins the double-buffered frame,
 * and renders the top header banner.
 */
CEL_Composable(TuiWindow, const char*, title, const char*, profile) {
    cel_lifecycle(TuiRendererLifecycle, NULL);
    CelsTerminalBeginFrame();
    CelsTerminalDrawHeader(title, profile);
}

/**
 * Renders a single text line inside the TUI window frame.
 */
CEL_Composable(TuiLine, const char*, text) {
    CelsTerminalDrawLine("%s", text ? text : "");
}

/**
 * Renders an internal horizontal dividing border.
 */
CEL_Composable(TuiDivider) {
    CelsTerminalDrawDivider();
}

/**
 * Renders an empty spacer line.
 */
CEL_Composable(TuiEmptyLine) {
    CelsTerminalDrawEmptyLine();
}

/**
 * Renders an animated ASCII / colored progress bar.
 */
CEL_Composable(TuiProgressBar, const char*, label, float, current, float, max, const char*, extra) {
    CelsTerminalDrawProgressBar(label, current, max, extra);
}

/**
 * Renders the bottom footer with keyboard controls and flushes the frame buffer.
 */
CEL_Composable(TuiFooter, const char*, controls) {
    CelsTerminalDrawFooter(controls);
    CelsTerminalEndFrame();
}

/**
 * Convenience macro for formatted text lines.
 */
#define TuiPrint(fmt, ...) do { \
    char _tui_fmt_buf[128]; \
    snprintf(_tui_fmt_buf, sizeof(_tui_fmt_buf), (fmt), ##__VA_ARGS__); \
    TuiLine(_tui_fmt_buf); \
} while (0)

#ifdef __cplusplus
}
#endif
