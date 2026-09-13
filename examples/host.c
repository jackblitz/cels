#include "cels.h"
#include "cels/app.h"
#include "cels/engine.h"
#include "cels/module.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CELS_HOT_RELOAD
    #if defined(NDEBUG)
        #define CELS_HOT_RELOAD 0
    #else
        #define CELS_HOT_RELOAD 1
    #endif
#endif

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <conio.h>
#else
    #include <unistd.h>
    #include <dirent.h>
    #include <signal.h>
#endif

/* ========================================================================= */
/* Path Resolution & File Existence Helpers (Debug Hot-Reload Mode)          */
/* ========================================================================= */

#if CELS_HOT_RELOAD
#ifndef CELS_APP_TARGET
    #define CELS_APP_TARGET "cel_app"
#endif

static bool FileReadable(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    return false;
}

static void GetExecutableDirectory(char *outDir, size_t maxLen)
{
#if defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, outDir, (DWORD)maxLen);
    if (len > 0 && len < maxLen) {
        char *lastSlash = strrchr(outDir, '\\');
        char *lastFwd = strrchr(outDir, '/');
        if (lastFwd && (!lastSlash || lastFwd > lastSlash)) {
            lastSlash = lastFwd;
        }
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            return;
        }
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", outDir, maxLen - 1);
    if (len > 0) {
        outDir[len] = '\0';
        char *lastSlash = strrchr(outDir, '/');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            return;
        }
    }
#endif
    outDir[0] = '\0';
}

static void ResolveApplicationDll(char *outPath, size_t maxLen, int argc,
                                  char **argv)
{
    /* 1. Explicit command-line override */
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            strncpy(outPath, argv[i], maxLen - 1);
            outPath[maxLen - 1] = '\0';
            return;
        }
    }

    /* 2. Co-located DLL: Always next to the running .exe */
    char exeDir[CELS_PATH_MAX] = {0};
    GetExecutableDirectory(exeDir, sizeof(exeDir));

    snprintf(outPath, maxLen, "%.480scel_app.dll", exeDir);
    if (FileReadable(outPath)) {
        return;
    }

    /* Fallback: cels_app.dll */
    snprintf(outPath, maxLen, "%.480scels_app.dll", exeDir);
    if (FileReadable(outPath)) {
        return;
    }

    /* Fallback: hot_module.dll in the exact same directory */
    char hotModule[CELS_PATH_MAX];
    snprintf(hotModule, sizeof(hotModule), "%.480shot_module.dll", exeDir);
    if (FileReadable(hotModule)) {
        strncpy(outPath, hotModule, maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return;
    }

    /* Default to cel_app.dll in the executable directory */
    snprintf(outPath, maxLen, "%.480scel_app.dll", exeDir);
}

static void RebuildApplicationDll(void)
{
    printf("\n======================================================================\n");
    printf("  [CelsEngine] Executing Rebuild Script (scripts/build_app)...\n");
    printf("======================================================================\n");

    int res = -1;
#if defined(_WIN32)
    char exeDir[CELS_PATH_MAX] = {0};
    GetExecutableDirectory(exeDir, sizeof(exeDir));
    char batPath[CELS_PATH_MAX];
    snprintf(batPath, sizeof(batPath), "%.450s..\\..\\..\\scripts\\build_app.bat", exeDir);

    if (FileReadable("scripts\\build_app.bat")) {
        res = system("\"scripts\\build_app.bat\"");
    } else if (FileReadable(batPath)) {
        char cmd[CELS_PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "\"\"%.450s\"\"", batPath);
        res = system(cmd);
    } else {
#if defined(CELS_CMAKE_COMMAND) && defined(CELS_BINARY_DIR)
        char cmd[CELS_PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof(cmd), "\"\"%.450s\" --build \"%.450s\" --target %s\"",
                 CELS_CMAKE_COMMAND, CELS_BINARY_DIR, CELS_APP_TARGET);
        res = system(cmd);
#else
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "cmake --build cmake-build-debug --target %s", CELS_APP_TARGET);
        res = system(cmd);
#endif
    }
