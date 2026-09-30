#pragma once

/**
 * @file fiber.h
 * @brief Cross-Platform Cooperative Execution Fibers for CELS.
 *
 * Provides stackful cooperative fibers for procedural tasks:
 * - Native Win32 Fibers on Windows (CreateFiberEx, SwitchToFiber, DeleteFiber).
 * - POSIX ucontext on Linux and macOS (getcontext, makecontext, swapcontext).
 * - Full stack preservation: local variables, nested function calls, and native C
 *   control flow (switch/case, loops) survive across yields.
 *
 * Typical usage:
 * @code
 *     // Initialize calling thread as fiber host
 *     CelsFiberInitThread();
 *     CelsFiber *caller = CelsFiberGetCurrent();
 *
 *     // Create procedural worker fiber
 *     CelsFiber *taskFiber = CelsFiberCreate(MyWorkerFn, context, 4096, 65536);
 *     if (taskFiber != NULL) {
 *         // Switch to worker fiber
 *         CelsFiberSwitch(taskFiber);
 *
 *         // Clean up fiber when completed
 *         CelsFiberDestroy(taskFiber);
 *     }
 * @endcode
 *
 * Thread safety: Fibers are inherently bound to the executing OS thread.
 * Switching between fibers must occur on the same thread that created or
 * hosts them. CelsFiberInitThread and CelsFiberGetCurrent operate on
 * thread-local state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CelsFiber CelsFiber;

/** Fiber entry function prototype. */
typedef void (*CelsFiberFn)(void *param);

/**
 * Initializes the current thread as a fiber host.
 * Safe to call multiple times on the same thread.
 *
 * Where to use: Called automatically by task scheduling or manually before
 * creating or switching fibers.
 *
 * @return True on success; false on failure.
 */
bool CelsFiberInitThread(void);

/**
 * Creates a new cooperative fiber with dedicated stack space.
 *
 * Where to use: Called during task instantiation to allocate stack memory.
 *
 * @param fn               Entry function executed when switched to for the first time. Non-NULL.
 * @param param            Opaque pointer passed to fn. Safe if NULL.
 * @param commitSizeBytes  Initial stack commit in bytes (e.g. 4096 for 4KB page).
 * @param reserveSizeBytes Virtual address space reservation in bytes (e.g. 65536 for 64KB).
 * @return Pointer to newly allocated CelsFiber, or NULL on error. Caller must free with CelsFiberDestroy.
 */
CelsFiber *CelsFiberCreate(
    CelsFiberFn fn,
    void *param,
    size_t commitSizeBytes,
    size_t reserveSizeBytes
);

/**
 * Switches execution cooperatively from the active fiber to targetFiber.
 *
 * Execution returns here when targetFiber yields or completes.
 *
 * Where to use: Called within task scheduling or inside a running fiber.
 *
 * @param targetFiber Destination fiber to resume. Ignored if NULL.
 */
void CelsFiberSwitch(CelsFiber *targetFiber);

/**
 * Destroys a cooperative fiber and frees its stack.
 *
 * Never call this function on the currently executing fiber.
 *
 * @param fiber Fiber handle to destroy. Safe if NULL (no-op).
 */
void CelsFiberDestroy(CelsFiber *fiber);

/**
 * Returns a handle to the currently active fiber on the calling thread.
 *
 * @return Active CelsFiber handle, or thread fiber handle.
 */
CelsFiber *CelsFiberGetCurrent(void);

#ifdef __cplusplus
}
#endif
