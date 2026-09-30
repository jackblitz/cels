#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

CEL_State(ChildState) {
    int value;
};

static int childRuns;
static int siblingRuns;

CEL_Composable(ChildNode) {
    const ChildState *st = cel_watch(ChildState, CEL_ID("ChildState"));
    (void)st;
    ++childRuns;
}

CEL_Composable(SiblingNode) {
    ++siblingRuns;
}

CEL_Composition(KeyRoot, void *userData) {
    (void)userData;
    ChildNode();
    SiblingNode();
}

static void TestHighKeys(void) {
    childRuns = 0;
    siblingRuns = 0;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("ChildState"), sizeof(ChildState), &((ChildState){ 0 }));
    cel_attach(&s, KeyRoot);

    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 1 && siblingRuns == 1);

    cel_mutate(&s, CEL_ID("ChildState"), ChildState) {
        this->value++;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 2 && siblingRuns == 1);

    /* Quiet recompose with no mutation */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(childRuns == 2 && siblingRuns == 1);

    CelsSessionDestroy(&s);
    assert(CelsGetCurrentSession() == NULL);
}

typedef struct Resource {
    int id;
    unsigned magic;
} Resource;

CEL_State(LayoutState) {
    int mode;
    int childVal;
};

static int creates;
static int destroys;
static int destroyedIds[64];
static Resource *resources[4];
static int *values[4];
static int *beforeChildren;
static int *afterChildren;
static int nodeRuns[4];

CEL_Lifecycle(ResourceLifecycle, Resource *resource) {
    mount {
        resource->magic = 0xabcdef12;
        ++creates;
    }
    unmount {
        assert(resource->magic == 0xabcdef12);
        destroyedIds[destroys++] = resource->id;
        resource->magic = 0;
    }
}

static void Node(int id) {
    CelsSession *sess = CelsGetCurrentSession();
    uint64_t key = CelsKeyIndex(CEL_ID("node"), (uint64_t)id);
    if (CelsEnterComposable(sess, key)) {
        ++nodeRuns[id];
        Resource *resource = cel_remember(Resource, ((Resource){ .id = id, .magic = 0 }));
        resource->id = id;
        cel_lifecycle(ResourceLifecycle, resource);
        int *value = cel_remember(int, id * 100);
        resources[id] = resource;
        values[id] = value;
        uint64_t valKey = CelsKeyIndex(CEL_ID("node-val"), (uint64_t)id);
        const int *st = cel_watch(int, valKey);
        (void)st;
    }
    CelsExitGroup(sess);
}

CEL_Composition(EditRoot, void *userData) {
    (void)userData;
    const LayoutState *st = cel_watch(LayoutState, CEL_ID("LayoutState"));
    int mode = st ? st->mode : 0;
    beforeChildren = cel_remember(int, 123);
    if (mode == 0) { Node(0); Node(1); Node(2); }
    if (mode == 1) { Node(0); Node(3); Node(1); Node(2); }
    if (mode == 2) { Node(2); Node(1); Node(3); Node(0); }
    if (mode == 3) { Node(1); Node(2); }
    afterChildren = cel_remember(int, 456);
}

static void TestEditsPreserveResources(void) {
    creates = 0;
    destroys = 0;
    memset(destroyedIds, 0, sizeof(destroyedIds));
    memset(resources, 0, sizeof(resources));
    memset(values, 0, sizeof(values));
    beforeChildren = NULL;
    afterChildren = NULL;
    memset(nodeRuns, 0, sizeof(nodeRuns));

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("LayoutState"), sizeof(LayoutState), &((LayoutState){ .mode = 0, .childVal = 100 }));
    for (int i = 0; i < 4; ++i) {
        uint64_t valKey = CelsKeyIndex(CEL_ID("node-val"), (uint64_t)i);
        int initV = i * 100;
        CelsSessionRememberState(&s, valKey, sizeof(int), &initV);
    }
    cel_attach(&s, EditRoot);

    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(creates == 3 && destroys == 0);
    Resource *savedResource = resources[1];
    int *savedValue = values[1];
    int *savedBefore = beforeChildren;
    int *savedAfter = afterChildren;

    for (int mode = 1; mode <= 3; ++mode) {
        cel_mutate(&s, CEL_ID("LayoutState"), LayoutState) {
            this->mode = mode;
        }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(resources[1] == savedResource && values[1] == savedValue);
        assert(beforeChildren == savedBefore && afterChildren == savedAfter);
        assert(*savedBefore == 123 && *savedAfter == 456);
        assert(*savedValue == 100 && savedResource->magic == 0xabcdef12);
    }
    assert(creates == 4 && destroys == 2);

    /* Mutate child state */
    uint64_t childKey = CelsKeyIndex(CEL_ID("node-val"), 1);
    cel_mutate(&s, childKey, int) {
        *this = 321;
    }
    *savedValue = 321;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(values[1] == savedValue && *values[1] == 321);
    assert(creates == 4 && destroys == 2);

    CelsSessionDestroy(&s);
    assert(destroys == creates);
}

