#include "cels/runtime/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/stat.h>
    #include <dlfcn.h>
    #include <unistd.h>
    #include <mach-o/dyld.h>
    #include <dirent.h>
#else
    #include <sys/stat.h>
    #include <dlfcn.h>
    #include <unistd.h>
    #include <dirent.h>
#endif

/* ========================================================================= */
/* Platform Abstraction Helpers                                              */
/* ========================================================================= */

/**
 * Queries the last modification timestamp of a file on disk.
 *
 * Uses Win32 GetFileAttributesExA or POSIX stat to retrieve file write time.
 *
 * @param path Path to the target file.
 * @return 64-bit timestamp value, or 0 if the file cannot be accessed.
 */
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

/**
 * Checks whether a file is completely written and accessible for reading.
 *
 * On Windows, attempts an exclusive open to ensure the compiler/linker has finished
 * writing and closed its file handles.
 *
 * @param path Path to the target file.
 * @return True if the file can be opened and read; false otherwise.
 */
static bool IsPathReadable(const char *path)
{
#if defined(_WIN32)
    /* Open with sharing mode 0 (exclusive) to verify the compiler/linker has completely closed the file */
    HANDLE h = CreateFileA(path,
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileA(path,
                        GENERIC_READ,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
    }
    CloseHandle(h);
    return true;
#else
    return access(path, R_OK) == 0;
#endif
}

/**
 * Validates whether a file is a complete, uncorrupted Windows Portable Executable (PE) binary.
 *
 * Checks DOS signature ('MZ'), NT signature ('PE\0\0'), non-empty sections, and valid image size.
 *
 * @param path Path to the candidate dynamic library binary.
 * @return True if the image is valid and ready to load; false otherwise.
 */
static bool IsValidPEImage(const char *path)
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

    DWORD sizeLow = GetFileSize(h, NULL);
    if (sizeLow < 4096) {
        CloseHandle(h);
        return false;
    }

    IMAGE_DOS_HEADER dosHeader;
    DWORD bytesRead = 0;
    if (!ReadFile(h, &dosHeader, sizeof(dosHeader), &bytesRead, NULL) || bytesRead != sizeof(dosHeader)) {
        CloseHandle(h);
        return false;
    }

    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(h);
        return false;
    }

    if (dosHeader.e_lfanew <= 0 || (DWORD)dosHeader.e_lfanew >= sizeLow - sizeof(IMAGE_NT_HEADERS)) {
        CloseHandle(h);
        return false;
    }

    if (SetFilePointer(h, dosHeader.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(h);
        return false;
    }

    IMAGE_NT_HEADERS ntHeaders;
    if (!ReadFile(h, &ntHeaders, sizeof(ntHeaders), &bytesRead, NULL) || bytesRead != sizeof(ntHeaders)) {
        CloseHandle(h);
        return false;
    }

    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(h);
        return false;
    }

    if (ntHeaders.FileHeader.NumberOfSections == 0 || ntHeaders.OptionalHeader.SizeOfImage == 0) {
        CloseHandle(h);
        return false;
    }

    CloseHandle(h);
    return true;
#else
    (void)path;
    return true;
#endif
}

/**
 * Dynamically loads a shared library (.dll / .so / .dylib) into the current process.
 *
 * @param path Path to the shared library binary.
 * @return Opaque platform module handle, or NULL on failure.
 */
static void *PlatformLoadLibrary(const char *path)
{
#if defined(_WIN32)
    DWORD prevMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, &prevMode);
    HMODULE mod = LoadLibraryA(path);
    SetThreadErrorMode(prevMode, NULL);
    return (void *)mod;
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

typedef void (*CelsGenericFn)(void);

/**
 * Resolves an exported symbol address from a loaded shared library handle.
 *
 * @param handle Opaque platform module handle.
 * @param symbol Name of the exported function or symbol.
 * @return Function pointer to the symbol, or NULL if not found.
 */
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

