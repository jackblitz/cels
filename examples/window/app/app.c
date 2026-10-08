#include "cels.h"
#include "composition/window.h"

static void WindowApp_OnStart(CelsEngine *engine)
{
    CelsSession *session = cel_engine_create_session(engine, "main", CELS_PROFILE_1K);
    cel_attach(session, WindowComposition, WindowEval);
}

static void WindowApp_OnEnd(CelsEngine *engine)
{
    (void)engine;
    /* Application teardown resources if needed */
}

/**
 * Application Manifest.
 * Attaches WindowComposition with WindowEval lifecycle evaluation.
 */
CEL_App_Manifest(WindowApp,
    .title      = "CELS Window & Layout Demo",
    .appVersion = "0.5.0",
    .onStart    = WindowApp_OnStart,
    .onReload   = WindowApp_OnStart,
    .onEnd      = WindowApp_OnEnd
);
