#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* --- Reactive State --- */
CEL_State(AppState) {
    bool isWindowOpen;
    int  clickCount;
};

static AppState g_appState = {
    .isWindowOpen = true,
    .clickCount = 0
};
/* --- Observer Resource --- */

CEL_Observer(SdlWindow) {
    void *dummyHandle;
};

void SdlWindow_OnRemembered(SdlWindow *self, CelsSession *s) {
    (void)s;
    self->dummyHandle = (void*)0x12345678;
    printf("  [Lifecycle] SdlWindow opened (%p)\n", self->dummyHandle);
}

void SdlWindow_OnForgotten(SdlWindow *self, CelsSession *s) {
    (void)s;
    printf("  [Lifecycle] SdlWindow closed (%p)\n", self->dummyHandle);
    self->dummyHandle = NULL;
}

CEL_BIND_OBSERVER(SdlWindow);

static const CelsObserverDesc Type##_Desc = { \
        .size = sizeof(Type), \
        .onRemembered = (void(*)(void*, CelsSession*))Type##_OnRemembered, \
        .onForgotten  = (void(*)(void*, CelsSession*))Type##_OnForgotten \
    }

/* --- Declarative Root Function --- */

void RootApp(CelsSession *s) {
    // Watch reactive state: this registers RootApp to g_appState
    AppState state = cel_watch(s, &g_appState);

    CEL_Composition(s, CEL_KEY("RootHost")) {
        if (state.isWindowOpen) {
            CEL_Composable(s, CEL_KEY("MainWindowNode")) {
                cel_remember_observer(s, SdlWindow);

                CEL_Composable(s, CEL_KEY("CounterText")) {
                    printf("    -> Window is open! Click count: %d\n", state.clickCount);
                } CEL_Close(s);

            } CEL_Close(s);
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

    printf("\n=== Quiet Check (Nothing Changed) ===\n");
    // Exits in O(1) immediately: queue is empty, nothing prints
    CelsSessionRecompose(&session);
    printf("Quiet recompose completed instantly (0 work done).\n");

    printf("\n=== Event: User clicks button (cel_update) ===\n");

    cel_mutate(&session, &g_appState) {
        this->clickCount = 1;
    }

    printf("=== Pass 2: Recompose triggered by state mutation ===\n");
    CelsSessionRecompose(&session);

    printf("\n=== Event: User closes window (cel_update) ===\n");
    cel_mutate(&session, &g_appState) {
        this->isWindowOpen = true;
    }

    printf("=== Pass 3: Recompose triggers pruning & onForgotten ===\n");
    CelsSessionRecompose(&session);

    CelsSessionDestroy(&session);
    return 0;
}