/**
 * Unloads a dynamically loaded shared library handle.
 *
 * @param handle Opaque platform module handle. Safe if NULL.
 */
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

/**
 * Deletes a file on disk, retrying multiple times on Windows if locked.
 *
 * @param path Path of the file to delete. Safe if NULL or empty.
 */
static void PlatformDeleteFile(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
#if defined(_WIN32)
    for (int retry = 0; retry < 15; ++retry) {
        if (DeleteFileA(path) != 0) {
            return;
        }
        Sleep(20);
    }
#else
    unlink(path);
#endif
}

/**
 * Extracts the directory path prefix from a full file path.
 *
 * @param path   Full file path.
 * @param outDir Destination buffer receiving directory path with trailing slash.
 * @param maxLen Size in bytes of outDir buffer.
 */
static void GetDirectoryFromPath(const char *path, char *outDir, size_t maxLen)
{
    if (path == NULL || outDir == NULL || maxLen == 0) {
        return;
    }
    snprintf(outDir, maxLen, "%s", path);
    char *lastSlash = strrchr(outDir, '\\');
    char *lastFwd = strrchr(outDir, '/');
    if (lastFwd && (!lastSlash || lastFwd > lastSlash)) {
        lastSlash = lastFwd;
    }
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    } else {
        snprintf(outDir, maxLen, "./");
    }
}

/**
 * Cleans up temporary shadow copy files left behind from previous hot reload cycles.
 *
 * @param dir Directory path containing shadow copy files.
 */
static void PlatformCleanupHotReloadFiles(const char *dir)
{
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
#if defined(_WIN32)
    char pattern[CELS_PATH_MAX * 2];
    snprintf(pattern, sizeof(pattern), "%s*.hot_*.tmp.*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char fpath[CELS_PATH_MAX * 2];
                snprintf(fpath, sizeof(fpath), "%s%s", dir, fd.cFileName);
                PlatformDeleteFile(fpath);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strstr(entry->d_name, ".hot_") != NULL && strstr(entry->d_name, ".tmp.") != NULL) {
                char fpath[CELS_PATH_MAX * 2];
                snprintf(fpath, sizeof(fpath), "%s/%s", dir, entry->d_name);
                PlatformDeleteFile(fpath);
            }
        }
        closedir(d);
    }
#endif
}

/**
 * Copies a file from source to destination path.
 *
 * @param src Source file path.
 * @param dst Destination file path.
 * @return True on success; false on failure.
 */
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

#define CELS_MAX_ACTIVE_MODULES 32
static CelsAppModule *s_activeModules[CELS_MAX_ACTIVE_MODULES];
static uint32_t s_activeModuleCount = 0;

static void CelsRegisterActiveModule(CelsAppModule *app)
{
    if (app == NULL) return;
    for (uint32_t i = 0; i < s_activeModuleCount; ++i) {
        if (s_activeModules[i] == app) return;
    }
    if (s_activeModuleCount < CELS_MAX_ACTIVE_MODULES) {
        s_activeModules[s_activeModuleCount++] = app;
    }
}

static void CelsUnregisterActiveModule(CelsAppModule *app)
{
    if (app == NULL) return;
    for (uint32_t i = 0; i < s_activeModuleCount; ++i) {
        if (s_activeModules[i] == app) {
            for (uint32_t j = i; j + 1 < s_activeModuleCount; ++j) {
                s_activeModules[j] = s_activeModules[j + 1];
            }
            s_activeModuleCount--;
            break;
        }
    }
}

/**
 * Process exit handler to unload all active modules and clean up shadow copies.
 */
static void CelsAppModuleCleanupAtExit(void)
{
    for (uint32_t i = s_activeModuleCount; i > 0; --i) {
        CelsAppModule *mod = s_activeModules[i - 1];
        if (mod != NULL && mod->isLoaded) {
            CelsAppModuleUnload(mod, NULL);
        }
    }
    s_activeModuleCount = 0;
}