#else
    char exeDir[CELS_PATH_MAX] = {0};
    GetExecutableDirectory(exeDir, sizeof(exeDir));
    char shPath[CELS_PATH_MAX];
    snprintf(shPath, sizeof(shPath), "%.450s../../../scripts/build_app.sh", exeDir);

    if (FileReadable("scripts/build_app.sh")) {
        res = system("/bin/sh scripts/build_app.sh");
    } else if (FileReadable(shPath)) {
        char cmd[CELS_PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "/bin/sh \"%.450s\"", shPath);
        res = system(cmd);
    } else {
#if defined(CELS_CMAKE_COMMAND) && defined(CELS_BINARY_DIR)
        char cmd[CELS_PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof(cmd), "\"%.450s\" --build \"%.450s\" --target %s",
                 CELS_CMAKE_COMMAND, CELS_BINARY_DIR, CELS_APP_TARGET);
        res = system(cmd);
#else
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "cmake --build cmake-build-debug --target %s", CELS_APP_TARGET);
        res = system(cmd);
#endif
    }
#endif

    if (res != 0) {
        printf("[CelsEngine] Build script returned error %d. (Check compiler output above)\n", res);
    } else {
        printf("[CelsEngine] Rebuild script finished successfully!\n");
    }
}

static void DeleteMatchingFiles(const char *dirPath, const char *pattern)
{
#if defined(_WIN32)
    char searchPath[CELS_PATH_MAX];
    snprintf(searchPath, sizeof(searchPath), "%.250s%.250s", dirPath, pattern);

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char filePath[CELS_PATH_MAX];
                snprintf(filePath, sizeof(filePath), "%.250s%.250s", dirPath, findData.cFileName);
                for (int retry = 0; retry < 5; ++retry) {
                    if (DeleteFileA(filePath) != 0) {
                        break;
                    }
                    Sleep(20);
                }
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
#else
    (void)pattern;
    DIR *d = opendir(dirPath);
    if (d != NULL) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_type == DT_REG) {
                if (strstr(entry->d_name, ".so") != NULL || strstr(entry->d_name, ".dylib") != NULL) {
                    char filePath[CELS_PATH_MAX];
                    snprintf(filePath, sizeof(filePath), "%.250s%.250s", dirPath, entry->d_name);
                    unlink(filePath);
                }
            }
        }
        closedir(d);
    }
#endif
}

static void CleanupHostDlls(void)
{
    char exeDir[CELS_PATH_MAX] = {0};
    GetExecutableDirectory(exeDir, sizeof(exeDir));
    if (exeDir[0] == '\0') {
        return;
    }

#if defined(_WIN32)
    DeleteMatchingFiles(exeDir, "*.dll");
    DeleteMatchingFiles(exeDir, "*.tmp.dll");
    DeleteMatchingFiles(exeDir, "*.tmp.pdb");
#elif defined(__APPLE__)
    DeleteMatchingFiles(exeDir, "*.dylib");
#else
    DeleteMatchingFiles(exeDir, "*.so");
#endif
}

static CelsAppModule *s_activeApp = NULL;
static CelsSession *s_activeSession = NULL;

#if defined(_WIN32)
static BOOL WINAPI
ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        if (s_activeApp != NULL && s_activeSession != NULL) {
            CelsAppModuleUnload(s_activeApp, s_activeSession);
            s_activeApp = NULL;
            s_activeSession = NULL;
        }
        CleanupHostDlls();
        ExitProcess(0);
    }
    return FALSE;
}
#else
static void SignalHandler(int sig)
{
    (void)sig;
    if (s_activeApp != NULL && s_activeSession != NULL) {
        CelsAppModuleUnload(s_activeApp, s_activeSession);
        s_activeApp = NULL;
        s_activeSession = NULL;
    }
    CleanupHostDlls();
    _exit(0);
}
#endif
#endif /* CELS_HOT_RELOAD */

