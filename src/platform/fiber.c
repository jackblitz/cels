#include "cels/runtime/fiber.h"
#include "cels/runtime/thread.h"
#include <stdlib.h>

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct CelsFiber {
    void *nativeFiber;
    bool  isThreadFiber;
};

static CELS_THREAD_LOCAL struct CelsFiber s_threadFiber = {0};
static CELS_THREAD_LOCAL bool s_threadFiberInitialized = false;

/**
 * Converts the calling OS thread into a primary fiber context.
 *
 * Checks if the thread has already been converted to a fiber. If not, invokes
 * ConvertThreadToFiber to obtain the native fiber handle, recording it in
 * thread-local storage. Subsequent calls on the same thread are idempotent O(1).
 *
 * @return True if the thread has a valid primary fiber context; false otherwise.
 */
bool CelsFiberInitThread(void)
{
    if (s_threadFiberInitialized) {
        return true;
    }
    void *fiber = NULL;
    if (!IsThreadAFiber()) {
        fiber = ConvertThreadToFiber(NULL);
    } else {
        fiber = GetCurrentFiber();
    }
    if (fiber == NULL) {
        fiber = GetCurrentFiber();
    }
    s_threadFiber.nativeFiber = fiber;
    s_threadFiber.isThreadFiber = true;
    s_threadFiberInitialized = (fiber != NULL);
    return s_threadFiberInitialized;
}

/**
 * Allocates and initializes a new stackful fiber on Windows using CreateFiberEx.
 *
 * Ensures the calling thread is initialized as a fiber, allocates a CelsFiber
 * wrapper structure on the heap, and creates the native OS fiber with the
 * specified stack commit and reserve boundaries and floating-point flags.
 *
 * @param fn               Fiber entry procedure to execute. Non-NULL.
 * @param param            User context pointer forwarded to the fiber entry function.
 * @param commitSizeBytes  Initial stack commit size in bytes (e.g. 4096).
 * @param reserveSizeBytes Total virtual stack reserve in bytes (e.g. 65536).
 * @return Pointer to the allocated CelsFiber instance, or NULL on allocation/OS failure.
 */
CelsFiber *CelsFiberCreate(
    CelsFiberFn fn,
    void *param,
    size_t commitSizeBytes,
    size_t reserveSizeBytes)
{
    if (!CelsFiberInitThread()) {
        return NULL;
    }
    CelsFiber *const fiber = (CelsFiber*)malloc(sizeof(CelsFiber));
    if (fiber == NULL) {
        return NULL;
    }
    fiber->isThreadFiber = false;
    DWORD flags = 0;
#if defined(FIBER_FLAG_FLOAT_SWITCH)
    flags = FIBER_FLAG_FLOAT_SWITCH;
#endif
    fiber->nativeFiber = CreateFiberEx(commitSizeBytes, reserveSizeBytes, flags, (LPFIBER_START_ROUTINE)fn, param);
    if (fiber->nativeFiber == NULL) {
        free(fiber);
        return NULL;
    }
    return fiber;
}

/**
 * Suspends the calling fiber and transfers CPU execution to the target fiber.
 *
 * Performs a Win32 SwitchToFiber context switch. Execution will return immediately
 * after this call when another fiber (or the target fiber) switches back.
 *
 * @param targetFiber Target fiber to resume. Safe if NULL or if nativeFiber is NULL.
 */
void CelsFiberSwitch(CelsFiber *targetFiber)
{
    if (targetFiber == NULL || targetFiber->nativeFiber == NULL) {
        return;
    }
    SwitchToFiber(targetFiber->nativeFiber);
}

/**
 * Destroys a secondary fiber and frees associated stack and heap memory.
 *
 * Checks whether the fiber is a primary thread fiber (which cannot be deleted via
 * DeleteFiber). If secondary, invokes Win32 DeleteFiber and frees the wrapper.
 *
 * @param fiber Fiber handle to destroy. Safe if NULL.
 */
