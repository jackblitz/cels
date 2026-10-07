#include "cels.h"
#include "composition/window.h"

CEL_OnStart(WindowApp_OnStart)
{
    cel_attach(session, WindowComposition, WindowEval);
}

CEL_OnEnd(WindowApp_OnEnd)
{
    /* Application teardown resources if needed */
}

/**
 * Application Definition.
 * Attaches WindowComposition with WindowEval lifecycle evaluation.
 */
CEL_App_Def(WindowApp,
    .onStart = WindowApp_OnStart,
    .onReload = WindowApp_OnStart,
    .onEnd = WindowApp_OnEnd
);