#if defined(_WIN32)
/**
 * Windows console control handler (Ctrl+C / Break / Close) to safely unload modules.
 *
 * @param ctrlType Control signal type.
 * @return FALSE to let default OS handling proceed after cleanup.
 */
static BOOL WINAPI CelsAppModuleConsoleCtrlHandler(DWORD ctrlType)
{
    (void)ctrlType;
    for (uint32_t i = s_activeModuleCount; i > 0; --i) {
        CelsAppModule *mod = s_activeModules[i - 1];
        if (mod != NULL && mod->isLoaded) {
            CelsAppModuleUnload(mod, NULL);
        }
    }
    s_activeModuleCount = 0;
    return FALSE;
}
#endif

/* ========================================================================= */
/* Public Application Module Lifecycle API                                   */
/* ========================================================================= */

/**
 * Loads a dynamic application module (.dll / .so / .dylib) and initializes its composition.
 *
 * Purges any stale shadow copies, creates a PID-tagged shadow copy on Windows to bypass
 * OS file locking during hot-reload recompilation, loads the binary into the process address
 * space, resolves the CELS_APP_ENTRY_SYMBOL manifest, checks/adjusts session slab sizing,
 * synchronizes session pointer across the DLL boundary, executes manifest->onStart, and
 * installs atexit and console Ctrl+C cleanup handlers.
 *
 * @param app         Pointer to uninitialized CelsAppModule struct. Non-NULL.
 * @param libraryPath Path to dynamic library file on disk. Non-NULL.
 * @param session     Owning host session context. Non-NULL.
 * @return True if module was loaded and initialized successfully; false on failure.
 */
