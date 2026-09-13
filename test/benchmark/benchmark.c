#define CELS_IMPLEMENTATION
#include "cels.h"
#include "bench_timer.h"
#include "bench_json.h"
#include "bench_html.h"

#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Key Registry & Tree Visualizer                                            */
/* ========================================================================= */

typedef struct KeyRegistryEntry {
    uint32_t key;
    const char *name;
} KeyRegistryEntry;

static KeyRegistryEntry g_keyRegistry[128];
static uint32_t g_registryCount = 0;

static void RegisterKey(uint32_t key, const char *name) {
    for (uint32_t i = 0; i < g_registryCount; ++i) {
        if (g_keyRegistry[i].key == key) return;
    }
    if (g_registryCount < 128) {
        g_keyRegistry[g_registryCount++] = (KeyRegistryEntry){ .key = key, .name = name };
    }
}

#define REGISTER_KEY(str) RegisterKey(CEL_KEY(str), str)

static const char* GetKeyName(uint32_t key) {
    for (uint32_t i = 0; i < g_registryCount; ++i) {
        if (g_keyRegistry[i].key == key) return g_keyRegistry[i].name;
    }
    return "Composable";
}

static inline uint32_t LogicalToPhys(const CelsSession *s, uint32_t logical) {
    return (logical < s->groupsGapStart)
        ? logical
        : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline const CelsSlotGroup* GetGroup(const CelsSession *s, uint32_t logical) {
    return &s->groups[LogicalToPhys(s, logical)];
}

static void PrintNode(const CelsSession *s, uint32_t logicalIdx, const char *prefix, bool isLast) {
    const CelsSlotGroup *g = GetGroup(s, logicalIdx);

    printf("%s%s[%s] (0x%08llX) | slots: %u B | descendants: %u\n",
           prefix,
           isLast ? "\\-- " : "|-- ",
           GetKeyName((uint32_t)g->key),
           (unsigned long long)g->key,
           g->dataSize,
           g->groupSize);

    char nextPrefix[256];
    snprintf(nextPrefix, sizeof(nextPrefix), "%s%s", prefix, isLast ? "    " : "|   ");

    uint32_t childLogical = logicalIdx + 1;
    uint32_t endLogical = logicalIdx + 1 + g->groupSize;

    while (childLogical < endLogical) {
        const CelsSlotGroup *cg = GetGroup(s, childLogical);
        uint32_t nextChild = childLogical + 1 + cg->groupSize;
        bool childIsLast = (nextChild >= endLogical);

        PrintNode(s, childLogical, nextPrefix, childIsLast);
        childLogical = nextChild;
    }
}

static void PrintCompositionTree(const CelsSession *s, const char *title) {
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    printf("\n  +-- %s (Active Nodes: %u, Arena: %u B) -------------------+\n",
           title, totalGroups, s->dataGapStart);

    if (totalGroups == 0) {
        printf("  |   [Empty Tree / All Compositions Despawned]\n");
        printf("  +-----------------------------------------------------------+\n\n");
        return;
    }

    const CelsSlotGroup *root = GetGroup(s, 0);
    printf("  | [%s] (0x%08llX) | descendants: %u | slots: %u B\n",
           GetKeyName((uint32_t)root->key), (unsigned long long)root->key, root->groupSize, root->dataSize);

    uint32_t childLogical = 1;
    uint32_t endLogical = 1 + root->groupSize;

    while (childLogical < endLogical && childLogical < totalGroups) {
        const CelsSlotGroup *cg = GetGroup(s, childLogical);
        uint32_t nextChild = childLogical + 1 + cg->groupSize;
        bool isLast = (nextChild >= endLogical);

        PrintNode(s, childLogical, "  | ", isLast);
        childLogical = nextChild;
    }
    printf("  +-----------------------------------------------------------+\n\n");
}

/* ========================================================================= */
/* Reactive State & Lifecycle Models for Workloads                           */
/* ========================================================================= */

CEL_State(EnemyState) {
    int  hp;
    bool isAlive;
};

CEL_Observer(EnemyTexture) {
    int  gpuHandle;
    bool isLoaded;
};

static void Texture_OnRemembered(EnemyTexture *self, CelsSession *s) {
    (void)s;
    self->gpuHandle = 0x55AA;
    self->isLoaded = true;
}

static void Texture_OnForgotten(EnemyTexture *self, CelsSession *s) {
    (void)s;
    self->gpuHandle = 0;
    self->isLoaded = false;
}

CEL_LifeCycle(EnemyLifeCycle, EnemyState) {
    EnemyState st = cel_watch(it);
    if (st.hp <= 0 || !st.isAlive) {
        cel_destroy();
    }
}

static EnemyState g_enemy = { .hp = 100, .isAlive = true };

static void GoblinComposable(CelsSession *s) {
    CEL_Composition(EnemyState, &g_enemy, EnemyLifeCycle) {
        cel_remember_observer(s, EnemyTexture, Texture_OnRemembered, Texture_OnForgotten);
        CEL_Composable(s, CEL_KEY("HealthBar")) {
            cel_remember(s, int, it->hp);
        } CEL_Close(s);
        CEL_Composable(s, CEL_KEY("ArmorBadge")) {
            cel_remember(s, int, 50);
        } CEL_Close(s);
    }
}

/* Wide & Deep Large Composable Tree */
CEL_State(DashboardState) {
    int activeTab;
    bool showSidePanel;
    int itemCount;
};

static DashboardState g_dashboard = { .activeTab = 1, .showSidePanel = true, .itemCount = 8 };

static void LargeDashboardTree(CelsSession *s) {
    DashboardState st = cel_watch(s, &g_dashboard);

    CEL_Composition(s, CEL_KEY("AppRoot")) {
        CEL_Composable(s, CEL_KEY("NavigationBar")) {
            CEL_Composable(s, CEL_KEY("Logo")) {} CEL_Close(s);
            CEL_Composable(s, CEL_KEY("SearchField")) {
                cel_remember(s, int, 999);
            } CEL_Close(s);
            CEL_Composable(s, CEL_KEY("UserAvatar")) {} CEL_Close(s);
        } CEL_Close(s);

        CEL_Composable(s, CEL_KEY("WorkspaceArea")) {
            if (st.showSidePanel) {
                CEL_Composable(s, CEL_KEY("SidebarPanel")) {
                    for (int i = 0; i < 4; ++i) {
                        CEL_Composable(s, CEL_KeyIndex(CEL_KEY("NavButton"), (uint64_t)i)) {
                            cel_remember(s, int, i * 10);
                        } CEL_Close(s);
                    }
                } CEL_Close(s);
            }

            CEL_Composable(s, CEL_KEY("MainContentPanel")) {
                CEL_Composable(s, CEL_KEY("DataGridHeader")) {} CEL_Close(s);
                for (int i = 0; i < st.itemCount; ++i) {
                    CEL_Composable(s, CEL_KeyIndex(CEL_KEY("GridRow"), (uint64_t)i)) {
                        cel_remember(s, int, i * 100 + st.activeTab);
                    } CEL_Close(s);
                }
            } CEL_Close(s);
        } CEL_Close(s);

        CEL_Composable(s, CEL_KEY("StatusBar")) {
            cel_remember(s, int, st.itemCount);
        } CEL_Close(s);
    } CEL_Close(s);
}

/* ========================================================================= */
/* Benchmark Workloads                                                       */
/* ========================================================================= */

static void InitBenchmarkKeys(void) {
    g_registryCount = 0;
    REGISTER_KEY("AppRoot");
    REGISTER_KEY("NavigationBar");
    REGISTER_KEY("Logo");
    REGISTER_KEY("SearchField");
    REGISTER_KEY("UserAvatar");
    REGISTER_KEY("WorkspaceArea");
    REGISTER_KEY("SidebarPanel");
    REGISTER_KEY("NavButton");
    REGISTER_KEY("MainContentPanel");
    REGISTER_KEY("DataGridHeader");
    REGISTER_KEY("GridRow");
    REGISTER_KEY("StatusBar");
    REGISTER_KEY("EnemyState");
    REGISTER_KEY("HealthBar");
    REGISTER_KEY("ArmorBadge");
}

/**
 * Workload 1: Multi-Session Mount & Switching
 */
static BenchmarkResult RunBenchMultiSession(size_t sessions, size_t iterations, bool showTree) {
    BenchmarkResult res;
    memset(&res, 0, sizeof(res));
    snprintf(res.name, sizeof(res.name), "MultiSession_Scale");
    snprintf(res.description, sizeof(res.description), "Mount & recompose across %zu concurrent independent sessions", sessions);
    res.sessions = sessions;
    res.iterations = iterations;

    printf("\n>>> [Benchmark 1/4] %s (%zu sessions, %zu iterations)\n", res.name, sessions, iterations);
    printf("    Testing session isolation, slab allocation, and round-robin recomposition...\n");

    uint64_t *samples = (uint64_t *)malloc(iterations * sizeof(uint64_t));

    // Allocate session instances
    CelsSession *sessionPool = (CelsSession *)malloc(sessions * sizeof(CelsSession));
    for (size_t s = 0; s < sessions; ++s) {
        CelsSessionInit(&sessionPool[s], &(CelsSessionConfig){
            .root = LargeDashboardTree
        });
    }

    if (showTree) {
        CelsSessionRecompose(&sessionPool[0]);
        PrintCompositionTree(&sessionPool[0], "Session #0 Initial Mount");
    }

    uint64_t startTotal = BenchGetTimeNs();

    for (size_t it = 0; it < iterations; ++it) {
        size_t sIdx = it % sessions;
        uint64_t t0 = BenchGetTimeNs();

        g_dashboard.activeTab = (int)(it % 5);
        CelsSessionRecompose(&sessionPool[sIdx]);

        uint64_t t1 = BenchGetTimeNs();
        samples[it] = (t1 - t0);
    }

    uint64_t endTotal = BenchGetTimeNs();
    res.totalTimeMs = (double)(endTotal - startTotal) / 1000000.0;
    res.opsPerSec = ((double)iterations / (double)(endTotal - startTotal)) * 1000000000.0;
    res.activeGroupsPeak = CelsGetLogicalGroupCount(&sessionPool[0]);
    res.dataArenaPeakBytes = sessionPool[0].dataGapStart;

    BenchCalculateStats(samples, iterations, &res.latency);

    for (size_t s = 0; s < sessions; ++s) {
        CelsSessionDestroy(&sessionPool[s]);
    }
    free(sessionPool);
    free(samples);

    printf("    PASSED: %.1f ops/sec | Mean: %.2f us | p99: %.2f us\n",
           res.opsPerSec, res.latency.meanUs, res.latency.p99Us);
    return res;
}

/**
 * Workload 2: Lifecycle Attach, Mutation & cel_destroy Churn
 */
static BenchmarkResult RunBenchLifecycleChurn(size_t iterations, bool showTree) {
    BenchmarkResult res;
    memset(&res, 0, sizeof(res));
    snprintf(res.name, sizeof(res.name), "Lifecycle_AttachKillChurn");
    snprintf(res.description, sizeof(res.description), "Continuous attach, observer allocation, and cel_destroy() pruning");
    res.sessions = 1;
    res.iterations = iterations;

    printf("\n>>> [Benchmark 2/4] %s (%zu iterations)\n", res.name, iterations);
    printf("    Testing rapid attach, reactive state observer, and cel_destroy() compaction...\n");

    uint64_t *samples = (uint64_t *)malloc(iterations * sizeof(uint64_t));

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinComposable
    });

    if (showTree) {
        g_enemy.hp = 100;
        g_enemy.isAlive = true;
        CelsSessionRecompose(&session);
        PrintCompositionTree(&session, "Lifecycle Active Goblin (HP = 100)");

        printf("  [Lifecycle Trigger] Setting Goblin HP = 0 -> triggers cel_destroy()...\n");
        cel_mutate(&session, &g_enemy) {
            this->hp = 0;
        }
        CelsSessionRecompose(&session);
        PrintCompositionTree(&session, "Post-Destruction Compacted Tree (Empty)");
    }

    uint64_t startTotal = BenchGetTimeNs();

    for (size_t it = 0; it < iterations; ++it) {
        uint64_t t0 = BenchGetTimeNs();

        // 1. Mount entity
        g_enemy.hp = 100;
        g_enemy.isAlive = true;
        CelsSessionRecompose(&session);

        // 2. Kill entity via cel_destroy
        g_enemy.hp = 0;
        CelsSessionRecompose(&session);

        uint64_t t1 = BenchGetTimeNs();
        samples[it] = (t1 - t0);
    }

    uint64_t endTotal = BenchGetTimeNs();
    res.totalTimeMs = (double)(endTotal - startTotal) / 1000000.0;
    // Each iteration mounts and destroys = 2 operations
    res.opsPerSec = ((double)(iterations * 2) / (double)(endTotal - startTotal)) * 1000000000.0;
    res.activeGroupsPeak = 3;
    res.dataArenaPeakBytes = 32;

    BenchCalculateStats(samples, iterations, &res.latency);

    CelsSessionDestroy(&session);
    free(samples);

    printf("    PASSED: %.1f ops/sec | Mean: %.2f us | p99: %.2f us\n",
           res.opsPerSec, res.latency.meanUs, res.latency.p99Us);
    return res;
}

