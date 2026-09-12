#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* ========================================================================= */
/* Key Registry & Tree Visualizer                                            */
/* ========================================================================= */

typedef struct KeyRegistryEntry {
    uint32_t key;
    const char *name;
} KeyRegistryEntry;

static KeyRegistryEntry g_keyRegistry[64];
static uint32_t g_registryCount = 0;

static void RegisterKey(uint32_t key, const char *name) {
    for (uint32_t i = 0; i < g_registryCount; ++i) {
        if (g_keyRegistry[i].key == key) return;
    }
    if (g_registryCount < 64) {
        g_keyRegistry[g_registryCount++] = (KeyRegistryEntry){ .key = key, .name = name };
    }
}

#define REGISTER_KEY(str) RegisterKey(CEL_KEY(str), str)

static const char* GetKeyName(uint32_t key) {
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
    return CELS_MAX_GROUPS - (s->groupsGapEnd - s->groupsGapStart);
}

static void PrintNode(const CelsSession *s, uint32_t logicalIdx, const char *prefix, bool isLast) {
    const CelsSlotGroup *g = GetGroup(s, logicalIdx);

    printf("%s%s[%s] (0x%08llX) | descendants: %u | slots: %u B | parent: %u\n",
           prefix,
           isLast ? "└── " : "├── ",
           GetKeyName((uint32_t)g->key),
           (unsigned long long)g->key,
           g->groupSize,
           g->dataSize,
           g->parentIndex);

    char nextPrefix[256];
    snprintf(nextPrefix, sizeof(nextPrefix), "%s%s", prefix, isLast ? "    " : "│   ");

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

void CelsDumpTree(const CelsSession *s) {
    uint32_t totalGroups = GetActiveCount(s);
    if (totalGroups == 0) {
        printf("\n=== TREE DUMP (Empty Tree) ===\n\n");
        return;
    }

    const CelsSlotGroup *root = GetGroup(s, 0);

    printf("\n=== COMPOSABLE TREE DUMP (%u active nodes, %u arena bytes) ===\n",
           totalGroups, s->dataGapStart);

    printf("[%s] (0x%08llX) | descendants: %u | slots: %u B\n",
           GetKeyName((uint32_t)root->key), (unsigned long long)root->key, root->groupSize, root->dataSize);

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

static AppState g_appState = {
    .showOptionalSidebar = true,
    .showSubMenu = true
};

CEL_Observer(SidebarResource) {
    int resourceHandle;
};

void SidebarResource_OnRemembered(SidebarResource *self, CelsSession *s) {
    (void)s;
    self->resourceHandle = 0xABCD;
    printf("  [RememberObserver] Sidebar allocated handle: 0x%X\n", self->resourceHandle);
}

void SidebarResource_OnForgotten(SidebarResource *self, CelsSession *s) {
    (void)s;
    printf("  [RememberObserver] Sidebar released handle: 0x%X\n", self->resourceHandle);
    self->resourceHandle = 0;
}

/* ========================================================================= */
/* Declarative Root Function                                                 */
/* ========================================================================= */

void RootApp(CelsSession *s) {
    AppState state = cel_watch(s, &g_appState);

    CEL_Composition(s, CEL_KEY("RootWindow")) {
        CEL_Composable(s, CEL_KEY("HeaderBar")) {
            CEL_Composable(s, CEL_KEY("TitleLabel")) {
                cel_remember(s, int, 42);
            } CEL_Close(s);

            CEL_Composable(s, CEL_KEY("CloseButton")) {
            } CEL_Close(s);
        } CEL_Close(s);

        CEL_Composable(s, CEL_KEY("MainBody")) {
            CEL_Composable(s, CEL_KEY("ContentArea")) {
            } CEL_Close(s);

            if (state.showOptionalSidebar) {
                CEL_Composable(s, CEL_KEY("Sidebar")) {
                    cel_remember_observer(s, SidebarResource, SidebarResource_OnRemembered, SidebarResource_OnForgotten);

                    CEL_Composable(s, CEL_KEY("ProfileWidget")) {
                    } CEL_Close(s);

                    if (state.showSubMenu) {
                        CEL_Composable(s, CEL_KEY("SubMenu")) {
                            CEL_Composable(s, CEL_KEY("SettingsItem")) {
                            } CEL_Close(s);
                        } CEL_Close(s);
                    }
                } CEL_Close(s);
            }
        } CEL_Close(s);
    } CEL_Close(s);
}

/* ========================================================================= */
/* Main                                                                      */
/* ========================================================================= */

int main(void) {
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

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = RootApp
    });

    printf("PASS 1: Initial Mount (All branches active)\n");
    CelsSessionRecompose(&session);
    CelsDumpTree(&session);

    printf("PASS 2: Toggling showSubMenu = false via cel_mutate (3-arg direct mutation)\n");
    cel_mutate(&session, &g_appState) {
        this->showSubMenu = false;
    }
    CelsSessionRecompose(&session);
    CelsDumpTree(&session);

    printf("PASS 3: Toggling showOptionalSidebar = false via cel_mutate (4-arg alias)\n");
    cel_mutate(&session, &g_appState) {
        this->showOptionalSidebar = false;
    }
    CelsSessionRecompose(&session);
    CelsDumpTree(&session);

    printf("PASS 4: Re-enabling showOptionalSidebar = true\n");
    cel_mutate(&session, &g_appState) {
        this->showOptionalSidebar = true;
        this->showSubMenu = false;
    }
    CelsSessionRecompose(&session);
    CelsDumpTree(&session);

    CelsSessionDestroy(&session);
    return 0;
}