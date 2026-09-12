#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* ========================================================================= */
/* 1. Reactive State Definitions                                             */
/* ========================================================================= */

// Reactive state governing window properties and lifetime
CEL_State(WindowState) {
    bool isOpen;
    int  width;
    int  height;
};

static WindowState g_windowState = {
    .isOpen = true,
    .width  = 800,
    .height = 600
};

// Application-level reactive state
CEL_State(AppState) {
    int counter;
};

static AppState g_appState = {
    .counter = 0
};

/* ========================================================================= */
/* 2. Native Resources & Lifecycle State                                     */
/* ========================================================================= */

// Represents an external/native resource (e.g. an SDL/GLFW window handle)
CEL_State(SdlWindow) {
    void *nativeHandle;
};

static void SdlWindow_OnCreated(SdlWindow *self, CelsSession *s) {
    (void)s;
    self->nativeHandle = (void*)0x12345678;
    printf("  [Lifecycle] SdlWindow opened (%p) [%dx%d]\n",
           self->nativeHandle, g_windowState.width, g_windowState.height);
}

static void SdlWindow_OnDestroyed(SdlWindow *self, CelsSession *s) {
    (void)s;
    printf("  [Lifecycle] SdlWindow closed (%p)\n", self->nativeHandle);
    self->nativeHandle = NULL;
}

/* ========================================================================= */
/* 3. Reusable UI Composables                                                */
/* ========================================================================= */

// Leaf composable: rendered conditionally inside other composables
CEL_Composable(CEL_StatusBadge, key) {
    (void)key;
    printf("    [Badge] Notification badge active!\n");
}

// Container composable: demonstrates persistent local memory and reactive state
CEL_Composable(CEL_WindowContent, key) {
    (void)key;

    // Component-local persistent memory: preserved across recompositions
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    // Reactive subscription: component automatically re-runs when g_appState changes
    AppState state = cel_watch(&g_appState);

    printf("  [Content] Local render count: %d | App counter: %d\n",
           *localRenderCount, state.counter);

    // Conditional composition: render child composable based on reactive state
    if (state.counter > 0) {
        CEL_StatusBadge(CEL_KEY("StatusBadge"));
    }
}

/* ========================================================================= */
/* 4. Composition & Lifecycle Controller                                     */
/* ========================================================================= */

// Composition: owns the subtree and manages native resource lifecycles
CEL_Composition(CEL_Window, key) {
    (void)key;

    // Bind native resource lifecycle to this composition node
    cel_lifecycle_state(s, SdlWindow, SdlWindow_OnCreated, SdlWindow_OnDestroyed);

    // Compose child hierarchy
    CEL_WindowContent(CEL_KEY("WindowContent"));
}

// Lifecycle evaluator: controls when the composition remains active or despawns
CEL_LifeCycle(WindowLifeCycle, WindowState) {
    // Subscribe composition lifetime to WindowState changes
    WindowState *target = it ? it : &g_windowState;
    WindowState win = cel_watch(target);

    if (!win.isOpen) {
        printf("  [WindowLifeCycle] Window close requested -> calling cel_destroy()\n");
        cel_destroy();
    }
}

/* ========================================================================= */
/* 5. Application Entry Point                                                */
/* ========================================================================= */

int main(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);

    // Attach the root composition with its lifecycle evaluator
    CEL_Attach(&session, CEL_Window, WindowLifeCycle);

    // Pass 1: Initial Mount
    // Builds the composition tree, allocates slots, and initializes native handles
    printf("=== Pass 1: Initial Mount ===\n");
    CelsSessionRecompose(&session);

    // State Query: Retrieve active state or native handles from the session by key
    SdlWindow *win = CEL_GetState(&session, CEL_KEY("CEL_Window"), SdlWindow);
    printf("  [CEL_GetState] Found native window handle: %p\n",
           win ? win->nativeHandle : NULL);

    // Quiet Check: No state changed, so recomposition skips the tree in O(1)
    printf("\n=== Quiet Check (Nothing Changed) ===\n");
    CelsSessionRecompose(&session);
    printf("  Quiet recompose completed instantly (0 work done).\n");

    // Event 1: Mutating reactive state triggers fine-grained recomposition
    printf("\n=== Event 1: Increment Counter (cel_mutate) ===\n");
    cel_mutate(&session, &g_appState) {
        this->counter++;
    }
    CelsSessionRecompose(&session);

    // Event 2: Further mutation re-evaluates watched components
    printf("\n=== Event 2: Increment Counter Again ===\n");
    cel_mutate(&session, &g_appState) {
        this->counter++;
    }
    CelsSessionRecompose(&session);

    // Event 3: Lifecycle mutation triggers composition despawn and cleanup
    printf("\n=== Event 3: Close Window ===\n");
    cel_mutate(&session, &g_windowState) {
        this->isOpen = false;
    }
    CelsSessionRecompose(&session);

    // Cleanup session and free internal arenas
    CelsSessionDestroy(&session);
    return 0;
}