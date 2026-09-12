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

// 1. Leaf Composable: called from inside another Composable (nested composable)
CEL_Composeable(CEL_Badge, key) {
    (void)key;
    printf("    [Badge Composable] Notification badge rendered (called from CEL_CounterText)!\n");
}

// 2. Mid-level Composable: called from inside a Composition, and calls CEL_Badge inside itself
CEL_Composeable(CEL_CounterText, key) {
    // Persistent Local Memory:
    // cel_remember allocates private, self-contained slots for this component.
    int  *clickCount = cel_remember(int, 0);
    bool *isHovered  = cel_remember(bool, false);

    int  count   = cel_watch(clickCount);
    bool hovered = cel_watch(isHovered);

    printf("    -> Window is open! Click count: %d (hovered: %s)\n", 
           count, hovered ? "true" : "false");

    // Calling a Composable from inside another Composable:
    if (count > 0) {
        CEL_Badge(CEL_KEY("BadgeNotification"));
    }

    // The component wires its own private remembered pointer into its click callback:
    g_incrementButton = (ButtonComponent){
        .onClick = OnIncrementClick,
        .userData = clickCount
    };
}

// 2. Root Window Composition (defined OUTSIDE RootApp): hosts SDL window lifecycle & children
CEL_Composition(CEL_Window, key) {
    cel_lifecycle_state(s, SdlWindow, SdlWindow_OnCreated, SdlWindow_OnDestroyed);
    CEL_CounterText(CEL_KEY("CounterText"));
}

/* --- Top-Level Composition Lifecycle --- */

CEL_LifeCycle(WindowLifeCycle, WindowState) {
    // cel_watch subscribes this Composition root to changes in WindowState
    WindowState *target = it ? it : &g_mainWindow;
    WindowState win = cel_watch(target);
    printf("  [WindowLifeCycle] Evaluating Window -> isOpen: %s [%dx%d]\n",
           win.isOpen ? "true" : "false", win.width, win.height);

    if (!win.isOpen) {
        printf("  [WindowLifeCycle] Close condition met -> triggering cel_destroy() to despawn Composition\n");
        cel_destroy();
    }
}

/* --- Declarative Root Function --- */

void RootApp(CelsSession *s) {
    (void)s;
    // CEL_Attach just adds/spawns the composition with its lifecycle!
    CEL_Attach(CEL_Window, WindowLifeCycle);
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
    SdlWindow *winObs = CEL_FindLifecycleState(&session, CEL_KEY("CEL_Window"), SdlWindow);
    printf("  [Observer Query] Found native window handle: %p\n", winObs ? winObs->nativeHandle : NULL);

    printf("\n=== Quiet Check (Nothing Changed) ===\n");
    // Exits in O(1) immediately: queue is empty, nothing prints
    CelsSessionRecompose(&session);
    printf("Quiet recompose completed instantly (0 work done).\n");

    printf("\n=== Event 1: User clicks button (0 -> 1) ===\n");
    if (g_incrementButton.onClick) {
        g_incrementButton.onClick(g_incrementButton.userData, &session);
    }

    printf("=== Pass 2: Recompose triggers dynamic spawning of BadgeNotification ===\n");
    CelsSessionRecompose(&session);

    printf("\n=== Event 2: User clicks button again (1 -> 2) ===\n");
    if (g_incrementButton.onClick) {
        g_incrementButton.onClick(g_incrementButton.userData, &session);
    }

    printf("=== Pass 3: Recompose existing badge (cel_spawn does NOT re-run) ===\n");
    CelsSessionRecompose(&session);

    printf("\n=== Event 3: User closes window (cel_mutate on WindowState) ===\n");
    cel_mutate(&session, &g_mainWindow) {
        this->isOpen = false;
    }

    printf("=== Pass 4: Recompose triggers CEL_LifeCycle -> cel_destroy() -> Despawn ===\n");
    CelsSessionRecompose(&session);

    CelsSessionDestroy(&session);
    return 0;
}