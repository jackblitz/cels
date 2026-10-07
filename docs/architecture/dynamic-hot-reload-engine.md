# CELS Architecture: Dynamic Hot-Reload Engine & Schema Evolution

## 1. Executive Summary & Design Rationale

Iteration speed is the single most critical factor in software development productivity for games, simulations, and interactive software. Traditional C and C++ workflows suffer from painful compile-link-restart cycles: a minor logic or UI adjustment requires closing the game, recompiling, relinking, restarting, and re-navigating to the game scene.

CELS implements a **production-grade, zero-restart dynamic hot-reload engine**:
- **Subsystem Persistence**: Engine subsystems (SDL windowing, Vulkan device/swapchain, Flecs ECS worlds, Audio devices) live in the host executable (`.exe`) and survive code reloads without restarting.
- **Windows File Lock Evasion**: Completely circumvents the Win32 OS-level DLL file lock using deterministic shadow-copying and PE image validation.
- **Cross-DLL Ambient State Synchronization**: Synchronizes thread-local pointers and ambient sessions across dynamic library boundaries without recompiling the host.
- **Slot Schema Evolution**: Detects user struct size modifications across reloads, automatically resetting modified components while preserving untouched state.

```
+===================================================================================================+
|                                    CELS HOST ENGINE ARCHITECTURE                                  |
+===================================================================================================+
|                                                                                                   |
|  HOST EXECUTABLE (.exe)                                                                           |
|  +---------------------------------------------------------------------------------------------+  |
|  | CelsEngine                                                                                  |  |
|  |  |-- Subsystem Modules (SDL3, Vulkan, Flecs, Audio) [Survives Reloads]                      |  |
|  |  `-- CelsSession (Slab Memory, Slot Table, Reactive Registry) [Survives Reloads]           |  |
|  +---------------------------------------------------------------------------------------------+  |
|                                                  |                                                |
|                        LoadLibraryA()            | Shadow Copy File                               |
|                        (Lock Evasion)            v                                                |
|  DYNAMIC APPLICATION LIBRARY (.dll)              |                                                |
|  +--------------------------------------------+  |                                                |
|  | app.hot_18420_1_2.tmp.dll                  |<--+ (Copied from game_app.dll)                    |
|  |  |-- CelsGetAppDef()                       |                                                  |
|  |  |-- def->setSession(session)              | (Synchronizes TLS ambient session pointer)       |
|  |  |-- def->onStart(engine, session)         | (Attaches root compositions via cel_attach)      |
|  |  `-- Declarative UI & Gameplay Logic       |                                                  |
|  +--------------------------------------------+                                                   |
+===================================================================================================+
```

---

## 2. Monolithic vs. Dynamic Dual-Target Compilation

CELS supports two distinct compilation modes via the `CELS_HOT_RELOAD` preprocessor switch:

| Mode | `CELS_HOT_RELOAD` | Target Binary | Behavior |
|---|---|---|---|
| **Monolithic Release** | `0` | Single `.exe` | Application code links statically into the executable. `CelsAppCheckReload` compiles to a no-op (`return false`). Zero overhead, zero dynamic symbols, deterministic shipping binary. |
| **Dynamic Debug** | `1` | `.exe` + `.dll` | Host engine compiles as a shell or multi-app orchestrator. Application logic compiles into one or more reloadable shared libraries. Polls file timestamps and hot-reloads on disk writes. |

### 2.1 The Application Definition Handshake (`CelsAppDef`)

Communication across the host-module boundary is governed by an ABI-stable application definition declared via `CEL_App` or `CEL_App_Def`:

```c
struct CelsAppDef {
    uint32_t version;                               /**< App definition version (defaults to 1) */
    const char *name;                               /**< Application identifier / display name */
    void (*setSession)(CelsSession *s);             /**< Internal session synchronization across DLL boundary */
    void (*onStart)(CelsEngine *engine, CelsSession *session);  /**< Setup callback: attach compositions via cel_attach */
    void (*onReload)(CelsEngine *engine, CelsSession *session); /**< Optional callback fired after code hot-swap */
    void (*onEnd)(CelsEngine *engine, CelsSession *session);    /**< Teardown callback on shutdown / unload */
};

#define CELS_APP_ENTRY_SYMBOL "CelsGetAppDef"
#define CELS_APP_LEGACY_ENTRY_SYMBOL "CelsGetAppManifest"
typedef const CelsAppDef *(*CelsAppEntryFn)(void);
typedef struct CelsAppDef CelsAppDef;
typedef struct CelsAppDef CelsAppManifest; /* Backwards compatibility alias */
```

---

## 3. Windows DLL File Lock Evasion via Shadow Copying

On Microsoft Windows, invoking `LoadLibraryA("game_app.dll")` causes the Windows NT kernel to map the file into process virtual memory with a shared read lock. As long as the handle remains open, any attempt by the compiler/linker (MSVC `link.exe` or Clang `lld-link`) to overwrite `game_app.dll` fails immediately with:

