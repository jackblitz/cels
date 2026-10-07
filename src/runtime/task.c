#include "cels/runtime/task.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/runtime/session.h"
#include "cels/runtime/thread.h"
#include "cels/runtime/fiber.h"

/**
 * Initializes or resets a task state struct to its default runnable state.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskInit(CelsTaskState *state)
{
    if (state == NULL) {
        return;
    }
    state->step = 0;
    state->waitTimerMs = 0;
    state->isRunning = true;
    state->isCancelled = false;
    state->isDone = false;
    state->taskFiber = NULL;
    state->callerFiber = NULL;
}

/**
 * Checks whether a waiting task deadline has elapsed.
 *
 * If the monotonic timer has not reached waitTimerMs, the task's session group
 * key is queued for invalidation on the next frame and this returns true.
 * If the deadline has passed, waitTimerMs is reset to zero and returns false.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Non-NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @return True if the task is still waiting; false if ready to resume.
 */
bool CelsTaskShouldWait(CelsSession *session, CelsTaskState *state, uint64_t key)
{
    if (state == NULL) {
        return false;
    }
    if (state->waitTimerMs > 0) {
        const uint64_t now = CelsGetTimeMs();
        if (now < state->waitTimerMs) {
            if (session != NULL) {
                CelsSessionInvalidateKey(session, key);
            }
            return true;
        }
        state->waitTimerMs = 0;
    }
    return false;
}

/**
 * Yields task execution until the subsequent recomposition pass.
 *
 * Records the line step identifier in state and registers an invalidation
 * on the session so the task will be evaluated on the next frame.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Non-NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @param step    Resume step identifier.
 */
void CelsTaskYield(CelsSession *session, CelsTaskState *state, uint64_t key, int step)
{
    if (state == NULL) {
        return;
    }
    state->step = step;
    if (session != NULL) {
        CelsSessionInvalidateKey(session, key);
    }
}

/**
 * Suspends task execution for a specified non-blocking delay in milliseconds.
 *
 * Computes a monotonic deadline timestamp, records the resume step, and
 * invalidates the session group key for continued polling.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Non-NULL.
 * @param key     Unique 64-bit key of the task composable group.
 * @param step    Resume step identifier.
 * @param delayMs Delay duration in milliseconds.
 */
void CelsTaskWait(CelsSession *session, CelsTaskState *state, uint64_t key, int step, uint32_t delayMs)
{
    if (state == NULL) {
        return;
    }
    state->waitTimerMs = CelsGetTimeMs() + (uint64_t)delayMs;
    state->step = step;
    if (session != NULL) {
        CelsSessionInvalidateKey(session, key);
    }
}

/**
 * Advances a task fiber by executing one step during recomposition.
 *
 * Checks whether the task is eligible to run (verifying not cancelled, not done,
 * timer deadline elapsed via CelsTaskShouldWait, and event conditions satisfied via
 * CelsTaskEventShouldWait). If eligible, creates the dedicated fiber if not yet
 * allocated, captures the current caller fiber as return target, switches execution
 * into the fiber, and upon return cleans up the fiber if the task completed or cancelled.
 *
 * @param session Active session owning the task. Safe if NULL.
 * @param state   Pointer to task state structure. Safe if NULL.
 * @param key     Unique 64-bit key of the task composable group for invalidation.
 * @param fiberFn Fiber entry trampoline function.
 * @param param   Context parameter forwarded to the fiber entry function.
 */
void CelsTaskStep(CelsSession *session, CelsTaskState *state, uint64_t key, CelsFiberFn fiberFn, void *param)
{
    if (state == NULL || state->isCancelled || state->isDone) {
        return;
    }
    if (CelsTaskShouldWait(session, state, key)) {
        return;
    }
    if (CelsTaskEventShouldWait(session, state, key)) {
        return;
    }

    CelsFiberInitThread();
    if (state->taskFiber == NULL) {
        state->taskFiber = CelsFiberCreate(fiberFn, param, 4096, 65536);
        if (state->taskFiber == NULL) {
            return;
        }
    }

    state->callerFiber = CelsFiberGetCurrent();
    state->isRunning = true;
    if (session != NULL) {
        session->isExecutingTask = true;
    }
    CelsFiberSwitch((CelsFiber*)state->taskFiber);
    if (session != NULL) {
        session->isExecutingTask = false;
    }

    // Safely destroy fiber from caller thread after task completes or cancels
    if (state->isDone || state->isCancelled) {
        if (state->taskFiber != NULL) {
            CelsFiberDestroy((CelsFiber*)state->taskFiber);
            state->taskFiber = NULL;
        }
    }
}

/**
 * Completes a task fiber and switches execution back to the caller fiber.
 *
 * Invoked at the end of a task fiber's run block. Marks the task as completed
 * (if not cancelled) and switches CPU context back to the session's calling fiber.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskFinishFiber(CelsTaskState *state)
{
    if (state == NULL) {
        return;
    }
    if (!state->isCancelled) {
        CelsTaskComplete(state);
    }
    CelsFiber *caller = (CelsFiber*)state->callerFiber;
    if (caller != NULL) {
        CelsFiberSwitch(caller);
    }
}

/**
 * Destroys any active stackful fiber associated with a task state.
 *
 * Deallocates the fiber and stack memory when a task unmounts from the composable
 * tree or is explicitly cancelled. Resets the taskFiber pointer to NULL.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskCleanup(CelsTaskState *state)
{
    if (state == NULL) {
        return;
    }
    if (state->taskFiber != NULL) {
        CelsFiberDestroy((CelsFiber*)state->taskFiber);
        state->taskFiber = NULL;
    }
}

/**
 * Marks a task as successfully completed.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskComplete(CelsTaskState *state)
{
    if (state == NULL) {
        return;
    }
    state->step = -1;
    state->isRunning = false;
    state->isDone = true;
}

/**
 * Transitions a task to the cancelled state.
 *
 * @param state Pointer to task state structure. Safe if NULL.
 */
void CelsTaskCancel(CelsTaskState *state)
{
    if (state == NULL) {
        return;
    }
    state->step = -2;
    state->isRunning = false;
    state->isCancelled = true;
}

/**
 * Resets a task state to step 0 and triggers session invalidation.
 *
 * @param session Active session owning the task. Can be NULL.
 * @param state   Pointer to task state structure. Non-NULL.
 * @param key     Unique 64-bit key of the task composable group.
 */
void CelsTaskRestart(CelsSession *session, CelsTaskState *state, uint64_t key)
{
    if (state == NULL) {
        return;
    }
    if (state->taskFiber != NULL) {
        CelsFiberDestroy((CelsFiber*)state->taskFiber);
        state->taskFiber = NULL;
    }
    state->step = 0;
    state->waitTimerMs = 0;
    state->isRunning = true;
    state->isCancelled = false;
    state->isDone = false;
    state->callerFiber = NULL;
    if (session != NULL) {
        CelsSessionInvalidateKey(session, key);
    }
}

/**
 * Tests whether a task is actively running.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if running and neither cancelled nor done.
 */
bool CelsTaskIsRunning(const CelsTaskState *state)
{
    return (state != NULL && state->isRunning && !state->isCancelled && !state->isDone);
}

/**
 * Tests whether a task has completed execution.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if completed.
 */
bool CelsTaskIsDone(const CelsTaskState *state)
{
    return (state != NULL && state->isDone);
}

/**
 * Tests whether a task was cancelled.
 *
 * @param state Pointer to task state structure. NULL returns false.
 * @return True if cancelled.
 */
bool CelsTaskIsCancelled(const CelsTaskState *state)
{
    return (state != NULL && state->isCancelled);
}