/* ========================================================================= */
/* Status and Diagnostic Output                                              */
/* ========================================================================= */

static void PrintStatus(CelsEngine *engine, const CelsAppManifest *manifest,
                        uint32_t iteration, const char *eventTitle)
{
    printf("\n======================================================================\n");
    printf("  >>> [Try #%u] %s\n", iteration, eventTitle);
    printf("======================================================================\n");

    /* 1. Live State Inspection */
    WindowState *win = CEL_GetState(&engine->session, CEL_KEY("CEL_Window"), WindowState);
    PlatformModule *platform = CEL_GetModule(engine, PlatformModule);
    if (platform == NULL) {
        platform = CEL_GetModule(PlatformModule);
    }

    printf("\n  [Live State Query]\n");
    if (win != NULL) {
        printf("    - WindowState   : width = %d, height = %d | isOpen = %s | nativeHandle = %p\n",
               win->width, win->height,
               win->isOpen ? "true" : "false",
               win->nativeHandle);
    } else {
        printf("    - WindowState   : (Unmounted / Reclaimed from Slab Arena)\n");
    }

    if (platform != NULL) {
        printf("    - Engine Module : %s (%d Hz, scale: %.2f) [Resident in host .exe]\n",
               platform->backendName, platform->refreshRateHz, platform->dpiScale);
    }

    /* 2. Composable Tree Structure */
    printf("\n  [Composable Tree Structure]\n");
    if (manifest != NULL && manifest->onPrintTree != NULL) {
        manifest->onPrintTree(&engine->session);
    } else {
        printf("  +-- Composable Tree (%u active nodes) -----------------------+\n",
               CelsGetLogicalGroupCount(&engine->session));
        printf("  +------------------------------------------------------------+\n");
    }
}

/* ========================================================================= */
/* Host Engine Entry Point (.exe)                                            */
/* ========================================================================= */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    bool onceMode = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0) {
            onceMode = true;
        }
    }

#if CELS_HOT_RELOAD
#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
#endif

    /* ===================================================================== */
    /* DEBUG MODE: Dynamic Hot-Reload Host Engine (.exe + .dll)              */
    /* ===================================================================== */

    printf("======================================================================\n");
    printf("              CELS Live Hot-Reload Host Engine (cel_host)             \n");
    printf("======================================================================\n");

    /* 1. Host initializes CelsEngine (.exe) */
    CelsEngine engine;
    CelsEngineInit(&engine, NULL, NULL);

    /* 2. Register host subsystem memory: PlatformModule survives DLL reloads */
    static PlatformModule platform = {
        .backendName = "SDL / Vulkan",
        .refreshRateHz = 144,
        .dpiScale = 1.25f
    };
    CEL_RegisterModule(&engine, PlatformModule, &platform);
    printf("Host engine subsystems initialized in .exe resident memory.\n");

    /* 3. Resolve application DLL path */
    char targetDll[CELS_PATH_MAX] = {0};
    ResolveApplicationDll(targetDll, sizeof(targetDll), argc, argv);
    printf("Target application library: %s\n", targetDll);

    /* 4. Graceful wait loop if the DLL is still being compiled */
    if (!FileReadable(targetDll)) {
        printf("\n[CelsEngine] Application DLL '%s' not found on disk yet.\n", targetDll);
        printf("[CelsEngine] Waiting for build... In CLion, click 'Play' on 'cel_app' or run:\n");
        printf("             .\\scripts\\build_app.bat (Windows) or ./scripts/build_app.sh (Linux/macOS)\n\n");

        while (!FileReadable(targetDll)) {
#if defined(_WIN32)
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 'q' || ch == 'Q' || ch == 27) {
                    printf("\n[CelsEngine] Aborted by user while waiting.\n");
                    CelsEngineDestroy(&engine);
                    return 0;
                }
            }
            Sleep(200);
