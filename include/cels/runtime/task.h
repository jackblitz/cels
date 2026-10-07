#pragma once

/**
 * @file task.h
 * @brief Procedural Coroutine Tasks and Cooperative State Machines for CELS.
 *
 * Provides a lightweight, stackless procedural coroutine DSL for CELS. Tasks
 * integrate directly with CELS lifecycle and slot memory:
 * - Persistent execution step and timer state pinned across recomposition passes.
 * - Non-blocking delays via `cel_wait(ms)` that cooperate with the host event loop.
 * - Automatic cancellation teardown on unmount or explicit cancellation.
 * - Clean argument preservation and typed cancellation blocks.
 *
 * Typical usage:
 * @code
 *     CEL_Task(ConnectServerTask, const char*, host, int, port) {
 *         cancel {
 *             DisconnectSocket();
 *         }
 *         run {
 *             ResolveDns(host);
 *             cel_wait(100); // 100ms non-blocking async delay
 *             Connect(host, port);
 *             cel_yield();   // suspend until next frame
 *         }
 *     }
 *
 *     CEL_Composition(NetworkView, void *userData) {
 *         cel_task(ConnectServerTask, "127.0.0.1", 8080);
 *     }
 * @endcode
 *
 * Thread safety: CEL_Task coroutines execute on the session's composition thread.
 * Status query functions (`CelsTaskIsRunning`, `CelsTaskIsDone`) can be called
 * concurrently with session read locks.
 */

#include "cels/runtime/slot_table.h"
#include "cels/runtime/session.h"
#include "cels/runtime/thread.h"
#include "cels/runtime/fiber.h"
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Execution phase: run block is active. */
#define CEL_TASK_PHASE_RUN    1

/** Execution phase: cancel teardown block is active. */
#define CEL_TASK_PHASE_CANCEL 2

/**
 * Persistent state structure tracked in CELS slot table for a task instance.
 */
typedef struct CelsTaskState {
    int      step;          /**< Yield step / line number (0 = initial, -1 = done, -2 = cancelled) */
    uint64_t waitTimerMs;   /**< Monotonic timestamp deadline for cel_wait delay */
    bool     isRunning;     /**< True while task is actively scheduled */
    bool     isCancelled;   /**< True if task was cancelled */
    bool     isDone;        /**< True if run block ran to completion */
    void    *taskFiber;     /**< Cooperative fiber executing the task */
    void    *callerFiber;   /**< Suspended caller fiber to yield back to */
} CelsTaskState;

/* ========================================================================= */
/* Public Task State Lifecycle API (implemented in src/task.c)              */
/* ========================================================================= */

/**
 * Initializes or resets a task state struct to its default runnable state.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskInit(CelsTaskState *state);

/**
 * Checks whether a waiting task deadline has elapsed.
 *
 * If the monotonic timer has not reached waitTimerMs, the task's session group
 * key is queued for invalidation on the next frame and this returns true.
 * If the deadline has passed, waitTimerMs is reset to zero and returns false.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Safe if NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @return True if the task is still waiting; false if ready to resume.
 */
bool CelsTaskShouldWait(CelsSession *session, CelsTaskState *state, uint64_t key);

/**
 * Yields task execution until the subsequent recomposition pass.
 *
 * Records the line step identifier in state and registers an invalidation
 * on the session so the task will be evaluated on the next frame.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Safe if NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @param step    Resume step identifier (typically __LINE__).
 */
void CelsTaskYield(CelsSession *session, CelsTaskState *state, uint64_t key, int step);

/**
 * Suspends task execution for a specified non-blocking delay in milliseconds.
 *
 * Computes a monotonic deadline timestamp, records the resume step, and
 * invalidates the session group key for continued polling.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Safe if NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @param step    Resume step identifier (typically __LINE__).
 * @param delayMs Delay duration in milliseconds.
 */
void CelsTaskWait(CelsSession *session, CelsTaskState *state, uint64_t key, int step, uint32_t delayMs);

/**
 * Advances a task fiber by one frame step.
 */
void CelsTaskStep(CelsSession *session, CelsTaskState *state, uint64_t key, CelsFiberFn fiberFn, void *param);

