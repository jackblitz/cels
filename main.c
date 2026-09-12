#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* ========================================================================= */
/* 1. Component State Definition (No Global Variables)                       */
/* ========================================================================= */

// Reactive state governing window properties and native resources
CEL_State(WindowState) {
    bool isOpen;
    int  width;
    int  height;
    void *nativeHandle;
};

/* ========================================================================= */
/* 2. Lifecycle Callbacks for Managed Resources                              */
/* ========================================================================= */

// Called on mount when the state slot is allocated in the slot table
static void Window_OnCreated(WindowState *self, CelsSession *s) {
    (void)s;
    self->nativeHandle = (void*)0x12345678;
    printf("  [Lifecycle] Window opened (%p) [%dx%d]\n",
           self->nativeHandle, self->width, self->height);
}

// Called on despawn when the composition is pruned from the tree
static void Window_OnDestroyed(WindowState *self, CelsSession *s) {
    (void)s;
    printf("  [Lifecycle] Window closed (%p)\n", self->nativeHandle);
    self->nativeHandle = NULL;
}

/* ========================================================================= */
/* 3. Reusable UI Composables                                                */
/* ========================================================================= */

// Leaf composable: auto-keyed by the engine, takes no parameters
CEL_Composable(CEL_StatusBadge, key) {
    printf("    [Badge] Window is active & visible!\n");
}

// Container composable: receives WindowState* passed down from parent composition
CEL_Composable(CEL_WindowContent, WindowState*, win) {
    // Component-local persistent memory: preserved across recompositions
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    // Reactive subscription: watches the passed-in state, re-runs when mutated
    WindowState state = cel_watch(win);

    printf("  [Content] Local render count: %d | Window: %dx%d (open: %s)\n",
           *localRenderCount, state.width, state.height, state.isOpen ? "true" : "false");

    // Render child badge while window is open
    if (state.isOpen) {
        CEL_StatusBadge();
    }
}

/* ========================================================================= */
/* 4. Composition & Lifecycle Controller                                     */
/* ========================================================================= */

// Root Composition: initializes lifecycle state on mount and passes it to children
CEL_Composition(CEL_Window, key) {
    // 1. Initialize only the fields you want; omitted fields default to 0/NULL/false:
    WindowState init = {
        .isOpen = true,
        .width  = 800,
        .height = 600
    };

    // 2. Type is deduced directly from 'init'
    WindowState *win = cel_lifecycle_state(init, Window_OnCreated, Window_OnDestroyed);

    // 3. Pass the state through the composition tree to child composables:
    CEL_WindowContent(win);
}

// Lifecycle evaluator: observes the composition's state and triggers despawn
CEL_LifeCycle(WindowLifeCycle, WindowState) {
    if (it != NULL) {
        // Watch the state managed by the composition
        WindowState state = cel_watch(it);
        if (!state.isOpen) {
            printf("  [WindowLifeCycle] Window close requested -> calling cel_destroy()\n");
            cel_destroy();
        }
    }
}

/* ========================================================================= */
/* 5. Application Entry Point                                                */
/* ========================================================================= */

int main(void) {
    // Initialize session: defaults to 32 KiB L1 data cache slab (CELS_SLAB_32K)
    // Developers can also configure .slabSize = CELS_SLAB_48K or custom sizes
    CelsSession session;
    CelsSessionInit(&session, NULL);

    // Attach root composition with its lifecycle evaluator
    CEL_Attach(&session, CEL_Window, WindowLifeCycle);

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

    // Event 3: Close the window -> triggers CEL_LifeCycle -> cel_destroy() -> OnDestroyed
    printf("\n=== Event 3: Close Window ===\n");
    cel_mutate(&session, win) {
        this->isOpen = false;
    }
    CelsSessionRecompose(&session);

    // Cleanup session and free internal arenas
    CelsSessionDestroy(&session);
    return 0;
}