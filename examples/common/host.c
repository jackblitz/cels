#include "host.h"
#include <stdio.h>
#include <string.h>

/* Forward declare event demo payloads for automated verification in --once mode */
typedef struct UserActionEvent {
    int  actionId;
    int  cost;
    char actionName[32];
} UserActionEvent;

typedef struct CombatSignal {
    int   targetId;
    float damage;
    bool  isCritical;
} CombatSignal;

typedef struct AudioBroadcast {
    char  soundEffect[32];
    float volume;
} AudioBroadcast;

typedef struct StartWorkflowSignal {
    bool active;
} StartWorkflowSignal;

int CelsRunHostEx(int argc, char **argv, const char *appName)
{
    bool once = (argc > 1 && strcmp(argv[1], "--once") == 0);

    CelsTerminalInit(!once);

    CelsEngine engine;
    if (CelsEngineInitWithProfile(&engine, appName, CELS_PROFILE_1K) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize CelsEngine.\n");
        CelsTerminalRestore();
        return 1;
    }

    CelsSession *session = cel_get_session(&engine, "main");

    if (once) {
        /* Run initial load and recomposition frame */
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine);

        /* If running the event example, run its automated multi-scope messaging test */
        if (appName != NULL && strcmp(appName, "cel_event_dll") == 0) {
            /* 1. Local Tree Event */
            cel_event(UserActionEvent, { .actionId = 10, .cost = 25, .actionName = "HealSpell" });
            CelsEngineRecompose(&engine);

            /* 2. Targeted Signal */
            cel_signal(session, CombatSignal, { .targetId = 99, .damage = 72.5f, .isCritical = true });
            CelsEngineRecompose(&engine);

            /* 3. Global Engine Broadcast */
            cel_broadcast(AudioBroadcast, { .soundEffect = "victory_fanfare.wav", .volume = 1.0f });
            CelsEngineRecompose(&engine);

            /* 4. Asynchronous Task Workflow */
            cel_signal(session, StartWorkflowSignal, { .active = true });
            CelsEngineRecompose(&engine);
            cel_event(UserActionEvent, { .actionId = 2, .cost = 5, .actionName = "Slash" });
            CelsEngineRecompose(&engine);
            cel_signal(session, CombatSignal, { .targetId = 1, .damage = 15.0f, .isCritical = false });
            CelsEngineRecompose(&engine);
            cel_broadcast(AudioBroadcast, { .soundEffect = "hit.wav", .volume = 0.5f });
            CelsEngineRecompose(&engine);
            CelsTerminalSleepMs(60);
            CelsEngineRecompose(&engine);
        }

        CelsEngineEnd(&engine);
        CelsEngineDestroy(&engine);
        CelsTerminalRestore();
        return 0;
    }

    /* Interactive Tick Loop */
    while (!engine.shouldQuit) {
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine);

        int key = CelsTerminalPollKey();
        if (key == 'q' || key == 'Q' || key == 27) {
            engine.shouldQuit = true;
            break;
        } else if (key > 0) {
            /* Dispatch discrete key signal to session */
            cel_signal(session, CelsKeySignal, { .key = key });
        }

        CelsTerminalSleepMs(16);
    }

    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    CelsTerminalRestore();
    return 0;
}

int CelsRunHost(int argc, char **argv)
{
    return CelsRunHostEx(argc, argv, NULL);
}

#ifndef CELS_NO_MAIN
int main(int argc, char **argv)
{
    return CelsRunHost(argc, argv);
}
#endif
