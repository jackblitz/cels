#include "cli/test_cli.h"

#include <assert.h>

/**
 * Validates that case-insensitive string matching works correctly.
 */
static void TestCliCaseInsensitiveMatching(void) {
    assert(TestCliStringEqualsIgnoreCase("slab", "slab"));
    assert(TestCliStringEqualsIgnoreCase("slab", "SLAB"));
    assert(TestCliStringEqualsIgnoreCase("Slab", "sLAb"));
    assert(TestCliStringEqualsIgnoreCase("TestDefaultSlab", "testdefaultslab"));
    assert(!TestCliStringEqualsIgnoreCase("slab", "slab2"));
    assert(!TestCliStringEqualsIgnoreCase("slab2", "slab"));
    assert(!TestCliStringEqualsIgnoreCase(NULL, "slab"));
    assert(!TestCliStringEqualsIgnoreCase("slab", NULL));
}

/**
 * Validates suite lookup by name across registered suites.
 */
static void TestCliCaseFindSuite(void) {
    const TestSuite *dummySuites[] = {
        GetCliTestSuite(),
        GetSlabTestSuite(),
        GetSlotTableTestSuite()
    };
    const size_t count = sizeof(dummySuites) / sizeof(dummySuites[0]);

    const TestSuite *foundSlab = TestCliFindSuite(dummySuites, count, "slab");
    assert(foundSlab != NULL);
    assert(strcmp(foundSlab->name, "slab") == 0);

    const TestSuite *foundCliUpper = TestCliFindSuite(dummySuites, count, "CLI");
    assert(foundCliUpper != NULL);
    assert(strcmp(foundCliUpper->name, "cli") == 0);

    const TestSuite *notFound = TestCliFindSuite(dummySuites, count, "nonexistent");
    assert(notFound == NULL);
}

/**
 * Validates individual test case lookup within a suite.
 */
static void TestCliCaseFindCase(void) {
    const TestSuite *slabSuite = GetSlabTestSuite();
    assert(slabSuite != NULL);

    const TestCase *tc = TestCliFindCase(slabSuite, "TestDefaultSlab");
    assert(tc != NULL);
    assert(strcmp(tc->name, "TestDefaultSlab") == 0);

    const TestCase *tcLower = TestCliFindCase(slabSuite, "testdefaultslab");
    assert(tcLower != NULL);
    assert(tcLower == tc);

    const TestCase *tcMissing = TestCliFindCase(slabSuite, "TestNonExistent");
    assert(tcMissing == NULL);
}

/**
 * Validates suite integrity and test counts across all registered suites.
 */
static void TestCliSuiteCount(void) {
    const TestSuite *suites[] = {
        GetCliTestSuite(),
        GetSlotTableTestSuite(),
        GetSlabTestSuite(),
        GetLifecycleTestSuite(),
        GetStateLifetimeTestSuite(),
        GetTreeTestSuite()
    };
    const size_t count = sizeof(suites) / sizeof(suites[0]);
    assert(count == 6);

    for (size_t i = 0; i < count; ++i) {
        assert(suites[i] != NULL);
        assert(suites[i]->name != NULL);
        assert(suites[i]->testCount > 0);
        assert(suites[i]->tests != NULL);
    }
}

static const TestCase s_cliTests[] = {
    { "TestCliCaseInsensitiveMatching", "Case-insensitive string comparison helper", TestCliCaseInsensitiveMatching },
    { "TestCliCaseFindSuite", "Suite lookup by name across registry", TestCliCaseFindSuite },
    { "TestCliCaseFindCase", "Test case lookup within suites", TestCliCaseFindCase },
    { "TestCliSuiteCount", "Verify registered suites and test count integrity", TestCliSuiteCount }
};

static const TestSuite s_cliSuite = {
    .name = "cli",
    .description = "Test CLI framework, argument dispatch and lookup",
    .tests = s_cliTests,
    .testCount = sizeof(s_cliTests) / sizeof(s_cliTests[0])
};

const TestSuite *GetCliTestSuite(void) {
    return &s_cliSuite;
}

int main(int argc, char **argv) {
    const TestSuite *suites[] = {
        GetCliTestSuite(),
        GetSlotTableTestSuite(),
        GetSlabTestSuite(),
        GetLifecycleTestSuite(),
        GetStateLifetimeTestSuite(),
        GetTreeTestSuite()
    };
    const size_t suiteCount = sizeof(suites) / sizeof(suites[0]);
    return TestCliRunAll(suites, suiteCount, argc, argv);
}