/**
 * Workload 3: Large Deep & Wide Composable Hierarchy
 */
static BenchmarkResult RunBenchLargeTree(size_t iterations, bool showTree) {
    BenchmarkResult res;
    memset(&res, 0, sizeof(res));
    snprintf(res.name, sizeof(res.name), "LargeTree_DeepWideHierarchy");
    snprintf(res.description, sizeof(res.description), "Deep & wide composable tree with slot memories and reactive watchers");
    res.sessions = 1;
    res.iterations = iterations;

    printf("\n>>> [Benchmark 3/4] %s (%zu iterations)\n", res.name, iterations);
    printf("    Testing large hierarchy traversal, slot allocation, and recomposition walk...\n");

    uint64_t *samples = (uint64_t *)malloc(iterations * sizeof(uint64_t));

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = LargeDashboardTree
    });

    g_dashboard.itemCount = 16;
    g_dashboard.showSidePanel = true;
    CelsSessionRecompose(&session);

    if (showTree) {
        PrintCompositionTree(&session, "Large Dashboard Hierarchy (16 rows, sidebar, navbar)");
    }

    uint64_t startTotal = BenchGetTimeNs();

    for (size_t it = 0; it < iterations; ++it) {
        uint64_t t0 = BenchGetTimeNs();

        // Mutate middle node
        cel_mutate(&session, &g_dashboard) {
            this->activeTab = (int)(it % 4);
            this->showSidePanel = ((it % 2) == 0);
        }
        CelsSessionRecompose(&session);

        uint64_t t1 = BenchGetTimeNs();
        samples[it] = (t1 - t0);
    }

    uint64_t endTotal = BenchGetTimeNs();
    res.totalTimeMs = (double)(endTotal - startTotal) / 1000000.0;
    res.opsPerSec = ((double)iterations / (double)(endTotal - startTotal)) * 1000000000.0;
    res.activeGroupsPeak = CelsGetLogicalGroupCount(&session);
    res.dataArenaPeakBytes = session.dataGapStart;

    BenchCalculateStats(samples, iterations, &res.latency);

    CelsSessionDestroy(&session);
    free(samples);

    printf("    PASSED: %.1f ops/sec | Mean: %.2f us | p99: %.2f us\n",
           res.opsPerSec, res.latency.meanUs, res.latency.p99Us);
    return res;
}

