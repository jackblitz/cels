#include "window.h"
#include <stdio.h>

/* ========================================================================= */
/* 1. Lifecycle Callbacks for Managed Resources                              */
/* ========================================================================= */

/**
 * Called on mount when WindowState is allocated in the slot table.
 *
 * Simulates acquiring native window resources (e.g. window handles).
 *
 * @param self Target WindowState struct. Non-NULL.
 * @param s    Owning CelsSession. Non-NULL.
 */
static void Window_OnCreated(WindowState *self, CelsSession *s)
{
    (void)s;
    self->nativeHandle = (void*)0x12345678;
    printf("  [Lifecycle] Window opened (%p) [%dx%d]\n",
           self->nativeHandle, self->width, self->height);
}

/**
 * Called on unmount when WindowState is pruned from the tree or session destroyed.
 *
 * Releases acquired native window resources.
 *
 * @param self Target WindowState struct. Non-NULL.
 * @param s    Owning CelsSession. Non-NULL.
 */
static void Window_OnDestroyed(WindowState *self, CelsSession *s)
{
    (void)s;
    printf("  [Lifecycle] Window closed (%p)\n", self->nativeHandle);
    self->nativeHandle = NULL;
}

/* ========================================================================= */
/* 2. Reusable UI Composables                                                */
/* ========================================================================= */

/**
 * Leaf composable rendering a status badge when the window is visible.
 *
 * Automatically keyed by callsite location. Takes no caller parameters.
 */
CEL_Composable(CEL_StatusBadge, void) {
    printf("    [Badge] Window is active & visible!\n");
}
/**
 * Container composable displaying window metrics and render statistics.
 *
 * Preserves a local render counter across recompositions via cel_remember,
 * and reacts to dimension/open state changes via cel_watch.
 *
 * @param win Pointer to reactive WindowState managed by parent. Non-NULL.
 */
CEL_Composable(CEL_WindowContent, WindowState*, win) {
    // 1. Query system/engine memory module registered by the App
    PlatformModule *platform = CEL_GetModule(PlatformModule);

    // 2. Component-local persistent memory: preserved across recompositions
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    // 3. Reactive subscription: watches the passed-in state, re-runs when mutated
    WindowState state = cel_watch(win);

    printf("  [Content] Local render count: %d | Window: %dx%d (open: %s) | Backend: %s (%d Hz, scale: %.2f)\n",
           *localRenderCount, state.width, state.height, state.isOpen ? "true" : "false",
           platform ? platform->backendName : "Generic",
           platform ? platform->refreshRateHz : 60,
           platform ? platform->dpiScale : 1.0f);

    // Render child badge while window is open
    if (state.isOpen) {
        CEL_StatusBadge();
    }
}

/* ========================================================================= */
/* 3. Root Composition & Lifecycle Controller                                */
/* ========================================================================= */

/**
 * Root composition for the window application tree.
 *
 * Allocates managed WindowState with mount/unmount callbacks on initial pass,
 * and passes the persistent pointer down the composition tree.
 */
CEL_Composition(CEL_Window, key) {
    // 1. Initialize only the fields you want; omitted fields default to 0/NULL/false:
    WindowState init = {
        .isOpen = true,
        .width  = 800,
        .height = 600
    };

    // 2. Type is deduced directly from 'init'
    WindowState *win = cel_lifecycle_state(init, Window_OnCreated, Window_OnDestroyed);

    // 3. Pass the state through the composition tree to child composables:
    CEL_WindowContent(win);
}

/**
 * Lifecycle evaluator observing WindowState.
 *
 * When state.isOpen becomes false, triggers cel_destroy() to flag the
 * composition for pruning, invoking Window_OnDestroyed.
 */
CEL_LifeCycle(WindowLifeCycle, WindowState) {
    if (it != NULL) {
        // Watch the state managed by the composition
        WindowState state = cel_watch(it);
        if (!state.isOpen) {
            printf("  [WindowLifeCycle] Window close requested -> calling cel_destroy()\n");
            cel_destroy();
        }
    }
}