```
fatal error LNK1104: cannot open file 'game_app.dll'
```

### 3.1 The Shadow Copy Pipeline

CELS evades this OS lock entirely by never loading `game_app.dll` directly. Instead, it implements a 4-step shadow copy pipeline:

```
[ Disk: game_app.dll ]
         |
         | 1. Exclusive Read Probe (CreateFileA, share mode 0)
         v
[ Validated & Inactive? ]
         |
         | 2. Valid PE Image Check (IsValidPEImage)
         v
[ Shadow Copy: game_app.hot_<PID>_<instanceId>_<reloadCount>.tmp.dll ]
         |
         | 3. Copy PDB Symbols: game_app.hot_<PID>_<instanceId>_<reloadCount>.tmp.pdb
         v
[ PlatformLoadLibrary(shadowPath) ]
```

### 3.2 Step 1: Linker Write Verification (`IsPathReadable`)

When a developer triggers a compile, the linker writes `game_app.dll` incrementally. If the host attempts to copy the file while the linker is still writing, it copies a corrupt, partial binary.

CELS verifies that the linker has closed the file handle by attempting an exclusive open:

```c
static bool IsPathReadable(const char *path)
{
#if defined(_WIN32)
    /* Open with sharing mode 0 (exclusive) to verify linker has closed the file */
    HANDLE h = CreateFileA(path,
                           GENERIC_READ,
                           0, /* Zero sharing: fails if linker holds handle */
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false; /* Linker is actively writing! Back off and retry next tick */
    }
    CloseHandle(h);
    return true;
#else
    return access(path, R_OK) == 0;
#endif
}
```

### 3.3 Step 2: Portable Executable (PE) Image Validation (`IsValidPEImage`)

Even if closed, anti-virus scanners or incremental linkers may leave an incomplete file header. CELS inspects the PE headers directly:

```c
static bool IsValidPEImage(const char *path)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD sizeLow = GetFileSize(h, NULL);
    if (sizeLow < 4096) { CloseHandle(h); return false; }

    IMAGE_DOS_HEADER dosHeader;
    DWORD bytesRead = 0;
    if (!ReadFile(h, &dosHeader, sizeof(dosHeader), &bytesRead, NULL) ||
        bytesRead != sizeof(dosHeader) ||
        dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(h);
        return false;
    }

    SetFilePointer(h, dosHeader.e_lfanew, NULL, FILE_BEGIN);
    IMAGE_NT_HEADERS ntHeaders;
    if (!ReadFile(h, &ntHeaders, sizeof(ntHeaders), &bytesRead, NULL) ||
        bytesRead != sizeof(ntHeaders) ||
        ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(h);
        return false;
    }

    /* Verify section table and non-zero virtual image size */
    if (ntHeaders.FileHeader.NumberOfSections == 0 ||
        ntHeaders.OptionalHeader.SizeOfImage == 0) {
        CloseHandle(h);
        return false;
    }

    CloseHandle(h);
    return true;
#else
    return true;
#endif
}
```

### 3.4 Step 3: Atomic Shadow Copy & Multi-DLL Isolation

Once verified, the file is copied to a unique PID-, instance-, and iteration-stamped shadow path:

```c
snprintf(candidatePath, sizeof(candidatePath),
         "%.440s.hot_%lu_%u_%u.tmp.dll",
         app->originalPath,
         (unsigned long)GetCurrentProcessId(),
         app->instanceId,
         nextReload);
PlatformCopyFile(app->originalPath, candidatePath);

/* Also copy .pdb file if present to preserve Visual Studio / RemedyBG debugger symbols */
char pdbSrc[CELS_PATH_MAX], pdbDst[CELS_PATH_MAX];
snprintf(pdbSrc, sizeof(pdbSrc), "%s", app->originalPath);
char *dot = strrchr(pdbSrc, '.');
if (dot != NULL) {
    *dot = '\0';
    snprintf(pdbDst, sizeof(pdbDst), "%.440s.hot_%lu_%u_%u.tmp.pdb",
             pdbSrc, (unsigned long)GetCurrentProcessId(), app->instanceId, nextReload);
    strncat(pdbSrc, ".pdb", sizeof(pdbSrc) - strlen(pdbSrc) - 1u);
    if (IsPathReadable(pdbSrc)) {
        PlatformCopyFile(pdbSrc, pdbDst);
    }
}
```

Because the OS loads `candidatePath` instead of `originalPath`, **`game_app.dll` is never locked**. Developers can recompile continuously while the host is running. Furthermore, the inclusion of `app->instanceId` ensures that multiple host applications (`CelsApp`) running in the same process never collide on shadow filenames or share Windows dynamic loader ref-counts.

### 3.5 Step 4: Graceful File Cleanup

When hot-reloading occurs, the previous shadow file cannot be immediately deleted because Windows still holds it in memory. CELS solves this by:
1. Loading the new shadow library.
2. Swapping the active function pointers and recomposing.
3. Unloading the old dynamic library via `FreeLibrary`.
4. Deleting the old shadow `.tmp.dll` and `.tmp.pdb` files.
5. Sweeping any orphaned `.tmp.*` files on startup and shutdown using `PlatformCleanupHotReloadFiles`, `atexit`, and Win32 `SetConsoleCtrlHandler`.

