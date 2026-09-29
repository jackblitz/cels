#pragma once

#include "cels.h"
#include "window.h"

/**
 * Container composable displaying window metrics and render count.
 * Declared in window_content.h and implemented in window_content.c.
 */
CEL_ComposableDecl(CEL_WindowContent, WindowState *win);
