# Asynchronous Tasks in CELS

`CEL_Task` provides a lightweight, stackless sequential DSL for asynchronous workflows in pure C99. It lets you write multi-frame logic—such as network connections, cutscenes, fade animations, or asset streaming—as clean, procedural C code, backed by CELS's reactive slot table and lifecycle system.

---

## 1. Overview & Motivation

In traditional declarative C, multi-frame operations require manual state machines with integer step flags:

```c
/* Without CEL_Task: Fragile, manual state machines */
typedef struct ConnectState { int stage; uint64_t timer; } ConnectState;
ConnectState *s = cel_remember(ConnectState, {0});
if (s->stage == 0) {
    OpenSocket();
    s->stage = 1;
} else if (s->stage == 1) {
    if (TimeElapsed(s->timer)) s->stage = 2;
}
```

With `CEL_Task`, the same workflow is written sequentially:

```c
CEL_Task(ConnectTask, const char*, server, int, port) {
    cancel {
        CloseSocket();
    }
    run {
        printf("Step 1: Connecting to %s:%d...\n", server, port);
        cel_yield();

        printf("Step 2: Sending handshake...\n");
        cel_wait(200); /* 200ms non-blocking delay */

        printf("Step 3: Connected!\n");
    }
}
```

---

## 2. Core Concepts

### A. The Two Phases: `run` and `cancel`
Every task is composed of two distinct blocks:
- **`run { ... }`**: The main sequential body. Contains statements interspersed with `cel_yield()`, `cel_wait(ms)`, and `cel_cancel()`. When execution reaches the end of the block, the task automatically marks itself complete (`cel_is_task_done`).
- **`cancel { ... }`**: The resource cleanup handler. **Guaranteed** to execute whenever the task is cancelled, interrupted, or unmounted from the composable tree.

```c
CEL_Task(DownloadFileTask, const char*, url) {
    cancel {
        /* Always executed on cancellation or tree unmount */
        printf("Download cancelled for %s, deleting temporary buffer.\n", url);
        FreeDownloadBuffer();
    }

    run {
        AllocateDownloadBuffer();
        cel_yield();

        while (!DownloadChunk()) {
            cel_yield(); /* Yield each frame until download finishes */
        }

        printf("Download complete!\n");
    }
}
```

---

## 3. Primitives Reference

| Macro | Description |
| :--- | :--- |
| `cel_task(Name, ...)` | Mounts or resumes the task in the current composable slot group. |
| `cel_yield()` | Pauses task execution for the current frame; automatically schedules resumption on the next frame's recompose pass. |
| `cel_wait(ms)` | Suspends the task for the specified duration in milliseconds. Non-blocking; skips execution until the deadline passes. |
| `cel_cancel()` | Cancels the task from *inside* `run` (e.g. on socket error or timeout). Immediately triggers `cancel { ... }` and exits. |
| `cel_cancel_task(Name)` | Cancels the task from *outside* (e.g. from an input event or parent composable). |
| `cel_is_task_running(Name)` | Returns `true` while the task is actively executing steps. |
| `cel_is_task_done(Name)` | Returns `true` once the `run` block has fully completed. |
| `cel_restart_task(Name)` | Resets the step pointer to 0 so the task can run again. |

---

## 4. Cancellation Guarantees

Tasks provide strict lifecycle guarantees through three distinct cancellation mechanisms:

```
                            [ Task Active in Tree ]
                                       │
            ┌──────────────────────────┼──────────────────────────┐
            ▼                          ▼                          ▼
  [ 1. Self-Cancel ]         [ 2. External Cancel ]     [ 3. Structural Unmount ]
   cel_cancel()               cel_cancel_task(Name)      Parent composable skips task
   (e.g. socket failed)       (e.g. user pressed [X])    (e.g. dialog / tab closed)
            │                          │                          │
            └──────────────────────────┼──────────────────────────┘
                                       ▼
                       [ ALWAYS Runs cancel { ... } ]
                                       ▼
                       [ Slots Cleaned & Group Pruned ]
```

### 1. Structural / Unmount Cancellation
If the parent composable stops invoking `cel_task(...)` (for instance, the user navigates away or toggles off a feature):
```c
if (net->isConnecting) {
    cel_task(NetworkConnectTask, "game.server", 7777);
}
```
When `net->isConnecting` becomes `false`, CELS's slot-table reconciliation detects that the task group is absent from the current pass. CELS automatically executes the task's `cancel { ... }` block before purging the slot memory.

### 2. External Cancellation
Any code in the same session can cancel an active task explicitly:
```c
if (ButtonPressed("Disconnect")) {
    cel_cancel_task(NetworkConnectTask);
}
```

### 3. Self-Cancellation
If an internal failure occurs during task execution:
```c
if (!OpenSocket(server, port)) {
    printf("Failed to open socket! Cancelling...\n");
    cel_cancel(); /* Immediately triggers cancel {} */
}
```

---

## 5. Parameter Passing & Preservation

Tasks can declare up to 4 typed parameters:
- `CEL_Task(Name)` (0 parameters)
- `CEL_Task(Name, Type1, Arg1)` (1 parameter)
- `CEL_Task(Name, Type1, Arg1, Type2, Arg2)` (2 parameters)

Parameters are stored alongside the task state in the session's slot table (`cel_remember`). This guarantees that if a task unmounts while the parent composable is no longer running, the `cancel { ... }` block still receives the exact original parameters passed on mount.

---

## 6. Frame Scheduling & Auto-Invalidation

In CELS, composables without mutated dependencies are skipped in O(1) time during recomposition.

When a task calls `cel_yield()` or `cel_wait(ms)`:
1. It registers its 64-bit key into the session's `nextFrameQueue`.
2. The current recomposition frame completes cleanly without looping.
3. At the end of the frame, `nextFrameQueue` is staged into the session's `invalidationQueue`.
4. On the next `CelsSessionRecompose`, the session automatically descends into the task to execute its next step.
5. Once the task finishes (`isDone = true`), it stops scheduling invalidations, allowing the tree to return to quiet O(1) steady-state skipping.

---

## 7. Platform Portability (Windows, Linux, macOS)

CELS tasks and threading primitives (`include/cels/thread.h`) are written in 100% portable C99:

- **Monotonic Clock (`CelsGetTimeMs`)**:
  - Windows: `QueryPerformanceCounter` via `QueryPerformanceFrequency`.
  - Linux / macOS: `clock_gettime(CLOCK_MONOTONIC)`.
- **Thread-Local Storage (`CELS_THREAD_LOCAL`)**:
  - MSVC: `__declspec(thread)`.
  - GCC / Clang on Linux & macOS: Native `__thread` (no external `libwinpthread` runtime dependency).
- **Threading Primitives (`CelsThread`, `CelsMutex`)**:
  - Windows: Native Win32 threads (`CreateThread`, `CRITICAL_SECTION`).
  - Linux / macOS: POSIX threads (`pthread_create`, `pthread_mutex_t`).
