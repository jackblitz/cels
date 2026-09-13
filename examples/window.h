#pragma once

#include "cels.h"

/* ========================================================================= */
/* System / Engine Memory Module                                             */
/* ========================================================================= */

/**
 * Platform/Windowing Subsystem Module.
 *
 * Represents system-level engine memory registered into the session by the App.
 * Accessible anywhere in the composition hierarchy via CEL_GetModule(PlatformModule).
 */
CEL_Module(PlatformModule) {
    const char *backendName;
    int refreshRateHz;
    float dpiScale;
};

/* ========================================================================= */
/* Reactive Window State Definition                                          */
/* ========================================================================= */

/**
 * Reactive state governing window properties and native resources.
 */
CEL_State(WindowState) {
    bool isOpen;
    int  width;
    int  height;
    void *nativeHandle;
};

/* ========================================================================= */
/* Window Composition Interface                                              */
/* ========================================================================= */

/**
 * Attaches the window composition and its lifecycle controller to the session.
 *
 * @param session Target session. Non-NULL.
 */
void Window_Attach(CelsSession *session);

/**
 * Returns a composition reference handle for CEL_Window.
 */
CelsCompositionRef Window_GetComposition(void);

/**
 * Prints the hierarchical composable tree and slab memory status of the session.
 *
 * @param session Target session. Non-NULL.
 */
void Window_PrintTree(const CelsSession *session);