CEL_State(ShowParent) {
    bool show;
};

CEL_Lifecycle(ParentLifecycle, Resource *parent) {
    mount {
        parent->magic = 0xabcdef12;
        ++creates;
    }
    unmount {
        assert(parent->magic == 0xabcdef12);
        destroyedIds[destroys++] = parent->id;
        parent->magic = 0;
    }
}

CEL_Composition(TeardownRoot, void *userData) {
    (void)userData;
    const ShowParent *sp = cel_watch(ShowParent, CEL_ID("ShowParent"));
    if (!sp || sp->show) {
        CelsSession *sess = CelsGetCurrentSession();
        if (CelsEnterComposable(sess, CEL_ID("parent"))) {
            Node(0);
            /* Parent resource is allocated after child */
            Resource *parent = cel_remember(Resource, ((Resource){ .id = 9, .magic = 0 }));
            parent->id = 9;
            cel_lifecycle(ParentLifecycle, parent);
        }
        CelsExitGroup(sess);
    }
}

static void TestChildBeforeParent(void) {
    creates = 0;
    destroys = 0;
    memset(destroyedIds, 0, sizeof(destroyedIds));

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("ShowParent"), sizeof(ShowParent), &((ShowParent){ .show = true }));
    CelsSessionRememberState(&s, CEL_ID("LayoutState"), sizeof(LayoutState), &((LayoutState){ .mode = 0, .childVal = 0 }));
    cel_attach(&s, TeardownRoot);

    assert(CelsSessionRecompose(&s) == CELS_OK);
    int start = destroys;

    cel_mutate(&s, CEL_ID("ShowParent"), ShowParent) {
        this->show = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(destroys == start + 2);
    /* Child resource (id 0) destroyed before parent resource (id 9) */
    assert(destroyedIds[start] == 0 && destroyedIds[start + 1] == 9);

    cel_mutate(&s, CEL_ID("ShowParent"), ShowParent) {
        this->show = true;
    }
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

CEL_State(NestedLayout) {
    int mode;
};

static int parentRuns[3];
static Resource *parentResources[3];

static void Parent(int id) {
    CelsSession *sess = CelsGetCurrentSession();
    uint64_t key = CelsKeyIndex(CEL_ID("nested-parent"), (uint64_t)id);
    if (CelsEnterComposable(sess, key)) {
        ++parentRuns[id];
        Resource *resource = cel_remember(Resource, ((Resource){ .id = 10 + id, .magic = 0 }));
        resource->id = 10 + id;
        cel_lifecycle(ParentLifecycle, resource);
        parentResources[id] = resource;
        Node(id);
    }
    CelsExitGroup(sess);
}

CEL_Composition(NestedRoot, void *userData) {
    (void)userData;
    const NestedLayout *nl = cel_watch(NestedLayout, CEL_ID("NestedLayout"));
    int mode = nl ? nl->mode : 0;
    if (mode == 0) { Parent(0); Parent(1); }
    if (mode == 1) { Parent(2); Parent(0); Parent(1); }
    if (mode == 2) { Parent(1); Parent(0); }
    if (mode == 3) { Parent(0); Parent(1); }
}

static void TestNestedEditsKeepSubscriptions(void) {
    creates = 0;
    destroys = 0;
    memset(parentRuns, 0, sizeof(parentRuns));
    memset(parentResources, 0, sizeof(parentResources));
    memset(nodeRuns, 0, sizeof(nodeRuns));
    memset(resources, 0, sizeof(resources));
    memset(values, 0, sizeof(values));

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("NestedLayout"), sizeof(NestedLayout), &((NestedLayout){ .mode = 0 }));
    for (int i = 0; i < 3; ++i) {
        uint64_t valKey = CelsKeyIndex(CEL_ID("node-val"), (uint64_t)i);
        int initV = i * 100;
        CelsSessionRememberState(&s, valKey, sizeof(int), &initV);
    }
    cel_attach(&s, NestedRoot);

    assert(CelsSessionRecompose(&s) == CELS_OK);
    Resource *parent = parentResources[1];
    Resource *child = resources[1];
    int *value = values[1];

    for (int mode = 1; mode <= 3; ++mode) {
        cel_mutate(&s, CEL_ID("NestedLayout"), NestedLayout) {
            this->mode = mode;
        }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(parentResources[1] == parent && resources[1] == child && values[1] == value);
        int parentBefore = parentRuns[1], childBefore = nodeRuns[1];
        int siblingBefore = parentRuns[0];

        uint64_t childKey = CelsKeyIndex(CEL_ID("node-val"), 1);
        cel_mutate(&s, childKey, int) {
            (*this)++;
        }
        assert(CelsSessionRecompose(&s) == CELS_OK);
        assert(parentRuns[1] == parentBefore + 1 && nodeRuns[1] == childBefore + 1);
        assert(parentRuns[0] == siblingBefore);
    }
    CelsSessionDestroy(&s);
    assert(destroys == creates);
}

