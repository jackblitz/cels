#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(_MSC_VER)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    typedef HANDLE CelsThreadHandle;
    typedef DWORD  CelsThreadId;
    typedef CRITICAL_SECTION CelsMutex;

    typedef DWORD (WINAPI *CelsThreadFn)(void *arg);
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <time.h>

    typedef pthread_t CelsThreadHandle;
    typedef pthread_t CelsThreadId;
    typedef pthread_mutex_t CelsMutex;

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
 * Returns monotonic time in milliseconds since an unspecified epoch.
 * Guaranteed strictly monotonic across Windows, Linux, and macOS.
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
 * Suspends calling thread for specified milliseconds.
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

static inline void CelsMutexInit(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    InitializeCriticalSection(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

static inline void CelsMutexLock(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static inline void CelsMutexUnlock(CelsMutex *mutex)
{
#if defined(_WIN32) || defined(_MSC_VER)
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

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

typedef struct CelsThread {
    CelsThreadHandle handle;
    bool isRunning;
} CelsThread;

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
