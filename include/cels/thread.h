#pragma once

/**
 * @file thread.h
 * @brief Cross-Platform Threading, Monotonic Timing, and Synchronization Primitives.
 *
 * Provides a minimal, zero-overhead abstraction layer over Win32 and POSIX
 * primitives for high-precision monotonic timing, mutex synchronization,
 * and background worker threads.
 *
 * Typical usage:
 * @code
 *     // High-precision timing
 *     uint64_t startMs = CelsGetTimeMs();
 *     CelsSleepMs(16);
 *     uint64_t elapsedMs = CelsGetTimeMs() - startMs;
 *
 *     // Mutex synchronization
 *     CelsMutex mutex;
 *     CelsMutexInit(&mutex);
 *     CelsMutexLock(&mutex);
 *     // ... critical section ...
 *     CelsMutexUnlock(&mutex);
 *     CelsMutexDestroy(&mutex);
 *
 *     // Background worker thread
 *     CelsThread thread;
 *     CelsThreadCreate(&thread, WorkerFunction, context);
 *     CelsThreadJoin(&thread);
 * @endcode
 *
 * Thread safety: Mutex operations are thread-safe. Thread handle functions
 * should be called by the thread's owner/creator.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_MSC_VER)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    /** Native thread handle type. */
    typedef HANDLE CelsThreadHandle;
    /** Native thread identifier type. */
    typedef DWORD  CelsThreadId;
    /** Native non-recursive mutex type. */
    typedef CRITICAL_SECTION CelsMutex;

    /** Native thread procedure signature. */
    typedef DWORD (WINAPI *CelsThreadFn)(void *arg);
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <time.h>

    /** Native thread handle type. */
    typedef pthread_t CelsThreadHandle;
    /** Native thread identifier type. */
    typedef pthread_t CelsThreadId;
    /** Native non-recursive mutex type. */
    typedef pthread_mutex_t CelsMutex;

    /** Native thread procedure signature. */
    typedef void* (*CelsThreadFn)(void *arg);
#endif

#ifndef CELS_THREAD_LOCAL
    #if defined(_MSC_VER)
        #define CELS_THREAD_LOCAL __declspec(thread)
    #elif (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL __thread
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL _Thread_local
    #else
        #define CELS_THREAD_LOCAL
    #endif
#endif

/* ========================================================================= */
/* High-Precision Monotonic Time (ms)                                        */
/* ========================================================================= */

/**
 * Returns monotonic time in milliseconds since an unspecified system epoch.
 *
 * Guaranteed strictly monotonic across Windows (via QueryPerformanceCounter)
 * and POSIX (via CLOCK_MONOTONIC). Unaffected by system clock changes.
 *
 * @return Current timestamp in milliseconds.
 */
static inline uint64_t CelsGetTimeMs(void)
{
#if defined(_WIN32) || defined(_MSC_VER)
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
#endif
}

/**
 * Suspends calling thread for specified duration in milliseconds.
 *
 * @param ms Sleep duration in milliseconds.
 */
static inline void CelsSleepMs(uint32_t ms)
{
#if defined(_WIN32) || defined(_MSC_VER)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ========================================================================= */
/* Cross-Platform Mutex Primitives                                           */
/* ========================================================================= */

/**
 * Initializes a mutex object before first use.
 *
 * @param mutex Pointer to uninitialized mutex structure. Non-NULL.
 */
static inline void CelsMutexInit(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    InitializeCriticalSection(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

/**
 * Acquires exclusive ownership of a mutex, blocking until available.
 *
 * @param mutex Pointer to initialized mutex structure. Non-NULL.
 */
static inline void CelsMutexLock(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

/**
 * Releases exclusive ownership of a mutex previously acquired via CelsMutexLock.
 *
 * @param mutex Pointer to locked mutex structure. Non-NULL.
 */
static inline void CelsMutexUnlock(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

/**
 * Destroys a mutex and releases any associated operating system resources.
 *
 * @param mutex Pointer to mutex structure. Non-NULL.
 */
static inline void CelsMutexDestroy(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

/* ========================================================================= */
/* Cross-Platform Thread Creation & Joining                                  */
/* ========================================================================= */

/**
 * Lightweight thread handle wrapper.
 */
typedef struct CelsThread {
    CelsThreadHandle handle;    /**< Underlying OS thread handle */
    bool             isRunning; /**< True while thread is active */
} CelsThread;

/**
 * Spawns a new OS thread executing the specified function.
 *
 * @param thread Pointer to thread struct to populate. Non-NULL.
 * @param fn     Thread entry point function. Non-NULL.
 * @param arg    Argument passed to thread procedure. Can be NULL.
 * @return True if thread was successfully created; false otherwise.
 */
static inline bool CelsThreadCreate(CelsThread *thread, CelsThreadFn fn, void *arg)
{
    if (thread == NULL || fn == NULL) return false;

#if defined(_WIN32) || defined(_MSC_VER)
    thread->handle = CreateThread(NULL, 0, fn, arg, 0, NULL);
    thread->isRunning = (thread->handle != NULL);
    return thread->isRunning;
#else
    int res = pthread_create(&thread->handle, NULL, fn, arg);
    thread->isRunning = (res == 0);
    return thread->isRunning;
#endif
}

/**
 * Waits for a thread to terminate and cleans up its handle.
 *
 * @param thread Pointer to active thread struct. Safe if NULL or inactive.
 */
static inline void CelsThreadJoin(CelsThread *thread)
{
    if (thread == NULL || !thread->isRunning) return;

#if defined(_WIN32) || defined(_MSC_VER)
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    thread->handle = NULL;
#else
    pthread_join(thread->handle, NULL);
#endif
    thread->isRunning = false;
}

#ifdef __cplusplus
}
#endif
