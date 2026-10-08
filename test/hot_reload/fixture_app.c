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

static void Fixture_OnStart(CelsEngine *engine)
{
    static FixtureModule mod = {
        .sampleRate = 48000,
        .volume = 90
    };
    cel_engine_set_context(engine, FixtureModule, &mod);
    CelsSession *session = cel_engine_create_session(engine, NULL, CELS_PROFILE_1K);
    cel_attach(session, FixtureRoot);
}

static void Fixture_OnEnd(CelsEngine *engine)
{
    (void)engine;
}

CEL_App_Manifest(FixtureApp,
    .title = "FixtureApp",
    .onStart = Fixture_OnStart,
    .onReload = Fixture_OnStart,
    .onEnd = Fixture_OnEnd
);