/**
 * Completes a task fiber and switches execution back to the session caller.
 */
void CelsTaskFinishFiber(CelsTaskState *state);

/**
 * Destroys any active fiber associated with a task state.
 */
void CelsTaskCleanup(CelsTaskState *state);

/**
 * Marks a task as successfully completed.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskComplete(CelsTaskState *state);

/**
 * Transitions a task to the cancelled state.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskCancel(CelsTaskState *state);

/**
 * Resets a task state to step 0 and triggers session invalidation.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Safe if NULL.
 * @param key     Unique 64-bit key of the task composable group.
 */
void CelsTaskRestart(CelsSession *session, CelsTaskState *state, uint64_t key);

/**
 * Tests whether a task is actively running.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if running and neither cancelled nor done.
 */
bool CelsTaskIsRunning(const CelsTaskState *state);

/**
 * Tests whether a task has completed execution.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if completed.
 */
bool CelsTaskIsDone(const CelsTaskState *state);

/**
 * Tests whether a task was cancelled.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if cancelled.
 */
bool CelsTaskIsCancelled(const CelsTaskState *state);

/* ========================================================================= */
/* Task Coroutine DSL Flow Control                                           */
/* ========================================================================= */

/* ========================================================================= */
/* Task Coroutine DSL Flow Control                                           */
/* ========================================================================= */

/**
 * @def cancel
 * @brief Defines the cancellation and unmount teardown block of a CEL_Task.
 *
 * What it does:
 * Designates a scoped block containing cleanup logic that executes whenever the task
 * is explicitly cancelled via cel_cancel() or cel_cancel_task(), or when the task is
 * unmounted because its parent composable was pruned from the session hierarchy.
 *
 * Expected outcome:
 * Guaranteed execution of resource cleanup (closing sockets, freeing temporary buffers,
 * resetting hardware/render states) before the task's slot memory is reclaimed.
 *
 * Where to use:
 * Place inside a CEL_Task definition, typically before or after the run block.
 *
 * Example:
 * @code
 *     CEL_Task(DownloadTask, const char *url) {
 *         cancel {
 *             AbortHttpRequest();
 *             CELS_LOG_INFO("Download aborted for %s", url);
 *         }
 *         run {
 *             // download steps
 *         }
 *     }
 * @endcode
 */
#ifndef cancel
#define cancel \
    _cels_task_cancel_entry: \
    if (_cels_task_phase == CEL_TASK_PHASE_CANCEL) \
        for (int _cels_canc_once = 1; _cels_canc_once; _cels_canc_once = 0)
#endif

/**
 * @def run
 * @brief Defines the cooperative coroutine execution body of a CEL_Task.
 *
 * What it does:
 * Encloses the procedural step-by-step logic of the task executing inside a cooperative
 * fiber. Preserves all local stack variables, supports nested helper function calls, and
 * allows native C switch statements across cel_yield() and cel_wait() suspension points.
 *
 * Expected outcome:
 * Executes cooperatively across multiple engine frame ticks without blocking the main thread.
 *
 * Where to use:
 * Place inside a CEL_Task definition.
 */
#ifndef run
#define run \
    if (0) goto _cels_task_cancel_entry; \
    if (_cels_task_phase == CEL_TASK_PHASE_RUN)
#endif

/**
 * @def cel_yield
 * @brief Suspends task execution and yields control to the engine until the next frame.
 *
 * What it does:
 * Suspends the task's cooperative fiber and yields control back to the session loop.
 * Schedules the task's composable group for recomposition on the subsequent frame.
 * Local variables on the C call stack are preserved intact.
 *
 * Where to use:
 * Call exclusively inside the run block of a CEL_Task (or inside any helper subroutine called by it).
 */