/* ========================================================================= */
/* 4. Public Attachment Interface                                            */
/* ========================================================================= */

/**
 * Attaches CEL_Window and WindowLifeCycle to the session.
 *
 * @param session Target session. Non-NULL.
 */
void Window_Attach(CelsSession *session)
{
    CEL_Attach(session, CEL_Window, WindowLifeCycle);
}

/**
 * Returns a composition reference handle for CEL_Window.
 */
CelsCompositionRef Window_GetComposition(void)
{
    return CEL_COMPOSITION(CEL_Window, WindowLifeCycle);
}

/* ========================================================================= */
/* 5. Composable Tree Visual Inspection                                      */
/* ========================================================================= */

static const char *
Window_GetNodeName(uint64_t key)
{
    if (key == CEL_KEY("CEL_Window")) return "CEL_Window (Root Composition)";
    if (key == CEL_KEY("CEL_WindowContent")) return "CEL_WindowContent (Container)";
    if (key == CEL_KEY("CEL_StatusBadge")) return "CEL_StatusBadge (Leaf Badge)";
    return "Composable";
}

static void Window_PrintNode(const CelsSession *s, uint32_t logicalIdx,
                             const char *prefix, bool isLast)
{
    const CelsSlotGroup *g = CelsGetGroup((CelsSession*)s, logicalIdx);
    const char *name = Window_GetNodeName(g->key);

    printf("  | %s%s[%s] (slots: %u B, descendants: %u)\n",
           prefix,
           isLast ? "\\-- " : "|-- ",
           name,
           g->dataSize,
           g->groupSize);

    char nextPrefix[256];
    snprintf(nextPrefix, sizeof(nextPrefix), "%s%s", prefix, isLast ? "    " : "|   ");

    uint32_t childLogical = logicalIdx + 1;
    uint32_t endLogical = logicalIdx + 1 + g->groupSize;

    while (childLogical < endLogical) {
        const CelsSlotGroup *cg = CelsGetGroup((CelsSession*)s, childLogical);
        uint32_t nextChild = childLogical + 1 + cg->groupSize;
        bool childIsLast = (nextChild >= endLogical);

        Window_PrintNode(s, childLogical, nextPrefix, childIsLast);
        childLogical = nextChild;
    }
}

void Window_PrintTree(const CelsSession *session)
{
    if (session == NULL) {
        printf("  +-- Composable Tree (No active session) ---------------------+\n");
        printf("  +------------------------------------------------------------+\n");
        return;
    }

    const uint32_t totalGroups = CelsGetLogicalGroupCount(session);
    if (totalGroups == 0) {
        printf("  +-- Composable Tree (0 active nodes, 0 arena bytes) ---------+\n");
        printf("  |   (Empty / Subtree unmounted & cleaned up)                 |\n");
        printf("  +------------------------------------------------------------+\n");
        return;
    }

    printf("  +-- Composable Tree (%u active nodes, %u arena bytes) ---------+\n",
           totalGroups, (unsigned int)session->dataGapStart);

    const CelsSlotGroup *root = CelsGetGroup((CelsSession*)session, 0);
    const char *rootName = Window_GetNodeName(root->key);
    printf("  | [%s] (slots: %u B, descendants: %u)\n",
           rootName, root->dataSize, root->groupSize);

    uint32_t childLogical = 1;
    uint32_t endLogical = 1 + root->groupSize;

    while (childLogical < endLogical && childLogical < totalGroups) {
        const CelsSlotGroup *cg = CelsGetGroup((CelsSession*)session, childLogical);
        uint32_t nextChild = childLogical + 1 + cg->groupSize;
        bool isLast = (nextChild >= endLogical);

        Window_PrintNode(session, childLogical, "", isLast);
        childLogical = nextChild;
    }

    printf("  +------------------------------------------------------------+\n");
}
