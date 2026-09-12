#pragma once

/**
 * @file test_cli.h
 * @brief Lightweight CLI test harness and runner for CELS test suites.
 *
 * Provides structured test registration, single-test or all-test execution,
 * listing of tests, and unified multi-suite test running.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Function pointer for a single test case.
 */
typedef void (*TestFn)(void);

/**
 * A named test case within a test suite.
 */
typedef struct TestCase {
    const char *name;
    const char *description;
    TestFn fn;
} TestCase;

/**
 * A test suite grouping multiple test cases under a feature name.
 */
typedef struct TestSuite {
    const char *name;
    const char *description;
    const TestCase *tests;
    size_t testCount;
} TestSuite;

/* Suite provider declarations across the CELS codebase */
const TestSuite *GetCliTestSuite(void);
const TestSuite *GetSlotTableTestSuite(void);
const TestSuite *GetSlabTestSuite(void);
const TestSuite *GetLifecycleTestSuite(void);
const TestSuite *GetStateLifetimeTestSuite(void);
const TestSuite *GetTreeTestSuite(void);
const TestSuite *GetBenchmarkTestSuite(void);

/**
 * Case-insensitive string comparison helper.
 */
static inline bool TestCliStringEqualsIgnoreCase(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/**
 * Executes a single test case.
 */
static inline bool TestCliRunCase(const char *suiteName, const TestCase *tc) {
    printf("  [RUN ] %s::%s\n", suiteName ? suiteName : "test", tc->name);
    fflush(stdout);
    tc->fn();
    printf("  [PASS] %s::%s\n", suiteName ? suiteName : "test", tc->name);
    fflush(stdout);
    return true;
}

/**
 * Finds a test case by name in a given suite (case-insensitive).
 */
static inline const TestCase *TestCliFindCase(const TestSuite *suite, const char *name) {
    if (suite == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < suite->testCount; ++i) {
        if (TestCliStringEqualsIgnoreCase(suite->tests[i].name, name)) {
            return &suite->tests[i];
        }
    }
    return NULL;
}

/**
 * Prints available tests in a single suite.
 */
static inline void TestCliListSuite(const TestSuite *suite) {
    printf("Available tests in suite '%s':\n", suite->name);
    for (size_t i = 0; i < suite->testCount; ++i) {
        if (suite->tests[i].description && suite->tests[i].description[0]) {
            printf("  - %-34s (%s)\n", suite->tests[i].name, suite->tests[i].description);
        } else {
            printf("  - %s\n", suite->tests[i].name);
        }
    }
}

/**
 * Runs all tests in a suite or a specific test case based on CLI arguments.
 */
static inline int TestCliRunSuite(const TestSuite *suite, int argc, char **argv) {
    if (argc < 2 || TestCliStringEqualsIgnoreCase(argv[1], "all")) {
        printf("====================================================\n");
        printf(" Running %s Test Suite (%zu tests)\n", suite->name, suite->testCount);
        printf("====================================================\n");
        for (size_t i = 0; i < suite->testCount; ++i) {
            TestCliRunCase(suite->name, &suite->tests[i]);
        }
        printf("====================================================\n");
        printf(" PASSED: All %zu tests in '%s' completed successfully!\n",
               suite->testCount, suite->name);
        printf("====================================================\n");
        return 0;
    }

    if (TestCliStringEqualsIgnoreCase(argv[1], "--list") ||
        TestCliStringEqualsIgnoreCase(argv[1], "-l") ||
        TestCliStringEqualsIgnoreCase(argv[1], "list")) {
        TestCliListSuite(suite);
        return 0;
    }

    if (TestCliStringEqualsIgnoreCase(argv[1], "--help") ||
        TestCliStringEqualsIgnoreCase(argv[1], "-h") ||
        TestCliStringEqualsIgnoreCase(argv[1], "help")) {
        printf("Usage: %s [OPTIONS] [TEST_NAME]\n\n", argv[0]);
        printf("Options:\n");
        printf("  -l, --list    List all available tests in this suite\n");
        printf("  -h, --help    Show this help message\n");
        printf("  all           Run all tests (default when no argument given)\n\n");
        printf("Available tests in '%s':\n", suite->name);
        for (size_t i = 0; i < suite->testCount; ++i) {
            printf("  %s\n", suite->tests[i].name);
        }
        return 0;
    }

    const char *testName = argv[1];
    const TestCase *tc = TestCliFindCase(suite, testName);
    if (tc != NULL) {
        printf("Running single test: %s::%s\n", suite->name, tc->name);
        TestCliRunCase(suite->name, tc);
        printf("PASSED: %s::%s\n", suite->name, tc->name);
        return 0;
    }

    fprintf(stderr, "Error: Unknown test '%s' in suite '%s'.\n\n", testName, suite->name);
    TestCliListSuite(suite);
    return 1;
}

/**
 * Finds a suite by name among an array of suites (case-insensitive).
 */
static inline const TestSuite *TestCliFindSuite(const TestSuite *const *suites,
                                                size_t suiteCount,
                                                const char *name) {
    if (suites == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < suiteCount; ++i) {
        if (suites[i] && TestCliStringEqualsIgnoreCase(suites[i]->name, name)) {
            return suites[i];
        }
    }
    return NULL;
}

/**
 * Runs a multi-suite CLI runner (like test_cli).
 */
static inline int TestCliRunAll(const TestSuite *const *suites,
                                size_t suiteCount,
                                int argc,
                                char **argv) {
    if (argc < 2 || TestCliStringEqualsIgnoreCase(argv[1], "all")) {
        size_t totalTests = 0;
        printf("====================================================\n");
        printf(" Running CELS Test Suite (%zu features)\n", suiteCount);
        printf("====================================================\n");
        for (size_t s = 0; s < suiteCount; ++s) {
            const TestSuite *suite = suites[s];
            if (!suite) continue;
            printf("\n--- Feature: %s (%zu tests) ---\n", suite->name, suite->testCount);
            for (size_t i = 0; i < suite->testCount; ++i) {
                TestCliRunCase(suite->name, &suite->tests[i]);
                totalTests++;
            }
        }
        printf("\n====================================================\n");
        printf(" ALL %zu TESTS PASSED ACROSS %zu FEATURES!\n", totalTests, suiteCount);
        printf("====================================================\n");
        return 0;
    }

    if (TestCliStringEqualsIgnoreCase(argv[1], "--list") ||
        TestCliStringEqualsIgnoreCase(argv[1], "-l") ||
        TestCliStringEqualsIgnoreCase(argv[1], "list")) {
        printf("Available CELS Test Features and Test Cases:\n");
        for (size_t s = 0; s < suiteCount; ++s) {
            const TestSuite *suite = suites[s];
            if (!suite) continue;
            printf("\nFeature: %s (%zu tests)%s%s\n",
                   suite->name,
                   suite->testCount,
                   (suite->description && suite->description[0]) ? " - " : "",
                   (suite->description && suite->description[0]) ? suite->description : "");
            for (size_t i = 0; i < suite->testCount; ++i) {
                if (suite->tests[i].description && suite->tests[i].description[0]) {
                    printf("  - %-34s (%s)\n", suite->tests[i].name, suite->tests[i].description);
                } else {
                    printf("  - %s\n", suite->tests[i].name);
                }
            }
        }
        return 0;
    }

    if (TestCliStringEqualsIgnoreCase(argv[1], "--help") ||
        TestCliStringEqualsIgnoreCase(argv[1], "-h") ||
        TestCliStringEqualsIgnoreCase(argv[1], "help")) {
        printf("Usage: %s [OPTIONS] [FEATURE] [TEST_NAME]\n\n", argv[0]);
        printf("Arguments:\n");
        printf("  FEATURE      Name of the feature to run (e.g. 'slab', 'slot_table', 'tree')\n");
        printf("  TEST_NAME    Name of a specific test case to run (e.g. 'TestDefaultSlab')\n\n");
        printf("Options:\n");
        printf("  -l, --list   List all available features and tests\n");
        printf("  -h, --help   Show this help message\n");
        printf("  all          Run all tests across all features (default)\n\n");
        printf("Examples:\n");
        printf("  %s                               Run all tests\n", argv[0]);
        printf("  %s slab                          Run all tests in feature 'slab'\n", argv[0]);
        printf("  %s slab TestDefaultSlab          Run specific test in 'slab'\n", argv[0]);
        printf("  %s TestDefaultSlab               Run specific test by name\n", argv[0]);
        return 0;
    }

    if (argc == 2) {
        const char *arg = argv[1];

        // Is it a feature/suite name?
        const TestSuite *matchedSuite = TestCliFindSuite(suites, suiteCount, arg);
        if (matchedSuite != NULL) {
            printf("====================================================\n");
            printf(" Running Feature '%s' (%zu tests)\n", matchedSuite->name, matchedSuite->testCount);
            printf("====================================================\n");
            for (size_t i = 0; i < matchedSuite->testCount; ++i) {
                TestCliRunCase(matchedSuite->name, &matchedSuite->tests[i]);
            }
            printf("====================================================\n");
            printf(" PASSED: All %zu tests in '%s' completed successfully!\n",
                   matchedSuite->testCount, matchedSuite->name);
            printf("====================================================\n");
            return 0;
        }

        // Is it a test name in any suite?
        for (size_t s = 0; s < suiteCount; ++s) {
            const TestSuite *suite = suites[s];
            if (!suite) continue;
            const TestCase *tc = TestCliFindCase(suite, arg);
            if (tc != NULL) {
                printf("Running single test: %s::%s\n", suite->name, tc->name);
                TestCliRunCase(suite->name, tc);
                printf("PASSED: %s::%s\n", suite->name, tc->name);
                return 0;
            }
        }

        fprintf(stderr, "Error: '%s' matches no known feature or test name.\n\n", arg);
        fprintf(stderr, "Use '%s --list' to see available features and tests.\n", argv[0]);
        return 1;
    }

    if (argc == 3) {
        const char *featureName = argv[1];
        const char *testName = argv[2];

        const TestSuite *matchedSuite = TestCliFindSuite(suites, suiteCount, featureName);
        if (matchedSuite == NULL) {
            fprintf(stderr, "Error: Unknown feature '%s'.\n", featureName);
            fprintf(stderr, "Use '%s --list' to see available features.\n", argv[0]);
            return 1;
        }

        const TestCase *tc = TestCliFindCase(matchedSuite, testName);
        if (tc == NULL) {
            fprintf(stderr, "Error: Unknown test '%s' in feature '%s'.\n\n", testName, featureName);
            TestCliListSuite(matchedSuite);
            return 1;
        }

        printf("Running single test: %s::%s\n", matchedSuite->name, tc->name);
        TestCliRunCase(matchedSuite->name, tc);
        printf("PASSED: %s::%s\n", matchedSuite->name, tc->name);
        return 0;
    }

    fprintf(stderr, "Error: Too many arguments.\n");
    fprintf(stderr, "Usage: %s [OPTIONS] [FEATURE] [TEST_NAME]\n", argv[0]);
    return 1;
}
