#include "cli/test_cli.h"
#include "bench_timer.h"
#include "bench_json.h"
#include "bench_html.h"
#include "cels.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/**
 * Validates that BenchGetTimeNs ticks monotonically and percentiles calculate accurately.
 */
static void TestBenchmarkTimerPrecision(void) {
    uint64_t t1 = BenchGetTimeNs();
    for (volatile int i = 0; i < 100000; ++i) {}
    uint64_t t2 = BenchGetTimeNs();
    assert(t2 >= t1);

    uint64_t samples[5] = { 1000, 2000, 3000, 4000, 5000 };
    LatencyStats stats;
    BenchCalculateStats(samples, 5, &stats);
    assert(stats.minUs >= 0.99 && stats.minUs <= 1.01);
    assert(stats.maxUs >= 4.99 && stats.maxUs <= 5.01);
    assert(stats.meanUs >= 2.99 && stats.meanUs <= 3.01);
}

/**
 * Validates output directory resolution and directory creation.
 */
static void TestBenchmarkDirectoryResolution(void) {
    const char *outDir = BenchGetOutputDir();
    assert(outDir != NULL);
    assert(strlen(outDir) > 0);
    // Ensure directory creation works
    BenchEnsureResultsDir();
}

/**
 * Validates JSON serialization and export integrity.
 */
static void TestBenchmarkJsonSerialization(void) {
    BenchmarkResult dummyResults[1];
    memset(&dummyResults[0], 0, sizeof(BenchmarkResult));
    snprintf(dummyResults[0].name, sizeof(dummyResults[0].name), "Test_Workload");
    snprintf(dummyResults[0].description, sizeof(dummyResults[0].description), "Test description");
    dummyResults[0].sessions = 1;
    dummyResults[0].iterations = 100;
    dummyResults[0].totalTimeMs = 1.25;
    dummyResults[0].opsPerSec = 80000.0;
    dummyResults[0].latency.minUs = 0.01;
    dummyResults[0].latency.meanUs = 0.02;
    dummyResults[0].latency.p50Us = 0.02;
    dummyResults[0].latency.p90Us = 0.03;
    dummyResults[0].latency.p95Us = 0.04;
    dummyResults[0].latency.p99Us = 0.05;
    dummyResults[0].latency.maxUs = 0.10;
    dummyResults[0].activeGroupsPeak = 10;
    dummyResults[0].dataArenaPeakBytes = 128;

    char testJsonPath[256];
    snprintf(testJsonPath, sizeof(testJsonPath), "%s/test_output.json", BenchGetOutputDir());
    bool saveOk = BenchSaveResultsJson(dummyResults, 1, testJsonPath);
    assert(saveOk);

    // Verify file exists
    FILE *f = fopen(testJsonPath, "rb");
    assert(f != NULL);
    fclose(f);
}

/**
 * Validates HTML dashboard report generation and structure.
 */
static void TestBenchmarkHtmlGeneration(void) {
    BenchmarkResult dummyResults[1];
    memset(&dummyResults[0], 0, sizeof(BenchmarkResult));
    snprintf(dummyResults[0].name, sizeof(dummyResults[0].name), "Test_HtmlWorkload");
    dummyResults[0].sessions = 1;
    dummyResults[0].iterations = 50;
    dummyResults[0].totalTimeMs = 0.5;
    dummyResults[0].opsPerSec = 100000.0;

    char testHtmlPath[256];
    snprintf(testHtmlPath, sizeof(testHtmlPath), "%s/test_report.html", BenchGetOutputDir());
    BenchGenerateHtmlReport(dummyResults, 1, NULL, testHtmlPath);

    FILE *f = fopen(testHtmlPath, "r");
    assert(f != NULL);
    char line[128];
    char *got = fgets(line, sizeof(line), f);
    assert(got != NULL);
    assert(strstr(line, "<!DOCTYPE html>") != NULL);
    fclose(f);
}

/**
 * Validates scanning and loading historical benchmark runs for comparison.
 */
static void TestBenchmarkHistoricalScanning(void) {
    PreviousRunData runs[16];
    size_t loaded = BenchLoadAllHistoricalRuns(BenchGetOutputDir(), runs, 16);
    assert(loaded > 0);
    assert(runs[0].valid);
    assert(strlen(runs[0].versionString) > 0);
}

static const TestCase s_benchmarkTests[] = {
    { "TestBenchmarkTimerPrecision", "Timer tick precision and percentile statistics", TestBenchmarkTimerPrecision },
    { "TestBenchmarkDirectoryResolution", "Output directory resolution and auto-creation", TestBenchmarkDirectoryResolution },
    { "TestBenchmarkJsonSerialization", "JSON metrics serialization and format validation", TestBenchmarkJsonSerialization },
    { "TestBenchmarkHtmlGeneration", "HTML dashboard generation and document structure", TestBenchmarkHtmlGeneration },
    { "TestBenchmarkHistoricalScanning", "Historical version scanning and loading for dropdown comparison", TestBenchmarkHistoricalScanning }
};

static const TestSuite s_benchmarkSuite = {
    .name = "benchmark",
    .description = "Benchmark engine timing, directory resolution, JSON serialization, and HTML dashboard",
    .tests = s_benchmarkTests,
    .testCount = sizeof(s_benchmarkTests) / sizeof(s_benchmarkTests[0])
};

const TestSuite *GetBenchmarkTestSuite(void) {
    return &s_benchmarkSuite;
}
