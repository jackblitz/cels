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

#include "cels/slot_table.h"
#include "cels/session.h"
#include "cels/thread.h"
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

/**
 * Defines the cancellation teardown block for a CEL_Task.
 * Executed upon unmount, cel_cancel(), or external cel_cancel_task().
 */
#define cancel \
    _cels_task_cancel_entry: \
    if (_cels_task_phase == CEL_TASK_PHASE_CANCEL) \
        for (int _cels_canc_once = 1; _cels_canc_once; _cels_canc_once = 0)

/**
 * Defines the executable body of a CEL_Task.
 * Automatically marks completion when the block finishes.
 */
#define run \
    if (0) goto _cels_task_cancel_entry; \
    if (_cels_task_phase == CEL_TASK_PHASE_RUN) \
        for (int _cels_task_loop = 1; _cels_task_loop; \
             _cels_task_loop = 0, \
             CelsTaskComplete(_cels_task_state)) \
            switch (_cels_task_state->step) case 0:

/**
 * Suspends task execution and yields control until the next frame.
 */
#define cel_yield() \
    do { \
        CelsTaskYield(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, __LINE__); \
        return; \
        case __LINE__:; \
    } while(0)

/**
 * Suspends task execution for a specified non-blocking delay in milliseconds.
 */
#define cel_wait(ms) \
    do { \
        CelsTaskWait(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, __LINE__, (uint32_t)(ms)); \
        return; \
        case __LINE__: \
            if (CelsTaskShouldWait(CelsGetCurrentSession(), _cels_task_state, _cels_task_key)) { \
                return; \
            } \
    } while(0)

/**
 * Cancels task execution from within the task body and jumps immediately
 * to the cancel teardown block.
 */
#define cel_cancel() \
    do { \
        CelsTaskCancel(_cels_task_state); \
        _cels_task_phase = CEL_TASK_PHASE_CANCEL; \
        goto _cels_task_cancel_entry; \
    } while(0)

/**
 * Invokes a task within an active composition.
 */
#define cel_task(Name, ...) Name(__VA_ARGS__)

/**
 * Externally cancels an active task instance.
 */
#define cel_cancel_task(Name) _cels_task_cancel_##Name(CelsGetCurrentSession())

/**
 * Queries whether a task is actively running.
 */
#define cel_is_task_running(Name) _cels_task_is_running_##Name(CelsGetCurrentSession())

/**
 * Queries whether a task has completed execution.
 */
#define cel_is_task_done(Name) _cels_task_is_done_##Name(CelsGetCurrentSession())

/**
 * Queries whether a task has been cancelled.
 */
#define cel_is_task_cancelled(Name) _cels_task_is_cancelled_##Name(CelsGetCurrentSession())

/**
 * Restarts a task from step 0 and triggers recomposition.
 */
#define cel_restart_task(Name) _cels_task_restart_##Name(CelsGetCurrentSession())

/* ========================================================================= */
/* CEL_Task 0 Arguments Implementation                                       */
/* ========================================================================= */

#define _CEL_TASK_0(Name) \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        CelsTaskState *state = (CelsTaskState*)instance; \
        if (state != NULL && !state->isDone && !state->isCancelled) { \
            CelsTaskCancel(state); \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, state, CelsHashKey(#Name)); \
        } \
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
                ((CelsTaskState){ .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }), \
                _cels_task_clean_##Name \
            ); \
            _cels_task_last_##Name = state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(state); \
                CelsSessionRegisterLifecycle(sess, state, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, state, _cels_task_clean_##Name); \
            } \
            if (!CelsTaskIsCancelled(state) && !CelsTaskIsDone(state)) { \
                if (CelsTaskShouldWait(sess, state, key)) { \
                    CelsExitGroup(sess); \
                    return; \
                } \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, state, key); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key)

/* ========================================================================= */
/* CEL_Task 1 Argument Implementation (Type1, Arg1)                          */
/* ========================================================================= */

#define _CEL_TASK_1(Name, Type1, Arg1) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState state; \
        Type1 Arg1; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->state.isDone && !box->state.isCancelled) { \
            CelsTaskCancel(&box->state); \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->state, CelsHashKey(#Name), box->Arg1); \
        } \
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
                ((_CelsTaskBox_##Name){ .state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }, .Arg1 = Arg1 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            _cels_task_last_##Name = &box->state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(&box->state); \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            if (!CelsTaskIsCancelled(&box->state) && !CelsTaskIsDone(&box->state)) { \
                if (CelsTaskShouldWait(sess, &box->state, key)) { \
                    CelsExitGroup(sess); \
                    return; \
                } \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->state, key, Arg1); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1)

/* ========================================================================= */
/* CEL_Task 2 Arguments Implementation (Type1, Arg1, Type2, Arg2)            */
/* ========================================================================= */

#define _CEL_TASK_2(Name, Type1, Arg1, Type2, Arg2) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState state; \
        Type1 Arg1; \
        Type2 Arg2; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->state.isDone && !box->state.isCancelled) { \
            CelsTaskCancel(&box->state); \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->state, CelsHashKey(#Name), box->Arg1, box->Arg2); \
        } \
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
                ((_CelsTaskBox_##Name){ .state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }, .Arg1 = Arg1, .Arg2 = Arg2 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            box->Arg2 = Arg2; \
            _cels_task_last_##Name = &box->state; \
            if (CelsIsFreshMount(sess)) { \
                CelsTaskInit(&box->state); \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            if (!CelsTaskIsCancelled(&box->state) && !CelsTaskIsDone(&box->state)) { \
                if (CelsTaskShouldWait(sess, &box->state, key)) { \
                    CelsExitGroup(sess); \
                    return; \
                } \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->state, key, Arg1, Arg2); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2)

/* ========================================================================= */
/* CEL_Task Macro Dispatcher                                                 */
/* ========================================================================= */

#define _CEL_GET_TASK_MACRO(_1, _2, _3, _4, _5, NAME, ...) NAME

/**
 * Declares a cooperative coroutine task with optional parameters.
 *
 * Supports:
 * - 0 parameters: CEL_Task(TaskName) { ... }
 * - 1 parameter:  CEL_Task(TaskName, Type1, Arg1) { ... }
 * - 2 parameters: CEL_Task(TaskName, Type1, Arg1, Type2, Arg2) { ... }
 */
#define CEL_Task(...) \
    _CEL_GET_TASK_MACRO(__VA_ARGS__, _CEL_TASK_2, _UNUSED, _CEL_TASK_1, _UNUSED, _CEL_TASK_0)(__VA_ARGS__)

#ifdef __cplusplus
}
#endif