/* ========================================================================= */
/* State Hoisting Tests (Pointer Instance Identity)                          */
/* ========================================================================= */

CEL_State(HoistedWindowState) {
    int width;
    int height;
    bool isOpen;
};

static int s_hoistedChildRuns = 0;
static int s_hoistedChildLastWidth = 0;

CEL_Composable(HoistedWindowChild, HoistedWindowState*, win) {
    cel_watch(win);
    s_hoistedChildRuns++;
    s_hoistedChildLastWidth = win->width;
}

static HoistedWindowState *s_capturedWinInstance = NULL;

CEL_Composition(HoistedRootComp, void *userData) {
    (void)userData;
    HoistedWindowState *win = cel_state(HoistedWindowState, ((HoistedWindowState){
        .width = 800,
        .height = 600,
        .isOpen = true
    }));
    s_capturedWinInstance = win;
    HoistedWindowChild(win);
}

static void TestStateHoistingBasic(void) {
    s_hoistedChildRuns = 0;
    s_hoistedChildLastWidth = 0;
    s_capturedWinInstance = NULL;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    cel_attach(&s, HoistedRootComp);

    /* Frame 1: Initial mount */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_hoistedChildRuns == 1);
    assert(s_hoistedChildLastWidth == 800);
    assert(s_capturedWinInstance != NULL);

    /* Frame 2: Mutate instance directly without string ID */
    cel_mutate(s_capturedWinInstance) {
        this->width = 1024;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_hoistedChildRuns == 2);
    assert(s_hoistedChildLastWidth == 1024);

    /* Frame 3: Quiet recompose */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_hoistedChildRuns == 2);

    CelsSessionDestroy(&s);
}

static int s_instRuns1 = 0;
static int s_instRuns2 = 0;
static HoistedWindowState *s_capturedWin1 = NULL;
static HoistedWindowState *s_capturedWin2 = NULL;

CEL_Composable(HoistedConsumer1, HoistedWindowState*, win) {
    cel_watch(win);
    s_instRuns1++;
}

CEL_Composable(HoistedConsumer2, HoistedWindowState*, win) {
    cel_watch(win);
    s_instRuns2++;
}

CEL_Composition(HoistedMultiRoot, void *userData) {
    (void)userData;
    HoistedWindowState *win1 = cel_state(HoistedWindowState, ((HoistedWindowState){ .width = 800, .height = 600, .isOpen = true }));
    HoistedWindowState *win2 = cel_state(HoistedWindowState, ((HoistedWindowState){ .width = 1920, .height = 1080, .isOpen = true }));
    s_capturedWin1 = win1;
    s_capturedWin2 = win2;

    HoistedConsumer1(win1);
    HoistedConsumer2(win2);
}

