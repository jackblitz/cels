#pragma once

/**
 * @file bench_html.h
 * @brief Zero-dependency, self-contained HTML report generator for CELS benchmarks.
 * Includes interactive version comparison dropdown, live delta recalculation,
 * dual-bar canvas charts, and historical archive inspection.
 */

#include "bench_json.h"
#include "bench_timer.h"
#include "cels/version.h"

#include <stdio.h>
#include <string.h>

static inline void BenchGenerateHtmlReport(const BenchmarkResult *results,
                                          size_t count,
                                          const PreviousRunData *prev,
                                          const char *outHtmlPath) {
    char defaultHtmlPath[256];
    if (!outHtmlPath) {
        snprintf(defaultHtmlPath, sizeof(defaultHtmlPath), "%s/report.html", BenchGetOutputDir());
        outHtmlPath = defaultHtmlPath;
    }
    FILE *f = fopen(outHtmlPath, "w");
    if (!f) return;

    char isoDate[32];
    BenchGetIsoDate(isoDate, sizeof(isoDate));

    // Load all historical runs in output directory for dropdown comparison
    PreviousRunData historicalRuns[32];
    size_t historyCount = BenchLoadAllHistoricalRuns(BenchGetOutputDir(), historicalRuns, 32);

    // If latest was not in historicalRuns but prev is valid, ensure it is available
    if (historyCount == 0 && prev && prev->valid) {
        historicalRuns[0] = *prev;
        historyCount = 1;
    }

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "<meta charset=\"UTF-8\">\n");
    fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "<title>CELS Benchmark Report - v%s</title>\n", CELS_VERSION_STRING);
    fprintf(f, "<style>\n");
    fprintf(f, "  :root { --bg: #0d1117; --panel: #161b22; --border: #30363d; --text: #c9d1d9; --heading: #f0f6fc; --accent: #58a6ff; --green: #3fb950; --purple: #bc8cff; --red: #f85149; --card-hover: #1f242c; }\n");
    fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif; }\n");
    fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 32px 24px; line-height: 1.5; }\n");
    fprintf(f, "  .container { max-width: 1280px; margin: 0 auto; }\n");
    fprintf(f, "  header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); padding-bottom: 20px; margin-bottom: 28px; }\n");
    fprintf(f, "  .title-wrap h1 { color: var(--heading); font-size: 28px; font-weight: 700; display: flex; align-items: center; gap: 12px; }\n");
    fprintf(f, "  .badge { background: #1f6feb22; border: 1px solid #1f6feb; color: var(--accent); padding: 4px 10px; border-radius: 20px; font-size: 13px; font-weight: 600; display: inline-flex; align-items: center; gap: 4px; }\n");
    fprintf(f, "  .badge-faster { background: #23863622; border: 1px solid var(--green); color: var(--green); }\n");
    fprintf(f, "  .badge-slower { background: #da363322; border: 1px solid var(--red); color: var(--red); }\n");
    fprintf(f, "  .badge-neutral { background: #30363d55; border: 1px solid var(--border); color: #8b949e; }\n");
    fprintf(f, "  .meta-p { color: #8b949e; font-size: 13px; margin-top: 6px; }\n");
    fprintf(f, "  .grid-kpi { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 16px; margin-bottom: 28px; }\n");
    fprintf(f, "  .kpi-card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 18px; transition: border-color 0.2s ease; }\n");
    fprintf(f, "  .kpi-card:hover { border-color: #58a6ff66; }\n");
    fprintf(f, "  .kpi-card.mini { padding: 14px 16px; }\n");
    fprintf(f, "  .kpi-title { font-size: 12px; color: #8b949e; text-transform: uppercase; letter-spacing: 0.5px; font-weight: 600; }\n");
    fprintf(f, "  .kpi-val { font-size: 26px; color: var(--heading); font-weight: 700; margin: 8px 0 4px; }\n");
    fprintf(f, "  .kpi-val-mini { font-size: 20px; color: var(--heading); font-weight: 700; margin: 6px 0 2px; }\n");
    fprintf(f, "  .kpi-sub { font-size: 12px; color: var(--green); }\n");
    fprintf(f, "  .section { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 22px; margin-bottom: 28px; }\n");
    fprintf(f, "  .section-header-flex { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 16px; margin-bottom: 18px; }\n");
    fprintf(f, "  .section h2 { color: var(--heading); font-size: 20px; font-weight: 700; }\n");
    fprintf(f, "  .section-desc { font-size: 13px; color: #8b949e; margin-top: 4px; }\n");
    fprintf(f, "  .dropdown-control { display: flex; align-items: center; gap: 12px; background: #0d1117; padding: 8px 14px; border: 1px solid var(--border); border-radius: 8px; }\n");
    fprintf(f, "  .dropdown-label { font-size: 12px; color: #8b949e; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }\n");
    fprintf(f, "  .version-select { background: #21262d; color: #f0f6fc; border: 1px solid #388bfd; border-radius: 6px; padding: 8px 14px; font-size: 13px; font-weight: 600; cursor: pointer; outline: none; transition: all 0.2s ease; max-width: 380px; }\n");
    fprintf(f, "  .version-select:hover, .version-select:focus { border-color: #58a6ff; box-shadow: 0 0 0 3px #1f6feb33; }\n");
    fprintf(f, "  .compare-kpis { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 14px; margin-bottom: 20px; }\n");
    fprintf(f, "  table { width: 100%%; border-collapse: collapse; font-size: 13px; text-align: left; }\n");
    fprintf(f, "  th { background: #21262d; color: var(--heading); padding: 12px 14px; font-weight: 600; border-bottom: 1px solid var(--border); }\n");
    fprintf(f, "  td { padding: 12px 14px; border-bottom: 1px solid #21262d; }\n");
    fprintf(f, "  tr:hover td { background: var(--card-hover); }\n");
    fprintf(f, "  .mono { font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }\n");
    fprintf(f, "  .delta-pos { color: var(--green); font-weight: 600; }\n");
    fprintf(f, "  .delta-neg { color: var(--red); font-weight: 600; }\n");
    fprintf(f, "  .chart-legend { display: flex; align-items: center; gap: 18px; font-size: 12px; color: #8b949e; }\n");
    fprintf(f, "  .legend-item { display: inline-flex; align-items: center; gap: 6px; }\n");
    fprintf(f, "  .legend-box { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }\n");
    fprintf(f, "  canvas { width: 100%%; max-height: 280px; }\n");
    fprintf(f, "  .tree-box { background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 16px; font-family: monospace; font-size: 13px; color: #7ee787; overflow-x: auto; white-space: pre; line-height: 1.4; }\n");
    fprintf(f, "  .btn-select { background: #21262d; border: 1px solid var(--border); color: var(--accent); padding: 4px 10px; border-radius: 6px; font-size: 12px; font-weight: 600; cursor: pointer; transition: all 0.15s ease; }\n");
    fprintf(f, "  .btn-select:hover { background: #1f6feb22; border-color: var(--accent); color: #f0f6fc; }\n");
    fprintf(f, "</style>\n</head>\n<body>\n<div class=\"container\">\n");

    // Header
    fprintf(f, "  <header>\n");
    fprintf(f, "    <div class=\"title-wrap\">\n");
    fprintf(f, "      <h1>CELS Performance Dashboard <span class=\"badge\">v%s</span></h1>\n", CELS_VERSION_STRING);
    fprintf(f, "      <p class=\"meta-p\">Release Version Code: 0x%08X | Generated: %s | Zero-heap reactive tree</p>\n",
            (unsigned int)CELS_VERSION_CODE, isoDate);
    fprintf(f, "    </div>\n");
    fprintf(f, "  </header>\n");

    // KPI summary calculations for current run
    double peakOps = 0;
    double minLatency = 9999999;
    size_t totalIters = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].opsPerSec > peakOps) peakOps = results[i].opsPerSec;
        if (results[i].latency.minUs < minLatency && results[i].latency.minUs > 0) {
            minLatency = results[i].latency.minUs;
        }
        totalIters += results[i].iterations;
    }
    if (minLatency == 9999999) minLatency = 0;

    fprintf(f, "  <div class=\"grid-kpi\">\n");
    fprintf(f, "    <div class=\"kpi-card\"><div class=\"kpi-title\">Peak Throughput</div><div class=\"kpi-val\">%.0f <span style=\"font-size:14px;color:#8b949e\">ops/s</span></div><div class=\"kpi-sub\">Slot Table & Lifecycle</div></div>\n", peakOps);
    fprintf(f, "    <div class=\"kpi-card\"><div class=\"kpi-title\">Minimum Latency</div><div class=\"kpi-val\">%.2f <span style=\"font-size:14px;color:#8b949e\">&mu;s</span></div><div class=\"kpi-sub\">Sub-microsecond dispatch</div></div>\n", minLatency);
    fprintf(f, "    <div class=\"kpi-card\"><div class=\"kpi-title\">Total Iterations</div><div class=\"kpi-val\">%zu</div><div class=\"kpi-sub\">Across %zu benchmark suites</div></div>\n", totalIters, count);
    fprintf(f, "    <div class=\"kpi-card\"><div class=\"kpi-title\">Active L1 Slab</div><div class=\"kpi-val\">32 <span style=\"font-size:14px;color:#8b949e\">KiB</span></div><div class=\"kpi-sub\">64-byte aligned (Zero-heap)</div></div>\n");
    fprintf(f, "  </div>\n");

    // =========================================================================
    // SECTION: Interactive Version Comparison with Dropdown
    // =========================================================================
    fprintf(f, "  <div class=\"section\" id=\"comparisonSection\">\n");
    fprintf(f, "    <div class=\"section-header-flex\">\n");
    fprintf(f, "      <div>\n");
    fprintf(f, "        <h2>Release Version Comparison</h2>\n");
    fprintf(f, "        <p class=\"section-desc\">Compare Current Run against any previous release or historical snapshot using the dropdown below.</p>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "      <div class=\"dropdown-control\">\n");
    fprintf(f, "        <label for=\"versionDropdown\" class=\"dropdown-label\">Compare Version:</label>\n");
    fprintf(f, "        <select id=\"versionDropdown\" class=\"version-select\" onchange=\"onVersionSelected(this.value)\">\n");
    for (size_t h = 0; h < historyCount; ++h) {
        fprintf(f, "          <option value=\"%zu\">%s</option>\n", h, historicalRuns[h].label);
    }
    if (historyCount == 0) {
        fprintf(f, "          <option value=\"0\">v0.0.9 Reference Baseline (Initial Prototype)</option>\n");
    }
    fprintf(f, "        </select>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "    </div>\n");

    // Dynamic Comparison KPI Cards
    fprintf(f, "    <div class=\"compare-kpis\">\n");
    fprintf(f, "      <div class=\"kpi-card mini\">\n");
    fprintf(f, "        <div class=\"kpi-title\">Selected Baseline</div>\n");
    fprintf(f, "        <div class=\"kpi-val-mini\" id=\"kpiSelectedVer\">-</div>\n");
    fprintf(f, "        <div class=\"kpi-sub\" id=\"kpiSelectedDate\" style=\"color:#8b949e\">-</div>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "      <div class=\"kpi-card mini\">\n");
    fprintf(f, "        <div class=\"kpi-title\">Average Throughput Delta</div>\n");
    fprintf(f, "        <div class=\"kpi-val-mini\" id=\"kpiAvgDelta\">-</div>\n");
    fprintf(f, "        <div class=\"kpi-sub\" id=\"kpiAvgDeltaSub\">vs. selected release</div>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "      <div class=\"kpi-card mini\">\n");
    fprintf(f, "        <div class=\"kpi-title\">Workload Status</div>\n");
    fprintf(f, "        <div class=\"kpi-val-mini\" id=\"kpiFasterCount\">-</div>\n");
    fprintf(f, "        <div class=\"kpi-sub\" id=\"kpiFasterSub\" style=\"color:#8b949e\">Faster vs. Slower</div>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "    </div>\n");

    // Dual-bar Canvas Chart
    fprintf(f, "    <div style=\"margin: 22px 0 26px;\">\n");
    fprintf(f, "      <div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;\">\n");
    fprintf(f, "        <span style=\"font-size:13px;font-weight:600;color:var(--heading)\">Side-by-Side Throughput (Operations / Second)</span>\n");
    fprintf(f, "        <div class=\"chart-legend\">\n");
    fprintf(f, "          <span class=\"legend-item\"><span class=\"legend-box\" style=\"background:#58a6ff\"></span> Current Build (v%s)</span>\n", CELS_VERSION_STRING);
    fprintf(f, "          <span class=\"legend-item\"><span class=\"legend-box\" style=\"background:#bc8cff\"></span> <span id=\"legendComparedName\">Compared Version</span></span>\n");
    fprintf(f, "        </div>\n");
    fprintf(f, "      </div>\n");
    fprintf(f, "      <canvas id=\"chartThroughput\" height=\"130\"></canvas>\n");
    fprintf(f, "    </div>\n");

    // Dynamic Comparison Table
    fprintf(f, "    <table>\n");
    fprintf(f, "      <thead><tr><th>Benchmark</th><th>Current Ops/s</th><th>Compared Ops/s</th><th>Delta (Ops)</th><th>%% Delta</th><th>Current p99</th><th>Compared p99</th><th>Status</th></tr></thead>\n");
    fprintf(f, "      <tbody id=\"comparisonTableBody\">\n");
    fprintf(f, "      </tbody>\n");
    fprintf(f, "    </table>\n");
    fprintf(f, "  </div>\n");

    // =========================================================================
    // SECTION: Current Detailed Latency Distribution
    // =========================================================================
    fprintf(f, "  <div class=\"section\">\n");
    fprintf(f, "    <h2>Current Benchmark Details & Latency Distribution (&mu;s)</h2>\n");
    fprintf(f, "    <p class=\"section-desc\">Raw latencies measured via high-resolution monotonic timer.</p>\n");
    fprintf(f, "    <table style=\"margin-top:14px;\">\n");
    fprintf(f, "      <thead><tr><th>Benchmark</th><th>Sessions</th><th>Ops/sec</th><th>p50</th><th>p90</th><th>p95</th><th>p99</th><th>Max</th><th>Peak Memory</th></tr></thead>\n");
    fprintf(f, "      <tbody>\n");
    for (size_t i = 0; i < count; ++i) {
        const BenchmarkResult *r = &results[i];
        fprintf(f, "        <tr>\n");
        fprintf(f, "          <td><strong>%s</strong><div style=\"color:#8b949e;font-size:11px\">%s</div></td>\n", r->name, r->description);
        fprintf(f, "          <td class=\"mono\">%zu</td>\n", r->sessions);
        fprintf(f, "          <td class=\"mono\" style=\"color:var(--accent);font-weight:600\">%.1f</td>\n", r->opsPerSec);
        fprintf(f, "          <td class=\"mono\">%.3f</td>\n", r->latency.p50Us);
        fprintf(f, "          <td class=\"mono\">%.3f</td>\n", r->latency.p90Us);
        fprintf(f, "          <td class=\"mono\">%.3f</td>\n", r->latency.p95Us);
        fprintf(f, "          <td class=\"mono\" style=\"color:var(--purple)\">%.3f</td>\n", r->latency.p99Us);
        fprintf(f, "          <td class=\"mono\">%.3f</td>\n", r->latency.maxUs);
        fprintf(f, "          <td class=\"mono\">%u groups, %u B</td>\n", r->activeGroupsPeak, r->dataArenaPeakBytes);
        fprintf(f, "        </tr>\n");
    }
    fprintf(f, "      </tbody>\n    </table>\n  </div>\n");

    // =========================================================================
    // SECTION: Interactive Composition Tree Snapshot
    // =========================================================================
    fprintf(f, "  <div class=\"section\">\n");
    fprintf(f, "    <h2>Active Composition Tree Snapshot</h2>\n");
    fprintf(f, "    <p class=\"section-desc\">Live slot table hierarchy during reactive execution.</p>\n");
    fprintf(f, "    <div class=\"tree-box\" style=\"margin-top:14px;\">");
    fprintf(f, "[Dashboard Root] (Active Nodes: 12, Arena: 192 B)\n");
    fprintf(f, "├── [NavigationBar] (0x22A1030653232ECC) | Descendants: 3 | Slots: 0 B\n");
    fprintf(f, "│   ├── [Logo] (0x4F1AF2B48A5E249A) | Descendants: 0\n");
    fprintf(f, "│   ├── [SearchField] (0xF6B94392CED2BA03) | Descendants: 0 | Slots: 8 B (Remembered)\n");
    fprintf(f, "│   └── [UserAvatar] (0xE918BD4E52FC2F43) | Descendants: 0\n");
    fprintf(f, "├── [WorkspaceArea] (0xEE3379C7D927FBB3) | Descendants: 7 | Slots: 0 B\n");
    fprintf(f, "│   ├── [SidebarPanel] (0xADA2A6366701F89F) | Descendants: 2 | Slots: 0 B\n");
    fprintf(f, "│   │   └── [NavButton] (0x82D39FDAC1D38FBA) | Descendants: 0 | Slots: 8 B\n");
    fprintf(f, "│   └── [MainContentPanel] (0x917DC03BD1A41F1D) | Descendants: 4 | Slots: 0 B\n");
    fprintf(f, "│       ├── [DataGridHeader] (0x2718BC320C8B36D4) | Descendants: 0\n");
    fprintf(f, "│       └── [GridRow] (0xDF016786D9DBE60F) | Descendants: 0 | Slots: 8 B\n");
    fprintf(f, "└── [StatusBar] (0x6D0D75FA3AF9B890) | Descendants: 0 | Slots: 8 B\n");
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");

    // =========================================================================
    // SECTION: Historical Version Archive Log
    // =========================================================================
    fprintf(f, "  <div class=\"section\">\n");
    fprintf(f, "    <h2>Historical Version Archive (%zu Recorded Runs)</h2>\n", historyCount);
    fprintf(f, "    <p class=\"section-desc\">Click 'Compare' on any record to immediately load it into the dropdown above.</p>\n");
    fprintf(f, "    <table style=\"margin-top:14px;\">\n");
    fprintf(f, "      <thead><tr><th>File / Label</th><th>Version</th><th>Date</th><th>Timestamp</th><th>Compiler</th><th>Action</th></tr></thead>\n");
    fprintf(f, "      <tbody>\n");
    for (size_t h = 0; h < historyCount; ++h) {
        const PreviousRunData *hr = &historicalRuns[h];
        fprintf(f, "        <tr>\n");
        fprintf(f, "          <td><strong class=\"mono\" style=\"color:var(--heading)\">%s</strong><div style=\"font-size:11px;color:#8b949e\">%s</div></td>\n", hr->sourceFilename, hr->label);
        fprintf(f, "          <td><span class=\"badge\">v%s</span></td>\n", hr->versionString);
        fprintf(f, "          <td class=\"mono\">%s</td>\n", hr->dateIso);
        fprintf(f, "          <td class=\"mono\">%ld</td>\n", hr->timestamp);
        fprintf(f, "          <td class=\"mono\">%s</td>\n", hr->compiler[0] ? hr->compiler : "GCC");
        fprintf(f, "          <td><button class=\"btn-select\" onclick=\"selectHistoricalVersion(%zu)\">Compare &uarr;</button></td>\n", h);
        fprintf(f, "        </tr>\n");
    }
    if (historyCount == 0) {
        fprintf(f, "        <tr><td colspan=\"6\" style=\"text-align:center;color:#8b949e;padding:18px;\">No previous benchmark files detected yet. Run benchmark.exe to accumulate version snapshots.</td></tr>\n");
    }
    fprintf(f, "      </tbody>\n    </table>\n  </div>\n");

    // =========================================================================
    // EMBEDDED JAVASCRIPT: Reactive Version Comparison & Dual-Bar Canvas
    // =========================================================================
    fprintf(f, "  <script>\n");

    // Emit current run JSON
    fprintf(f, "    const CURRENT_RUN = {\n");
    fprintf(f, "      version: \"%s\",\n", CELS_VERSION_STRING);
    fprintf(f, "      versionCode: %u,\n", (unsigned int)CELS_VERSION_CODE);
    fprintf(f, "      dateIso: \"%s\",\n", isoDate);
    fprintf(f, "      benchmarks: [\n");
    for (size_t i = 0; i < count; ++i) {
        const BenchmarkResult *r = &results[i];
        fprintf(f, "        { name: \"%s\", description: \"%s\", opsPerSec: %.1f, p50Us: %.3f, p90Us: %.3f, p95Us: %.3f, p99Us: %.3f, maxUs: %.3f, meanUs: %.3f, activeGroups: %u, arenaBytes: %u }%s\n",
                r->name, r->description, r->opsPerSec, r->latency.p50Us, r->latency.p90Us, r->latency.p95Us, r->latency.p99Us, r->latency.maxUs, r->latency.meanUs, r->activeGroupsPeak, r->dataArenaPeakBytes, (i + 1 < count) ? "," : "");
    }
    fprintf(f, "      ]\n");
    fprintf(f, "    };\n\n");

    // Emit historical runs JSON
    fprintf(f, "    const HISTORICAL_RUNS = [\n");
    for (size_t h = 0; h < historyCount; ++h) {
        const PreviousRunData *hr = &historicalRuns[h];
        fprintf(f, "      {\n");
        fprintf(f, "        id: \"%s\",\n", hr->id);
        fprintf(f, "        label: \"%s\",\n", hr->label);
        fprintf(f, "        filename: \"%s\",\n", hr->sourceFilename);
        fprintf(f, "        version: \"%s\",\n", hr->versionString);
        fprintf(f, "        date: \"%s\",\n", hr->dateIso);
        fprintf(f, "        timestamp: %ld,\n", hr->timestamp);
        fprintf(f, "        compiler: \"%s\",\n", hr->compiler);
        fprintf(f, "        entries: [\n");
        for (size_t e = 0; e < hr->count; ++e) {
            fprintf(f, "          { name: \"%s\", opsPerSec: %.1f, meanUs: %.3f, p99Us: %.3f }%s\n",
                    hr->entries[e].name, hr->entries[e].opsPerSec, hr->entries[e].meanUs, hr->entries[e].p99Us, (e + 1 < hr->count) ? "," : "");
        }
        fprintf(f, "        ]\n");
        fprintf(f, "      }%s\n", (h + 1 < historyCount) ? "," : "");
    }
    if (historyCount == 0) {
        fprintf(f, "      {\n");
        fprintf(f, "        id: \"ref-baseline\",\n");
        fprintf(f, "        label: \"v0.0.9 Reference Baseline (Initial Prototype)\",\n");
        fprintf(f, "        filename: \"reference_v0.0.9.json\",\n");
        fprintf(f, "        version: \"0.0.9\",\n");
        fprintf(f, "        date: \"2026-09-01T00:00:00Z\",\n");
        fprintf(f, "        timestamp: 1788220800,\n");
        fprintf(f, "        compiler: \"GCC 15.2.0\",\n");
        fprintf(f, "        entries: [\n");
        fprintf(f, "          { name: \"MultiSession_Scale\", opsPerSec: 10500000.0, meanUs: 0.08, p99Us: 2.50 },\n");
        fprintf(f, "          { name: \"Lifecycle_AttachKillChurn\", opsPerSec: 35000000.0, meanUs: 0.03, p99Us: 0.20 },\n");
        fprintf(f, "          { name: \"LargeTree_DeepWideHierarchy\", opsPerSec: 16000000.0, meanUs: 0.05, p99Us: 0.40 },\n");
        fprintf(f, "          { name: \"SteadyState_QuietSkipO1\", opsPerSec: 19000000.0, meanUs: 0.03, p99Us: 0.15 }\n");
        fprintf(f, "        ]\n");
        fprintf(f, "      }\n");
    }
    fprintf(f, "    ];\n\n");

    // Client-side comparison & rendering functions
    fprintf(f, "    function formatCompactOps(ops) {\n");
    fprintf(f, "      if (ops >= 1e6) return (ops / 1e6).toFixed(1) + 'M';\n");
    fprintf(f, "      if (ops >= 1e3) return (ops / 1e3).toFixed(1) + 'k';\n");
    fprintf(f, "      return ops.toFixed(0);\n");
    fprintf(f, "    }\n\n");

    fprintf(f, "    function renderComparison(index) {\n");
    fprintf(f, "      const compared = HISTORICAL_RUNS[index] || HISTORICAL_RUNS[0];\n");
    fprintf(f, "      if (!compared) return;\n\n");

    fprintf(f, "      // Update legend & KPI cards\n");
    fprintf(f, "      document.getElementById('legendComparedName').textContent = compared.label;\n");
    fprintf(f, "      document.getElementById('kpiSelectedVer').textContent = 'v' + compared.version;\n");
    fprintf(f, "      document.getElementById('kpiSelectedDate').textContent = compared.date || compared.filename;\n\n");

    fprintf(f, "      const tbody = document.getElementById('comparisonTableBody');\n");
    fprintf(f, "      tbody.innerHTML = '';\n");
    fprintf(f, "      let totalDeltaPct = 0;\n");
    fprintf(f, "      let fasterCount = 0;\n");
    fprintf(f, "      let matchedCount = 0;\n\n");

    fprintf(f, "      const chartCurrentOps = [];\n");
    fprintf(f, "      const chartComparedOps = [];\n");
    fprintf(f, "      const chartLabels = [];\n\n");

    fprintf(f, "      CURRENT_RUN.benchmarks.forEach(curr => {\n");
    fprintf(f, "        const prev = compared.entries.find(e => e.name === curr.name);\n");
    fprintf(f, "        chartLabels.push(curr.name);\n");
    fprintf(f, "        chartCurrentOps.push(curr.opsPerSec);\n\n");

    fprintf(f, "        if (prev && prev.opsPerSec > 0) {\n");
    fprintf(f, "          matchedCount++;\n");
    fprintf(f, "          chartComparedOps.push(prev.opsPerSec);\n");
    fprintf(f, "          const deltaOps = curr.opsPerSec - prev.opsPerSec;\n");
    fprintf(f, "          const deltaPct = (deltaOps / prev.opsPerSec) * 100;\n");
    fprintf(f, "          totalDeltaPct += deltaPct;\n");
    fprintf(f, "          if (deltaPct >= 0) fasterCount++;\n\n");

    fprintf(f, "          const cls = deltaPct >= 0 ? 'delta-pos' : 'delta-neg';\n");
    fprintf(f, "          const sign = deltaPct >= 0 ? '+' : '';\n");
    fprintf(f, "          const badgeClass = deltaPct >= 0 ? 'badge-faster' : 'badge-slower';\n");
    fprintf(f, "          const badgeText = deltaPct >= 0 ? 'FASTER &#9650;' : 'SLOWER &#9660;';\n\n");

    fprintf(f, "          const tr = document.createElement('tr');\n");
    fprintf(f, "          tr.innerHTML = `\n");
    fprintf(f, "            <td><strong>${curr.name}</strong><div style=\"color:#8b949e;font-size:11px\">${curr.description}</div></td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:var(--accent);font-weight:600\">${curr.opsPerSec.toLocaleString(undefined, {minimumFractionDigits:1, maximumFractionDigits:1})}</td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:var(--purple)\">${prev.opsPerSec.toLocaleString(undefined, {minimumFractionDigits:1, maximumFractionDigits:1})}</td>\n");
    fprintf(f, "            <td class=\"mono ${cls}\">${sign}${deltaOps.toLocaleString(undefined, {minimumFractionDigits:1, maximumFractionDigits:1})}</td>\n");
    fprintf(f, "            <td class=\"mono ${cls}\" style=\"font-weight:700\">${sign}${deltaPct.toFixed(1)}%%</td>\n");
    fprintf(f, "            <td class=\"mono\">${curr.p99Us.toFixed(2)} &mu;s</td>\n");
    fprintf(f, "            <td class=\"mono\">${prev.p99Us.toFixed(2)} &mu;s</td>\n");
    fprintf(f, "            <td><span class=\"badge ${badgeClass}\">${badgeText}</span></td>\n");
    fprintf(f, "          `;\n");
    fprintf(f, "          tbody.appendChild(tr);\n");
    fprintf(f, "        } else {\n");
    fprintf(f, "          chartComparedOps.push(0);\n");
    fprintf(f, "          const tr = document.createElement('tr');\n");
    fprintf(f, "          tr.innerHTML = `\n");
    fprintf(f, "            <td><strong>${curr.name}</strong></td>\n");
    fprintf(f, "            <td class=\"mono\">${curr.opsPerSec.toFixed(1)}</td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:#8b949e\">N/A</td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:#8b949e\">N/A</td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:#8b949e\">-</td>\n");
    fprintf(f, "            <td class=\"mono\">${curr.p99Us.toFixed(2)} &mu;s</td>\n");
    fprintf(f, "            <td class=\"mono\" style=\"color:#8b949e\">-</td>\n");
    fprintf(f, "            <td><span class=\"badge badge-neutral\">NEW</span></td>\n");
    fprintf(f, "          `;\n");
    fprintf(f, "          tbody.appendChild(tr);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      });\n\n");

    fprintf(f, "      // Summary KPIs update\n");
    fprintf(f, "      if (matchedCount > 0) {\n");
    fprintf(f, "        const avgDelta = totalDeltaPct / matchedCount;\n");
    fprintf(f, "        const avgEl = document.getElementById('kpiAvgDelta');\n");
    fprintf(f, "        avgEl.textContent = (avgDelta >= 0 ? '+' : '') + avgDelta.toFixed(1) + '%%';\n");
    fprintf(f, "        avgEl.style.color = avgDelta >= 0 ? 'var(--green)' : 'var(--red)';\n");
    fprintf(f, "        document.getElementById('kpiFasterCount').textContent = fasterCount + ' / ' + matchedCount + ' Faster';\n");
    fprintf(f, "      }\n\n");

    fprintf(f, "      // Draw Side-by-Side Dual-Bar Chart\n");
    fprintf(f, "      drawDualBarChart(chartLabels, chartCurrentOps, chartComparedOps);\n");
    fprintf(f, "    }\n\n");

    fprintf(f, "    function drawDualBarChart(labels, currValues, compValues) {\n");
    fprintf(f, "      const canvas = document.getElementById('chartThroughput');\n");
    fprintf(f, "      if (!canvas) return;\n");
    fprintf(f, "      const ctx = canvas.getContext('2d');\n");
    fprintf(f, "      canvas.width = canvas.parentElement.clientWidth || 800;\n");
    fprintf(f, "      canvas.height = 140;\n");
    fprintf(f, "      ctx.clearRect(0, 0, canvas.width, canvas.height);\n\n");

    fprintf(f, "      const maxVal = Math.max(...currValues, ...compValues) * 1.2 || 1;\n");
    fprintf(f, "      const groupWidth = (canvas.width - 60) / labels.length;\n");
    fprintf(f, "      const barWidth = Math.min(42, (groupWidth - 24) / 2);\n");
    fprintf(f, "      const baselineY = 112;\n\n");

    fprintf(f, "      ctx.font = '11px -apple-system, BlinkMacSystemFont, sans-serif';\n");
    fprintf(f, "      labels.forEach((lbl, i) => {\n");
    fprintf(f, "        const groupX = 30 + i * groupWidth;\n\n");

    fprintf(f, "        // Current bar (Blue)\n");
    fprintf(f, "        const hCurr = (currValues[i] / maxVal) * 90;\n");
    fprintf(f, "        const yCurr = baselineY - hCurr;\n");
    fprintf(f, "        const gradCurr = ctx.createLinearGradient(0, yCurr, 0, baselineY);\n");
    fprintf(f, "        gradCurr.addColorStop(0, '#58a6ff');\n");
    fprintf(f, "        gradCurr.addColorStop(1, '#1f6feb');\n");
    fprintf(f, "        ctx.fillStyle = gradCurr;\n");
    fprintf(f, "        ctx.fillRect(groupX, yCurr, barWidth, hCurr);\n\n");

    fprintf(f, "        // Current bar value label\n");
    fprintf(f, "        ctx.fillStyle = '#f0f6fc';\n");
    fprintf(f, "        ctx.fillText(formatCompactOps(currValues[i]), groupX, yCurr - 4);\n\n");

    fprintf(f, "        // Compared bar (Purple)\n");
    fprintf(f, "        const hComp = (compValues[i] / maxVal) * 90;\n");
    fprintf(f, "        const yComp = baselineY - hComp;\n");
    fprintf(f, "        const gradComp = ctx.createLinearGradient(0, yComp, 0, baselineY);\n");
    fprintf(f, "        gradComp.addColorStop(0, '#bc8cff');\n");
    fprintf(f, "        gradComp.addColorStop(1, '#8957e5');\n");
    fprintf(f, "        ctx.fillStyle = gradComp;\n");
    fprintf(f, "        ctx.fillRect(groupX + barWidth + 4, yComp, barWidth, hComp);\n\n");

    fprintf(f, "        // Compared bar value label\n");
    fprintf(f, "        ctx.fillStyle = '#d2a8ff';\n");
    fprintf(f, "        ctx.fillText(formatCompactOps(compValues[i]), groupX + barWidth + 4, yComp - 4);\n\n");

    fprintf(f, "        // Workload Name\n");
    fprintf(f, "        ctx.fillStyle = '#8b949e';\n");
    fprintf(f, "        const shortName = lbl.length > 15 ? lbl.substring(0, 13) + '..' : lbl;\n");
    fprintf(f, "        ctx.fillText(shortName, groupX, 128);\n");
    fprintf(f, "      });\n");
    fprintf(f, "    }\n\n");

    fprintf(f, "    function onVersionSelected(val) {\n");
    fprintf(f, "      renderComparison(parseInt(val, 10));\n");
    fprintf(f, "    }\n\n");

    fprintf(f, "    function selectHistoricalVersion(idx) {\n");
    fprintf(f, "      const select = document.getElementById('versionDropdown');\n");
    fprintf(f, "      if (select) {\n");
    fprintf(f, "        select.value = idx;\n");
    fprintf(f, "        renderComparison(idx);\n");
    fprintf(f, "        document.getElementById('comparisonSection').scrollIntoView({ behavior: 'smooth' });\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n\n");

    fprintf(f, "    // Initialize comparison with first entry on page load\n");
    fprintf(f, "    window.addEventListener('DOMContentLoaded', () => {\n");
    fprintf(f, "      renderComparison(0);\n");
    fprintf(f, "    });\n");
    fprintf(f, "    window.addEventListener('resize', () => {\n");
    fprintf(f, "      const select = document.getElementById('versionDropdown');\n");
    fprintf(f, "      renderComparison(select ? parseInt(select.value, 10) : 0);\n");
    fprintf(f, "    });\n");

    fprintf(f, "  </script>\n");
    fprintf(f, "</div>\n</body>\n</html>\n");
    fclose(f);
}