bool CelsAppModuleLoadEntryInternal(CelsAppModule *app, const char *libraryPath,
                                   CelsSession *session, const char *entrySymbol,
                                   bool autoStart)
{
    if (app == NULL || libraryPath == NULL || session == NULL) {
        return false;
    }

    static uint32_t s_moduleInstanceCounter = 0;

    if (s_activeModuleCount == 0) {
        /* Sweep and purge any orphaned temporary hot reload files from previous sessions */
        char initialDir[CELS_PATH_MAX];
        GetDirectoryFromPath(libraryPath, initialDir, sizeof(initialDir));
        PlatformCleanupHotReloadFiles(initialDir);
    }

    memset(app, 0, sizeof(*app));
    snprintf(app->originalPath, sizeof(app->originalPath), "%s", libraryPath);
    if (entrySymbol != NULL && entrySymbol[0] != '\0') {
        snprintf(app->entrySymbol, sizeof(app->entrySymbol), "%s", entrySymbol);
    }

    if (!IsPathReadable(libraryPath)) {
        fprintf(stderr, "[CELS App] Cannot read library file '%s'.\n", libraryPath);
        return false;
    }

    app->lastWriteTime = GetPathWriteTime(libraryPath);
    app->instanceId = ++s_moduleInstanceCounter;
    app->reloadCount = 1;

#if defined(_WIN32)
    /* Copy to shadow file on Windows to bypass OS DLL locking */
    snprintf(app->loadedPath,
             sizeof(app->loadedPath),
             "%.440s.hot_%lu_%u_%u.tmp.dll",
             libraryPath,
             (unsigned long)GetCurrentProcessId(),
             app->instanceId,
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
        snprintf(pdbDst, sizeof(pdbDst), "%.440s.hot_%lu_%u_%u.tmp.pdb", pdbSrc, (unsigned long)GetCurrentProcessId(), app->instanceId, app->reloadCount);
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
#if defined(_WIN32)
        DWORD err = GetLastError();
        fprintf(stderr,
                "[CELS App] Failed to load dynamic library '%s' (error code: %lu).\n",
                app->loadedPath, (unsigned long)err);
#else
        fprintf(stderr,
                "[CELS App] Failed to load dynamic library '%s'.\n",
                app->loadedPath);
#endif
        PlatformDeleteFile(app->loadedPath);
        return false;
    }

    CelsAppEntryFn getDef = NULL;
    if (app->entrySymbol[0] != '\0') {
        getDef = (CelsAppEntryFn)PlatformGetProcAddress(app->handle, app->entrySymbol);
    } else {
        getDef = (CelsAppEntryFn)PlatformGetProcAddress(app->handle, CELS_APP_ENTRY_SYMBOL);
        if (getDef == NULL) {
            getDef = (CelsAppEntryFn)PlatformGetProcAddress(app->handle, CELS_APP_LEGACY_ENTRY_SYMBOL);
        }
    }

    if (getDef == NULL) {
        fprintf(stderr,
                "[CELS App] Missing '%s' symbol in '%s'. Did you add CEL_App(MyApp, ...)?\n",
                app->entrySymbol[0] != '\0' ? app->entrySymbol : CELS_APP_ENTRY_SYMBOL,
                libraryPath);
        PlatformFreeLibrary(app->handle);
        PlatformDeleteFile(app->loadedPath);
        app->handle = NULL;
        return false;
    }

    app->def = getDef();
    app->manifest = app->def;
    if (app->def != NULL) {
        /* Reconfigure session slab if manifest declares a specific slab size before initial composition
           and the host is currently using the default slab size (CLI overrides take precedence) */
        if (app->def->slabSize > 0 && !session->hasComposedOnce && session->slabSize == CELS_DEFAULT_SLAB_SIZE) {
            if (session->slabSize != app->def->slabSize ||
                (app->def->maxGroups > 0 && session->maxGroups != app->def->maxGroups)) {
                CelsSessionConfig newCfg = {
                    .slabSize = app->def->slabSize,
                    .maxGroups = app->def->maxGroups,
                    .engine = session->engine,
                    .maxDrainIterations = session->maxDrainIterations
                };
                CelsEngine *eng = session->engine;
                CelsSessionDestroy(session);
                CelsSessionInit(session, &newCfg);
                session->engine = eng;
            }
        }

        /* Synchronize ambient session across dynamic library boundary */
        if (app->def->setSession != NULL) {
            app->def->setSession(session);
        }

        /* Execute onStart: developer sets up custom modules and attaches compositions */
        if (autoStart && app->def->onStart != NULL) {
            app->def->onStart(session->engine, session);
            if (session->attachedCount > 0) {
                app->attachedKey = session->attachedCompositions[0].key;
            }
        }
    }

    app->isLoaded = true;
    CelsRegisterActiveModule(app);
    static bool s_cleanupHandlersInstalled = false;
    if (!s_cleanupHandlersInstalled) {
        s_cleanupHandlersInstalled = true;
        atexit(CelsAppModuleCleanupAtExit);
#if defined(_WIN32)
        SetConsoleCtrlHandler(CelsAppModuleConsoleCtrlHandler, TRUE);
#endif
    }
    return true;
}

bool CelsAppModuleLoad(CelsAppModule *app, const char *libraryPath,
                       CelsSession *session)
{
    return CelsAppModuleLoadEntryInternal(app, libraryPath, session, NULL, true);
}

/**
 * Checks if the underlying library file has changed on disk and executes a live hot-reload.
 *
 * Checks modification timestamp and verifies that compiler output has completed writing.
 * Creates an incremented shadow file, loads the new library, swaps library handles,
 * executes manifest->onStart to refresh function pointers, triggers CelsSessionHotReload
 * to reconcile changed struct layouts, recomposes the tree, and purges the previous shadow file.
 *
 * @param app     Pointer to active CelsAppModule. Non-NULL.
 * @param session Owning host session. Non-NULL.
 * @return True if a reload occurred; false if no change or file is locked/incomplete.
 */
