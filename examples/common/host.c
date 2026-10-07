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
        } else if (appName != NULL && strcmp(appName, "cel_input_dll") == 0) {
            /* Automated verification of Gameplay vs UI Input Mapping and CEL_Layout Focus: */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. In Gameplay: Send 'W' to move player character */
            cel_mutate(input) {
                this->rawKey = 'W';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = 'W' });
            CelsEngineRecompose(&engine);

            /* 2. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 3. Send [Tab] to toggle into UI Menu mode */
            cel_mutate(input) {
                this->rawKey = '\t';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = '\t' });
            CelsEngineRecompose(&engine);

            /* 4. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 5. In UI Menu: Item 0 (Volume) has focus. Send 'D' to increase volume */
            cel_mutate(input) {
                this->rawKey = 'D';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = 'D' });
            CelsEngineRecompose(&engine);

            /* 6. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 7. Send 'S' to navigate focus down to Item 1 (Audio Mute) */
            cel_mutate(input) {
                this->rawKey = 'S';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = 'S' });
            CelsEngineRecompose(&engine);

            /* 8. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 9. Send [Space] to activate focused item (toggle mute) */
            cel_mutate(input) {
                this->rawKey = ' ';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = ' ' });
            CelsEngineRecompose(&engine);

            /* 10. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 11. Send 'S' to navigate focus down to Item 2 (Graphics Preset) */
            cel_mutate(input) {
                this->rawKey = 'S';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = 'S' });
            CelsEngineRecompose(&engine);

            /* 12. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 13. Send 'D' to cycle graphics preset (HIGH -> ULTRA) */
            cel_mutate(input) {
                this->rawKey = 'D';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = 'D' });
            CelsEngineRecompose(&engine);

            /* 14. Clear key */
            cel_mutate(input) {
                this->rawKey = 0;
                this->handled = false;
            }
            CelsEngineRecompose(&engine);

            /* 15. Send [Space] to cycle graphics preset forward (ULTRA -> LOW) */
            cel_mutate(input) {
                this->rawKey = ' ';
                this->handled = false;
                this->frameId++;
            }
            cel_signal(session, CelsKeySignal, { .key = ' ' });
            CelsEngineRecompose(&engine);
        } else if (appName != NULL && strcmp(appName, "cel_component_dll") == 0) {
            /* Automated verification of Declarative ECS Components & Reconciliation */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. Toggle Burn Debuff ('b') */
            cel_mutate(input) { this->rawKey = 'b'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsEngineRecompose(&engine);

            /* 2. Toggle Energy Shield ('s') */
            cel_mutate(input) { this->rawKey = 's'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsEngineRecompose(&engine);

            /* 3. Toggle Boss Tag ('t') */
            cel_mutate(input) { this->rawKey = 't'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsEngineRecompose(&engine);
        } else if (appName != NULL && strcmp(appName, "cel_theming_dll") == 0) {
            /* Automated verification of Ambient Theming and Modal Overrides */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. Cycle Theme to Dark Mode ('t') */
            cel_mutate(input) { this->rawKey = 't'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsEngineRecompose(&engine);

            /* 2. Toggle High-Contrast Alert ('a') */
            cel_mutate(input) { this->rawKey = 'a'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsEngineRecompose(&engine);
        } else if (appName != NULL && strcmp(appName, "cel_transition_dll") == 0) {
            /* Automated verification of Temporal Motion & Easing */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. Trigger Damage ('d') */
            cel_mutate(input) { this->rawKey = 'd'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsTerminalSleepMs(50);
            CelsEngineRecompose(&engine);
        } else if (appName != NULL && strcmp(appName, "cel_task_dll") == 0) {
            /* Automated verification of Asynchronous Multi-Step Task */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. Trigger Connect ('c') */
            cel_mutate(input) { this->rawKey = 'c'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
            CelsTerminalSleepMs(220);
            CelsEngineRecompose(&engine);
        } else if (appName != NULL && strcmp(appName, "cel_dll") == 0) {
            /* Automated verification of Window Component & Status Badge */
            CelsSetCurrentSession(session);
            CelsInputState *input = cel_remember_state(CelsInputState, {0});

            /* 1. Toggle Badge ('b') */
            cel_mutate(input) { this->rawKey = 'b'; this->handled = false; this->frameId++; }
            CelsEngineRecompose(&engine);
            cel_mutate(input) { this->rawKey = 0; }
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

        int key = CelsTerminalPollKey();
        if (key == 'q' || key == 'Q' || key == 27) {
            engine.shouldQuit = true;
            break;
        }

        /* Update reactive input state in the session */
        CelsSetCurrentSession(session);
        CelsInputState *input = cel_remember_state(CelsInputState, {0});
        if (input != NULL) {
            if (key > 0) {
                cel_mutate(input) {
                    this->rawKey = key;
                    this->handled = false;
                    this->frameId++;
                }
                /* Also dispatch discrete key signal for backward compatibility */
                cel_signal(session, CelsKeySignal, { .key = key });
            } else if (input->rawKey != 0) {
                /* Clear key on subsequent frame (1-frame pulse) */
                cel_mutate(input) {
                    this->rawKey = 0;
                    this->handled = false;
                }
            }
        }

        CelsEngineRecompose(&engine);
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
