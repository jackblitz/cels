#include "cels.h"
#include "composition/window.h"
#include <stdio.h>

/* ========================================================================= */
/* Application Lifecycle Hooks                                               */
/* ========================================================================= */

static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    printf("  [App Lifecycle] OnStart: Application mounted.\n");
    return Window_GetComposition();
}

static void App_OnEnd(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    printf("  [App Lifecycle] OnEnd: Application teardown complete.\n");
}

/* ========================================================================= */
/* Application Manifest                                                      */
/* ========================================================================= */

CEL_App(WindowApp,
    .onStart = App_OnStart,
    .onEnd = App_OnEnd
);
