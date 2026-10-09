#include "cels.h"

CEL_Module(FixtureModule) {
    int sampleRate;
    int volume;
};

CEL_Composition(FixtureRoot, void *userData) {
    (void)userData;
    FixtureModule *mod = CEL_GetModule(FixtureModule);
    (void)mod;
}

static void Fixture_OnStart(CelsSession *session)
{
    static FixtureModule mod = {
        .sampleRate = 48000,
        .volume = 90
    };
    CEL_RegisterModule(session, FixtureModule, &mod);
    cel_attach(session, FixtureRoot);
}

static void Fixture_OnEnd(CelsSession *session)
{
    (void)session;
}

CEL_App_Manifest(FixtureApp,
    .title = "FixtureApp",
    .onStart = Fixture_OnStart,
    .onReload = Fixture_OnStart,
    .onEnd = Fixture_OnEnd
);