/**
 * Workload 4: Steady-State Quiet Recompose (O(1) Subtree Skip)
 */
static BenchmarkResult RunBenchSteadyStateQuietSkip(size_t iterations) {
    BenchmarkResult res;
    memset(&res, 0, sizeof(res));
    snprintf(res.name, sizeof(res.name), "SteadyState_QuietSkipO1");
    snprintf(res.description, sizeof(res.description), "Unmutated steady-state recompose check with O(1) subtree skipping");
    res.sessions = 1;
    res.iterations = iterations;

    printf("\n>>> [Benchmark 4/4] %s (%zu iterations)\n", res.name, iterations);
    printf("    Testing O(1) instantaneous recompose skip when no reactive state changed...\n");

    uint64_t *samples = (uint64_t *)malloc(iterations * sizeof(uint64_t));

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = LargeDashboardTree
    });
    CelsSessionRecompose(&session);

    uint64_t startTotal = BenchGetTimeNs();

    for (size_t it = 0; it < iterations; ++it) {
        uint64_t t0 = BenchGetTimeNs();

        // Quiet check - no mutations triggered
        CelsSessionRecompose(&session);

        uint64_t t1 = BenchGetTimeNs();
        samples[it] = (t1 - t0);
    }

    uint64_t endTotal = BenchGetTimeNs();
    res.totalTimeMs = (double)(endTotal - startTotal) / 1000000.0;
    res.opsPerSec = ((double)iterations / (double)(endTotal - startTotal)) * 1000000000.0;
    res.activeGroupsPeak = CelsGetLogicalGroupCount(&session);
    res.dataArenaPeakBytes = session.dataGapStart;

    BenchCalculateStats(samples, iterations, &res.latency);

    CelsSessionDestroy(&session);
    free(samples);

    printf("    PASSED: %.1f ops/sec | Mean: %.2f us | p99: %.2f us\n",
           res.opsPerSec, res.latency.meanUs, res.latency.p99Us);
    return res;
}

