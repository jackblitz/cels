#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

static void GetExeDir(char *outDir, size_t maxLen)
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

static int FileExists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n======================================================================\n");
    printf("  [CELS] Triggering 'cel_app' rebuild script via CLion Play...\n");
    printf("======================================================================\n");

    char exeDir[512] = {0};
    GetExeDir(exeDir, sizeof(exeDir));

    int res = -1;

#if defined(_WIN32)
    char candidate1[512];
    char candidate2[512];
    snprintf(candidate1, sizeof(candidate1), "scripts\\build_app.bat");
    snprintf(candidate2, sizeof(candidate2), "%.400s..\\..\\..\\scripts\\build_app.bat", exeDir);

    char cmd[1024];
    if (FileExists(candidate1)) {
        snprintf(cmd, sizeof(cmd), "\"%.500s\"", candidate1);
    } else if (FileExists(candidate2)) {
        snprintf(cmd, sizeof(cmd), "\"%.500s\"", candidate2);
    } else {
        snprintf(cmd, sizeof(cmd), "cmake --build cmake-build-debug --target cel_app");
    }

    char fullCmd[1100];
    snprintf(fullCmd, sizeof(fullCmd), "\"%s\"", cmd);
    res = system(fullCmd);
#else
    char candidate1[512];
    char candidate2[512];
    snprintf(candidate1, sizeof(candidate1), "scripts/build_app.sh");
    snprintf(candidate2, sizeof(candidate2), "%.400s../../../scripts/build_app.sh", exeDir);

    char cmd[1024];
    if (FileExists(candidate1)) {
        snprintf(cmd, sizeof(cmd), "/bin/sh \"%.500s\"", candidate1);
    } else if (FileExists(candidate2)) {
        snprintf(cmd, sizeof(cmd), "/bin/sh \"%.500s\"", candidate2);
    } else {
        snprintf(cmd, sizeof(cmd), "cmake --build cmake-build-debug --target cel_app");
    }

    res = system(cmd);
#endif

    if (res == 0) {
        printf("\n[CelsEngine] Ready! If cel_host is running, it will reload in <50ms.\n");
    } else {
        printf("\n[CelsEngine] Build script returned code %d.\n", res);
    }

    return res;
}