#else
            usleep(200000);
#endif
            ResolveApplicationDll(targetDll, sizeof(targetDll), argc, argv);
        }
        printf("[CelsEngine] Application DLL detected: %s\n", targetDll);
    }

    /* 5. Dynamically load the application module */
    CelsAppModule app;
    if (!CelsAppModuleLoad(&app, targetDll, &engine.session)) {
        fprintf(stderr, "[Error] Failed to load application DLL '%s'.\n", targetDll);
        CelsEngineDestroy(&engine);
        CleanupHostDlls();
        return 1;
    }

    s_activeApp = &app;
    s_activeSession = &engine.session;

    printf("Application '%s' loaded successfully via shadow copy '%s'.\n",
           app.manifest ? app.manifest->name : "Unknown",
           app.loadedPath);

    /* 6. Initial mount pass */
    CelsSessionRecompose(&engine.session);

    uint32_t iteration = 1;
    PrintStatus(&engine, app.manifest, iteration, "Initial Mount & Slot Allocation (Live from DLL)");

    /* 7. Live Interactive Engine Loop */
    printf("\n[CelsEngine Live Loop Active]\n");
    printf("  - Watching for rebuilds of: %s\n", targetDll);
    printf("  - Keyboard shortcuts:\n");
    printf("      [m] Mutate WindowState (cycle dimensions 800x600 -> 1024x768 -> 1920x1080)\n");
    printf("      [r] / [b] Rebuild Application DLL on disk & Hot-Reload\n");
    printf("      [t] Print Composable Tree & live state\n");
    printf("      [q] / [ESC] Graceful shutdown\n\n");

    int dimensionCycle = 0;

    while (!engine.shouldQuit && !onceMode) {
        /* Check if the application DLL was rebuilt on disk */
        if (CelsAppModuleCheckAndReload(&app, &engine.session)) {
            iteration++;
            printf("\n======================================================================\n");
            printf("  >>> [HOT-RELOAD DETECTED #%u] Application DLL Rebuilt on Disk!\n",
                   app.reloadCount - 1);
            printf("      New shadow copy active: %s\n", app.loadedPath);
            printf("======================================================================\n");
            PrintStatus(&engine, app.manifest, iteration, "Hot-Reload Recomposition (State Retained!)");
        }

        /* Interactive non-blocking keyboard input */
#if defined(_WIN32)
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                printf("\n[CelsEngine] Exit requested.\n");
                break;
            } else if (ch == 'm' || ch == 'M') {
                dimensionCycle = (dimensionCycle + 1) % 3;
                int w = 800, h = 600;
                if (dimensionCycle == 1) {
                    w = 1024; h = 768;
                } else if (dimensionCycle == 2) {
                    w = 1920; h = 1080;
                }

                WindowState *win = CEL_GetState(&engine.session, CEL_KEY("CEL_Window"), WindowState);
                if (win != NULL) {
                    cel_mutate(&engine.session, win) {
                        this->width = w;
                        this->height = h;
                    }
                    CelsSessionRecompose(&engine.session);
                    iteration++;
                    char title[128];
                    snprintf(title, sizeof(title), "State Mutation via [m] key (Resized to %dx%d)", w, h);
                    PrintStatus(&engine, app.manifest, iteration, title);
                }
            } else if (ch == 'r' || ch == 'R' || ch == 'b' || ch == 'B') {
                RebuildApplicationDll();
            } else if (ch == 't' || ch == 'T') {
                PrintStatus(&engine, app.manifest, iteration, "Status Inspection via [t] key");
            }
        }
        Sleep(50);
#else
        usleep(50000);
