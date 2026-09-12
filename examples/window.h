#pragma once

#include "cels.h"

/* ========================================================================= */
/* Window State Definition                                                   */
/* ========================================================================= */

// Reactive state governing window properties and native resources
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
