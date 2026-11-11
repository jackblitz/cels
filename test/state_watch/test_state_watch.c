/** @file test_state_watch.c
 * @brief State watch ownership, failure and teardown checks active in Release.
 */
#include "cels/cels.h"
#include "cli/test_cli.h"
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#ifdef cel_watch
#error "The retired cel_watch spelling must not be exposed"
#endif

typedef struct WatchSettings { int value; } WatchSettings;
static WatchSettings *s_state;
static const WatchSettings *s_foreignState;
static CelsSession *s_foreignSession;
static int s_pointerEvaluations;
static int s_compositions;

static const WatchSettings *StateArgument(void) {
    ++s_pointerEvaluations;
    return s_state;
}

CEL_Composition(WatchRoot, void *userData) {
    (void)userData;
    ++s_compositions;
    s_state = cel_state(WatchSettings, ((WatchSettings){ .value = 7 }));
    CHECK(s_state != NULL);
    CHECK(Cel.State.Watch(StateArgument()) == s_state);
    CHECK(Cel.State.Watch(s_state) == s_state);

    /* Rejected pointers must not be dereferenced or gain subscriptions. */
    const WatchSettings local = { .value = 7 };
    CHECK(Cel.State.Watch(&local) == NULL);
    CHECK(Cel.State.Watch(NULL) == NULL);
    CHECK(Cel.State.Watch(s_foreignState) == NULL);
    CHECK(CelsWatchStateInstance(s_foreignSession, s_foreignState) == NULL);
    CHECK(Cel.State.Watch((const unsigned char *)s_state + 1) == NULL);

    CelsSession *session = CelsGetCurrentSession();
    CelsStateCell *cell = NULL;
    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        if (session->stateRegistry.cells[i].frontBuffer == s_state) {
            cell = &session->stateRegistry.cells[i];
            break;
        }
    }
    CHECK(cell != NULL && cell->watcherCount == 1);
    CHECK(Cel.State.Watch(cell->backBuffer) == NULL);

    /* Explicitly inject exhausted capacity, then verify no fabricated success. */
    uint64_t savedKeys[CELS_MAX_WATCHERS];
    memcpy(savedKeys, cell->watcherKeys, sizeof(savedKeys));
    const uint16_t savedCount = cell->watcherCount;
    memset(cell->watcherKeys, 0, sizeof(cell->watcherKeys));
    cell->watcherCount = CELS_MAX_WATCHERS;
    CHECK(Cel.State.Watch(s_state) == NULL);
    CHECK(cell->watcherCount == CELS_MAX_WATCHERS);
    memcpy(cell->watcherKeys, savedKeys, sizeof(savedKeys));
    cell->watcherCount = savedCount;
    CHECK(Cel.State.Watch(s_state) == s_state);

    /* Retired storage is rejected by registry membership, not header probing. */
    cell->inUse = false;
    CHECK(Cel.State.Watch(s_state) == NULL);
    cell->inUse = true;
}

static void TestCheckedStateWatch(void) {
    CelsSession *session = calloc(1, sizeof(*session));
    s_foreignSession = calloc(1, sizeof(*s_foreignSession));
    CHECK(session != NULL && s_foreignSession != NULL);
    CelsSessionInit(s_foreignSession, NULL);
    s_foreignState = CelsSessionRememberState(s_foreignSession, CEL_ID("foreign"),
        sizeof(WatchSettings), &((WatchSettings){ .value = 100 }));
    CHECK(s_foreignState != NULL);
    CelsSessionInit(session, NULL);
    s_pointerEvaluations = 0;
    s_compositions = 0;
    cel_attach(session, WatchRoot);
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    CHECK(s_pointerEvaluations == 1 && s_compositions == 1);
    CHECK(s_state->value == 7);
    CHECK(CelsWatchStateInstance(session, s_state) == NULL);

    cel_mutate(s_state) {
        this->value = 9;
    }
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    CHECK(s_state->value == 9 && s_compositions == 2 && s_pointerEvaluations == 2);
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    CHECK(s_compositions == 2);

    CelsSessionDestroy(session);
    CelsSessionDestroy(s_foreignSession);
    free(session);
    free(s_foreignSession);
    s_state = NULL;
    s_foreignState = NULL;
    s_foreignSession = NULL;
}

static bool s_childMounted;
static bool s_checkRetired;
static const WatchSettings *s_retiredState;

CEL_Composable(RetiringChild) {
    s_retiredState = cel_state(WatchSettings, ((WatchSettings){ .value = 1 }));
    CHECK(Cel.State.Watch(s_retiredState) == s_retiredState);
}

CEL_Composition(RetiringRoot, void *userData) {
    (void)userData;
    if (s_childMounted) {
        RetiringChild();
    } else if (s_checkRetired) {
        CHECK(s_retiredState != NULL && Cel.State.Watch(s_retiredState) == NULL);
    }
}

/** @brief Checks a real subtree unmount retires subscriptions before later watches. */
static void TestWatchAfterUnmount(void) {
    CelsSession *session = calloc(1, sizeof(*session));
    CHECK(session != NULL);
    CelsSessionInit(session, NULL);
    s_childMounted = true;
    s_checkRetired = false;
    s_retiredState = NULL;
    cel_attach(session, RetiringRoot);
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    CHECK(s_retiredState != NULL);

    s_childMounted = false;
    CelsSessionInvalidateKey(session, CEL_ID("RetiringRoot"));
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    s_checkRetired = true;
    CelsSessionInvalidateKey(session, CEL_ID("RetiringRoot"));
    CHECK(CelsSessionRecompose(session) == CELS_OK);
    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        if (session->stateRegistry.cells[i].frontBuffer == s_retiredState) {
            CHECK(!session->stateRegistry.cells[i].inUse);
            CHECK(session->stateRegistry.cells[i].watcherCount == 0);
        }
    }
    CelsSessionDestroy(session);
    free(session);
    s_retiredState = NULL;
}

int main(void) {
    CHECK(Cel.abiVersion == 1u && Cel.structSize == sizeof(CelsApi));
    CHECK(CelsApiV1() == CelsApiV1());
    CHECK(strcmp(CelsGetVersionString(), "0.5.0") == 0);
    TestCheckedStateWatch();
    TestWatchAfterUnmount();
    puts("Checked pointer state watching passed.");
    /* Reuse the existing hoisting, subtree teardown and reordering regressions. */
    return TestCliRunSuite(GetStateLifetimeTestSuite(), 0, NULL);
}
