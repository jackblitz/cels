#include "cels/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <dlfcn.h>
    #include <unistd.h>
#endif

/* ========================================================================= */
/* Platform Abstraction Helpers                                              */
/* ========================================================================= */

static uint64_t GetPathWriteTime(const char *path)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER ull;
    ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return (uint64_t)ull.QuadPart;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_mtime;
#endif
}

static bool IsPathReadable(const char *path)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path,
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
#else
    return access(path, R_OK) == 0;
#endif
}

static void *PlatformLoadLibrary(const char *path)
{
#if defined(_WIN32)
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

typedef void (*CelsGenericFn)(void);

static CelsGenericFn PlatformGetProcAddress(void *handle, const char *symbol)
{
#if defined(_WIN32)
    return (CelsGenericFn)GetProcAddress((HMODULE)handle, symbol);
#else
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
    #endif
    CelsGenericFn fn = (CelsGenericFn)dlsym(handle, symbol);
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
    #endif
    return fn;
#endif
}

static void PlatformFreeLibrary(void *handle)
{
    if (handle == NULL) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

static void PlatformDeleteFile(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
#if defined(_WIN32)
    for (int retry = 0; retry < 5; ++retry) {
        if (DeleteFileA(path) != 0) {
            return;
        }
        Sleep(20);
    }
#else
    unlink(path);
#endif
}

static bool PlatformCopyFile(const char *src, const char *dst)
{
#if defined(_WIN32)
    return CopyFileA(src, dst, FALSE) != 0;
#else
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    char buffer[8192];
    size_t bytes = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, bytes, out) != bytes) {
            fclose(in);
            fclose(out);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
#endif
}

/* ========================================================================= */
/* Public Application Module Lifecycle API                                   */
/* ========================================================================= */

bool CelsAppModuleLoad(CelsAppModule *app, const char *libraryPath,
                       CelsSession *session)
{
    if (app == NULL || libraryPath == NULL || session == NULL) {
        return false;
    }

    memset(app, 0, sizeof(*app));
    snprintf(app->originalPath, sizeof(app->originalPath), "%s", libraryPath);

    if (!IsPathReadable(libraryPath)) {
        fprintf(stderr, "[CELS App] Cannot read library file '%s'.\n", libraryPath);
        return false;
    }

    app->lastWriteTime = GetPathWriteTime(libraryPath);
    app->reloadCount = 1;

#if defined(_WIN32)
    /* Copy to shadow file on Windows to bypass OS DLL locking */
    snprintf(app->loadedPath,
             sizeof(app->loadedPath),
             "%.480s.hot_%u.tmp.dll",
             libraryPath,
             app->reloadCount);
    if (!PlatformCopyFile(libraryPath, app->loadedPath)) {
        fprintf(stderr,
                "[CELS App] Failed to create shadow copy '%s'.\n",
                app->loadedPath);
        return false;
    }

    /* Also copy .pdb if present for debug symbols */
    char pdbSrc[CELS_PATH_MAX];
    char pdbDst[CELS_PATH_MAX];
    snprintf(pdbSrc, sizeof(pdbSrc), "%s", libraryPath);
    char *dot = strrchr(pdbSrc, '.');
    if (dot != NULL) {
        *dot = '\0';
        snprintf(pdbDst, sizeof(pdbDst), "%.480s.hot_%u.tmp.pdb", pdbSrc, app->reloadCount);
        strncat(pdbSrc, ".pdb", sizeof(pdbSrc) - strlen(pdbSrc) - 1u);
        if (IsPathReadable(pdbSrc)) {
            PlatformCopyFile(pdbSrc, pdbDst);
        }
    }
#else
    snprintf(app->loadedPath, sizeof(app->loadedPath), "%s", libraryPath);
#endif

    app->handle = PlatformLoadLibrary(app->loadedPath);
    if (app->handle == NULL) {
        fprintf(stderr,
                "[CELS App] Failed to load dynamic library '%s'.\n",
                app->loadedPath);
        PlatformDeleteFile(app->loadedPath);
        return false;
    }

    CelsAppEntryFn getManifest = (CelsAppEntryFn)PlatformGetProcAddress(
        app->handle,
        CELS_APP_ENTRY_SYMBOL);

    if (getManifest == NULL) {
        fprintf(stderr,
                "[CELS App] Missing '%s' symbol in '%s'. Did you add CEL_App(MyApp, ...)?\n",
                CELS_APP_ENTRY_SYMBOL,
                libraryPath);
        PlatformFreeLibrary(app->handle);
        PlatformDeleteFile(app->loadedPath);
        app->handle = NULL;
        return false;
    }

    app->manifest = getManifest();
    if (app->manifest != NULL) {
        /* Synchronize ambient session across dynamic library boundary */
        if (app->manifest->setSession != NULL) {
            app->manifest->setSession(session);
        }

        /* Execute onStart: developer sets up custom modules and returns root composition */
        if (app->manifest->onStart != NULL) {
            CelsCompositionRef ref = app->manifest->onStart(session->engine, session);
            if (ref.body != NULL && ref.key != 0) {
                app->attachedKey = ref.key;
                CelsSessionAttachComposition(session, ref.key, ref.body, ref.lifecycleEval, NULL);
            }
        }
    }

    app->isLoaded = true;
    return true;
}

bool CelsAppModuleCheckAndReload(CelsAppModule *app, CelsSession *session)
{
    if (app == NULL || !app->isLoaded || session == NULL) {
        return false;
    }

    const uint64_t currentWriteTime = GetPathWriteTime(app->originalPath);
    if (currentWriteTime == 0 || currentWriteTime <= app->lastWriteTime) {
        return false;
    }

    /* Wait if the compiler/linker is currently writing to the file */
    if (!IsPathReadable(app->originalPath)) {
        return false;
    }

    PlatformFreeLibrary(app->handle);
    app->handle = NULL;

    char oldLoadedPath[CELS_PATH_MAX];
    snprintf(oldLoadedPath, sizeof(oldLoadedPath), "%s", app->loadedPath);

    app->reloadCount++;
#if defined(_WIN32)
    snprintf(app->loadedPath,
             sizeof(app->loadedPath),
             "%.480s.hot_%u.tmp.dll",
             app->originalPath,
             app->reloadCount);
    if (!PlatformCopyFile(app->originalPath, app->loadedPath)) {
        fprintf(stderr,
                "[CELS App] Failed to create shadow copy '%s' during reload.\n",
                app->loadedPath);
        return false;
    }

    /* Also copy .pdb if present for debug symbols */
    char pdbSrc[CELS_PATH_MAX];
    char pdbDst[CELS_PATH_MAX];
    snprintf(pdbSrc, sizeof(pdbSrc), "%s", app->originalPath);
    char *dot = strrchr(pdbSrc, '.');
    if (dot != NULL) {
        *dot = '\0';
        snprintf(pdbDst, sizeof(pdbDst), "%.480s.hot_%u.tmp.pdb", pdbSrc, app->reloadCount);
        strncat(pdbSrc, ".pdb", sizeof(pdbSrc) - strlen(pdbSrc) - 1u);
        if (IsPathReadable(pdbSrc)) {
            PlatformCopyFile(pdbSrc, pdbDst);
        }
    }
#else
    snprintf(app->loadedPath, sizeof(app->loadedPath), "%s", app->originalPath);
#endif

    app->handle = PlatformLoadLibrary(app->loadedPath);
    if (app->handle == NULL) {
        fprintf(stderr,
                "[CELS App] Failed to reload dynamic library '%s'.\n",
                app->loadedPath);
        return false;
    }

    /* Delete old shadow file after loading the new one */
    PlatformDeleteFile(oldLoadedPath);

    CelsAppEntryFn getManifest = (CelsAppEntryFn)PlatformGetProcAddress(
        app->handle,
        CELS_APP_ENTRY_SYMBOL);

    if (getManifest == NULL) {
        fprintf(stderr,
                "[CELS App] Reloaded library is missing '%s'.\n",
                CELS_APP_ENTRY_SYMBOL);
        return false;
    }

    app->manifest = getManifest();
    app->lastWriteTime = currentWriteTime;

    if (app->manifest != NULL) {
        /* Re-bind ambient session in reloaded dynamic library */
        if (app->manifest->setSession != NULL) {
            app->manifest->setSession(session);
        }

        /* Refresh function pointer from onStart */
        if (app->manifest->onStart != NULL) {
            CelsCompositionRef ref = app->manifest->onStart(session->engine, session);
            if (ref.body != NULL && ref.key != 0) {
                app->attachedKey = ref.key;
                CelsSessionAttachComposition(session, ref.key, ref.body, ref.lifecycleEval, NULL);
            }
        }
    }

    /* Force recomposition re-evaluation of newly loaded code while preserving slots */
    CelsSessionHotReload(session);
    CelsSessionRecompose(session);

    return true;
}

void CelsAppModuleUnload(CelsAppModule *app, CelsSession *session)
{
    if (app == NULL || !app->isLoaded) {
        return;
    }

    if (app->manifest != NULL && app->manifest->onEnd != NULL && session != NULL) {
        app->manifest->onEnd(session->engine, session);
    }

    if (app->attachedKey != 0 && session != NULL) {
        CelsPruneSubtreeByKey(session, app->attachedKey);
        CelsSessionDetachComposition(session, app->attachedKey);
        app->attachedKey = 0;
    }

    PlatformFreeLibrary(app->handle);
    app->handle = NULL;

    PlatformDeleteFile(app->loadedPath);

    app->isLoaded = false;
    app->manifest = NULL;
}