void CelsFiberDestroy(CelsFiber *fiber)
{
    if (fiber == NULL) {
        return;
    }
    if (!fiber->isThreadFiber && fiber->nativeFiber != NULL) {
        DeleteFiber(fiber->nativeFiber);
    }
    free(fiber);
}

/**
 * Resolves the active thread's primary fiber handle.
 *
 * Lazily initializes the calling thread as a fiber if not already converted,
 * and returns a pointer to the thread-local primary fiber structure.
 *
 * @return Pointer to the active thread's primary CelsFiber structure.
 */
CelsFiber *CelsFiberGetCurrent(void)
{
    if (!s_threadFiberInitialized) {
        CelsFiberInitThread();
    }
    return &s_threadFiber;
}

/* ========================================================================= */
/* MinGW-w64 Static Linking Compatibility Shims                              */
/* ========================================================================= */
#if defined(__GNUC__)
#include <stdio.h>
#include <stdarg.h>

/**
 * When statically linking libwinpthread (-lwinpthread) on MinGW-w64,
 * libwinpthread expects __intrinsic_setjmpex and __ms_vsnprintf.
 * Providing these lightweight shims allows full static embedding of
 * libwinpthread with zero external runtime DLL dependencies.
 */
int _setjmpex(void *env, void *frame);

/**
 * MinGW-w64 static linking compatibility shim for setjmp execution context setup.
 *
 * Delegates execution to the MSVCRT _setjmpex runtime symbol.
 *
 * @param env   Buffer receiving execution context environment state.
 * @param frame Stack frame pointer.
 * @return 0 on direct return, or non-zero value passed to longjmp.
 */
int __intrinsic_setjmpex(void *env, void *frame)
{
    return _setjmpex(env, frame);
}

/**
 * Formatted variable argument printing shim for MinGW static winpthread linking.
 *
 * Directs formatting to the MSVCRT _vsnprintf function.
 *
 * @param buffer Output destination string buffer.
 * @param count  Maximum number of characters to write.
 * @param format Format string.
 * @param argptr Variable arguments list.
 * @return Number of characters written, or negative value on truncation/error.
 */
int __ms_vsnprintf(char *buffer, size_t count, const char *format, va_list argptr)
{
    return _vsnprintf(buffer, count, format, argptr);
}
#endif /* defined(__GNUC__) */

#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)

#if defined(__APPLE__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <ucontext.h>

struct CelsFiber {
    ucontext_t ctx;
    void      *stack;
    size_t     stackSize;
    bool       isThreadFiber;
};

static CELS_THREAD_LOCAL struct CelsFiber s_threadFiber = {0};
static CELS_THREAD_LOCAL bool s_threadFiberInitialized = false;
static CELS_THREAD_LOCAL CelsFiber *s_currentFiber = NULL;

/**
 * Initializes the calling POSIX thread as the primary fiber context.
 *
 * Sets up thread-local tracking so that the primary thread context can serve as the
 * return point when secondary fibers yield or terminate. Idempotent.
 *
 * @return True always on POSIX.
 */
bool CelsFiberInitThread(void)
{
    if (!s_threadFiberInitialized) {
        s_threadFiber.isThreadFiber = true;
        s_threadFiberInitialized = true;
        s_currentFiber = &s_threadFiber;
    }
    return true;
}

typedef struct CelsFiberThunk {
    CelsFiberFn fn;
    void       *param;
} CelsFiberThunk;

/**
 * Trampoline function executed on the newly created fiber stack.
 *
 * @param low  Low 32 bits of the heap-allocated CelsFiberThunk pointer.
 * @param high High 32 bits of the heap-allocated CelsFiberThunk pointer.
 */
static void CelsFiberTrampoline(uint32_t low, uint32_t high)
{
#if UINTPTR_MAX > 0xFFFFFFFFU
    const uintptr_t ptr = (uintptr_t)low | ((uintptr_t)high << 32);
#else
    (void)high;
    const uintptr_t ptr = (uintptr_t)low;
#endif
    CelsFiberThunk *const thunk = (CelsFiberThunk*)ptr;
    if (thunk != NULL) {
        CelsFiberFn fn = thunk->fn;
        void *const param = thunk->param;
        free(thunk);
        if (fn != NULL) {
            fn(param);
        }
    }
    s_currentFiber = &s_threadFiber;
}

