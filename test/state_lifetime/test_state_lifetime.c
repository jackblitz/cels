#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int childState;
static int childRuns;
static int siblingRuns;

static void KeyRoot(CelsSession *s) {
    CEL_Composition(s, UINT64_C(0x1234567800000001)) {
        CEL_Composable(UINT64_C(0xfedcba9800000002)) {
            (void)cel_watch(&childState);
            ++childRuns;
        }
        CEL_Composable(UINT64_C(0xfedcba9800000003)) {
            ++siblingRuns;
        }
    } CEL_Close(s);
}

static void TestHighKeys(void) {
    childState = 0;
    childRuns = 0;
    siblingRuns = 0;

    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .root = KeyRoot });
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 1 && siblingRuns == 1);
    cel_mutate(&s, &childState) { ++*this; }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 2 && siblingRuns == 1);
    cel_mutate(&s, &childState) { *this = childState; }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 2 && siblingRuns == 1);
    CelsSessionDestroy(&s);
    assert(CelsGetCurrentSession() == NULL);
}

typedef struct Resource {
    int id;
    unsigned magic;
} Resource;

static int layout;
static int creates;
static int destroys;
static int destroyedIds[64];
static Resource *resources[4];
static int *values[4];
static int *beforeChildren;
static int *afterChildren;
static int nodeRuns[4];

static void ResourceCreate(void *instance, CelsSession *s) {
    assert(CelsGetCurrentSession() == s);
    Resource *resource = instance;
    resource->magic = 0xabcdef12;
    ++creates;
}

static void ResourceDestroy(void *instance, CelsSession *s) {
    assert(CelsGetCurrentSession() == s);
    Resource *resource = instance;
    assert(resource->magic == 0xabcdef12);
    destroyedIds[destroys++] = resource->id;
    resource->magic = 0;
}

static void Node(int id) {
    CEL_Composable(CEL_KeyIndex(CEL_KEY("node"), id)) {
        ++nodeRuns[id];
        Resource *resource = cel_observer(Resource, ResourceCreate, ResourceDestroy);
        int *value = cel_remember(int, id * 100);
        cel_init { resource->id = id; }
        resources[id] = resource;
        values[id] = value;
        (void)cel_watch(value);
    }
}

static void EditRoot(CelsSession *s) {
    CEL_Composition(s, CEL_KEY("edit-root")) {
        int mode = cel_watch(&layout);
        beforeChildren = cel_remember(int, 123);
        if (mode == 0) { Node(0); Node(1); Node(2); }
        if (mode == 1) { Node(0); Node(3); Node(1); Node(2); }
        if (mode == 2) { Node(2); Node(1); Node(3); Node(0); }
        if (mode == 3) { Node(1); Node(2); }
        afterChildren = cel_remember(int, 456);
    } CEL_Close(s);
}

static void TestEditsPreserveResources(void) {
    layout = 0;
    creates = 0;
    destroys = 0;
    memset(destroyedIds, 0, sizeof(destroyedIds));
    memset(resources, 0, sizeof(resources));
    memset(values, 0, sizeof(values));
    beforeChildren = NULL;
    afterChildren = NULL;
    memset(nodeRuns, 0, sizeof(nodeRuns));

    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .root = EditRoot });
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(creates == 3 && destroys == 0);
    Resource *savedResource = resources[1];
    int *savedValue = values[1];
    int *savedBefore = beforeChildren;
    int *savedAfter = afterChildren;
    for (int mode = 1; mode <= 3; ++mode) {
        cel_mutate(&s, &layout) { *this = mode; }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(resources[1] == savedResource && values[1] == savedValue);
        assert(beforeChildren == savedBefore && afterChildren == savedAfter);
        assert(*savedBefore == 123 && *savedAfter == 456);
        assert(*savedValue == 100 && savedResource->magic == 0xabcdef12);
    }
    assert(creates == 4 && destroys == 2);
    cel_mutate(&s, savedValue) { *this = 321; }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(values[1] == savedValue && *values[1] == 321);
    assert(creates == 4 && destroys == 2);
    CelsSessionDestroy(&s);
    assert(destroys == creates);
}

static bool showParent = true;

