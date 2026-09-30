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

void CelsFiberSwitch(CelsFiber *targetFiber)
{
    if (targetFiber == NULL || targetFiber->nativeFiber == NULL) {
        return;
    }
    SwitchToFiber(targetFiber->nativeFiber);
}

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

CelsFiber *CelsFiberGetCurrent(void)
{
    if (!s_threadFiberInitialized) {
        CelsFiberInitThread();
    }
    return &s_threadFiber;
}

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

void CelsFiberSwitch(CelsFiber *targetFiber)
{
    if (targetFiber == NULL) {
        return;
    }
    CelsFiber *const prev = s_currentFiber ? s_currentFiber : &s_threadFiber;
    s_currentFiber = targetFiber;
    swapcontext(&prev->ctx, &targetFiber->ctx);
}

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
