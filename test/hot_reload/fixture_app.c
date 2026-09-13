#include "cels.h"

CEL_Module(FixtureModule) {
    int sampleRate;
    int volume;
};

CEL_Composition(FixtureRoot, key) {
    FixtureModule *mod = CEL_GetModule(FixtureModule);
    (void)mod;
}

static CelsCompositionRef Fixture_OnStart(CelsEngine *engine,
                                          CelsSession *session)
{
    (void)session;
    static FixtureModule mod = {
        .sampleRate = 48000,
        .volume = 90
    };
    CEL_RegisterModule(engine, FixtureModule, &mod);
    return CEL_COMPOSITION(FixtureRoot);
}

static void Fixture_OnEnd(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
}

CEL_App(FixtureApp,
    .onStart = Fixture_OnStart,
    .onEnd = Fixture_OnEnd
);