bool CelsAppModuleCheckAndReload(CelsAppModule *app, CelsSession *session)
{
    if (app == NULL || !app->isLoaded || session == NULL) {
        return false;
    }

    const uint64_t currentWriteTime = GetPathWriteTime(app->originalPath);
    if (currentWriteTime == 0 || currentWriteTime <= app->lastWriteTime) {
        return false;
    }

    /* Wait if the compiler/linker is currently writing to the file or PE is incomplete */
    if (!IsPathReadable(app->originalPath) || !IsValidPEImage(app->originalPath)) {
        return false;
    }

    char oldLoadedPath[CELS_PATH_MAX];
    snprintf(oldLoadedPath, sizeof(oldLoadedPath), "%s", app->loadedPath);

    char candidatePath[CELS_PATH_MAX];
    uint32_t nextReload = app->reloadCount + 1;
#if defined(_WIN32)
    snprintf(candidatePath,
             sizeof(candidatePath),
             "%.440s.hot_%lu_%u_%u.tmp.dll",
             app->originalPath,
             (unsigned long)GetCurrentProcessId(),
             app->instanceId,
             nextReload);
    if (!PlatformCopyFile(app->originalPath, candidatePath)) {
        return false;
    }

    /* Verify that the shadow copy itself is fully intact before loading */
    if (!IsValidPEImage(candidatePath)) {
        PlatformDeleteFile(candidatePath);
        return false;
    }

    /* Also copy .pdb if present for debug symbols */
    char pdbSrc[CELS_PATH_MAX];
    char pdbDst[CELS_PATH_MAX];
    snprintf(pdbSrc, sizeof(pdbSrc), "%s", app->originalPath);
    char *dot = strrchr(pdbSrc, '.');
    if (dot != NULL) {
        *dot = '\0';
        snprintf(pdbDst, sizeof(pdbDst), "%.440s.hot_%lu_%u_%u.tmp.pdb", pdbSrc, (unsigned long)GetCurrentProcessId(), app->instanceId, nextReload);
        strncat(pdbSrc, ".pdb", sizeof(pdbSrc) - strlen(pdbSrc) - 1u);
        if (IsPathReadable(pdbSrc)) {
            PlatformCopyFile(pdbSrc, pdbDst);
        }
    }
#else
    snprintf(candidatePath, sizeof(candidatePath), "%s", app->originalPath);
#endif

    void *newHandle = PlatformLoadLibrary(candidatePath);
    if (newHandle == NULL) {
        PlatformDeleteFile(candidatePath);
        return false;
    }

    CelsAppEntryFn getDef = NULL;
    if (app->entrySymbol[0] != '\0') {
        getDef = (CelsAppEntryFn)PlatformGetProcAddress(newHandle, app->entrySymbol);
    } else {
        getDef = (CelsAppEntryFn)PlatformGetProcAddress(newHandle, CELS_APP_ENTRY_SYMBOL);
        if (getDef == NULL) {
            getDef = (CelsAppEntryFn)PlatformGetProcAddress(newHandle, CELS_APP_LEGACY_ENTRY_SYMBOL);
        }
    }

    if (getDef == NULL) {
        PlatformFreeLibrary(newHandle);
        PlatformDeleteFile(candidatePath);
        return false;
    }

    /* New library is verified and ready. Swap cleanly. */
    void *oldHandle = app->handle;
    app->handle = newHandle;
    snprintf(app->loadedPath, sizeof(app->loadedPath), "%s", candidatePath);
    app->reloadCount = nextReload;

    app->def = getDef();
    app->manifest = app->def;
    app->lastWriteTime = currentWriteTime;

    if (app->def != NULL) {
        /* Re-bind ambient session in reloaded dynamic library */
        if (app->def->setSession != NULL) {
            app->def->setSession(session);
        }

        /* Refresh function pointers: invoke onReload if defined, or fallback to onStart */
        if (app->def->onReload != NULL) {
            app->def->onReload(session->engine, session);
        } else if (app->def->onStart != NULL) {
            app->def->onStart(session->engine, session);
        }
        if (session->attachedCount > 0) {
            app->attachedKey = session->attachedCompositions[0].key;
        }
    }

    /* Force recomposition re-evaluation of newly loaded code while preserving slots */
    CelsSessionHotReload(session);
    CelsSessionRecompose(session);

    /* Safely release old library and cleanup shadow file AFTER recomposition has completed */
    if (oldHandle != NULL) {
        PlatformFreeLibrary(oldHandle);
    }
    PlatformDeleteFile(oldLoadedPath);
    char oldPdb[CELS_PATH_MAX];
    snprintf(oldPdb, sizeof(oldPdb), "%s", oldLoadedPath);
    char *ext = strstr(oldPdb, ".tmp.dll");
    if (ext != NULL) {
        memcpy(ext, ".tmp.pdb", 8);
        PlatformDeleteFile(oldPdb);
    }

    return true;
}

