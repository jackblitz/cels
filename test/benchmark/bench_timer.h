#pragma once

/**
 * @file bench_timer.h
 * @brief High-precision timer and latency statistics for CELS benchmarks.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

static inline uint64_t BenchGetTimeNs(void) {
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ULL) / freq.QuadPart);
}

#else
#include <time.h>

static inline uint64_t BenchGetTimeNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

typedef struct LatencyStats {
    double minUs;
    double meanUs;
    double p50Us;
    double p90Us;
    double p95Us;
    double p99Us;
    double maxUs;
} LatencyStats;

typedef struct BenchmarkResult {
    char name[64];
    char description[128];
    size_t sessions;
    size_t iterations;
    double totalTimeMs;
    double opsPerSec;
    LatencyStats latency;
    uint32_t activeGroupsPeak;
    uint32_t dataArenaPeakBytes;
} BenchmarkResult;

static int CompareUint64(const void *a, const void *b) {
    const uint64_t ua = *(const uint64_t *)a;
    const uint64_t ub = *(const uint64_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static inline void BenchCalculateStats(uint64_t *samplesNs, size_t sampleCount, LatencyStats *outStats) {
    if (sampleCount == 0 || samplesNs == NULL || outStats == NULL) {
        if (outStats) {
            outStats->minUs = 0;
            outStats->meanUs = 0;
            outStats->p50Us = 0;
            outStats->p90Us = 0;
            outStats->p95Us = 0;
            outStats->p99Us = 0;
            outStats->maxUs = 0;
        }
        return;
    }

    qsort(samplesNs, sampleCount, sizeof(uint64_t), CompareUint64);

    uint64_t sumNs = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        sumNs += samplesNs[i];
    }

    outStats->minUs = (double)samplesNs[0] / 1000.0;
    outStats->maxUs = (double)samplesNs[sampleCount - 1] / 1000.0;
    outStats->meanUs = ((double)sumNs / (double)sampleCount) / 1000.0;

    size_t idx50 = (size_t)(sampleCount * 0.50);
    size_t idx90 = (size_t)(sampleCount * 0.90);
    size_t idx95 = (size_t)(sampleCount * 0.95);
    size_t idx99 = (size_t)(sampleCount * 0.99);

    if (idx50 >= sampleCount) idx50 = sampleCount - 1;
    if (idx90 >= sampleCount) idx90 = sampleCount - 1;
    if (idx95 >= sampleCount) idx95 = sampleCount - 1;
    if (idx99 >= sampleCount) idx99 = sampleCount - 1;

    outStats->p50Us = (double)samplesNs[idx50] / 1000.0;
    outStats->p90Us = (double)samplesNs[idx90] / 1000.0;
    outStats->p95Us = (double)samplesNs[idx95] / 1000.0;
    outStats->p99Us = (double)samplesNs[idx99] / 1000.0;
}
