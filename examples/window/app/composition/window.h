#pragma once

#include "cels.h"

/**
 * Compile-time unique 64-bit identifier for the Window root composition & state.
 * Allows using CEL_Window directly instead of CEL_ID("CEL_Window").
 */
#define CEL_Window CEL_ID("CEL_Window")

/* ========================================================================= */
/* Reactive Window State Definition                                          */
/* ========================================================================= */

CEL_State(WindowState) {
    bool isOpen;
    bool showBadge;
    int  width;
    int  height;
    void *nativeHandle;
};

/* ========================================================================= */
/* Window Composition Interface                                              */
/* ========================================================================= */

CelsCompositionRef Window_GetComposition(void);