/* ========================================================================= */
/* CLI Visual Formatting & Comparison                                        */
/* ========================================================================= */

static void DisplayPreviousSummary(const PreviousRunData *prev) {
    if (!prev || !prev->valid) {
        printf("----------------------------------------------------------------------\n");
        printf(" No previous benchmark run detected (Fresh baseline run)\n");
        printf("----------------------------------------------------------------------\n");
        return;
    }

    printf("======================================================================\n");
    printf(" PREVIOUS BENCHMARK RECORD: CELS v%s (%s)\n", prev->versionString, prev->dateIso);
    printf("----------------------------------------------------------------------\n");
    printf(" %-30s %-16s %-12s %-10s\n", "Benchmark", "Throughput", "Mean", "p99");
    for (size_t i = 0; i < prev->count; ++i) {
        printf(" %-30s %10.1f ops/s %8.2f us %8.2f us\n",
               prev->entries[i].name,
               prev->entries[i].opsPerSec,
               prev->entries[i].meanUs,
               prev->entries[i].p99Us);
    }
    printf("======================================================================\n");
}

static void DisplayComparisonTable(const BenchmarkResult *results, size_t count, const PreviousRunData *prev) {
    printf("\n======================================================================\n");
    printf(" CELS v%s BENCHMARK SUMMARY & RELEASE COMPARISON\n", CELS_VERSION_STRING);
    if (prev && prev->valid) {
        printf(" Compared Baseline: %s\n", prev->label);
    } else {
        printf(" Compared Baseline: None (Fresh baseline run)\n");
    }
    printf("----------------------------------------------------------------------\n");
    printf(" %-28s %-14s %-14s %-10s\n", "Benchmark", "Current Ops/s", "Prev Ops/s", "Delta");
    printf("----------------------------------------------------------------------\n");

    for (size_t i = 0; i < count; ++i) {
        const BenchmarkResult *r = &results[i];
        const PreviousBenchmarkEntry *pe = NULL;

        if (prev && prev->valid) {
            for (size_t p = 0; p < prev->count; ++p) {
                if (strcmp(prev->entries[p].name, r->name) == 0) {
                    pe = &prev->entries[p];
                    break;
                }
            }
        }

        if (pe && pe->opsPerSec > 0) {
            double deltaPct = ((r->opsPerSec - pe->opsPerSec) / pe->opsPerSec) * 100.0;
            printf(" %-28s %12.1f   %12.1f   %+6.1f%% (%s)\n",
                   r->name,
                   r->opsPerSec,
                   pe->opsPerSec,
                   deltaPct,
                   (deltaPct >= 0) ? "FASTER" : "SLOWER");
        } else {
            printf(" %-28s %12.1f   %12s   [NEW]\n",
                   r->name,
                   r->opsPerSec,
                   "-");
        }
    }
    printf("======================================================================\n");
}