/**
 * Gracefully unloads a dynamic application module and cleans up temporary files.
 *
 * Invokes manifest->onEnd cleanup lifecycle hook, prunes and detaches the root composition
 * from the session, frees the OS dynamic library handle, deletes temporary shadow DLL/PDB files,
 * and resets the CelsAppModule structure.
 *
 * @param app     Pointer to CelsAppModule. Safe if NULL or not loaded.
 * @param session Owning host session. Safe if NULL.
 */
void CelsAppModuleUnload(CelsAppModule *app, CelsSession *session)
{
    if (app == NULL || !app->isLoaded) {
        return;
    }

    CelsUnregisterActiveModule(app);

    if (app->def != NULL && app->def->onEnd != NULL && session != NULL) {
        app->def->onEnd(session->engine, session);
    }

    if (app->attachedKey != 0 && session != NULL) {
        CelsPruneSubtreeByKey(session, app->attachedKey);
        CelsSessionDetachComposition(session, app->attachedKey);
        app->attachedKey = 0;
    }

    if (app->handle != NULL) {
        PlatformFreeLibrary(app->handle);
        app->handle = NULL;
    }

    /* Delete shadow copy DLL and PDB */
    PlatformDeleteFile(app->loadedPath);
    char pdbPath[CELS_PATH_MAX];
    snprintf(pdbPath, sizeof(pdbPath), "%s", app->loadedPath);
    char *pdbExt = strstr(pdbPath, ".tmp.dll");
    if (pdbExt != NULL) {
        memcpy(pdbExt, ".tmp.pdb", 8);
        PlatformDeleteFile(pdbPath);
    }

    /* Sweep and purge remaining hot reload temporary files only when all modules have unloaded */
    if (s_activeModuleCount == 0) {
        char dir[CELS_PATH_MAX];
        GetDirectoryFromPath(app->loadedPath, dir, sizeof(dir));
        PlatformCleanupHotReloadFiles(dir);
    }

    app->isLoaded = false;
    app->def = NULL;
    app->manifest = NULL;
}

/**
 * Resolves the absolute or relative file path of an application module binary on disk.
 *
 * Searches direct paths, current working directory, executable binary directory,
 * and platform shared library conventions (.dll, .so, .dylib, lib*.so).
 *
 * @param appName User-supplied name, target token, or file path. Safe if NULL.
 * @param outPath Output buffer receiving the resolved file path. Non-NULL.
 * @param maxLen  Size in bytes of outPath buffer.
 * @return True if a readable binary matching appName was located; false otherwise.
 */