#endif
    }

    /* 8. Clean teardown */
    printf("\n======================================================================\n");
    printf("=== Shutting down host engine ===\n");
    CelsAppModuleUnload(&app, &engine.session);
    s_activeApp = NULL;
    s_activeSession = NULL;
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);

    /* Delete all .dll files on close per user configuration */
    CleanupHostDlls();
    printf("[cel_host] Cleaned up all dynamic library files (*.dll) on exit.\n");
    printf("Host engine exited cleanly.\n");
    printf("======================================================================\n");

    return 0;

#else
    /* ===================================================================== */
    /* RELEASE MODE: Monolithic Standalone Executable (Single .exe)          */
    /* ===================================================================== */

    printf("======================================================================\n");
    printf("              CELS Standalone Engine (cel_host) [Release]             \n");
    printf("======================================================================\n");

    const CelsAppManifest *manifest = CelsGetAppManifest();
    printf("Application '%s' statically linked (monolithic standalone executable).\n",
           manifest ? manifest->name : "WindowApp");

    /* 1. Initialize engine directly with statically linked manifest */
    CelsEngine engine;
    CelsEngineInit(&engine, manifest, NULL);
    CelsSetCurrentEngine(&engine);

    /* 2. Register resident subsystem memory */
    static PlatformModule platform = {
        .backendName = "SDL / Vulkan",
        .refreshRateHz = 144,
        .dpiScale = 1.25f
    };
    CEL_RegisterModule(&engine, PlatformModule, &platform);

    /* 3. Start engine: calls manifest->onStart, attaches composition, recomposes */
    if (CelsEngineStart(&engine) != CELS_OK) {
        fprintf(stderr, "[Error] Failed to start CelsEngine.\n");
        CelsEngineDestroy(&engine);
        return 1;
    }

    uint32_t iteration = 1;
    PrintStatus(&engine, manifest, iteration, "Initial Mount & Slot Allocation (Monolithic Standalone)");

    /* 4. Interactive loop (zero file-watching overhead) */
    printf("\n[CelsEngine Standalone Loop Active]\n");
    printf("  - Mode: Release (Direct static execution, no DLL dependencies)\n");
    printf("  - Keyboard shortcuts:\n");
    printf("      [m] Mutate WindowState (cycle dimensions 800x600 -> 1024x768 -> 1920x1080)\n");
    printf("      [t] Print Composable Tree & live state\n");
    printf("      [q] / [ESC] Graceful shutdown\n\n");

    int dimensionCycle = 0;

    while (!engine.shouldQuit && !onceMode) {
#if defined(_WIN32)
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                printf("\n[CelsEngine] Exit requested.\n");
                break;
            } else if (ch == 'm' || ch == 'M') {
                dimensionCycle = (dimensionCycle + 1) % 3;
                int w = 800, h = 600;
                if (dimensionCycle == 1) {
                    w = 1024; h = 768;
                } else if (dimensionCycle == 2) {
                    w = 1920; h = 1080;
                }

                WindowState *win = CEL_GetState(&engine.session, CEL_KEY("CEL_Window"), WindowState);
                if (win != NULL) {
                    cel_mutate(&engine.session, win) {
                        this->width = w;
                        this->height = h;
                    }
                    CelsSessionRecompose(&engine.session);
                    iteration++;
                    char title[128];
                    snprintf(title, sizeof(title), "State Mutation via [m] key (Resized to %dx%d)", w, h);
                    PrintStatus(&engine, manifest, iteration, title);
                }
            } else if (ch == 't' || ch == 'T') {
                PrintStatus(&engine, manifest, iteration, "Status Inspection via [t] key");
            }
        }
        Sleep(50);
#else
        usleep(50000);
#endif
    }

    /* 5. Clean teardown */
    printf("\n======================================================================\n");
    printf("=== Shutting down standalone engine ===\n");
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    printf("Standalone engine exited cleanly.\n");
    printf("======================================================================\n");

    return 0;
#endif
}
