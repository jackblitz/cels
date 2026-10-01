# CELS API Reference & Quick Cheat Sheet

> **CELS (Composition, Evaluation, Lifecycle, State)**  
> High-performance, cache-aligned declarative composition engine for C99.  
> Single-page exhaustive lookup table of all DSL macros, runtime operations, and engine plumbing APIs.

---

## Allowed Calling Scopes Legend

| Scope Token | Meaning & Execution Context |
| :--- | :--- |
| **`FILE`** | File scope (global/header declarations outside any function body). |
| **`COMPOSABLE`** | Inside an active `CEL_Composable` function body during recomposition pass. |
| **`COMPOSITION`** | Inside a root `CEL_Composition` function body during recomposition pass. |
| **`TASK_RUN`** | Inside the `run { ... }` block of a `CEL_Task` (cooperative fiber context). |
| **`TASK_CANCEL`** | Inside the `cancel { ... }` teardown block of a `CEL_Task`. |
| **`LIFECYCLE`** | Inside the `mount { ... }` or `unmount { ... }` blocks of a `CEL_Lifecycle`. |
| **`PREDICATE`** | Inside a `CEL_Evaluate` lifecycle evaluation predicate function callback. |
| **`HOST_LOOP`** | Host application tick loop, window event loop, or main thread initialization. |
| **`ANY_THREAD`** | Thread-safe; safe to invoke concurrently from any application or worker thread. |

---

## Tier 1: `CEL_` Structural Declarative DSL

Macros used to declare types, structural composition nodes, tasks, lifecycles, and persistent subsystems.

