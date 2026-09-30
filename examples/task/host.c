#include "cels.h"
#include "cels/engine.h"
#include "network_state.h"
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

    static void RestoreTerminal(void) {
        if (s_termiosSet) {
            tcsetattr(STDIN_FILENO, TCSANOW, &s_origTermios);
            s_termiosSet = false;
        }
    }

    static void SetupTerminal(void) {
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

    static inline int PollKey(void) {
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

int main(int argc, char **argv)
{
    bool once = (argc > 1 && strcmp(argv[1], "--once") == 0);

    /*
     * 1. Initialize host engine and primary session with workload profile:
     *
     * Profile Selection: CELS_PROFILE_512
     * - Capacity: Up to 512 active composables
     * - Slab Size: 64 KiB contiguous 64-byte cache-aligned slab
     * - Use Case: L1/L2 cache-resident reactive state machines and asynchronous coroutines.
     *   Keeps task execution stacks, state cells, and resumption frames ultra-fast and cache-hot.
     */
    CelsEngine engine;
    if (CelsEngineInitWithProfile(&engine, NULL, CELS_PROFILE_512) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize engine\n");
        return 1;
    }

    printf("[Host] Task Example loaded (CELS_PROFILE_512: 64 KiB slab, 512 composables).\n");
    printf("[Host] Controls:\n");
    printf("[Host]   [C] Connect to server (start CEL_Task)\n");
    printf("[Host]   [X] Cancel connection (unmounts task, invokes cancel {} block)\n");
    printf("[Host]   [Q] Quit application\n\n");

    SetupTerminal();

    /* 2. Main loop */
    while (!engine.shouldQuit) {
        CelsAppRuntimeCheck(&engine);
        CelsEngineRecompose(&engine);

        if (once) break;

        int key = PollKey();
        if (key == 'c' || key == 'C') {
            printf("[Host] Key 'C' pressed: Starting network connect task...\n");
            cel_mutate(&engine.session, NetworkState) {
                this->isConnecting = true;
            }
        } else if (key == 'x' || key == 'X') {
            printf("[Host] Key 'X' pressed: Cancelling network task...\n");
            cel_mutate(&engine.session, NetworkState) {
                this->isConnecting = false;
            }
        } else if (key == 'q' || key == 'Q' || key == 27) {
            printf("[Host] Key 'Q' pressed: Exiting...\n");
            break;
        }

        SleepMs(16);
    }

    /* 3. Teardown */
    printf("[Host] Shutting down...\n");
    RestoreTerminal();
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    printf("[Host] Exited cleanly.\n");

    return 0;
}