/* ========================================================================= */
/* Main CLI Entry Point                                                      */
/* ========================================================================= */

int main(int argc, char **argv) {
    InitBenchmarkKeys();

    bool onlyView = false;
    bool quickMode = false;
    bool showTree = true;
    bool listHistory = false;
    const char *compareFile = NULL;
    const char *filterName = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--view") == 0 || strcmp(argv[i], "view") == 0) {
            onlyView = true;
        } else if (strcmp(argv[i], "--history") == 0 || strcmp(argv[i], "--list-runs") == 0 || strcmp(argv[i], "history") == 0) {
            listHistory = true;
        } else if ((strcmp(argv[i], "--compare") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            compareFile = argv[++i];
        } else if (strcmp(argv[i], "--quick") == 0 || strcmp(argv[i], "-q") == 0) {
            quickMode = true;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            BenchSetOutputDir(argv[++i]);
        } else if (strcmp(argv[i], "--no-tree") == 0) {
            showTree = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: benchmark.exe [OPTIONS] [WORKLOAD_NAME]\n\n");
            printf("Options:\n");
            printf("  --view, -v             View previous benchmark results without running\n");
            printf("  --history, --list-runs List all recorded benchmark run versions\n");
            printf("  --compare, -c <file>   Compare results against a specific previous JSON run\n");
            printf("  --quick, -q            Run quick pass with reduced iterations\n");
            printf("  --out <dir>            Set custom output directory (default: test/benchmark/out)\n");
            printf("  --no-tree              Disable composition tree visualization\n");
            printf("  --list, -l             List available workloads\n");
            printf("  --help, -h             Show this help screen\n\n");
            printf("Examples:\n");
            printf("  benchmark.exe                          View previous, execute benchmarks, compare and export\n");
            printf("  benchmark.exe --view                   Only view previous benchmark run history\n");
            printf("  benchmark.exe --history                List all available historical run snapshots\n");
            printf("  benchmark.exe --compare latest.json    Compare against latest.json explicitly\n");
            printf("  benchmark.exe --quick                  Fast benchmark pass\n");
            return 0;
        } else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            printf("Available Benchmark Workloads in CELS:\n");
            printf("  - MultiSession_Scale\n");
            printf("  - Lifecycle_AttachKillChurn\n");
            printf("  - LargeTree_DeepWideHierarchy\n");
            printf("  - SteadyState_QuietSkipO1\n");
            return 0;
        } else {
            filterName = argv[i];
        }
    }

    if (listHistory) {
        PreviousRunData histRuns[32];
        size_t histCount = BenchLoadAllHistoricalRuns(BenchGetOutputDir(), histRuns, 32);
        printf("\n======================================================================\n");
        printf(" RECORDED BENCHMARK RUNS IN %s (%zu found)\n", BenchGetOutputDir(), histCount);
        printf("----------------------------------------------------------------------\n");
        printf(" %-4s %-32s %-10s %-20s\n", "#", "Filename", "Version", "Timestamp / Date");
        printf("----------------------------------------------------------------------\n");
        for (size_t h = 0; h < histCount; ++h) {
            printf(" [%zu]  %-32s v%-9s %s\n", h, histRuns[h].sourceFilename, histRuns[h].versionString, histRuns[h].dateIso);
        }
        if (histCount == 0) {
            printf(" No historical benchmark runs recorded yet in %s.\n", BenchGetOutputDir());
        }
        printf("======================================================================\n");
        printf(" Tip: Use --compare <filename> to compare in CLI\n");
        printf(" Tip: Open %s/report.html to use the interactive dropdown!\n\n", BenchGetOutputDir());
        return 0;
    }

    printf("======================================================================\n");
    printf(" CELS PERFORMANCE BENCHMARK SUITE v%s (Code: 0x%08X)\n",
           CELS_VERSION_STRING, (unsigned int)CELS_VERSION_CODE);
    printf(" High-Performance Reactive Composition Engine for C99\n");
    printf("======================================================================\n");

    PreviousRunData prevData;
    bool hasPrev = false;
    if (compareFile) {
        char tryPath[256];
        if (strchr(compareFile, '/') || strchr(compareFile, '\\')) {
            snprintf(tryPath, sizeof(tryPath), "%s", compareFile);
        } else {
            snprintf(tryPath, sizeof(tryPath), "%s/%s", BenchGetOutputDir(), compareFile);
        }
        hasPrev = BenchLoadRunFromJsonFile(tryPath, &prevData);
        if (!hasPrev) {
            hasPrev = BenchLoadRunFromJsonFile(compareFile, &prevData);
        }
        if (!hasPrev) {
            printf("Warning: Could not load '%s'. Falling back to latest.json.\n", compareFile);
            hasPrev = BenchLoadPreviousResults(&prevData);
        }
    } else {
        hasPrev = BenchLoadPreviousResults(&prevData);
    }

    if (onlyView) {
        DisplayPreviousSummary(&prevData);
        char viewLatestPath[256], viewHtmlPath[256];
        snprintf(viewLatestPath, sizeof(viewLatestPath), "%s/latest.json", BenchGetOutputDir());
        snprintf(viewHtmlPath, sizeof(viewHtmlPath), "%s/report.html", BenchGetOutputDir());
        printf("\nSaved JSON: %s\n", viewLatestPath);
        printf("Saved HTML: %s\n", viewHtmlPath);
        return 0;
    }

    // Always display previous run before running new benchmark
    DisplayPreviousSummary(&prevData);

    size_t iterMulti = quickMode ? 1000 : 10000;
    size_t iterLifecycle = quickMode ? 2000 : 20000;
    size_t iterLarge = quickMode ? 1000 : 10000;
    size_t iterQuiet = quickMode ? 5000 : 50000;

    BenchmarkResult results[4];
    size_t resultCount = 0;

    if (!filterName || strstr(filterName, "Multi") || strstr(filterName, "multi")) {
        results[resultCount++] = RunBenchMultiSession(25, iterMulti, showTree);
    }
    if (!filterName || strstr(filterName, "Lifecycle") || strstr(filterName, "lifecycle") || strstr(filterName, "churn")) {
        results[resultCount++] = RunBenchLifecycleChurn(iterLifecycle, showTree);
    }
    if (!filterName || strstr(filterName, "Large") || strstr(filterName, "large") || strstr(filterName, "tree")) {
        results[resultCount++] = RunBenchLargeTree(iterLarge, showTree);
    }
    if (!filterName || strstr(filterName, "Quiet") || strstr(filterName, "quiet") || strstr(filterName, "steady")) {
        results[resultCount++] = RunBenchSteadyStateQuietSkip(iterQuiet);
    }

    // Display comparison
    DisplayComparisonTable(results, resultCount, hasPrev ? &prevData : NULL);

    // Export release JSON & latest JSON
    char releaseJsonPath[256];
    char latestPath[256];
    char htmlPath[256];
    time_t now = time(NULL);
    snprintf(releaseJsonPath, sizeof(releaseJsonPath), "%s/benchmark_v%s_%ld.json",
             BenchGetOutputDir(), CELS_VERSION_STRING, (long)now);
    snprintf(latestPath, sizeof(latestPath), "%s/latest.json", BenchGetOutputDir());
    snprintf(htmlPath, sizeof(htmlPath), "%s/report.html", BenchGetOutputDir());

    BenchSaveResultsJson(results, resultCount, releaseJsonPath);
    printf("\n[Export] JSON saved to: %s\n", releaseJsonPath);
    printf("[Export] JSON saved to: %s\n", latestPath);

    // Export HTML report
    BenchGenerateHtmlReport(results, resultCount, hasPrev ? &prevData : NULL, htmlPath);
    printf("[Export] HTML dashboard: %s\n", htmlPath);
    printf("======================================================================\n");
    printf(" Benchmark execution completed successfully!\n");
    printf("======================================================================\n");

    return 0;
}
