#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* --- Reactive Window State --- */
CEL_State(WindowState) {
    bool isOpen;
    int  width;
    int  height;
};

static WindowState g_mainWindow = {
    .isOpen = true,
    .width  = 800,
    .height = 600
};

/* --- Observer Resource (Native Lifecycle) --- */

CEL_State(SdlWindow) {
    void *nativeHandle;
};

static void SdlWindow_OnCreated(SdlWindow *self, CelsSession *s) {
    (void)s;
    self->nativeHandle = (void*)0x12345678;
    printf("  [Lifecycle] SdlWindow opened (%p) [%dx%d]\n", 
           self->nativeHandle, g_mainWindow.width, g_mainWindow.height);
}

static void SdlWindow_OnDestroyed(SdlWindow *self, CelsSession *s) {
    (void)s;
    printf("  [Lifecycle] SdlWindow closed (%p)\n", self->nativeHandle);
    self->nativeHandle = NULL;
}

/* --- UI Event Wiring (Self-Contained Component Memory) --- */

// In a real UI system (SDL/GLFW), components register event callbacks with the event loop.
// The component attaches its own private remembered pointer as userData.
typedef struct ButtonComponent {
    void (*onClick)(void *userData, CelsSession *s);
    void *userData;
} ButtonComponent;

static ButtonComponent g_incrementButton;

static void OnIncrementClick(void *userData, CelsSession *s) {
    int *clickCount = (int*)userData;
    cel_mutate(s, clickCount) {
        (*this)++;
    }
}

/* --- Reusable Composable Component Definitions --- */

// 1. Container Composable: manages SDL window lifecycle and hosts child components
CEL_Composeable(CEL_SDLWindow, key) {
    cel_lifecycle_state(s, SdlWindow, SdlWindow_OnCreated, SdlWindow_OnDestroyed);
}

// 2. Leaf Composable: manages its own local remembered state across recompositions
CEL_Composeable(CEL_CounterText, key) {
    // Persistent Local Memory:
    // cel_remember allocates private, self-contained slots for this component.
    // Multiple variables are remembered sequentially and persist across recompositions:
    int  *clickCount = cel_remember(int, 0);
    bool *isHovered  = cel_remember(bool, false);

    int  count   = cel_watch(clickCount);
    bool hovered = cel_watch(isHovered);

    printf("    -> Window is open! Click count: %d (hovered: %s)\n", 
           count, hovered ? "true" : "false");

    // The component wires its own private remembered pointer into its click callback:
    g_incrementButton = (ButtonComponent){
        .onClick = OnIncrementClick,
        .userData = clickCount
    };
}

/* --- Declarative Root Function --- */

void RootApp(CelsSession *s) {
    CEL_Composition(s, CEL_KEY("RootHost")) {
        WindowState win = cel_watch(&g_mainWindow);
        if (win.isOpen)
        {
            // Container composable with children nested inside
            CEL_Composable(CEL_SDLWindow, CEL_KEY("MainWindow")) {
                CEL_CounterText(CEL_KEY("CounterText"));
            }
        }
    } CEL_Close(s);
}

/* --- Main --- */

int main(void) {
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = RootApp
    });

    printf("=== Pass 1: Initial Mount ===\n");
    CelsSessionRecompose(&session);

    // Lifecycle state (native handles/resources) can be queried externally by key:
    SdlWindow *winObs = CEL_FindLifecycleState(&session, CEL_KEY("MainWindow"), SdlWindow);
    printf("  [Observer Query] Found native window handle: %p\n", winObs ? winObs->nativeHandle : NULL);

    printf("\n=== Quiet Check (Nothing Changed) ===\n");
    // Exits in O(1) immediately: queue is empty, nothing prints
    CelsSessionRecompose(&session);
    printf("Quiet recompose completed instantly (0 work done).\n");

    printf("\n=== Event: User clicks button (dispatched via component callback) ===\n");

    // Event loop dispatches to the button's registered callback with its private userData:
    if (g_incrementButton.onClick) {
        g_incrementButton.onClick(g_incrementButton.userData, &session);
    }

    printf("=== Pass 2: Recompose triggered by local state mutation ===\n");
    CelsSessionRecompose(&session);

    printf("\n=== Event: User closes window (cel_mutate on WindowState) ===\n");
    cel_mutate(&session, &g_mainWindow) {
        this->isOpen = false;
    }

    printf("=== Pass 3: Recompose triggers pruning & onForgotten ===\n");
    CelsSessionRecompose(&session);

    CelsSessionDestroy(&session);
    return 0;
}