bool CelsResolveModulePath(const char *appName, char *outPath, size_t maxLen)
{
    if (outPath == NULL || maxLen == 0) {
        return false;
    }
    outPath[0] = '\0';

    const char *name = appName;
#if defined(CELS_APP_TARGET)
    if (name == NULL || name[0] == '\0') {
        name = CELS_APP_TARGET;
    }
#endif
    if (name == NULL || name[0] == '\0') {
        name = "app";
    }

    /* 1. Direct path exists */
    if (IsPathReadable(name)) {
        snprintf(outPath, maxLen, "%s", name);
        return true;
    }

    /* 2. Locate directory containing current executable */
    char exeDir[CELS_PATH_MAX] = {0};
#if defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, exeDir, (DWORD)sizeof(exeDir));
    if (len > 0 && len < sizeof(exeDir)) {
        char *lastSlash = strrchr(exeDir, '\\');
        char *lastFwd = strrchr(exeDir, '/');
        if (lastFwd && (!lastSlash || lastFwd > lastSlash)) {
            lastSlash = lastFwd;
        }
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
        }
    }
#elif defined(__APPLE__)
    uint32_t size = sizeof(exeDir);
    if (_NSGetExecutablePath(exeDir, &size) == 0) {
        char *lastSlash = strrchr(exeDir, '/');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
        }
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", exeDir, sizeof(exeDir) - 1);
    if (len > 0) {
        exeDir[len] = '\0';
        char *lastSlash = strrchr(exeDir, '/');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
        }
    }
#endif

    /* 3. Probe library extensions in executable directory */
    char candidate[CELS_PATH_MAX * 2];