/**
 * Allocates and initializes a new stackful fiber on POSIX using ucontext.
 *
 * Allocates a separate heap memory stack, initializes the ucontext_t structure
 * via getcontext/makecontext, and wires execution through CelsFiberTrampoline.
 *
 * @param fn               Fiber entry procedure to execute. Non-NULL.
 * @param param            User context pointer forwarded to the fiber entry function.
 * @param commitSizeBytes  Ignored on POSIX systems.
 * @param reserveSizeBytes Stack allocation size in bytes (defaults to 65536 if 0).
 * @return Pointer to the allocated CelsFiber instance, or NULL on failure.
 */
CelsFiber *CelsFiberCreate(
    CelsFiberFn fn,
    void *param,
    size_t commitSizeBytes,
    size_t reserveSizeBytes)
{
    (void)commitSizeBytes;
    if (!CelsFiberInitThread()) {
        return NULL;
    }
    CelsFiber *const fiber = (CelsFiber*)malloc(sizeof(CelsFiber));
    if (fiber == NULL) {
        return NULL;
    }
    const size_t stackSize = reserveSizeBytes ? reserveSizeBytes : 65536;
    fiber->stack = malloc(stackSize);
    if (fiber->stack == NULL) {
        free(fiber);
        return NULL;
    }
    fiber->stackSize = stackSize;
    fiber->isThreadFiber = false;

    if (getcontext(&fiber->ctx) != 0) {
        free(fiber->stack);
        free(fiber);
        return NULL;
    }
    fiber->ctx.uc_stack.ss_sp = fiber->stack;
    fiber->ctx.uc_stack.ss_size = stackSize;
    fiber->ctx.uc_stack.ss_flags = 0;
    fiber->ctx.uc_link = &s_threadFiber.ctx;

    CelsFiberThunk *const thunk = (CelsFiberThunk*)malloc(sizeof(CelsFiberThunk));
    if (thunk == NULL) {
        free(fiber->stack);
        free(fiber);
        return NULL;
    }
    thunk->fn = fn;
    thunk->param = param;
    const uintptr_t ptr = (uintptr_t)thunk;
    const uint32_t low = (uint32_t)(ptr & 0xFFFFFFFFU);
#if UINTPTR_MAX > 0xFFFFFFFFU
    const uint32_t high = (uint32_t)((ptr >> 32) & 0xFFFFFFFFU);
#else
    const uint32_t high = 0;
#endif

    makecontext(&fiber->ctx, (void (*)(void))CelsFiberTrampoline, 2, low, high);
    return fiber;
}

/**
 * Suspends the calling fiber and transfers execution to the target fiber via swapcontext.
 *
 * @param targetFiber Target fiber to resume. Safe if NULL.
 */
void CelsFiberSwitch(CelsFiber *targetFiber)
{
    if (targetFiber == NULL) {
        return;
    }
    CelsFiber *const prev = s_currentFiber ? s_currentFiber : &s_threadFiber;
    s_currentFiber = targetFiber;
    swapcontext(&prev->ctx, &targetFiber->ctx);
}

/**
 * Destroys a POSIX fiber and frees its dedicated stack buffer and wrapper.
 *
 * @param fiber Fiber handle to destroy. Safe if NULL or if thread fiber.
 */
void CelsFiberDestroy(CelsFiber *fiber)
{
    if (fiber == NULL) {
        return;
    }
    if (!fiber->isThreadFiber) {
        if (fiber->stack != NULL) {
            free(fiber->stack);
        }
    }
    free(fiber);
}

/**
 * Resolves the currently active fiber handle on POSIX.
 *
 * @return Pointer to the active CelsFiber structure.
 */
CelsFiber *CelsFiberGetCurrent(void)
{
    if (!s_threadFiberInitialized) {
        CelsFiberInitThread();
    }
    return s_currentFiber ? s_currentFiber : &s_threadFiber;
}

#if defined(__APPLE__)
    #pragma clang diagnostic pop
#endif

#endif