static void TeardownRoot(CelsSession *s) {
    CEL_Composition(s, CEL_KEY("teardown-root")) {
        if (cel_watch(&showParent)) {
            CEL_Composable(CEL_KEY("parent")) {
                Node(0);
                /* Parent resource is allocated after the child's resource. */
                Resource *parent = cel_observer(Resource, ResourceCreate, ResourceDestroy);
                cel_init { parent->id = 9; }
            }
        }
    } CEL_Close(s);
}

static void TestChildBeforeParent(void) {
    showParent = true;
    creates = 0;
    destroys = 0;
    memset(destroyedIds, 0, sizeof(destroyedIds));

    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .root = TeardownRoot });
    assert(CelsSessionRecompose(&s) == CELS_OK);
    int start = destroys;
    cel_mutate(&s, &showParent) { *this = false; }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(destroys == start + 2);
    assert(destroyedIds[start] == 0 && destroyedIds[start + 1] == 9);
    cel_mutate(&s, &showParent) { *this = true; }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    start = destroys;
    CelsSession other;
    CelsSessionInit(&other, NULL);
    CelsSetCurrentSession(&other);
    CelsSessionDestroy(&s);
    assert(destroys == start + 2);
    assert(destroyedIds[start] == 0 && destroyedIds[start + 1] == 9);
    assert(CelsGetCurrentSession() == &other);
    CelsSessionDestroy(&other);
    assert(CelsGetCurrentSession() == NULL);
}

static int nestedLayout;
static int parentRuns[3];
static Resource *parentResources[3];

static void Parent(int id) {
    CEL_Composable(CEL_KeyIndex(CEL_KEY("nested-parent"), id)) {
        ++parentRuns[id];
        Resource *resource = cel_observer(Resource, ResourceCreate, ResourceDestroy);
        cel_init { resource->id = 10 + id; }
        parentResources[id] = resource;
        Node(id);
    }
}

static void NestedRoot(CelsSession *s) {
    CEL_Composition(s, CEL_KEY("nested-root")) {
        int mode = cel_watch(&nestedLayout);
        if (mode == 0) { Parent(0); Parent(1); }
        if (mode == 1) { Parent(2); Parent(0); Parent(1); }
        if (mode == 2) { Parent(1); Parent(0); }
        if (mode == 3) { Parent(0); Parent(1); }
    } CEL_Close(s);
}

static void TestNestedEditsKeepSubscriptions(void) {
    nestedLayout = 0;
    creates = 0;
    destroys = 0;
    memset(parentRuns, 0, sizeof(parentRuns));
    memset(parentResources, 0, sizeof(parentResources));
    memset(nodeRuns, 0, sizeof(nodeRuns));
    memset(resources, 0, sizeof(resources));
    memset(values, 0, sizeof(values));

    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .root = NestedRoot });
    assert(CelsSessionRecompose(&s) == CELS_OK);
    Resource *parent = parentResources[1];
    Resource *child = resources[1];
    int *value = values[1];
    for (int mode = 1; mode <= 3; ++mode) {
        cel_mutate(&s, &nestedLayout) { *this = mode; }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(parentResources[1] == parent && resources[1] == child && values[1] == value);
        int parentBefore = parentRuns[1], childBefore = nodeRuns[1];
        int siblingBefore = parentRuns[0];
        cel_mutate(&s, value) { ++*this; }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(parentRuns[1] == parentBefore + 1 && nodeRuns[1] == childBefore + 1);
        assert(parentRuns[0] == siblingBefore);
    }
    CelsSessionDestroy(&s);
    assert(destroys == creates);
}

static const TestCase s_stateLifetimeTests[] = {
    { "TestHighKeys", "64-bit key dispatch and mutation tracking", TestHighKeys },
    { "TestEditsPreserveResources", "Tree edits and reordering preserve resource identity", TestEditsPreserveResources },
    { "TestChildBeforeParent", "Teardown order: child resources destroyed before parent", TestChildBeforeParent },
    { "TestNestedEditsKeepSubscriptions", "Nested tree edits maintain reactive subscriptions", TestNestedEditsKeepSubscriptions }
};

static const TestSuite s_stateLifetimeSuite = {
    .name = "state_lifetime",
    .description = "Reactive state lifetime, resource observer hooks, and edit stability",
    .tests = s_stateLifetimeTests,
    .testCount = sizeof(s_stateLifetimeTests) / sizeof(s_stateLifetimeTests[0])
};

const TestSuite *GetStateLifetimeTestSuite(void) {
    return &s_stateLifetimeSuite;
}
