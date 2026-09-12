#pragma once

/**
 * @file bench_json.h
 * @brief JSON export and previous-run comparison for CELS benchmarks.
 */

#include "bench_timer.h"
#include "cels/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

typedef struct PreviousBenchmarkEntry {
    char name[64];
    char description[128];
    double opsPerSec;
    double meanUs;
    double p99Us;
} PreviousBenchmarkEntry;

typedef struct PreviousRunData {
    bool valid;
    char id[64];
    char label[128];
    char sourceFilename[64];
    char versionString[32];
    uint32_t versionCode;
    long timestamp;
    char dateIso[32];
    char compiler[64];
    char os[32];
    size_t count;
    PreviousBenchmarkEntry entries[32];
} PreviousRunData;

static const char *g_benchOutputDir = NULL;

static inline void BenchSetOutputDir(const char *dir) {
    g_benchOutputDir = dir;
}

static inline bool BenchDirExists(const char *path) {
#if defined(_WIN32)
    DWORD dwAttrib = GetFileAttributesA(path);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}

static inline const char *BenchGetOutputDir(void) {
    if (g_benchOutputDir && g_benchOutputDir[0] != '\0') {
        return g_benchOutputDir;
    }
    if (BenchDirExists("test/benchmark")) {
        return "test/benchmark/out";
    }
    if (BenchDirExists("benchmark")) {
        return "benchmark/out";
    }
    if (BenchDirExists("../test/benchmark")) {
        return "../test/benchmark/out";
    }
    if (BenchDirExists("../../test/benchmark")) {
        return "../../test/benchmark/out";
    }
    if (BenchDirExists("../../../test/benchmark")) {
        return "../../../test/benchmark/out";
    }
    return "test/benchmark/out";
}

static inline void BenchEnsureDir(const char *path) {
    if (!path || path[0] == '\0') return;
    char temp[256];
    snprintf(temp, sizeof(temp), "%s", path);
    size_t len = strlen(temp);
    for (size_t i = 1; i < len; ++i) {
        if (temp[i] == '/' || temp[i] == '\\') {
            char prev = temp[i];
            temp[i] = '\0';
            MKDIR(temp);
            temp[i] = prev;
        }
    }
    MKDIR(temp);
}

static inline void BenchEnsureResultsDir(void) {
    BenchEnsureDir(BenchGetOutputDir());
}

static inline void BenchGetIsoDate(char *outBuf, size_t bufSize) {
    time_t now = time(NULL);
    struct tm *tmInfo = gmtime(&now);
    if (tmInfo != NULL) {
        strftime(outBuf, bufSize, "%Y-%m-%dT%H:%M:%SZ", tmInfo);
    } else {
        snprintf(outBuf, bufSize, "unknown");
    }
}

/**
 * Loads and parses a single benchmark run from a JSON file.
 */