---

## 4. Cross-Boundary State Synchronization

When code is split across a host executable and a dynamic library on Windows:
- Each binary has its own copy of C Runtime (CRT) state and **Thread-Local Storage (TLS)** variables.
- The host's `s_currentSession` thread-local variable is **not** shared with the DLL.

CELS bridges this gap via the application definition's `setSession` callback:

```c
/* Generated inside the DLL by CEL_App or CEL_App_Def macro: */
static void _cels_app_set_session_MyApp(CelsSession *session) {
    CelsSetCurrentSession(session);
}

CELS_APP_EXPORT const CelsAppDef *CelsGetAppDef(void) {
    static const CelsAppDef def = {
        .name = "MyApp",
        .setSession = _cels_app_set_session_MyApp,
        .onStart = MyApp_OnStart,
        .onReload = MyApp_OnReload,
        .onEnd = MyApp_OnEnd
    };
    return &def;
}
```

Immediately after loading the dynamic module:
```c
if (app->module.def->setSession != NULL) {
    app->module.def->setSession(app->session); /* Passes host session pointer into DLL's TLS */
}
```

This guarantees that macro calls like `cel_remember`, `cel_watch`, and `cel_mutate` inside the DLL resolve to the host's existing `CelsSession` slab with zero configuration.

---

## 5. Slot Schema Evolution on Struct Resize

During active development, programmers frequently alter component state structs:
- Adding a new field (e.g. adding `float maxArmor;` to `PlayerState`).
- Removing an obsolete field.
- Changing an integer to a float.

In typical reflection or serialization systems, changing a struct size causes memory corruption, offset mismatches, or crashes.

CELS implements **automatic schema evolution** in `CelsResolveSlot`:

### 5.1 The `userSize` Invariant

In `CelsSlotAllocation`:
```c
typedef struct CelsSlotAllocation {
    uint32_t groupId;      /* Owning group identity */
    uint32_t slotOffset;   /* Offset relative to group */
    uint32_t arenaOffset;  /* Absolute byte offset into s->dataArena */
    uint16_t size;         /* 8-byte aligned allocation size */
    uint16_t userSize;     /* Exact requested user struct size (sizeof(T)) */
} CelsSlotAllocation;
```

### 5.2 Mismatch Detection & Graceful Reset

During recomposition following a hot-reload, `CelsResolveSlot` checks whether the newly compiled struct size matches `slot->userSize`:

```c
if (slot->groupId == (uint32_t)group->userData && slot->slotOffset == s->currentSlotOffset) {
    s_slotHint = idx + 1;

    /* Check if the user struct size changed across reload */
    if (slot->userSize != (uint16_t)size) {
        fprintf(stderr,
                "[CELS HOT-RELOAD] Struct size changed for group 0x%016llX (was %u B, now %zu B). "
                "Resetting component to initial state.\n",
                (unsigned long long)group->key, (unsigned)slot->userSize, size);

        /* 1. Fire cleanups for old resources */
        FireCleanupsForGroup(s, (uint32_t)group->userData);

        /* 2. Reclaim old slot memory in arena */
        ReleaseSlotsForGroup(s, (uint32_t)group->userData);

        /* 3. Re-mount component fresh with new struct size and initial values */
        group->flags |= CELS_FLAG_FRESH_MOUNT;
        return CelsResolveSlot(s, size, initVal);
    }

    s->currentSlotOffset += (uint32_t)alignedSize;
    return &s->dataArena[slot->arenaOffset];
}
```

```
Scenario A: Struct size unchanged (Logic edit only)
   [ Slot: 32 B ] ========> Retains 100% of state! Pinned address preserved.

Scenario B: Struct size changed (e.g. 32 B -> 48 B)
   [ Old Slot: 32 B ] ====> Cleanups fired, old slot released.
   [ New Slot: 48 B ] ====> Re-allocated with new schema, seeded with initVal.
                            Application continues running without crashing!
```

### 5.3 Non-Destructive Recomposition Trigger (`CelsSessionHotReload`)

When a code reload completes, CELS flags all existing groups for re-evaluation:

```c
void CelsSessionHotReload(CelsSession *s)
{
    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < totalGroups; ++i) {
        CelsSlotGroup *const g = CelsGetGroup(s, i);
        if (g != NULL) {
            g->flags |= CELS_FLAG_INVALIDATED;
            if (i > 0) g->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
            g->flags &= ~CELS_FLAG_FRESH_MOUNT; /* Crucial: preserve existing slots! */
        }
    }
    s->isHotReloadPending = true;
}
```

By ensuring `CELS_FLAG_FRESH_MOUNT` is cleared, all un-resized slots, component positions, entity IDs, and active timers are **100% preserved**. The new code immediately takes over rendering and execution on the very next frame tick.