#ifndef cel_yield
#define cel_yield() \
    do { \
        CelsTaskYield(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, 0); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait
 * @brief Suspends task execution for a specified non-blocking delay in milliseconds.
 *
 * What it does:
 * Calculates a monotonic deadline timestamp (`now + ms`) and suspends the cooperative fiber.
 * On every frame before the deadline expires, the task remains suspended without switching
 * into the fiber. Once elapsed, execution resumes right after cel_wait() with full stack preservation.
 *
 * @param ms Delay duration in milliseconds.
 */
#ifndef cel_wait
#define cel_wait(ms) \
    do { \
        CelsTaskWait(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, 0, (uint32_t)(ms)); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait_for
 * @brief Suspends task fiber non-blockingly until an event/signal/broadcast of Type arrives.
 */
#ifndef cel_wait_for
#define cel_wait_for(Type, outPtr) \
    do { \
        CelsTaskWaitForEvent(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, \
                             CelsHashKey(#Type), CELS_EVENT_SCOPE_ANY, (outPtr), sizeof(Type), 0); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait_signal
 * @brief Suspends task fiber non-blockingly until a targeted signal of Type arrives.
 */
#ifndef cel_wait_signal
#define cel_wait_signal(Type, outPtr) \
    do { \
        CelsTaskWaitForEvent(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, \
                             CelsHashKey(#Type), CELS_EVENT_SCOPE_SIGNAL, (outPtr), sizeof(Type), 0); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait_broadcast
 * @brief Suspends task fiber non-blockingly until a global broadcast of Type arrives.
 */
#ifndef cel_wait_broadcast
#define cel_wait_broadcast(Type, outPtr) \
    do { \
        CelsTaskWaitForEvent(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, \
                             CelsHashKey(#Type), CELS_EVENT_SCOPE_BROADCAST, (outPtr), sizeof(Type), 0); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait_for_timeout
 * @brief Suspends task fiber until an event of Type arrives or timeoutMs elapses.
 *
 * @param Type      Event/signal struct type name.
 * @param outPtr    Target pointer to write received event data to.
 * @param timeoutMs Timeout duration in milliseconds.
 * @return True if event was received; false if timed out.
 */
#ifndef cel_wait_for_timeout
#define cel_wait_for_timeout(Type, outPtr, timeoutMs) \
    CelsTaskWaitForTimeout(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, \
                           CelsHashKey(#Type), CELS_EVENT_SCOPE_ANY, (outPtr), sizeof(Type), (uint32_t)(timeoutMs))
#endif

/**
 * @def cel_cancel
 * @brief Cancels task execution from within the task body and executes teardown.
 *
 * What it does:
 * Transitions the task state to cancelled and performs an immediate jump to the
 * task's cancel block.
 */
#ifndef cel_cancel
#define cel_cancel() \
    do { \
        CelsTaskCancel(_cels_task_state); \
        _cels_task_phase = CEL_TASK_PHASE_CANCEL; \
        goto _cels_task_cancel_entry; \
    } while(0)
#endif

/**
 * @def cel_task
 * @brief Invokes and schedules a declared CEL_Task inside an active composition.
 *
 * What it does:
 * Enters the task's composable group in the session slot table, initializes or restores
 * the coroutine state, executes active steps, and exits the group.
 *
 * Expected outcome:
 * Advances the task state machine by one step (or polls its waiting timer).
 *
 * Where to use:
 * Inside any CEL_Composition or CEL_Composable body.
 *
 * Example:
 * @code
 *     CEL_Composition(GameScreen, void *userData) {
 *         cel_task(EnemySpawnTask, 5, 2.5f);
 *     }
 * @endcode
 */
#ifndef cel_task
#define cel_task(Name, ...) Name(__VA_ARGS__)
#endif

/**
 * @def cel_cancel_task
 * @brief Externally cancels the active instance of a declared task.
 *
 * What it does:
 * Marks the specified task's slot state as cancelled and immediately triggers its
 * cancel teardown block.
 *
 * Expected outcome:
 * The task ceases running and executes any registered cleanup logic.
 *
 * Where to use:
 * Call from UI event handlers, buttons, or parent compositions.
 *
 * Example:
 * @code
 *     if (UserClickedAbort()) {
 *         cel_cancel_task(ConnectServerTask);
 *     }
 * @endcode
 */
#ifndef cel_cancel_task
#define cel_cancel_task(Name) _cels_task_cancel_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_running
 * @brief Queries whether a declared task is actively running.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task has been started and has neither completed nor been cancelled.
 *
 * Where to use:
 * Call in composables to toggle loading spinners, disable buttons, or coordinate tasks.
 */
#ifndef cel_is_task_running
#define cel_is_task_running(Name) _cels_task_is_running_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_done
 * @brief Queries whether a declared task has completed execution.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task's run block executed to completion.
 *
 * Where to use:
 * Call in composables to detect task completion and trigger subsequent UI flows.
 */
#ifndef cel_is_task_done
#define cel_is_task_done(Name) _cels_task_is_done_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_cancelled
 * @brief Queries whether a declared task was cancelled.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task was cancelled via cel_cancel() or cel_cancel_task().
 *
 * Where to use:
 * Call in composables to display cancellation banners or retry prompts.
 */
#ifndef cel_is_task_cancelled
#define cel_is_task_cancelled(Name) _cels_task_is_cancelled_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_restart_task
 * @brief Resets a completed or cancelled task back to step 0.
 *
 * What it does:
 * Clears the task's done and cancelled flags, resets its execution step to 0, and
 * invalidates the session group so it restarts execution on the next frame.
 *
 * Expected outcome:
 * The task begins executing from the start of its run block on the next frame.
 *
 * Where to use:
 * Call from retry buttons or restart event handlers.
 */
#ifndef cel_restart_task
#define cel_restart_task(Name) _cels_task_restart_##Name(CelsGetCurrentSession())
#endif

/* ========================================================================= */
/* CEL_Task 0 Arguments Implementation                                       */
/* ========================================================================= */

#define _CEL_TASK_0(Name) \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_fiber_proc_##Name(void *param) { \
        CelsTaskState *state = (CelsTaskState*)param; \
        _cels_task_body_##Name(CEL_TASK_PHASE_RUN, state, CelsHashKey(#Name)); \
        CelsTaskFinishFiber(state); \
    } \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        CelsTaskState *state = (CelsTaskState*)instance; \
        if (state != NULL && !state->isDone && !state->isCancelled) { \
            CelsTaskCancel(state); \
            if (session != NULL) session->isExecutingTask = true; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, state, CelsHashKey(#Name)); \
            if (session != NULL) session->isExecutingTask = false; \
        } \
        CelsTaskCleanup(state); \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsRunning(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsDone(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_cancelled_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsCancelled(_cels_task_last_##Name); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            CelsTaskRestart(sess, _cels_task_last_##Name, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(void) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            CelsTaskState *state = cel_remember(CelsTaskState, \
                ((CelsTaskState){ .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false, .taskFiber = NULL, .callerFiber = NULL }), \
                _cels_task_clean_##Name \
            ); \
            _cels_task_last_##Name = state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(state); \
                CelsSessionRegisterLifecycle(sess, state, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, state, _cels_task_clean_##Name); \
            } \
            CelsTaskStep(sess, state, key, _cels_task_fiber_proc_##Name, state); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key)

/* ========================================================================= */
/* CEL_Task 1 Argument Implementation (Type1, Arg1)                          */
/* ========================================================================= */

#define _CEL_TASK_1(Name, Type1, Arg1) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState _cels_state; \
        Type1 Arg1; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_fiber_proc_##Name(void *param) { \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)param; \
        _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->_cels_state, CelsHashKey(#Name), box->Arg1); \
        CelsTaskFinishFiber(&box->_cels_state); \
    } \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->_cels_state.isDone && !box->_cels_state.isCancelled) { \
            CelsTaskCancel(&box->_cels_state); \
            if (session != NULL) session->isExecutingTask = true; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->_cels_state, CelsHashKey(#Name), box->Arg1); \
            if (session != NULL) session->isExecutingTask = false; \
        } \
        if (box != NULL) CelsTaskCleanup(&box->_cels_state); \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsRunning(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsDone(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_cancelled_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsCancelled(_cels_task_last_##Name); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            CelsTaskRestart(sess, _cels_task_last_##Name, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            _CelsTaskBox_##Name *box = cel_remember(_CelsTaskBox_##Name, \
                ((_CelsTaskBox_##Name){ ._cels_state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false, .taskFiber = NULL, .callerFiber = NULL }, .Arg1 = Arg1 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            _cels_task_last_##Name = &box->_cels_state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(&box->_cels_state); \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            CelsTaskStep(sess, &box->_cels_state, key, _cels_task_fiber_proc_##Name, box); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1)

/* ========================================================================= */
/* CEL_Task 2 Arguments Implementation (Type1, Arg1, Type2, Arg2)            */
/* ========================================================================= */

#define _CEL_TASK_2(Name, Type1, Arg1, Type2, Arg2) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState _cels_state; \
        Type1 Arg1; \
        Type2 Arg2; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_fiber_proc_##Name(void *param) { \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)param; \
        _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->_cels_state, CelsHashKey(#Name), box->Arg1, box->Arg2); \
        CelsTaskFinishFiber(&box->_cels_state); \
    } \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->_cels_state.isDone && !box->_cels_state.isCancelled) { \
            CelsTaskCancel(&box->_cels_state); \
            if (session != NULL) session->isExecutingTask = true; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->_cels_state, CelsHashKey(#Name), box->Arg1, box->Arg2); \
            if (session != NULL) session->isExecutingTask = false; \
        } \
        if (box != NULL) CelsTaskCleanup(&box->_cels_state); \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsRunning(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsDone(_cels_task_last_##Name); \
    } \
    static inline bool _cels_task_is_cancelled_##Name(CelsSession *sess) { \
        (void)sess; \
        return CelsTaskIsCancelled(_cels_task_last_##Name); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            CelsTaskRestart(sess, _cels_task_last_##Name, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            _CelsTaskBox_##Name *box = cel_remember(_CelsTaskBox_##Name, \
                ((_CelsTaskBox_##Name){ ._cels_state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false, .taskFiber = NULL, .callerFiber = NULL }, .Arg1 = Arg1, .Arg2 = Arg2 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            box->Arg2 = Arg2; \
            _cels_task_last_##Name = &box->_cels_state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(&box->_cels_state); \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            CelsTaskStep(sess, &box->_cels_state, key, _cels_task_fiber_proc_##Name, box); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2)

/* ========================================================================= */
/* CEL_Task Macro Dispatcher                                                 */
/* ========================================================================= */

#define _CEL_GET_TASK_MACRO(_1, _2, _3, _4, _5, NAME, ...) NAME

/**
 * @def CEL_Task
 * @brief Declares a procedural coroutine task with persistent step tracking and unmount cleanup.
 *
 * What it does:
 * Generates an inline composable function and internal coroutine state machine. The task
 * allocates a `CelsTaskState` struct in the session's slot memory using cel_remember(),
 * pins execution step and non-blocking timers across recomposition passes, runs the `run`
 * block step-by-step cooperatively, and invokes the `cancel` teardown block when aborted
 * or unmounted.
 *
 * Expected outcome:
 * Enables procedural, multi-frame logic (animations, sequential API calls, timers, delays)
 * within declarative CELS code without spawning OS threads or blocking the main frame loop.
 *
 * Where to use:
 * Declare at file/global scope. Invoke inside any CEL_Composition or CEL_Composable using
 * `cel_task(TaskName, ...)`.
 *
 * Supported signatures:
 * - 0 arguments: CEL_Task(TaskName) { ... }
 * - 1 argument:  CEL_Task(TaskName, Type1, Arg1) { ... }
 * - 2 arguments: CEL_Task(TaskName, Type1, Arg1, Type2, Arg2) { ... }
 *
 * Example:
 * @code
 *     CEL_Task(ConnectTask, const char*, host, int, port) {
 *         cancel {
 *             CloseSocket();
 *         }
 *         run {
 *             InitSocket();
 *             cel_wait(100); // 100ms async wait
 *             ConnectTo(host, port);
 *             cel_yield();   // pause until next frame
 *             SendHandshake();
 *         }
 *     }
 *
 *     CEL_Composition(MainView, void *userData) {
 *         cel_task(ConnectTask, "127.0.0.1", 9000);
 *     }
 * @endcode
 */
#ifndef CEL_Task
#define CEL_Task(...) \
    _CEL_GET_TASK_MACRO(__VA_ARGS__, _CEL_TASK_2, _UNUSED, _CEL_TASK_1, _UNUSED, _CEL_TASK_0)(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