static inline bool BenchLoadRunFromJsonFile(const char *filePath, PreviousRunData *outData) {
    if (filePath == NULL || outData == NULL) return false;
    memset(outData, 0, sizeof(PreviousRunData));

    FILE *f = fopen(filePath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 500000) {
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t readBytes = fread(buf, 1, (size_t)size, f);
    buf[readBytes] = '\0';
    fclose(f);

    const char *slash = strrchr(filePath, '/');
    const char *bslash = strrchr(filePath, '\\');
    const char *base = slash ? slash + 1 : (bslash ? bslash + 1 : filePath);
    snprintf(outData->sourceFilename, sizeof(outData->sourceFilename), "%s", base);
    snprintf(outData->id, sizeof(outData->id), "%s", base);

    // Extract version string
    char *vStr = strstr(buf, "\"string\": \"");
    if (vStr) {
        vStr += 11;
        char *end = strchr(vStr, '"');
        if (end) {
            size_t len = (size_t)(end - vStr);
            if (len >= sizeof(outData->versionString)) len = sizeof(outData->versionString) - 1;
            memcpy(outData->versionString, vStr, len);
            outData->versionString[len] = '\0';
        }
    }

    // Extract version code
    char *codeStr = strstr(buf, "\"code\":");
    if (codeStr) {
        codeStr += 7;
        outData->versionCode = (uint32_t)strtoul(codeStr, NULL, 10);
    }

    // Extract timestamp
    char *tsStr = strstr(buf, "\"timestamp\":");
    if (tsStr) {
        tsStr += 12;
        outData->timestamp = strtol(tsStr, NULL, 10);
    }

    // Extract date
    char *dStr = strstr(buf, "\"iso_date\": \"");
    if (dStr) {
        dStr += 13;
        char *end = strchr(dStr, '"');
        if (end) {
            size_t len = (size_t)(end - dStr);
            if (len >= sizeof(outData->dateIso)) len = sizeof(outData->dateIso) - 1;
            memcpy(outData->dateIso, dStr, len);
            outData->dateIso[len] = '\0';
        }
    }

    // Extract compiler
    char *compStr = strstr(buf, "\"compiler\": \"");
    if (compStr) {
        compStr += 13;
        char *end = strchr(compStr, '"');
        if (end) {
            size_t len = (size_t)(end - compStr);
            if (len >= sizeof(outData->compiler)) len = sizeof(outData->compiler) - 1;
            memcpy(outData->compiler, compStr, len);
            outData->compiler[len] = '\0';
        }
    }

    // Parse benchmark entries
    char *cursor = buf;
    while ((cursor = strstr(cursor, "\"name\": \"")) != NULL) {
        if (outData->count >= 32) break;
        cursor += 9;
        char *endName = strchr(cursor, '"');
        if (!endName) break;

        size_t nlen = (size_t)(endName - cursor);
        if (nlen >= sizeof(outData->entries[0].name)) nlen = sizeof(outData->entries[0].name) - 1;
        memcpy(outData->entries[outData->count].name, cursor, nlen);
        outData->entries[outData->count].name[nlen] = '\0';

        char *nextName = strstr(endName, "\"name\": \"");

        // ops_per_sec
        char *opsStr = strstr(endName, "\"ops_per_sec\":");
        if (opsStr && (!nextName || opsStr < nextName)) {
            opsStr += 14;
            outData->entries[outData->count].opsPerSec = strtod(opsStr, NULL);
        }

        // mean
        char *meanStr = strstr(endName, "\"mean\":");
        if (meanStr && (!nextName || meanStr < nextName)) {
            meanStr += 7;
            outData->entries[outData->count].meanUs = strtod(meanStr, NULL);
        }

        // p99
        char *p99Str = strstr(endName, "\"p99\":");
        if (p99Str && (!nextName || p99Str < nextName)) {
            p99Str += 6;
            outData->entries[outData->count].p99Us = strtod(p99Str, NULL);
        }

        outData->count++;
        cursor = endName;
    }

    free(buf);
    outData->valid = (outData->count > 0);

    if (strcmp(outData->sourceFilename, "latest.json") == 0) {
        snprintf(outData->label, sizeof(outData->label), "Baseline: latest.json (v%s)",
                 outData->versionString);
    } else {
        snprintf(outData->label, sizeof(outData->label), "v%s (%s)",
                 outData->versionString, outData->dateIso);
    }

    return outData->valid;
}

/**
 * Scans a directory for historical benchmark JSON files.
 */
static inline size_t BenchScanHistoricalFiles(const char *dir, char filePaths[][512], size_t maxFiles) {
    size_t count = 0;
#if defined(_WIN32)
    char searchPattern[512];
    snprintf(searchPattern, sizeof(searchPattern), "%s/*.json", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if ((strstr(fd.cFileName, "benchmark_v") || strcmp(fd.cFileName, "latest.json") == 0) &&
                    !strstr(fd.cFileName, "test_output")) {
                    if (count < maxFiles) {
                        snprintf(filePaths[count], 512, "%s/%s", dir, fd.cFileName);
                        count++;
                    }
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strstr(de->d_name, ".json") &&
                (strstr(de->d_name, "benchmark_v") || strcmp(de->d_name, "latest.json") == 0) &&
                !strstr(de->d_name, "test_output")) {
                if (count < maxFiles) {
                    snprintf(filePaths[count], 512, "%s/%s", dir, de->d_name);
                    count++;
                }
            }
        }
        closedir(d);
    }
#endif
    return count;
}

static int CompareRunsByDateDesc(const void *a, const void *b) {
    const PreviousRunData *ra = (const PreviousRunData *)a;
    const PreviousRunData *rb = (const PreviousRunData *)b;
    if (strcmp(ra->sourceFilename, "latest.json") == 0) return -1;
    if (strcmp(rb->sourceFilename, "latest.json") == 0) return 1;
    if (ra->timestamp > rb->timestamp) return -1;
    if (ra->timestamp < rb->timestamp) return 1;
    return 0;
}

/**
 * Loads all historical benchmark runs present in the directory.
 */
static inline size_t BenchLoadAllHistoricalRuns(const char *dir, PreviousRunData *outRuns, size_t maxRuns) {
    if (!dir || !outRuns || maxRuns == 0) return 0;
    char filePaths[64][512];
    size_t numFiles = BenchScanHistoricalFiles(dir, filePaths, 64);
    size_t loaded = 0;

    for (size_t i = 0; i < numFiles && loaded < maxRuns; ++i) {
        if (BenchLoadRunFromJsonFile(filePaths[i], &outRuns[loaded])) {
            loaded++;
        }
    }

    if (loaded > 1) {
        qsort(outRuns, loaded, sizeof(PreviousRunData), CompareRunsByDateDesc);
    }
    return loaded;
}

/**
 * Reads previous benchmark results from latest.json if present.
 */
