# CELS Architecture: Stackful Cooperative Fibers & Coroutine Mechanics

## 1. Architectural Motivation & Stackful vs. Stackless Analysis

In games, simulations, and interactive user interfaces, many operations are naturally multi-frame and sequential:
- Asset loading and streaming pipelines.
- Network handshakes and authentication sequences.
- Scripted cutscenes, dialog flows, and tutorial steps.
- Procedural animations, fade transitions, and camera shakes.

### 1.1 The Failure of Stackless Switch-Case Coroutines (Duff's Device)

Many C libraries attempt to implement coroutines using the "stackless" Duff's Device macro pattern (`#define yield case __LINE__: ...`). While lightweight, stackless coroutines impose severe restrictions:
1. **No Local Stack Variables**: Local variables declared inside the coroutine do not persist across yields; all state must be manually hoisted into structs.
2. **Broken Control Flow**: Yield macros cannot be placed inside nested switch statements, as the internal `case` label collides with the switch.
3. **No Call Stack Preservation**: You cannot yield from inside a nested helper subroutine; yielding is strictly limited to the top-level macro function.

### 1.2 The CELS Stackful Solution

CELS implements **true stackful cooperative fibers**:
- **Windows**: Native Win32 Fiber subsystem (`ConvertThreadToFiber`, `CreateFiberEx`, `SwitchToFiber`, `DeleteFiber`).
- **POSIX (Linux / macOS)**: Direct `ucontext_t` machine context switching (`getcontext`, `makecontext`, `swapcontext`).
- **Dedicated Fiber Stack**: Each task possesses its own dedicated stack ($4\text{ KiB}$ initial commit, $64\text{ KiB}$ reserved address space).
- **Full Call-Stack Preservation**: Local variables on the C stack, nested function calls, recursive routines, loops, and switches survive intact across `cel_yield()` and `cel_wait(ms)`.

```
Main Thread Execution:
[ Session Frame Loop ]
        |
        |--- CelsTaskStep()
        |          |
        |          | (Saves thread registers, switches SP/IP)
        |          v
        |    [ Task Fiber Stack ]
        |    - Local variables intact
        |    - Nested call: ResolveHost()
        |    - cel_wait(500)
        |          |
        |          | (Saves fiber registers, restores thread SP/IP)
        |          v
        |<-- Returns to Session Frame Loop (Zero Main Thread Blocking!)
        |
[ Next Frame / Timer Poll ]
```

---

## 2. Platform Fiber Implementation & ABI Mechanics

The cooperative fiber engine is implemented in `src/platform/fiber.c` and exposed through `include/cels/runtime/fiber.h`.

### 2.1 The Fiber Abstraction

```c
typedef void (*CelsFiberFn)(void *param);

typedef struct CelsFiber CelsFiber;

bool       CelsFiberInitThread(void);
CelsFiber *CelsFiberCreate(CelsFiberFn fn, void *param, size_t commitSizeBytes, size_t reserveSizeBytes);
void       CelsFiberSwitch(CelsFiber *targetFiber);
void       CelsFiberDestroy(CelsFiber *fiber);
CelsFiber *CelsFiberGetCurrent(void);
```

### 2.2 Win32 Native Fibers (`_WIN32`)

On Windows, CELS binds directly to the OS kernel fiber subsystem:

```c
struct CelsFiber {
    void *nativeFiber;    /* LPVOID returned by CreateFiberEx */
    bool  isThreadFiber;  /* True if host thread converted to fiber */
};

static CELS_THREAD_LOCAL struct CelsFiber s_threadFiber = {0};
static CELS_THREAD_LOCAL bool s_threadFiberInitialized = false;

bool CelsFiberInitThread(void)
{
    if (s_threadFiberInitialized) return true;

    void *fiber = NULL;
    if (!IsThreadAFiber()) {
        fiber = ConvertThreadToFiber(NULL);
    } else {
        fiber = GetCurrentFiber();
    }
    s_threadFiber.nativeFiber = fiber;
    s_threadFiber.isThreadFiber = true;
    s_threadFiberInitialized = (fiber != NULL);
    return s_threadFiberInitialized;
}
```