| Macro / Symbol | Exact Signature / Arity | Allowed Scope | Return / Expansion | Description |
| :--- | :--- | :--- | :--- | :--- |
| [`CEL_State`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L108) | `CEL_State(TypeName)` | `FILE` | `typedef struct TypeName ...; struct TypeName` | Declares a double-buffered reactive state structure type. |
| [`CEL_Module`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L136) | `CEL_Module(TypeName)` | `FILE` | `typedef struct TypeName ...; struct TypeName` | Declares an engine subsystem module struct surviving code reload. |
| [`CEL_Composition`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L179) | `CEL_Composition(Name)`<br>`CEL_Composition(Name, ArgDecl)`<br>`CEL_Composition(Name, Type, Param)` | `FILE` | `void Name(void *cels_userData)`<br>`void Name(ArgDecl)`<br>`void Name(Type Param)` | Declares a root composition entry point attached to a session. 1-arg form eliminates unused parameter warnings. |
| [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L370) | `CEL_Composable(Name, ...)`<br>*(0 to 4 typed parameter pairs)* | `FILE` | `static inline void Name(...)` + internal body | Declares a single-file or header-only child composable with automatic slot caching. |
| [`CEL_ComposableDef`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L485) | `CEL_ComposableDef(Name, ...)`<br>*(0 to 4 typed parameter pairs)* | `FILE` | `void Name(...)` definition + internal body | Implements a non-static, linkable composable in a `.c` file. Pair with standard C `void Name(...);` in headers. |
| [`CEL_ComposableDecl`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L391) | `CEL_ComposableDecl(Name, ...)` | `FILE` | `void Name(...)` prototype | (Optional) Declares an external composable prototype in a header file. Standard C `void Name(...);` is preferred. |
| [`CEL_Lifecycle`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L614) | `CEL_Lifecycle(Name)`<br>`CEL_Lifecycle(Name, ParamDecl)`<br>`CEL_Lifecycle(Name, Type, Param)` | `FILE` | Static implementation & attach wrappers | Declares a reusable resource controller with mount and unmount blocks. |
| [`mount`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L515) | `mount { ... }` | `LIFECYCLE` | Conditional block statement | Encloses resource acquisition logic executed when node is mounted. |
| [`unmount`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L528) | `unmount { ... }` | `LIFECYCLE` | Conditional block statement | Encloses teardown logic executed when node leaves composition tree. |
| [`CEL_Task`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1295) | `CEL_Task(Name, ...)`<br>*(0, 1, or 2 typed parameter pairs)* | `FILE` | Inline composable + fiber state machine | Declares a multi-frame cooperative coroutine task with cleanup. |
| [`run`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1070) | `run { ... }` | `TASK_RUN` | Conditional coroutine body | Encloses step-by-step procedural logic inside a cooperative fiber. |
| [`cancel`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1048) | `cancel { ... }` | `TASK_CANCEL` | Scoped cleanup block with jump entry | Encloses cancellation and unmount teardown logic for a task. |
| [`CEL_Evaluate`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L213) | `CEL_Evaluate(Predicate, ctx)` | `PREDICATE`, `HOST_LOOP` | `bool` | Evaluates whether a composition branch should mount or execute. |
| [`CEL_Evaluation`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L240) | `CEL_Evaluation(Name)`<br>`CEL_Evaluation(Name, ArgDecl)`<br>`CEL_Evaluation(Name, Type, Param)` | `FILE` | `bool Name(void *cels_evalCtx)`<br>`bool Name(ArgDecl)` | Declares a lifecycle evaluation predicate function. 1-arg form eliminates unused context warnings. |
| [`CEL_EvaluateFn`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L242) | `CEL_EvaluateFn(...)` | `FILE` | Alias for `CEL_Evaluation` | Alias for `CEL_Evaluation`. |
| [`CEL_OnStart`](file:///D:/cels-workspace/library/cels/include/cels/app.h#L73) | `CEL_OnStart(Name)` | `FILE` | `static void Name(CelsEngine*, CelsSession*)` | Declares an application startup lifecycle callback without unused parameter warnings. |
| [`CEL_OnEnd`](file:///D:/cels-workspace/library/cels/include/cels/app.h#L89) | `CEL_OnEnd(Name)` | `FILE` | `static void Name(CelsEngine*, CelsSession*)` | Declares an application teardown lifecycle callback without unused parameter warnings. |
| [`CEL_RegisterModule`](file:///D:/cels-workspace/library/cels/include/cels/engine.h#L333) | `CEL_RegisterModule(Type, ptr)`<br>`CEL_RegisterModule(target, Type, ptr)` | `HOST_LOOP` | `void` | Registers an engine subsystem module binding with host or session. |
| [`CEL_RegisterModuleWithHooks`](file:///D:/cels-workspace/library/cels/include/cels/engine.h#L335) | `CEL_RegisterModuleWithHooks(target, Type, ptr, onReload, onDestroy)` | `HOST_LOOP` | `void` | Registers subsystem module with explicit reload and destroy callbacks. |
| [`CEL_GetModule`](file:///D:/cels-workspace/library/cels/include/cels/engine.h#L344) | `CEL_GetModule(Type)`<br>`CEL_GetModule(ctx, Type)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `Type*` | Resolves a registered subsystem module pointer from ambient/target engine. |
| [`CEL_ID`](file:///D:/cels-workspace/library/cels/include/cels/slot_table.h#L64) | `CEL_ID(const char *str)` | `ANY_THREAD` | `uint64_t` (64-bit FNV-1a) | Computes a 64-bit FNV-1a hash key from a string constant. |
| [`CEL_KEY`](file:///D:/cels-workspace/library/cels/include/cels/slot_table.h#L65) | `CEL_KEY(const char *str)` | `ANY_THREAD` | `uint64_t` (64-bit FNV-1a) | Alias for `CEL_ID`. |
| [`CEL_SLAB`](file:///D:/cels-workspace/library/cels/include/cels/session.h#L58) | `CEL_SLAB(name, size)` | `FILE`, `HOST_LOOP` | `__declspec` / `__attribute__` aligned byte array | Declares a 64-byte cache-aligned memory buffer for session slabs. |
| [`CEL_Entity`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1551) | `CEL_Entity(ecs_world_t *w, const char *n, uint64_t k)` | `COMPOSABLE`, `COMPOSITION` | Compound `for` loop binding `it` | Mounts a declarative Flecs ECS entity in slot memory with cleanup. |
| [`CEL_Attach`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L657) | `CEL_Attach(...)` | `HOST_LOOP` | Alias for `cel_attach` | Alias for `cel_attach`. |
| [`CEL_GetState`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L658) | `CEL_GetState(...)` | `COMPOSABLE`, `COMPOSITION` | Alias for `cel_get_state` | Alias for `cel_get_state`. |
| [`CEL_None`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L659) | `CEL_None` | Any | `NULL` | Convenience sentinel representation for null callbacks / context. |

---

## Tier 2: `cel_` Runtime DSL & Composable Operations

Operations invoked inside composable functions, tasks, or event handlers to interact with state, time, and session.

| Operation / Macro | Exact Signature / Arity | Allowed Scope | Return Type | Description |
| [`cel_attach`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L246) | `cel_attach(session, comp)`<br>`cel_attach(session, comp, eval)`<br>`cel_attach(session, comp, eval, userData)`<br>`cel_attach(session, comp, eval, userData, evalCtx)`<br>`cel_attach_keyed(session, id, comp)` | `APP_ON_START`, `HOST_LOOP` | `void` | Attaches a root composition to a session with auto-derived key and optional evaluation predicate. |
| [`cel_state`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L746) | `cel_state(Type)`<br>`cel_state(Type, { .field = val, ... })` | `COMPOSABLE` | `Type*` | Allocates hoisted reactive state pinned to the composable's slot; cleaned on unmount. |
| [`cel_remember`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L797) | `cel_remember(Type, Init)`<br>`cel_remember(Type, Init, OnDestroy)` | `COMPOSABLE`, `COMPOSITION` | `Type*` | Allocates or retrieves private slot memory pinned across frames with cleanup. |
| [`cel_remember_state`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L710) | `cel_remember_state(Type, { .field = val, ... })`<br>`cel_remember_state_keyed(id, Type, { ... })` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `Type*` | Allocates/resolves persistent double-buffered state in ambient session slab (auto-hashes Type name). |
| [`cel_watch`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L848) | `cel_watch(instancePtr)`<br>`cel_watch(Type, id)`<br>`cel_watch(session, Type, id)` | `COMPOSABLE` | `const Type*` | Reads published state snapshot and registers calling composable as observer. |
| [`cel_mutate`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L920) | `cel_mutate(ptr) { this->... }` | `TASK_RUN`, Event Callbacks, Composable Handlers | Scoped block | Modifies state instance back buffer and schedules observers for recomposition. Sessions only mutate their own state! |
| [`cel_event`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1046) | `cel_event(Type, ...)` | `COMPOSABLE`, `TASK_RUN`, Callbacks | `bool` | Emits a local tree event that bubbles up to ancestor composables with zero frame delay. |
| [`cel_listen`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1064) | `cel_listen(Type, var) { ... }` | `COMPOSABLE` | Scoped loop | Iterates over unconsumed local events bubbling from descendants within current frame. |
| [`cel_signal`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1082) | `cel_signal(targetSession, Type, ...)` | `ANY_SCOPE` | `bool` | Sends targeted discrete signal directly into another session's inbox. |
| [`cel_connect`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1099) | `cel_connect(Type, var) { ... }` | `COMPOSABLE` | Scoped loop | Iterates over unconsumed targeted signals received by this session. |
| [`cel_broadcast`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1117) | `cel_broadcast(Type, ...)` | `ANY_THREAD`, `ANY_SCOPE` | `bool` | Publishes global thread-safe engine broadcast distributed across all sessions. |
| [`cel_bind`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1128) | `cel_bind(Type, var) { ... }` | `COMPOSABLE` | Scoped loop | Binds to unconsumed engine-wide broadcasts matching Type. |
| [`cel_wait_for`](file:///D:/cels-workspace/library/cels/include/cels/runtime/task.h#L292) | `cel_wait_for(Type, outPtr)` | `TASK_RUN` | `void` | Suspends task fiber non-blockingly until local event, signal, or broadcast of Type arrives. |
| [`cel_wait_signal`](file:///D:/cels-workspace/library/cels/include/cels/runtime/task.h#L305) | `cel_wait_signal(Type, outPtr)` | `TASK_RUN` | `void` | Suspends task fiber non-blockingly until targeted signal of Type arrives. |
| [`cel_wait_broadcast`](file:///D:/cels-workspace/library/cels/include/cels/runtime/task.h#L318) | `cel_wait_broadcast(Type, outPtr)` | `TASK_RUN` | `void` | Suspends task fiber non-blockingly until engine broadcast of Type arrives. |
| [`cel_wait_for_timeout`](file:///D:/cels-workspace/library/cels/include/cels/runtime/task.h#L336) | `cel_wait_for_timeout(Type, outPtr, timeoutMs)` | `TASK_RUN` | `bool` | Suspends task fiber until event arrives or timeoutMs elapses; returns true if received. |
| [`cel_lifecycle`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L654) | `cel_lifecycle(Name)`<br>`cel_lifecycle(Name, argument)` | `COMPOSABLE` | `void` | Attaches a declared `CEL_Lifecycle` controller instance to the current composable. |
| [`cel_transition`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1495) | `cel_transition(target, durationMs)`<br>`cel_transition(target, durationMs, easing)` | `COMPOSABLE` | `float` | Declaratively eases a scalar value toward target over time with auto-invalidation. |
| [`cel_task`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1153) | `cel_task(Name, ...)` | `COMPOSABLE`, `COMPOSITION` | `void` | Schedules and advances a declared `CEL_Task` inside composition hierarchy. |
| [`cel_yield`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1088) | `cel_yield()` | `TASK_RUN` | `void` | Suspends cooperative task fiber and yields control until next frame pass. |
| [`cel_wait`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1107) | `cel_wait(ms)` | `TASK_RUN` | `void` | Suspends cooperative task fiber for non-blocking monotonic delay in milliseconds. |
| [`cel_cancel`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1123) | `cel_cancel()` | `TASK_RUN` | `void` | Aborts task internally and immediately transitions execution to `cancel` block. |
| [`cel_cancel_task`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1178) | `cel_cancel_task(Name)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `void` | Externally cancels the active instance of a declared task. |
| [`cel_is_task_running`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1195) | `cel_is_task_running(Name)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `bool` | Queries whether a declared task is currently active (started, not done/cancelled). |
| [`cel_is_task_done`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1212) | `cel_is_task_done(Name)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `bool` | Queries whether a declared task has completed its `run` block. |
| [`cel_is_task_cancelled`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1229) | `cel_is_task_cancelled(Name)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `bool` | Queries whether a declared task was cancelled. |
| [`cel_restart_task`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1247) | `cel_restart_task(Name)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `void` | Resets a completed or cancelled task to step 0 and queues it for execution. |
| [`cel_stage_set`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1325) | `cel_stage_set(targetId, Type, ...)` | `COMPOSABLE`, `COMPOSITION`, `TASK_RUN` | `bool` | Stages lockless component/payload assignment in active transaction batch. |
| [`cel_stage_remove`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1354) | `cel_stage_remove(targetId, Type)` | `COMPOSABLE`, `COMPOSITION`, `TASK_RUN` | `bool` | Stages component/tag removal operation in active transaction batch. |
| [`cel_stage_delete`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1379) | `cel_stage_delete(targetId)` | `COMPOSABLE`, `COMPOSITION`, `TASK_RUN` | `bool` | Stages target entity/resource deletion in active transaction batch. |
| [`cel_stage_custom`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1403) | `cel_stage_custom(opCode, targetId, Type, ...)` | `COMPOSABLE`, `COMPOSITION`, `TASK_RUN` | `bool` | Stages custom user-defined transaction opcode and payload in transaction batch. |
| [`cel_user_data`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1434) | `cel_user_data(Type)` | `COMPOSABLE`, `COMPOSITION` | `Type*` | Retrieves user context pointer attached to ambient session. |
| [`cel_session_user_data`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1453) | `cel_session_user_data(session, Type)` | `HOST_LOOP`, `ANY_THREAD` | `Type*` | Retrieves user context pointer attached to explicit session instance. |
| [`cel_get_module`](file:///D:/cels-workspace/library/cels/include/cels/engine.h#L347) | `cel_get_module(Type)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `Type*` | Alias for `CEL_GetModule(Type)`. |
| [`cel_engine_get`](file:///D:/cels-workspace/library/cels/include/cels/engine.h#L348) | `cel_engine_get(Type)` | `COMPOSABLE`, `COMPOSITION`, `HOST_LOOP` | `Type*` | Alias for `CEL_GetModule(Type)`. |
| [`cel_quit`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1013) | `cel_quit()` | `COMPOSABLE`, `HOST_LOOP`, Callbacks | `void` | Requests host engine to terminate frame loop at next boundary. |
| [`cel_engine_quit`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1007) | `cel_engine_quit()` | `COMPOSABLE`, `HOST_LOOP`, Callbacks | `void` | Alias for `cel_quit()`. |
| [`cel_create_session`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L998) | `cel_create_session(engine, name, profile)` | `HOST_LOOP` | `CelsSession*` | Creates and registers a named secondary session supervised by the engine. |
| [`cel_get_session`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1015) | `cel_get_session(engine, name)` | `HOST_LOOP`, `ANY_THREAD` | `CelsSession*` | Retrieves an engine-managed session by name ("main", "root", or secondary). |
| [`cel_session`](file:///D:/cels-workspace/library/cels/include/cels/runtime/session.h#L269) | `cel_session(CEL_Id sessionId)` | `HOST_LOOP`, `ANY_THREAD` | `CEL_Session*` | Retrieves a registered session by its 64-bit identifier. |
| [`cel_active_session`](file:///D:/cels-workspace/library/cels/include/cels/runtime/session.h#L276) | `cel_active_session()` | `COMPOSABLE`, `COMPOSITION` | `CEL_Session*` | Returns current ambient session bound to calling thread. |
| [`cel_recompose_all`](file:///D:/cels-workspace/library/cels/include/cels/runtime/session.h#L391) | `cel_recompose_all()` | `HOST_LOOP` | `CelsResult` | Recomposes all registered active sessions across the runtime. |
| [`cel_print_log`](file:///D:/cels-workspace/library/cels/include/cels/runtime/log.h#L58) | `cel_print_log(level, ...)` | `ANY_THREAD` | `void` | Non-blocking ring-buffer logger capturing `__FILE__` and `__LINE__`. |

---

## Tier 3: `cels_` / `Cels` Engine, Session & Low-Level Plumbing C API

Direct C functions, engine coordinators, slot table dual gap buffers, and internal plumbing.

### 3.1 Host Engine Lifecycle (`include/cels/engine.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsEngineInit` | `CelsResult CelsEngineInit(CelsEngine *engine, const char *appName)` | `HOST_LOOP` | `CelsResult` | Initializes engine runtime, loads app DLL or binds static manifest. |
| `CelsEngineInitWithOptions` | `CelsResult CelsEngineInitWithOptions(CelsEngine *engine, const char *appName, const CelsSessionConfig *config)` | `HOST_LOOP` | `CelsResult` | Initializes engine with explicit session slab/profile configuration. |
| `CelsEngineInitWithProfile` | `CelsResult CelsEngineInitWithProfile(CelsEngine *engine, const char *appName, CelsSessionProfile profile)` | `HOST_LOOP` | `CelsResult` | Initializes engine with named workload capacity profile (e.g. `CELS_PROFILE_1K`). |
| `CelsEngineDestroy` | `void CelsEngineDestroy(CelsEngine *engine)` | `HOST_LOOP` | `void` | Tears down engine, destroying modules, primary session, and app DLL. |
| `CelsEngineStart` | `CelsResult CelsEngineStart(CelsEngine *engine)` | `HOST_LOOP` | `CelsResult` | Starts execution and performs initial composition mount. |
| `CelsEngineEnd` | `void CelsEngineEnd(CelsEngine *engine)` | `HOST_LOOP` | `void` | Stops execution and invokes teardown hooks. |
| `CelsEngineRecompose` | `CelsResult CelsEngineRecompose(CelsEngine *engine)` | `HOST_LOOP` | `CelsResult` | Triggers a recomposition pass on primary session and active sessions. |
| `CelsEngineLoadApp` | `CelsResult CelsEngineLoadApp(CelsEngine *engine, const char *appName)` | `HOST_LOOP` | `CelsResult` | Discovers, shadow-copies, and links app DLL (Debug) or manifest (Release). |
| `CelsEnginePollReload` | `bool CelsEnginePollReload(CelsEngine *engine)` | `HOST_LOOP` | `bool` | Polls disk for modified app binary and hot-reloads symbols without restart. |
| `CelsEngineRegisterModule` | `void CelsEngineRegisterModule(CelsEngine *e, uint64_t k, const char *n, void *inst, void (*onDestroy)(void*))` | `HOST_LOOP` | `void` | Registers a subsystem module binding in engine surviving DLL reloads. |
| `CelsEngineGetModule` | `void *CelsEngineGetModule(const CelsEngine *engine, uint64_t key)` | `HOST_LOOP`, `COMPOSABLE` | `void*` | Retrieves subsystem module instance pointer by 64-bit key. |
| `CelsGetCurrentEngine` | `CelsEngine *CelsGetCurrentEngine(void)` | `HOST_LOOP`, `ANY_THREAD` | `CelsEngine*` | Returns ambient host engine bound to calling thread. |
| `CelsEngineCreateSession` | `CelsSession *CelsEngineCreateSession(CelsEngine *engine, const char *name, CelsSessionProfile profile)` | `HOST_LOOP` | `CelsSession*` | Creates and registers named secondary session supervised by engine. |
| `CelsEngineGetSession` | `CelsSession *CelsEngineGetSession(CelsEngine *engine, const char *name)` | `HOST_LOOP`, `ANY_THREAD` | `CelsSession*` | Retrieves supervised session by name ("main", "root", or secondary). |
| `CelsEngineQuit` | `void CelsEngineQuit(CelsEngine *engine)` | `HOST_LOOP`, `COMPOSABLE` | `void` | Requests engine to terminate main loop (`engine->shouldQuit = true`). |
| `CelsEngineRunStandalone` | `CelsResult CelsEngineRunStandalone(const CelsAppManifest *m, const CelsSessionConfig *c)` | `HOST_LOOP` | `CelsResult` | Runs complete monolithic standalone runtime loop until exit. |

### 3.2 Session Coordinator & Workload Capacity Profiles (`include/cels/runtime/session.h`)

| Function / Type | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsSessionProfile` | `enum CelsSessionProfile` | `FILE`, `HOST_LOOP` | `enum` | Capacity profiles (`CELS_PROFILE_128` through `8K`, and `DEFAULT`). |
| `CelsSlabSizeFromProfile` | `size_t CelsSlabSizeFromProfile(CelsSessionProfile profile)` | `ANY_THREAD` | `size_t` | Returns raw slab size in bytes (16 KiB up to 1 MiB) for profile. |
| `CelsMaxComposablesFromProfile` | `uint32_t CelsMaxComposablesFromProfile(CelsSessionProfile profile)` | `ANY_THREAD` | `uint32_t` | Returns maximum active composables (128 up to 8,192) for profile. |
| `CelsSessionProfileConfig` | `CelsSessionConfig CelsSessionProfileConfig(CelsSessionProfile profile)` | `ANY_THREAD` | `CelsSessionConfig` | Constructs session configuration initialized for named profile. |
| `CelsSessionCapacityConfig` | `CelsSessionConfig CelsSessionCapacityConfig(uint32_t maxComposables)` | `ANY_THREAD` | `CelsSessionConfig` | Constructs session configuration auto-sizing slab from composable count. |
| `CelsSessionInitWithProfile` | `void CelsSessionInitWithProfile(CelsSession *s, CelsSessionProfile profile)` | `HOST_LOOP` | `void` | Initializes session struct directly with named workload profile. |
| `CelSessionCreate` | `CEL_Session *CelSessionCreate(CEL_Id id, const CelSessionOptions *opts)` | `HOST_LOOP` | `CEL_Session*` | Creates and registers a session by its 64-bit identifier. |
| `CelSessionCreateWithProfile` | `CEL_Session *CelSessionCreateWithProfile(CEL_Id id, CelsSessionProfile profile)` | `HOST_LOOP` | `CEL_Session*` | Creates and registers a heap session configured with named profile. |
| `CelsSessionInit` | `void CelsSessionInit(CelsSession *s, const CelsSessionConfig *config)` | `HOST_LOOP` | `void` | Initializes session struct with memory slab, profile, or capacity count. |
| `CelsSessionDestroy` | `void CelsSessionDestroy(CelsSession *s)` | `HOST_LOOP` | `void` | Tears down session, invoking unmount cleanups and releasing slab. |
| `CelsSessionRecompose` | `CelsResult CelsSessionRecompose(CelsSession *s)` | `HOST_LOOP` | `CelsResult` | Drains invalidations, executes dirty subtrees, publishes state. |
| `CelsRecomposeAllSessions` | `CelsResult CelsRecomposeAllSessions(void)` | `HOST_LOOP` | `CelsResult` | Recomposes all registered active sessions across runtime. |
| `CelsSessionHotReload` | `void CelsSessionHotReload(CelsSession *s)` | `HOST_LOOP` | `void` | Flags all mounted groups dirty for re-evaluation following code reload. |
| `CelsGetCurrentSession` | `CelsSession *CelsGetCurrentSession(void)` | `COMPOSABLE`, `HOST_LOOP` | `CelsSession*` | Returns active ambient session bound to calling thread. |
| `CelsSetCurrentSession` | `void CelsSetCurrentSession(CelsSession *s)` | `HOST_LOOP` | `void` | Binds active session to calling thread. |
| `CelsSessionAllocData` | `void *CelsSessionAllocData(CelsSession *s, size_t size)` | `COMPOSABLE`, `HOST_LOOP` | `void*` | Allocates aligned persistent bytes from session nonmoving arena. |
| `CelsSessionRememberState` | `void *CelsSessionRememberState(CEL_Session *s, CEL_Id id, size_t sz, const void *def)` | `COMPOSABLE`, `HOST_LOOP` | `void*` | Resolves or allocates addressable persistent state in session slab. |
| `CelsSessionAttachComposition` | `void CelsSessionAttachComposition(CEL_Session *s, CEL_Id k, void (*b)(void*), void *u, bool (*e)(void*), void *ec)` | `HOST_LOOP` | `void` | Attaches root composition with optional lifecycle evaluation predicate. |
| `CelsSessionDetachComposition` | `void CelsSessionDetachComposition(CEL_Session *s, CEL_Id key)` | `HOST_LOOP` | `void` | Detaches and prunes root composition from session. |
| `CelsSessionRegisterLifecycle` | `void CelsSessionRegisterLifecycle(CEL_Session *s, void *inst, void (*onC)(void*, CEL_Session*), void (*onD)(void*, CEL_Session*))` | `COMPOSABLE` | `void` | Registers mount (`onCreate`) and unmount (`onDestroy`) lifecycle hooks. |
| `CelsSessionUpdateLifecycle` | `void CelsSessionUpdateLifecycle(CEL_Session *s, void *inst, void (*onD)(void*, CEL_Session*))` | `COMPOSABLE` | `void` | Refreshes unmount hook code pointer after code reload/remount. |
| `CelsSessionRegisterModule` | `void CelsSessionRegisterModule(CelsSession *s, uint64_t k, const char *n, void *inst, void (*onR)(void*, CelsSession*), void (*onD)(void*))` | `HOST_LOOP` | `void` | Registers a subsystem module directly with the session. |
| `CelsSessionGetModule` | `void *CelsSessionGetModule(const CelsSession *s, uint64_t key)` | `COMPOSABLE`, `HOST_LOOP` | `void*` | Retrieves session-registered subsystem module pointer by key. |
| `CelsEnterComposition` | `bool CelsEnterComposition(CelsSession *s, uint64_t rootKey)` | `COMPOSITION` | `bool` | Enters root composition; returns true if body should execute. |
| `CelsEnterComposable` | `bool CelsEnterComposable(CelsSession *s, uint64_t key)` | `COMPOSABLE` | `bool` | Enters child composable group; returns true if body should execute. |
| `CelsExitGroup` | `void CelsExitGroup(CelsSession *s)` | `COMPOSABLE`, `COMPOSITION` | `void` | Exits current group node and pops traversal stack. |
| `CelsPruneSubtree` | `void CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex)` | `HOST_LOOP`, Internal | `void` | Prunes subtree by logical index, running cleanups in reverse order. |
| `CelsPruneSubtreeByKey` | `void CelsPruneSubtreeByKey(CelsSession *s, uint64_t key)` | `HOST_LOOP`, Internal | `void` | Prunes subtree matching 64-bit group key with reverse cleanups. |
| `CelsResolveSlot` | `void *CelsResolveSlot(CelsSession *s, size_t size, const void *initVal)` | `COMPOSABLE` | `void*` | Resolves persistent slot memory pinned to current execution position. |
| `CelsResolveSlotWithCleanup` | `void *CelsResolveSlotWithCleanup(CelsSession *s, size_t sz, const void *init, void (*onD)(void*, CelsSession*))` | `COMPOSABLE` | `void*` | Resolves persistent slot memory and binds unmount cleanup callback. |
| `CelsGetState` | `void *CelsGetState(CelsSession *s, uint64_t key)` | `COMPOSABLE`, `HOST_LOOP` | `void*` | Finds active state instance by 64-bit key in session. |
| `CelsSessionInvalidateKey` | `void CelsSessionInvalidateKey(CelsSession *s, uint64_t key)` | `HOST_LOOP`, `TASK_RUN` | `void` | Queues 64-bit group key for invalidation and marks ancestor chain dirty. |
| `CelsSessionGetCurrentGroupKey`| `uint64_t CelsSessionGetCurrentGroupKey(const CelsSession *s)`| `COMPOSABLE` | `uint64_t` | Returns active 64-bit group key of executing composable. |
| `CelsSessionGetUserData` | `void *CelsSessionGetUserData(const CelsSession *s)` | `ANY_THREAD` | `void*` | Returns session user context pointer. |
| `CelsSessionSetUserData` | `void CelsSessionSetUserData(CelsSession *s, void *userData)` | `HOST_LOOP` | `void` | Sets session user context pointer. |
| `CelsSessionSetPostRecomposeHook` | `void CelsSessionSetPostRecomposeHook(CelsSession *s, CelsPostRecomposeFn hook, void *userData)` | `HOST_LOOP` | `void` | Registers callback fired immediately after recomposition converges. |
| `CelsIsFreshMount` | `bool CelsIsFreshMount(CelsSession *s)` | `COMPOSABLE` | `bool` | Infallible inline test: returns true if current node is newly mounted. |

### 3.3 State Registry & Double Buffering (`include/cels/runtime/state.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsGetStateHeader` | `CelsStateHeader *CelsGetStateHeader(const void *ptr)` | `ANY_THREAD` | `CelsStateHeader*` | Resolves 32-byte header preceding hoisted state instance in O(1). |
| `CelsStateRegistryInit` | `void CelsStateRegistryInit(CelsStateRegistry *r)` | `HOST_LOOP` | `void` | Initializes reactive state registry and zero-initializes cell slots. |
| `CelsStateRegistryFindCell` | `CelsStateCell *CelsStateRegistryFindCell(CelsStateRegistry *r, CEL_Id id)` | `COMPOSABLE`, `HOST_LOOP` | `CelsStateCell*` | Finds existing reactive state cell by 64-bit ID. |
| `CelsStateGetOrCreateCell` | `CelsStateCell *CelsStateGetOrCreateCell(CelsSession *s, CEL_Id id, size_t sz, const void *def)` | `COMPOSABLE`, `HOST_LOOP` | `CelsStateCell*` | Resolves or allocates double-buffered reactive state cell in slab. |
| `CelsStateWatch` | `const void *CelsStateWatch(CelsSession *s, CEL_Id id, size_t size)` | `COMPOSABLE` | `const void*` | Subscribes active composable and returns front-buffer pointer. |
| `CelsStateGet` | `const void *CelsStateGet(CelsSession *s, CEL_Id id, size_t size)` | `COMPOSABLE`, `HOST_LOOP` | `const void*` | Passively reads front-buffer snapshot without subscribing. |
| `CelsStateMutate` | `void *CelsStateMutate(CelsSession *s, CEL_Id id, size_t size)` | `HOST_LOOP`, Callbacks | `void*` | Retrieves mutable back-buffer pointer and queues observers dirty. |
| `cels_session_mutate` | `cels_session_mutate(session, Type)`<br>`cels_session_mutate(session, id, Type)` | `TEST_FIXTURE`, Internal | Scoped block | Tier 3 Low-Level: Directly stages a mutation on a session cell by key for raw unit tests. |
| `CelsStatePublishDirty` | `void CelsStatePublishDirty(CelsSession *s)` | `HOST_LOOP` | `void` | Copies modified back buffers to front buffers and resets dirty flags. |
| `CelsStateRegistryUnsubscribeKey` | `void CelsStateRegistryUnsubscribeKey(CelsStateRegistry *r, uint64_t groupKey)` | Internal, Unmount | `void` | Unsubscribes group key from all observed reactive state cells. |
| `CelsWatchStateInstance` | `const void *CelsWatchStateInstance(CelsSession *s, const void *ptr)` | `COMPOSABLE` | `const void*` | Subscribes active composable to a hoisted state instance pointer. |
| `CelsMutateStateInstance` | `void *CelsMutateStateInstance(void *ptr)` | `HOST_LOOP`, Callbacks | `void*` | Resolves header from hoisted pointer and returns mutable back buffer. |
| `CelsResolveStateInstance` | `void *CelsResolveStateInstance(CelsSession *s, size_t size, const void *initVal)` | `COMPOSABLE` | `void*` | Allocates hoisted state instance pinned to current composable slot. |

### 3.4 Slot Table & Dual Gap Buffer (`include/cels/runtime/slot_table.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsHashKey` | `uint64_t CelsHashKey(const char *str)` | `ANY_THREAD` | `uint64_t` | FNV-1a 64-bit string hashing helper. |
| `CelsKeyIndex` | `uint64_t CelsKeyIndex(uint64_t baseKey, uint64_t index)` | `ANY_THREAD` | `uint64_t` | Computes indexed child key hash for dynamic list elements. |
| `CelsResultToString` | `const char *CelsResultToString(CelsResult result)` | `ANY_THREAD` | `const char*` | Returns static, human-readable error description string. |
| `CelsSlotTableInit` | `CelsResult CelsSlotTableInit(CelsSlotTable *t, void *slab, size_t sz, uint32_t maxG)` | `HOST_LOOP` | `CelsResult` | Carves dual gap buffer out of a single 64-byte aligned slab. |
| `CelsSlotTableReset` | `CelsResult CelsSlotTableReset(CelsSlotTable *t)` | `HOST_LOOP` | `CelsResult` | Resets table to empty state without reallocating slab memory. |
| `CelsSlotTableGroupCount` | `uint32_t CelsSlotTableGroupCount(const CelsSlotTable *t)` | `ANY_THREAD` | `uint32_t` | Returns total active group count (excluding gap buffer space). |
| `CelsSlotTableSlotCount` | `uint32_t CelsSlotTableSlotCount(const CelsSlotTable *t)` | `ANY_THREAD` | `uint32_t` | Returns total active 64-bit slot count (excluding gap space). |
| `CelsSlotTableGroupCapacity`| `uint32_t CelsSlotTableGroupCapacity(const CelsSlotTable *t)`| `ANY_THREAD` | `uint32_t` | Returns total structural group capacity allocated in slab. |
| `CelsSlotTableSlotCapacity` | `uint32_t CelsSlotTableSlotCapacity(const CelsSlotTable *t)` | `ANY_THREAD` | `uint32_t` | Returns total word slot capacity allocated in slab. |
| `CelsSlotTableGroupToPhysicalIdx` | `CelsResult CelsSlotTableGroupToPhysicalIdx(const CelsSlotTable *t, uint32_t logIdx, uint32_t *outPhys)` | `ANY_THREAD` | `CelsResult` | Translates contiguous logical group index to physical array index. |
| `CelsSlotTableGroupFlags` | `uint16_t CelsSlotTableGroupFlags(const CelsSlotTable *t, CelsComposableId c)` | `ANY_THREAD` | `uint16_t` | Reads invalidation flags bitmask for a group (`CelsGroupFlags`). |
| `CelsSlotTableGroupInvalidate` | `CelsResult CelsSlotTableGroupInvalidate(CelsSlotTable *t, CelsComposableId c)` | `HOST_LOOP`, Internal | `CelsResult` | Marks target dirty and climbs parent chain placing `CONTAINS_INVALIDATED`. |
| `CelsSlotTableGroupClearFlags` | `void CelsSlotTableGroupClearFlags(CelsSlotTable *t, CelsComposableId c)` | Internal | `void` | Clears invalidation flags on entry into composable group. |
| `CelsSlotTableClearAllFlags` | `void CelsSlotTableClearAllFlags(CelsSlotTable *t)` | `HOST_LOOP` | `void` | Clears invalidation flags across all active groups. |
| `CelsSlotTableGroupsShiftParents` | `void CelsSlotTableGroupsShiftParents(CelsSlotTable *t, uint32_t thresh, int32_t delta)` | Internal | `void` | Renumbers parentIndex chain after gap insertion or deletion. |
| `CelsSlotTableFindGroup` | `CelsSlotGroup *CelsSlotTableFindGroup(const CelsSlotTable *t, uint64_t key, uint32_t *outLog)` | `ANY_THREAD` | `CelsSlotGroup*` | Linear search for group matching 64-bit key; returns pointer and logical index. |
| `CelsSlotTableMoveGapTo` | `CelsResult CelsSlotTableMoveGapTo(CelsSlotTable *t, uint32_t targetLogIdx)` | Internal | `CelsResult` | Shifts group gap and slot gap in lockstep to target index. |
| `CelsSlotTableReaderOpen` | `CelsResult CelsSlotTableReaderOpen(const CelsSlotTable *t, CelsSlotReader *r)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Opens a read-only cursor on slot table. |
| `CelsSlotReaderClose` | `CelsResult CelsSlotReaderClose(CelsSlotReader *r)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Closes active reader cursor. |
| `CelsSlotReaderGroupStart` | `CelsResult CelsSlotReaderGroupStart(CelsSlotReader *r, CelsSlotGroup *outG)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Enters next group in traversal order. |
| `CelsSlotReaderGroupEnd` | `CelsResult CelsSlotReaderGroupEnd(CelsSlotReader *r)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Leaves current group, restoring cursor to enclosing parent boundary. |
| `CelsSlotReaderGroupSkip` | `CelsResult CelsSlotReaderGroupSkip(CelsSlotReader *r)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Skips current group and all its transitive descendants in O(1). |
| `CelsSlotReaderSlotRead` | `CelsResult CelsSlotReaderSlotRead(CelsSlotReader *r, CelsSlotValue *outVal)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Reads next 64-bit slot value belonging to current group. |
| `CelsSlotReaderGroupGet` | `CelsResult CelsSlotReaderGroupGet(const CelsSlotReader *r, uint32_t logIdx, CelsSlotGroup *outG)` | `HOST_LOOP`, `ANY_THREAD` | `CelsResult` | Random-access read of group metadata by logical index. |
| `CelsSlotTableWriterOpen` | `CelsResult CelsSlotTableWriterOpen(CelsSlotTable *t, CelsSlotWriter *w)` | `HOST_LOOP` | `CelsResult` | Opens mutating writer cursor on slot table (exclusive access). |
| `CelsSlotWriterClose` | `CelsResult CelsSlotWriterClose(CelsSlotWriter *w)` | `HOST_LOOP` | `CelsResult` | Closes mutating writer, verifying all groups were closed. |
| `CelsSlotWriterGapMoveTo` | `CelsResult CelsSlotWriterGapMoveTo(CelsSlotWriter *w, uint32_t targetLogIdx)` | `HOST_LOOP` | `CelsResult` | Moves writer gap to target logical group index. |
| `CelsSlotWriterGroupStart` | `CelsResult CelsSlotWriterGroupStart(CelsSlotWriter *w, uint64_t k, uint64_t u, uint32_t *outIdx)` | `HOST_LOOP` | `CelsResult` | Begins insertion of a new group at writer gap. |
| `CelsSlotWriterGroupEnd` | `CelsResult CelsSlotWriterGroupEnd(CelsSlotWriter *w)` | `HOST_LOOP` | `CelsResult` | Closes open group, backfilling transitive subtree groupSize. |
| `CelsSlotWriterNodeEmit` | `CelsResult CelsSlotWriterNodeEmit(CelsSlotWriter *w, uint16_t count)` | `HOST_LOOP` | `CelsResult` | Emits materialized node counts into current open group and ancestors. |
| `CelsSlotWriterSlotWrite` | `CelsResult CelsSlotWriterSlotWrite(CelsSlotWriter *w, CelsSlotValue val, uint32_t *outSlot)` | `HOST_LOOP` | `CelsResult` | Writes 64-bit slot value into slot gap for open group. |
| `CelsSlotWriterSlotSet` | `CelsResult CelsSlotWriterSlotSet(CelsSlotWriter *w, uint32_t logSlotIdx, CelsSlotValue val)` | `HOST_LOOP` | `CelsResult` | In-place update of an existing 64-bit slot value. |
| `CelsSlotWriterGroupSkip` | `CelsResult CelsSlotWriterGroupSkip(CelsSlotWriter *w, uint32_t *outSkipped)` | `HOST_LOOP` | `CelsResult` | Skips group at writer position and all children in O(1). |

### 3.5 Tasks & Fibers (`include/cels/runtime/task.h` & `fiber.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsTaskInit` | `void CelsTaskInit(CelsTaskState *state)` | `HOST_LOOP`, `COMPOSABLE` | `void` | Initializes task state struct to runnable defaults (step 0). |
| `CelsTaskShouldWait` | `bool CelsTaskShouldWait(CelsSession *s, CelsTaskState *st, uint64_t k)` | `COMPOSABLE` | `bool` | Checks if monotonic delay deadline has elapsed; re-queues if waiting. |
| `CelsTaskYield` | `void CelsTaskYield(CelsSession *s, CelsTaskState *st, uint64_t k, int step)` | `TASK_RUN` | `void` | Records yield step and queues session group for next-frame execution. |
| `CelsTaskWait` | `void CelsTaskWait(CelsSession *s, CelsTaskState *st, uint64_t k, int step, uint32_t ms)` | `TASK_RUN` | `void` | Calculates deadline timestamp and queues session group for polling. |
| `CelsTaskStep` | `void CelsTaskStep(CelsSession *s, CelsTaskState *st, uint64_t k, CelsFiberFn fn, void *p)` | `COMPOSABLE` | `void` | Advances task cooperative fiber by one execution step. |
| `CelsTaskFinishFiber` | `void CelsTaskFinishFiber(CelsTaskState *state)` | `TASK_RUN` | `void` | Marks fiber finished and switches execution back to session caller. |
| `CelsTaskCleanup` | `void CelsTaskCleanup(CelsTaskState *state)` | Internal, Unmount | `void` | Destroys active OS fiber context and reclaims stack allocation. |
| `CelsTaskComplete` | `void CelsTaskComplete(CelsTaskState *state)` | `TASK_RUN` | `void` | Sets `isDone = true`, `isRunning = false`, `step = -1`. |
| `CelsTaskCancel` | `void CelsTaskCancel(CelsTaskState *state)` | `TASK_RUN`, `HOST_LOOP` | `void` | Sets `isCancelled = true`, `isRunning = false`, `step = -2`. |
| `CelsTaskRestart` | `void CelsTaskRestart(CelsSession *s, CelsTaskState *st, uint64_t key)` | `HOST_LOOP`, `COMPOSABLE` | `void` | Resets task to step 0 and queues group for immediate recomposition. |
| `CelsTaskIsRunning` | `bool CelsTaskIsRunning(const CelsTaskState *state)` | `ANY_THREAD` | `bool` | Returns true if task is started and neither completed nor cancelled. |
| `CelsTaskIsDone` | `bool CelsTaskIsDone(const CelsTaskState *state)` | `ANY_THREAD` | `bool` | Returns true if task run block executed to completion. |
| `CelsTaskIsCancelled` | `bool CelsTaskIsCancelled(const CelsTaskState *state)` | `ANY_THREAD` | `bool` | Returns true if task was cancelled. |

### 3.6 Transitions & Easing Curves (`include/cels/runtime/transition.h`)

| Function / Symbol | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsTransitionStep` | `float CelsTransitionStep(CelsSession *s, CelsTransitionState *st, uint64_t k, float tgt, uint32_t durMs, CelsEasingFn ease)` | `COMPOSABLE` | `float` | Evaluates temporal easing step, retargets in-flight, queues invalidation. |
| `CelsEaseLinear` | `float CelsEaseLinear(float t)` | `ANY_THREAD` | `float` | Linear easing: $f(t) = t$. |
| `CelsEaseInQuad` | `float CelsEaseInQuad(float t)` | `ANY_THREAD` | `float` | Quadratic acceleration: $f(t) = t^2$. |
| `CelsEaseOutQuad` | `float CelsEaseOutQuad(float t)` | `ANY_THREAD` | `float` | Quadratic deceleration: $f(t) = 2t - t^2$. Default transition curve. |
| `CelsEaseInOutQuad` | `float CelsEaseInOutQuad(float t)` | `ANY_THREAD` | `float` | Quadratic acceleration until halfway, then deceleration. |
| `CelsEaseInCubic` | `float CelsEaseInCubic(float t)` | `ANY_THREAD` | `float` | Cubic acceleration: $f(t) = t^3$. |
| `CelsEaseOutCubic` | `float CelsEaseOutCubic(float t)` | `ANY_THREAD` | `float` | Cubic deceleration: $f(t) = 1 - (1-t)^3$. |
| `CelsEaseInOutCubic` | `float CelsEaseInOutCubic(float t)` | `ANY_THREAD` | `float` | Cubic acceleration then deceleration. |
| `CelsEaseInSine` | `float CelsEaseInSine(float t)` | `ANY_THREAD` | `float` | Sinusoidal acceleration: $f(t) = 1 - \cos(t\pi/2)$. |
| `CelsEaseOutSine` | `float CelsEaseOutSine(float t)` | `ANY_THREAD` | `float` | Sinusoidal deceleration: $f(t) = \sin(t\pi/2)$. |
| `CelsEaseInOutSine` | `float CelsEaseInOutSine(float t)` | `ANY_THREAD` | `float` | Smooth sinusoidal S-curve. |
| `CelsEaseOutBounce` | `float CelsEaseOutBounce(float t)` | `ANY_THREAD` | `float` | Decaying parabolic bounce settling at 1.0. |
| `CelsEaseOutBack` | `float CelsEaseOutBack(float t)` | `ANY_THREAD` | `float` | Deceleration with elastic overshoot beyond target before settling. |

### 3.7 Transactions & Batch Staging (`include/cels/runtime/transaction.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsSessionStageSet` | `bool CelsSessionStageSet(CelsSession *s, uint64_t tgtId, uint64_t typeK, size_t sz, const void *data)` | `COMPOSABLE`, `TASK_RUN` | `bool` | Stages `CELS_OP_SET` and copies payload into linear arena without locks. |
| `CelsSessionStageRemove` | `bool CelsSessionStageRemove(CelsSession *s, uint64_t tgtId, uint64_t typeK)` | `COMPOSABLE`, `TASK_RUN` | `bool` | Stages `CELS_OP_REMOVE` for target ID and component type key. |
| `CelsSessionStageDelete` | `bool CelsSessionStageDelete(CelsSession *s, uint64_t tgtId)` | `COMPOSABLE`, `TASK_RUN` | `bool` | Stages `CELS_OP_DELETE` targeting entity/resource destruction. |
| `CelsSessionStageCustom` | `bool CelsSessionStageCustom(CelsSession *s, uint32_t op, uint64_t tgtId, uint64_t typeK, size_t sz, const void *d)` | `COMPOSABLE`, `TASK_RUN` | `bool` | Stages custom user opcode and typed payload in transaction batch. |
| `CelsSessionCommitTransactions` | `uint32_t CelsSessionCommitTransactions(CelsSession *s, CelsTransactionHandler h, void *u)` | `HOST_LOOP` | `uint32_t` | Dispatches staged operations to handler in submission order and clears batch. |
| `CelsSessionSwapTransactionBatches` | `void CelsSessionSwapTransactionBatches(CelsSession *s)` | `HOST_LOOP` | `void` | Swaps active and ready double-buffered batches for worker thread handoff. |
| `CelsSessionGetReadyBatch` | `const CelsTransactionBatch *CelsSessionGetReadyBatch(const CelsSession *s)` | `ANY_THREAD` | `const CelsTransactionBatch*` | Returns read-only pointer to ready batch for background thread consumption. |
| `CelsSessionClearReadyBatch` | `void CelsSessionClearReadyBatch(CelsSession *s)` | `HOST_LOOP`, `ANY_THREAD` | `void` | Resets ready batch after external consumers complete processing. |

### 3.8 Threading, Timing & Logging (`include/cels/runtime/thread.h` & `log.h`)

| Function | Exact Signature | Allowed Scope | Return | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CelsGetTimeMs` | `uint64_t CelsGetTimeMs(void)` | `ANY_THREAD` | `uint64_t` | High-precision strictly monotonic timestamp in milliseconds. |
| `CelsSleepMs` | `void CelsSleepMs(uint32_t ms)` | `ANY_THREAD` | `void` | Suspends calling OS thread for specified millisecond duration. |
| `CelsMutexInit` | `void CelsMutexInit(CelsMutex *m)` | `HOST_LOOP` | `void` | Initializes native non-recursive mutex structure. |
| `CelsMutexLock` | `void CelsMutexLock(CelsMutex *m)` | `ANY_THREAD` | `void` | Acquires exclusive ownership of mutex, blocking until acquired. |
| `CelsMutexUnlock` | `void CelsMutexUnlock(CelsMutex *m)` | `ANY_THREAD` | `void` | Releases exclusive ownership of mutex. |
| `CelsMutexDestroy` | `void CelsMutexDestroy(CelsMutex *m)` | `HOST_LOOP` | `void` | Releases underlying OS mutex resources. |
| `CelsThreadCreate` | `bool CelsThreadCreate(CelsThread *t, CelsThreadFn fn, void *arg)` | `HOST_LOOP` | `bool` | Spawns background OS worker thread. |
| `CelsThreadJoin` | `void CelsThreadJoin(CelsThread *t)` | `HOST_LOOP` | `void` | Blocks calling thread until worker finishes and cleans up handle. |
| `CelsLog` | `void CelsLog(CelsLogLevel lvl, const char *f, int l, const char *fmt, ...)` | `ANY_THREAD` | `void` | Writes log message into non-blocking fixed-size ring buffer. |
| `CelsFlushLogs` | `void CelsFlushLogs(void)` | `HOST_LOOP` | `void` | Flushes accumulated ring buffer logs to stdout/stderr. |
| `CelsGetVersionString` | `const char *CelsGetVersionString(void)` | `ANY_THREAD` | `const char*` | Returns static semantic version string (e.g. `"0.1.2"`). |
| `CelsGetVersionCode` | `uint32_t CelsGetVersionCode(void)` | `ANY_THREAD` | `uint32_t` | Returns packed 32-bit integer version code (`0xMMmmPPbb`). |