#if defined(_WIN32)
    snprintf(candidate, sizeof(candidate), "%s%s.dll", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%s%s_app.dll", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s.dll", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s_app.dll", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    /* Fallback default path */
    snprintf(outPath, maxLen, "%s%s.dll", exeDir, name);
    return false;
#elif defined(__APPLE__)
    snprintf(candidate, sizeof(candidate), "%s%s.dylib", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%s%s_app.dylib", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s.dylib", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s_app.dylib", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(outPath, maxLen, "%slib%s.dylib", exeDir, name);
    return false;
#else
    snprintf(candidate, sizeof(candidate), "%s%s.so", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%s%s_app.so", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s.so", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%slib%s_app.so", exeDir, name);
    if (IsPathReadable(candidate)) {
        snprintf(outPath, maxLen, "%s", candidate);
        return true;
    }
    snprintf(outPath, maxLen, "%slib%s.so", exeDir, name);
    return false;
#endif
}

/* ========================================================================= */
/* High-Level Host Application Management API                                */
/* ========================================================================= */

/**
 * Loads an application dynamic library and binds it to an engine and target session.
 *
 * @param app      Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine   Owning host engine. Non-NULL.
 * @param session  Target session for this application. Non-NULL.
 * @param dllPath  Path or target name of the dynamic library. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppLoad(CelsApp *app, CelsEngine *engine, CelsSession *session, const char *dllPath)
{
    return CelsAppLoadEntry(app, engine, session, dllPath, NULL);
}

/**
 * Loads an application dynamic library with a specific entry symbol.
 *
 * Allows multiple apps compiled into the same DLL to be loaded independently.
 *
 * @param app         Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine      Owning host engine. Non-NULL.
 * @param session     Target session. Non-NULL.
 * @param dllPath     Path or target name of the dynamic library. Non-NULL.
 * @param entrySymbol Exported entry symbol (defaults to "CelsGetAppDef" if NULL).
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppLoadEntry(CelsApp *app, CelsEngine *engine, CelsSession *session, const char *dllPath, const char *entrySymbol)
{
    if (app == NULL || engine == NULL || session == NULL || dllPath == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(app, 0, sizeof(*app));
    app->engine = engine;
    app->session = session;
    app->isStarted = false;
    app->isStatic = false;

    if (session->engine == NULL) {
        session->engine = engine;
    }

    char resolvedPath[CELS_PATH_MAX];
    const char *loadPath = dllPath;
    if (CelsResolveModulePath(dllPath, resolvedPath, sizeof(resolvedPath))) {
        loadPath = resolvedPath;
    }

    if (!CelsAppModuleLoadEntryInternal(&app->module, loadPath, session, entrySymbol, false)) {
        return CELS_ERROR_INVALID_STATE;
    }

    return CELS_OK;
}

/**
 * Binds a static application definition for monolithic / non-reloadable builds.
 *
 * @param app     Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine  Owning host engine. Non-NULL.
 * @param session Target session. Non-NULL.
 * @param def     Static application definition. Non-NULL.
 * @return CELS_OK on success.
 */
CelsResult CelsAppBindStatic(CelsApp *app, CelsEngine *engine, CelsSession *session, const struct CelsAppDef *def)
{
    if (app == NULL || engine == NULL || session == NULL || def == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(app, 0, sizeof(*app));
    app->engine = engine;
    app->session = session;
    app->isStarted = false;
    app->isStatic = true;
    app->module.def = def;
    app->module.manifest = def;
    app->module.isLoaded = true;

    if (session->engine == NULL) {
        session->engine = engine;
    }

    if (def->setSession != NULL) {
        def->setSession(session);
    }

    return CELS_OK;
}

/**
 * Starts execution of a loaded application.
 *
 * Synchronizes ambient session across the binary boundary, invokes the app's onStart hook
 * passing (engine, session), and executes the initial composition pass.
 *
 * @param app Target application handle. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppStart(CelsApp *app)
{
    if (app == NULL || app->session == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (app->isStarted) {
        return CELS_OK;
    }

    const CelsAppDef *def = app->module.def;
    if (def != NULL) {
        if (def->setSession != NULL) {
            def->setSession(app->session);
        }
        if (def->onStart != NULL) {
            def->onStart(app->engine, app->session);
            if (app->session->attachedCount > 0) {
                app->module.attachedKey = app->session->attachedCompositions[0].key;
            }
        }
    }

    app->isStarted = true;

    /* Execute initial composition pass */
    CelsSessionRecompose(app->session);
    return CELS_OK;
}

/**
 * Checks if the application dynamic library was modified on disk and hot-reloads it.
 *
 * @param app Target application handle. Safe if NULL.
 * @return True if a reload occurred; false otherwise.
 */
bool CelsAppCheckReload(CelsApp *app)
{
    if (app == NULL || app->isStatic || !app->module.isLoaded || app->session == NULL) {
        return false;
    }

    return CelsAppModuleCheckAndReload(&app->module, app->session);
}

/**
 * Forces an immediate reload of the application dynamic library.
 *
 * @param app Target application handle. Safe if NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppReload(CelsApp *app)
{
    if (app == NULL || app->isStatic || !app->module.isLoaded || app->session == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    app->module.lastWriteTime = 0;
    if (CelsAppModuleCheckAndReload(&app->module, app->session)) {
        return CELS_OK;
    }
    return CELS_ERROR_INVALID_STATE;
}

/**
 * Stops execution, unloads the dynamic library, and cleans up shadow copy files.
 *
 * Invokes the app's onEnd hook passing (engine, session) and detaches its root composition.
 *
 * @param app Target application handle. Safe if NULL.
 */
void CelsAppDestroy(CelsApp *app)
{
    if (app == NULL) {
        return;
    }

    if (app->isStatic) {
        if (app->module.def != NULL && app->module.def->onEnd != NULL && app->session != NULL) {
            app->module.def->onEnd(app->engine, app->session);
        }
        if (app->module.attachedKey != 0 && app->session != NULL) {
            CelsPruneSubtreeByKey(app->session, app->module.attachedKey);
            CelsSessionDetachComposition(app->session, app->module.attachedKey);
            app->module.attachedKey = 0;
        }
        app->module.isLoaded = false;
        app->module.def = NULL;
        app->module.manifest = NULL;
    } else {
        CelsAppModuleUnload(&app->module, app->session);
    }

    app->isStarted = false;
    app->engine = NULL;
    app->session = NULL;
}