#### Fiber Creation & Floating Point State:
```c
CelsFiber *CelsFiberCreate(CelsFiberFn fn, void *param, size_t commitSizeBytes, size_t reserveSizeBytes)
{
    if (!CelsFiberInitThread()) return NULL;

    CelsFiber *fiber = (CelsFiber*)malloc(sizeof(CelsFiber));
    fiber->isThreadFiber = false;

    DWORD flags = 0;
#if defined(FIBER_FLAG_FLOAT_SWITCH)
    flags = FIBER_FLAG_FLOAT_SWITCH; /* Preserve SSE/AVX floating point state */
#endif

    fiber->nativeFiber = CreateFiberEx(commitSizeBytes, reserveSizeBytes, flags,
                                      (LPFIBER_START_ROUTINE)fn, param);
    return fiber;
}
```

#### Low-Latency Switch:
`SwitchToFiber(fiber->nativeFiber)` executes a register context swap:
- Saves caller RSP, RBP, RBX, R12-R15, and SSE control words.
- Updates TEB (Thread Environment Block) stack limits to match the target fiber stack.
- Restores target RSP and jumps to target RIP.
- **Latency**: Approximately **$25 \sim 40\text{ CPU cycles}$** (far faster than an OS thread context switch, which takes $1{,}000 \sim 3{,}000\text{ cycles}$).

---

### 2.3 POSIX `ucontext_t` Architecture (Linux & macOS)

On POSIX platforms, CELS manages fiber contexts and stacks manually:

```c
struct CelsFiber {
    ucontext_t ctx;        /* Machine context structure (registers, signal masks) */
    void      *stack;      /* Heap-allocated stack memory buffer */
    size_t     stackSize;  /* Allocated stack capacity in bytes */
    bool       isThreadFiber;
};
```

#### Context Initialization & Trampoline:
When creating a fiber on POSIX, a 64-bit parameter must be passed across `makecontext`'s 32-bit `int` parameters. CELS utilizes a split-word trampoline:

```c
static void CelsFiberTrampoline(uint32_t low, uint32_t high)
{
    const uintptr_t ptr = (uintptr_t)low | ((uintptr_t)high << 32);
    CelsFiberThunk *const thunk = (CelsFiberThunk*)ptr;
    if (thunk != NULL) {
        CelsFiberFn fn = thunk->fn;
        void *param = thunk->param;
        free(thunk);
        if (fn != NULL) fn(param);
    }
    s_currentFiber = &s_threadFiber;
}
```

Switching occurs via `swapcontext(&prev->ctx, &targetFiber->ctx)`.

---

## 3. Persistent Task State & Memory Geometry

Every task instantiated via `cel_task(...)` retains a persistent state structure inside the session's nonmoving data arena via `cel_remember`:

```c
typedef struct CelsTaskState {
    int      step;          /* Execution step / state code (0 = init, -1 = done, -2 = cancelled) */
    uint64_t waitTimerMs;   /* Monotonic timestamp deadline for cel_wait */
    bool     isRunning;     /* True while task is active */
    bool     isCancelled;   /* True if task was aborted */
    bool     isDone;        /* True if run block completed */
    void    *taskFiber;     /* CelsFiber* executing procedural body */
    void    *callerFiber;   /* CelsFiber* representing the session caller */
} CelsTaskState;
```

```
Byte Offset:
 0          4              12       13       14   16                24                32
+----------+---------------+--------+--------+----+-----------------+-----------------+
|   step   |  waitTimerMs  | isRun  | isCanc |isDn|    taskFiber    |   callerFiber   |
| (int32)  |   (uint64)    | (bool) | (bool) |(b) |  (CelsFiber*)   |  (CelsFiber*)   |
+----------+---------------+--------+--------+----+-----------------+-----------------+
|<--- Control Metadata --->|<------ Flags ------>|<--------- Fiber Context ---------->|
```

### Parameter Preservation (`_CelsTaskBox_##Name`)

When a task declares arguments (e.g. `CEL_Task(ConnectTask, const char*, host, int, port)`), CELS wraps the state and arguments in a contiguous box struct:

```c
typedef struct _CelsTaskBox_ConnectTask {
    CelsTaskState state;
    const char *host;
    int port;
} _CelsTaskBox_ConnectTask;
```

Because `_CelsTaskBox` is allocated in the session's nonmoving arena via `cel_remember`:
- Arguments are stored **by value** in pinned memory.
- If the task unmounts unexpectedly, the arguments are still preserved and valid when the `cancel { ... }` block executes.

---

## 4. Frame Stepping & Non-Blocking Timer Queues

