#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>

/* ========================================================================= */
/* Key Registry & Tree Visualizer                                            */
/* ========================================================================= */

typedef struct KeyRegistryEntry {
    uint64_t key;
    const char *name;
} KeyRegistryEntry;

static KeyRegistryEntry g_keyRegistry[64];
static uint32_t g_registryCount = 0;

static void RegisterKey(uint64_t key, const char *name) {
    for (uint32_t i = 0; i < g_registryCount; ++i) {
        if (g_keyRegistry[i].key == key) return;
    }
    if (g_registryCount < 64) {
        g_keyRegistry[g_registryCount++] = (KeyRegistryEntry){ .key = key, .name = name };
    }
}

#define REGISTER_KEY(str) RegisterKey(CEL_ID(str), str)

static const char* GetKeyName(uint64_t key) {
    for (uint32_t i = 0; i < g_registryCount; ++i) {
        if (g_keyRegistry[i].key == key) return g_keyRegistry[i].name;
    }
    return "Anonymous";
}

static inline uint32_t LogicalToPhys(const CelsSession *s, uint32_t logical) {
    return (logical < s->groupsGapStart)
        ? logical
        : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline const CelsSlotGroup* GetGroup(const CelsSession *s, uint32_t logical) {
    return &s->groups[LogicalToPhys(s, logical)];
}

static inline uint32_t GetActiveCount(const CelsSession *s) {
    return s->maxGroups - (s->groupsGapEnd - s->groupsGapStart);
}

static void PrintNode(const CelsSession *s, uint32_t logicalIdx, const char *prefix, bool isLast) {
    const CelsSlotGroup *g = GetGroup(s, logicalIdx);

    printf("%s%s[%s] (0x%016llX) | descendants: %u | slots: %u B | parent: %u\n",
           prefix,
           isLast ? "\\-- " : "|-- ",
           GetKeyName(g->key),
           (unsigned long long)g->key,
           g->groupSize,
           g->slotCount,
           g->parentIndex);

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

static void CelsDumpTree(const CelsSession *s) {
    uint32_t totalGroups = GetActiveCount(s);
    if (totalGroups == 0) {
        printf("\n=== TREE DUMP (Empty Tree) ===\n\n");
        return;
    }

    const CelsSlotGroup *root = GetGroup(s, 0);

    printf("\n=== COMPOSABLE TREE DUMP (%u active nodes, %u arena bytes) ===\n",
           totalGroups, (unsigned int)s->dataGapStart);

    printf("[%s] (0x%016llX) | descendants: %u | slots: %u B\n",
           GetKeyName(root->key), (unsigned long long)root->key, root->groupSize, root->slotCount);

    uint32_t childLogical = 1;
    uint32_t endLogical = 1 + root->groupSize;

    while (childLogical < endLogical && childLogical < totalGroups) {
        const CelsSlotGroup *cg = GetGroup(s, childLogical);
        uint32_t nextChild = childLogical + 1 + cg->groupSize;
        bool isLast = (nextChild >= endLogical);

        PrintNode(s, childLogical, "", isLast);
        childLogical = nextChild;
    }
    printf("===================================================================\n\n");
}

/* ========================================================================= */
/* Reactive State & Observers                                                */
/* ========================================================================= */

CEL_State(AppState) {
    bool showOptionalSidebar;
    bool showSubMenu;
};

CEL_State(SidebarResource) {
    int resourceHandle;
};

CEL_Lifecycle(SidebarLifecycle, SidebarResource *self) {
    mount {
        self->resourceHandle = 0xABCD;
    }
    unmount {
        self->resourceHandle = 0;
    }
}

/* ========================================================================= */
/* Declarative Tree Composables                                              */
/* ========================================================================= */

CEL_Composable(SettingsItem) {}

CEL_Composable(SubMenu) {
    SettingsItem();
}

CEL_Composable(ProfileWidget) {}

CEL_Composable(Sidebar) {
    SidebarResource *res = cel_remember(SidebarResource, 0);
    cel_lifecycle(SidebarLifecycle, res);
    ProfileWidget();
    const AppState *state = cel_watch(AppState, CEL_ID("AppState"));
    if (state && state->showSubMenu) {
        SubMenu();
    }
}

CEL_Composable(ContentArea) {}

CEL_Composable(MainBody) {
    ContentArea();
    const AppState *state = cel_watch(AppState, CEL_ID("AppState"));
    if (state && state->showOptionalSidebar) {
        Sidebar();
    }
}

CEL_Composable(CloseButton) {}

CEL_Composable(TitleLabel) {
    int *val = cel_remember(int, 42);
    (void)val;
}

CEL_Composable(HeaderBar) {
    TitleLabel();
    CloseButton();
}

CEL_Composition(RootWindow, void *userData) {
    (void)userData;
    HeaderBar();
    MainBody();
}

static void InitKeyRegistry(void) {
    g_registryCount = 0;
    REGISTER_KEY("RootWindow");
    REGISTER_KEY("HeaderBar");
    REGISTER_KEY("TitleLabel");
    REGISTER_KEY("CloseButton");
    REGISTER_KEY("MainBody");
    REGISTER_KEY("ContentArea");
    REGISTER_KEY("Sidebar");
    REGISTER_KEY("ProfileWidget");
    REGISTER_KEY("SubMenu");
    REGISTER_KEY("SettingsItem");
}

static void ResetTreeState(CelsSession *session) {
    InitKeyRegistry();
    CelsSessionRememberState(session, CEL_ID("AppState"), sizeof(AppState), &((AppState){
        .showOptionalSidebar = true,
        .showSubMenu = true
    }));
    cel_mutate(session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = true;
        this->showSubMenu = true;
    }
    cel_attach(session, RootWindow);
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void TestTreeInitialMount(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetTreeState(&session);

    CelsResult res = CelsSessionRecompose(&session);
    assert(res == CELS_OK);
    uint32_t active = GetActiveCount(&session);
    assert(active == 10);

    CelsSessionDestroy(&session);
}

static void TestTreeToggleSubMenu(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetTreeState(&session);

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 10);

    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showSubMenu = false;
    }

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 8);

    CelsSessionDestroy(&session);
}

