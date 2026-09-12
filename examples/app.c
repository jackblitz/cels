#include "cels.h"
#include "window.h"
#include <stdio.h>

/* ========================================================================= */
/* Application Entry Point & Session Lifecycle                               */
/* ========================================================================= */

/**
 * Example application entry point demonstrating session lifecycle and reactivity.
 *
 * Initializes a CelsSession, attaches the CEL_Window composition, and executes
 * passes demonstrating initial mount, quiet check, state mutations, and teardown.
 *
 * @return 0 on success.
 */
int
main(void)
{
    // 1. Initialize session: defaults to 32 KiB L1 data cache slab (CELS_SLAB_32K)
    // Developers can also configure .slabSize = CELS_SLAB_48K or custom sizes
    CelsSession session;
    CelsSessionInit(&session, NULL);

    // 2. Attach window composition with its lifecycle evaluator
    Window_Attach(&session);

    // Pass 1: Initial Mount
    // Builds tree, calls Window_OnCreated to initialize state in the slot table
    printf("=== Pass 1: Initial Mount ===\n");
    CelsSessionRecompose(&session);

    // State Query: Retrieve live state from the session by key (no global variables!)
    WindowState *win = CEL_GetState(&session, CEL_KEY("CEL_Window"), WindowState);
    printf("  [CEL_GetState] Found window state: %p [%dx%d, open: %s]\n",
           win ? win->nativeHandle : NULL,
           win ? win->width : 0,
           win ? win->height : 0,
           (win && win->isOpen) ? "true" : "false");

    // Quiet Check: No state changed -> O(1) instant skip
    printf("\n=== Quiet Check (Nothing Changed) ===\n");
    CelsSessionRecompose(&session);
    printf("  Quiet recompose completed instantly (0 work done).\n");

    // Event 1: Mutate the state retrieved from the session
    printf("\n=== Event 1: Resize Window to 1024x768 (cel_mutate) ===\n");
    cel_mutate(&session, win) {
        this->width  = 1024;
        this->height = 768;
    }
    CelsSessionRecompose(&session);

    // Event 2: Mutate window size again
    printf("\n=== Event 2: Resize Window to 1920x1080 ===\n");
    cel_mutate(&session, win) {
        this->width  = 1920;
        this->height = 1080;
    }
    CelsSessionRecompose(&session);

    // Event 3: Close the window -> triggers WindowLifeCycle -> cel_destroy() -> OnDestroyed
    printf("\n=== Event 3: Close Window ===\n");
    cel_mutate(&session, win) {
        this->isOpen = false;
    }
    CelsSessionRecompose(&session);

    // Cleanup session and free internal slab
    CelsSessionDestroy(&session);
    return 0;
}