The interaction between the frame loop and a task is mediated by `CelsTaskStep`:

```c
void CelsTaskStep(CelsSession *session, CelsTaskState *state, uint64_t key,
                  CelsFiberFn fiberFn, void *param)
{
    if (state == NULL || state->isCancelled || state->isDone) {
        return;
    }

    /* 1. Timer Poll Check */
    if (CelsTaskShouldWait(session, state, key)) {
        return; /* Deadline has not expired; do not switch into fiber */
    }

    CelsFiberInitThread();

    /* 2. Lazy Fiber Allocation */
    if (state->taskFiber == NULL) {
        state->taskFiber = CelsFiberCreate(fiberFn, param, 4096, 65536);
        if (state->taskFiber == NULL) return;
    }

    /* 3. Switch into Task Fiber */
    state->callerFiber = CelsFiberGetCurrent();
    state->isRunning = true;
    if (session != NULL) {
        session->isExecutingTask = true; /* Permits cel_mutate calls inside tasks */
    }

    CelsFiberSwitch((CelsFiber*)state->taskFiber);

    if (session != NULL) {
        session->isExecutingTask = false;
    }

    /* 4. Cleanup if Fiber Completed or Cancelled */
    if (state->isDone || state->isCancelled) {
        if (state->taskFiber != NULL) {
            CelsFiberDestroy((CelsFiber*)state->taskFiber);
            state->taskFiber = NULL;
        }
    }
}
```

### 4.1 Monotonic Timer Poll Mechanics (`cel_wait`)

When a task executes `cel_wait(100)`:
1. `CelsTaskWait` computes deadline: `state->waitTimerMs = CelsGetTimeMs() + 100`.
2. It calls `CelsSessionInvalidateKey(session, key)`, scheduling the task for re-evaluation next frame.
3. It switches back to `state->callerFiber`.
4. On subsequent frames:
   ```c
   bool CelsTaskShouldWait(CelsSession *session, CelsTaskState *state, uint64_t key)
   {
       if (state->waitTimerMs > 0) {
           const uint64_t now = CelsGetTimeMs();
           if (now < state->waitTimerMs) {
               if (session != NULL) {
                   CelsSessionInvalidateKey(session, key); /* Re-queue for next frame */
               }
               return true; /* Fiber is NOT resumed */
           }
           state->waitTimerMs = 0; /* Deadline reached! */
       }
       return false;
   }
   ```
5. While waiting, `CelsTaskStep` returns immediately without executing a fiber context switch, saving CPU registers and cache state.

---

## 5. Lifecycle Guarantees & Unmount Teardown

A core strength of CELS tasks is the **unmount cancellation guarantee**.

### 5.1 The Unmount Hazard in Conventional Game Engines

In standard engines, if a coroutine or async promise is launched (e.g. `DownloadFileAsync`), and the user closes the menu screen or exits to the lobby, the coroutine continues executing in the background. When it attempts to update UI widgets that have been destroyed, it triggers a null-pointer dereference or use-after-free crash.

### 5.2 CELS Deterministic Cancellation

In CELS, tasks are mounted as nodes in the slot table. If the parent composable stops invoking `cel_task(...)`:
1. `CelsExitGroup` detects that the task's group was omitted.
2. `CelsPruneSubtree` runs and encounters the task's cleanup hook registered via `CelsSessionRegisterLifecycle`.
3. The generated cleanup handler executes:
   ```c
   static void _cels_task_clean_MyTask(void *instance, CelsSession *session)
   {
       _CelsTaskBox_MyTask *box = (_CelsTaskBox_MyTask*)instance;
       if (box != NULL && !box->state.isDone && !box->state.isCancelled) {
           CelsTaskCancel(&box->state);

           /* Execute the cancel block in the main thread context */
           if (session != NULL) session->isExecutingTask = true;
           _cels_task_body_MyTask(CEL_TASK_PHASE_CANCEL, &box->state, key, box->Arg1);
           if (session != NULL) session->isExecutingTask = false;
       }

       /* Immediately delete the OS fiber and free its stack */
       if (box != NULL) {
           CelsTaskCleanup(&box->state);
       }
   }
   ```
4. The `cancel { ... }` block executes with guaranteed access to its original parameters.
5. The fiber stack is deleted, terminating execution immediately. No zombie tasks can ever outlive their composable parent.
