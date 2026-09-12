#include "window.h"
#include <stdio.h>

/* ========================================================================= */
/* 1. Lifecycle Callbacks for Managed Resources                              */
/* ========================================================================= */

/**
 * Called on mount when WindowState is allocated in the slot table.
 *
 * Simulates acquiring native window resources (e.g. window handles).
 *
 * @param self Target WindowState struct. Non-NULL.
 * @param s    Owning CelsSession. Non-NULL.
 */
static void
Window_OnCreated(WindowState *self, CelsSession *s)
{
    (void)s;
    self->nativeHandle = (void*)0x12345678;
    printf("  [Lifecycle] Window opened (%p) [%dx%d]\n",
           self->nativeHandle, self->width, self->height);
}

/**
 * Called on unmount when WindowState is pruned from the tree or session destroyed.
 *
 * Releases acquired native window resources.
 *
 * @param self Target WindowState struct. Non-NULL.
 * @param s    Owning CelsSession. Non-NULL.
 */
static void
Window_OnDestroyed(WindowState *self, CelsSession *s)
{
    (void)s;
    printf("  [Lifecycle] Window closed (%p)\n", self->nativeHandle);
    self->nativeHandle = NULL;
}

/* ========================================================================= */
/* 2. Reusable UI Composables                                                */
/* ========================================================================= */

/**
 * Leaf composable rendering a status badge when the window is visible.
 *
 * Automatically keyed by callsite location. Takes no caller parameters.
 */
CEL_Composable(CEL_StatusBadge, key) {
    printf("    [Badge] Window is active & visible!\n");
}

/**
 * Container composable displaying window metrics and render statistics.
 *
 * Preserves a local render counter across recompositions via cel_remember,
 * and reacts to dimension/open state changes via cel_watch.
 *
 * @param win Pointer to reactive WindowState managed by parent. Non-NULL.
 */
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
/* 3. Root Composition & Lifecycle Controller                                */
/* ========================================================================= */

/**
 * Root composition for the window application tree.
 *
 * Allocates managed WindowState with mount/unmount callbacks on initial pass,
 * and passes the persistent pointer down the composition tree.
 */
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

/**
 * Lifecycle evaluator observing WindowState.
 *
 * When state.isOpen becomes false, triggers cel_destroy() to flag the
 * composition for pruning, invoking Window_OnDestroyed.
 */
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
/* 4. Public Attachment Interface                                            */
/* ========================================================================= */

/**
 * Attaches CEL_Window and WindowLifeCycle to the session.
 *
 * @param session Target session. Non-NULL.
 */
void
Window_Attach(CelsSession *session)
{
    CEL_Attach(session, CEL_Window, WindowLifeCycle);
}