static void TestStateHoistingMultipleInstances(void) {
    s_instRuns1 = 0;
    s_instRuns2 = 0;
    s_capturedWin1 = NULL;
    s_capturedWin2 = NULL;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    cel_attach(&s, HoistedMultiRoot);

    /* Frame 1: Mount both instances */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_instRuns1 == 1 && s_instRuns2 == 1);
    assert(s_capturedWin1 != s_capturedWin2);
    assert(s_capturedWin1->width == 800);
    assert(s_capturedWin2->width == 1920);

    /* Mutate only instance 1 */
    cel_mutate(s_capturedWin1) {
        this->width = 1280;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_instRuns1 == 2);
    assert(s_instRuns2 == 1); /* Instance 2 was NOT invalidated */

    /* Mutate only instance 2 */
    cel_mutate(s_capturedWin2) {
        this->width = 2560;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_instRuns1 == 2); /* Instance 1 was NOT invalidated */
    assert(s_instRuns2 == 2);

    CelsSessionDestroy(&s);
}

CEL_State(TeardownToggleState) {
    bool enable;
};

static TeardownToggleState *s_capturedToggle = NULL;
static int s_teardownChildRuns = 0;
static HoistedWindowState *s_capturedTeardownWin = NULL;

CEL_Composable(HoistedTeardownChild) {
    HoistedWindowState *win = cel_state(HoistedWindowState, ((HoistedWindowState){ .width = 640 }));
    s_capturedTeardownWin = win;
    s_teardownChildRuns++;
}

CEL_Composition(HoistedTeardownRoot, void *userData) {
    (void)userData;
    TeardownToggleState *toggle = cel_state(TeardownToggleState, ((TeardownToggleState){ .enable = true }));
    cel_watch(toggle);
    s_capturedToggle = toggle;

    if (toggle->enable) {
        HoistedTeardownChild();
    }
}

static void TestStateHoistingAutomaticTeardown(void) {
    s_capturedToggle = NULL;
    s_teardownChildRuns = 0;
    s_capturedTeardownWin = NULL;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    cel_attach(&s, HoistedTeardownRoot);

    /* Frame 1: Child is active */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_teardownChildRuns == 1);
    assert(s_capturedTeardownWin != NULL);
    assert(s.stateRegistry.cellCount > 0);

    CelsStateHeader *hdr = CelsGetStateHeader(s_capturedTeardownWin);
    assert(hdr != NULL);
    CelsStateCell *cell = CelsStateRegistryFindCell(&s.stateRegistry, hdr->id);
    assert(cell != NULL && cell->inUse == true);

    /* Frame 2: Mutate toggle -> invalidates root, skips child, triggers pruning */
    cel_mutate(s_capturedToggle) {
        this->enable = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_teardownChildRuns == 1);

    /* Slot cleanup automatically deactivated the child state cell! */
    assert(cell->inUse == false);

    CelsSessionDestroy(&s);
}

static const TestCase s_stateLifetimeTests[] = {
    { "TestHighKeys", "64-bit key dispatch and mutation tracking", TestHighKeys },
    { "TestEditsPreserveResources", "Tree edits and reordering preserve resource identity", TestEditsPreserveResources },
    { "TestChildBeforeParent", "Teardown order: child resources destroyed before parent", TestChildBeforeParent },
    { "TestNestedEditsKeepSubscriptions", "Nested tree edits maintain reactive subscriptions", TestNestedEditsKeepSubscriptions },
    { "TestStateHoistingBasic", "State hoisting instance allocation, cel_watch(ptr), and cel_mutate(ptr)", TestStateHoistingBasic },
    { "TestStateHoistingMultipleInstances", "Multiple distinct instances of the same struct without string keys", TestStateHoistingMultipleInstances },
    { "TestStateHoistingAutomaticTeardown", "Automatic state cell deactivation when composable unmounts", TestStateHoistingAutomaticTeardown }
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