static inline bool BenchLoadPreviousResults(PreviousRunData *outData) {
    if (outData == NULL) return false;
    memset(outData, 0, sizeof(PreviousRunData));

    char primaryPath[256];
    snprintf(primaryPath, sizeof(primaryPath), "%s/latest.json", BenchGetOutputDir());

    const char *candidatePaths[] = {
        primaryPath,
        "test/benchmark/out/latest.json",
        "benchmark/out/latest.json",
        "../../../test/benchmark/out/latest.json",
        "../../test/benchmark/out/latest.json",
        "../test/benchmark/out/latest.json"
    };
    size_t candidateCount = sizeof(candidatePaths) / sizeof(candidatePaths[0]);

    for (size_t i = 0; i < candidateCount; ++i) {
        if (BenchLoadRunFromJsonFile(candidatePaths[i], outData)) {
            return true;
        }
    }
    return false;
}

/**
 * Saves benchmark results to JSON format.
 */
static inline bool BenchSaveResultsJson(const BenchmarkResult *results, size_t count, const char *extraFilename) {
    BenchEnsureResultsDir();

    char isoDate[32];
    BenchGetIsoDate(isoDate, sizeof(isoDate));
    time_t now = time(NULL);

    char latestPath[256];
    snprintf(latestPath, sizeof(latestPath), "%s/latest.json", BenchGetOutputDir());
    const char *filenames[2] = { latestPath, extraFilename };
    size_t fileLimit = extraFilename ? 2 : 1;

    for (size_t fIdx = 0; fIdx < fileLimit; ++fIdx) {
        if (!filenames[fIdx]) continue;
        FILE *f = fopen(filenames[fIdx], "w");
        if (!f) continue;

        fprintf(f, "{\n");
        fprintf(f, "  \"cels_version\": {\n");
        fprintf(f, "    \"string\": \"%s\",\n", CELS_VERSION_STRING);
        fprintf(f, "    \"code\": %u,\n", (unsigned int)CELS_VERSION_CODE);
        fprintf(f, "    \"major\": %d,\n", CELS_VERSION_MAJOR);
        fprintf(f, "    \"minor\": %d,\n", CELS_VERSION_MINOR);
        fprintf(f, "    \"patch\": %d,\n", CELS_VERSION_PATCH);
        fprintf(f, "    \"build\": %d\n", CELS_VERSION_BUILD);
        fprintf(f, "  },\n");

        fprintf(f, "  \"metadata\": {\n");
        fprintf(f, "    \"timestamp\": %ld,\n", (long)now);
        fprintf(f, "    \"iso_date\": \"%s\",\n", isoDate);
#if defined(_WIN32)
        fprintf(f, "    \"os\": \"Windows\",\n");
#elif defined(__APPLE__)
        fprintf(f, "    \"os\": \"macOS\",\n");
#else
        fprintf(f, "    \"os\": \"Linux\",\n");
#endif
#if defined(__GNUC__)
        fprintf(f, "    \"compiler\": \"GCC %d.%d.%d\",\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
        fprintf(f, "    \"compiler\": \"MSVC %d\",\n", _MSC_VER);
#elif defined(__clang__)
        fprintf(f, "    \"compiler\": \"Clang %s\",\n", __clang_version__);
#else
        fprintf(f, "    \"compiler\": \"Unknown\",\n");
#endif
        fprintf(f, "    \"arch\": \"x86_64\"\n");
        fprintf(f, "  },\n");

        fprintf(f, "  \"benchmarks\": [\n");
        for (size_t i = 0; i < count; ++i) {
            const BenchmarkResult *r = &results[i];
            fprintf(f, "    {\n");
            fprintf(f, "      \"name\": \"%s\",\n", r->name);
            fprintf(f, "      \"description\": \"%s\",\n", r->description);
            fprintf(f, "      \"sessions\": %zu,\n", r->sessions);
            fprintf(f, "      \"iterations\": %zu,\n", r->iterations);
            fprintf(f, "      \"total_time_ms\": %.3f,\n", r->totalTimeMs);
            fprintf(f, "      \"ops_per_sec\": %.1f,\n", r->opsPerSec);
            fprintf(f, "      \"latency_us\": {\n");
            fprintf(f, "        \"min\": %.2f,\n", r->latency.minUs);
            fprintf(f, "        \"mean\": %.2f,\n", r->latency.meanUs);
            fprintf(f, "        \"p50\": %.2f,\n", r->latency.p50Us);
            fprintf(f, "        \"p90\": %.2f,\n", r->latency.p90Us);
            fprintf(f, "        \"p95\": %.2f,\n", r->latency.p95Us);
            fprintf(f, "        \"p99\": %.2f,\n", r->latency.p99Us);
            fprintf(f, "        \"max\": %.2f\n", r->latency.maxUs);
            fprintf(f, "      },\n");
            fprintf(f, "      \"memory\": {\n");
            fprintf(f, "        \"active_groups_peak\": %u,\n", r->activeGroupsPeak);
            fprintf(f, "        \"data_arena_peak_bytes\": %u\n", r->dataArenaPeakBytes);
            fprintf(f, "      }\n");
            fprintf(f, "    }%s\n", (i + 1 < count) ? "," : "");
        }
        fprintf(f, "  ]\n");
        fprintf(f, "}\n");
        fclose(f);
    }
    return true;
}
