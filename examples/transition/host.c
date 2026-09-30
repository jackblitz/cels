#include "cels.h"
#include "cels/engine.h"
#include "transition_state.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <stdlib.h>
    #define SleepMs(ms) Sleep(ms)

    static DWORD s_origConsoleMode = 0;
    static bool s_consoleModeSet = false;

    static void RestoreTerminal(void) {
        if (s_consoleModeSet) {
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            if (hIn != INVALID_HANDLE_VALUE) {
                SetConsoleMode(hIn, s_origConsoleMode);
            }
            s_consoleModeSet = false;
        }
    }

    static void SetupTerminal(void) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE) {
            if (GetConsoleMode(hIn, &s_origConsoleMode)) {
                DWORD rawMode = s_origConsoleMode;
                rawMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
                SetConsoleMode(hIn, rawMode);
                s_consoleModeSet = true;
                atexit(RestoreTerminal);
            }
        }
    }

    static inline int PollKey(void) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE) {
            DWORD numEvents = 0;
            if (GetNumberOfConsoleInputEvents(hIn, &numEvents) && numEvents > 0) {
                INPUT_RECORD rec;
                DWORD numRead = 0;
                while (PeekConsoleInput(hIn, &rec, 1, &numRead) && numRead > 0) {
                    ReadConsoleInput(hIn, &rec, 1, &numRead);
                    if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                        char ch = rec.Event.KeyEvent.uChar.AsciiChar;
                        if (ch != 0) return (int)(unsigned char)ch;
                    }
                }
            }
            /* Fallback for redirected pipes / automated tests */
            DWORD bytesAvail = 0;
            if (PeekNamedPipe(hIn, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
                char ch = 0;
                DWORD bytesRead = 0;
                if (ReadFile(hIn, &ch, 1, &bytesRead, NULL) && bytesRead > 0) {
                    return (int)(unsigned char)ch;
                }
            }
        }
        return 0;
    }
#else
    #include <unistd.h>
    #include <termios.h>
    #include <sys/select.h>
    #include <stdlib.h>
    #define SleepMs(ms) usleep((ms) * 1000)

    static struct termios s_origTermios;
    static bool s_termiosSet = false;

    static void RestoreTerminal(void)
    {
        if (s_termiosSet) {
            tcsetattr(STDIN_FILENO, TCSANOW, &s_origTermios);
            s_termiosSet = false;
        }
    }

    static void SetupTerminal(void)
    {
        if (isatty(STDIN_FILENO)) {
            if (tcgetattr(STDIN_FILENO, &s_origTermios) == 0) {
                struct termios raw = s_origTermios;
                raw.c_lflag &= ~(ICANON | ECHO);
                tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                s_termiosSet = true;
                atexit(RestoreTerminal);
            }
        }
    }

    static inline int PollKey(void)
    {
        struct timeval tv = { 0L, 0L };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        if (select(1, &fds, NULL, NULL, &tv) > 0) {
            char ch = 0;
            if (read(0, &ch, 1) > 0) return (int)(unsigned char)ch;
        }
        return 0;
    }
#endif

int main(int argc, char **argv) {
    bool once = (argc > 1 && strcmp(argv[1], "--once") == 0);

    /*
     * 1. Initialize host engine and primary session with workload profile:
     *
     * Profile Selection: CELS_PROFILE_512
     * - Capacity: Up to 512 active composables
     * - Slab Size: 64 KiB contiguous 64-byte cache-aligned slab
     * - Use Case: L1/L2 cache-resident UI panels and temporal motion convergence.
     *   Ensures that per-frame interpolation (cel_transition), state double-buffering,
     *   and continuous easing calculations run with zero cache evictions at 60+ FPS.
     */
    CelsEngine engine;
    if (CelsEngineInitWithProfile(&engine, NULL, CELS_PROFILE_512) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize CelsEngine.\n");
        return 1;
    }

    printf("[Host] Transition Example loaded (CELS_PROFILE_512: 64 KiB slab, 512 composables).\n");
    printf("[Host] Controls:\n");
    printf("[Host]   [D] Take 25 Damage (Authoritative discrete step)\n");
    printf("[Host]   [H] Heal 25 Health (Authoritative discrete step)\n");
    printf("[Host]   [Q] Quit application\n\n");

    SetupTerminal();

    /* 2. Main loop */
    while (!engine.shouldQuit) {
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine);

        if (once) break;

        int key = PollKey();
        if (key == 'q' || key == 'Q' || key == 27) {
            printf("\n[Host] Key 'Q' pressed: Exiting...\n");
            break;
        } else if (key == 'd' || key == 'D') {
            cel_mutate(&engine.session, PlayerGaugeState) {
                if (this->currentHealth > 0.0f) {
                    this->currentHealth -= 25.0f;
                    if (this->currentHealth < 0.0f) this->currentHealth = 0.0f;
                }
            }
        } else if (key == 'h' || key == 'H') {
            cel_mutate(&engine.session, PlayerGaugeState) {
                if (this->currentHealth < this->maxHealth) {
                    this->currentHealth += 25.0f;
                    if (this->currentHealth > this->maxHealth) this->currentHealth = this->maxHealth;
                }
            }
        }

        SleepMs(16);
    }

    /* 3. Teardown */
    printf("\n[Host] Shutting down...\n");
    RestoreTerminal();
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    printf("[Host] Exited cleanly.\n");
    return 0;
}