static void TestTreeToggleOptionalSidebar(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetTreeState(&session);

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 10);

    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = false;
    }

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 6);

    CelsSessionDestroy(&session);
}

static void TestTreeReenableOptionalSidebar(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetTreeState(&session);

    assert(CelsSessionRecompose(&session) == CELS_OK);

    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 6);

    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = true;
        this->showSubMenu = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 8);

    CelsSessionDestroy(&session);
}

static void TestTreeFullPassSequence(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetTreeState(&session);

    /* PASS 1: Initial Mount */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 10);
    CelsDumpTree(&session);

    /* PASS 2: Toggling showSubMenu = false */
    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showSubMenu = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 8);
    CelsDumpTree(&session);

    /* PASS 3: Toggling showOptionalSidebar = false */
    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 6);
    CelsDumpTree(&session);

    /* PASS 4: Re-enabling showOptionalSidebar = true */
    cel_mutate(&session, CEL_ID("AppState"), AppState) {
        this->showOptionalSidebar = true;
        this->showSubMenu = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(GetActiveCount(&session) == 8);
    CelsDumpTree(&session);

    CelsSessionDestroy(&session);
}

static const TestCase s_treeTests[] = {
    { "TestTreeInitialMount", "Initial mount with complete hierarchy", TestTreeInitialMount },
    { "TestTreeToggleSubMenu", "Conditional child branch toggle (showSubMenu)", TestTreeToggleSubMenu },
    { "TestTreeToggleOptionalSidebar", "Conditional branch despawn and observer release", TestTreeToggleOptionalSidebar },
    { "TestTreeReenableOptionalSidebar", "Re-attaching previously pruned conditional branch", TestTreeReenableOptionalSidebar },
    { "TestTreeFullPassSequence", "Complete multi-pass hierarchy manipulation sequence", TestTreeFullPassSequence }
};

static const TestSuite s_treeSuite = {
    .name = "tree",
    .description = "Composable tree hierarchy, conditional branching and pruning",
    .tests = s_treeTests,
    .testCount = sizeof(s_treeTests) / sizeof(s_treeTests[0])
};

const TestSuite *GetTreeTestSuite(void) {
    return &s_treeSuite;
}
