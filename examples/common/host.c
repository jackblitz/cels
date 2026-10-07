#include "host.h"
#include <stdio.h>
#include <string.h>

/**
 * @file host.c
 * @brief Generic, application-agnostic executable host runner for CELS applications.
 *
 * Implements the application lifecycle, TUI terminal abstraction, scripted input
 * playback, and interactive tick loop via PdcursesInputContext. Contains zero
 * hardcoded application or test logic—all headless testing is encapsulated in test/cli.
 */

int CelsRunHostEx(int argc, char **argv, const char *appName)
{
    bool once = false;
    const char *playbackKeys = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0) {
            once = true;
        } else if ((strcmp(argv[i], "--keys") == 0 || strcmp(argv[i], "--playback") == 0) && i + 1 < argc) {
            playbackKeys = argv[++i];
        }
    }

    /* Headless smoke test or scripted playback runs non-interactively */
    const bool interactive = (!once && playbackKeys == NULL);
    CelsTerminalInit(interactive);

    CelsEngine engine;
    if (CelsEngineInitWithProfile(&engine, appName, CELS_PROFILE_1K) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize CelsEngine.\n");
        CelsTerminalRestore();
        return 1;
    }

    CelsSession *session = cel_get_session(&engine, "main");
    PdcursesInputContext inputCtx;
    PdcursesInputInit(&inputCtx);

    if (playbackKeys != NULL) {
        /* Scripted input playback mode for integration verification */
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine); /* Frame 0 initial mount pass */

        for (const char *p = playbackKeys; *p != '\0'; ++p) {
            char ch = *p;
            if (ch == ',') continue; /* Allow comma-separated key sequences */

            /* 1. Staged Key Pulse: set & publish to CELS reactive state */
            PdcursesInputSetKey(&inputCtx, (int)(unsigned char)ch);
            PdcursesInputPublishKey(&inputCtx, session);
            CelsEngineRecompose(&engine);

            /* 2. Pulse Decay: clear key back to 0 resting state */
            PdcursesInputPublishKey(&inputCtx, session);
            CelsEngineRecompose(&engine);
        }
    } else if (once) {
        /* Single-frame smoke test mode: compile, link, mount, recompose, and exit */
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine);
    } else {
        /* Interactive Tick Loop */
        while (!engine.shouldQuit) {
            CelsAppRuntimeCheck(&engine);

            int key = PdcursesInputReadKey(&inputCtx);
            if (key == 'q' || key == 'Q' || key == 27) {
                engine.shouldQuit = true;
                break;
            }

            PdcursesInputPublishKey(&inputCtx, session);
            CelsEngineRecompose(&engine);
            CelsTerminalSleepMs(16);
        }
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
