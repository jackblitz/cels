#include "cels.h"
#include "window.h"
#include <stdio.h>

/* ========================================================================= */
/* Application Lifecycle Hooks (Defined in Application / DLL)               */
/* ========================================================================= */

/**
 * Called when the application starts.
 *
 * Configures system/engine memory modules into the host engine and attaches
 * the root composition by returning its CelsCompositionRef handle.
 *
 * @param engine  Target host engine (.exe). Non-NULL.
 * @param session Target session. Non-NULL.
 * @return Composition reference to mount as the root tree.
 */
static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session)
{
    (void)session;
    printf("  [App Lifecycle] OnStart: Initializing system/engine memory in host .exe...\n");

    /* 1. Register system / engine memory module into the host engine (.exe) */
    static PlatformModule platform = {
        .backendName = "SDL / Vulkan",
        .refreshRateHz = 144,
        .dpiScale = 1.25f
    };
    CEL_RegisterModule(engine, PlatformModule, &platform);

    /* 2. Attach root composition by returning it! */
    return Window_GetComposition();
}

/**
 * Called on application shutdown to clean up any application-level resources.
 *
 * @param engine  Target host engine (.exe). Non-NULL.
 * @param session Target session. Non-NULL.
 */
static void App_OnEnd(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    printf("  [App Lifecycle] OnEnd: Application teardown complete.\n");
}

/* ========================================================================= */
/* Declarative Application Registration (App Module)                         */
/* ========================================================================= */

CEL_App(WindowApp,
    .onStart = App_OnStart,
    .onEnd = App_OnEnd,
    .onPrintTree = Window_PrintTree
);
