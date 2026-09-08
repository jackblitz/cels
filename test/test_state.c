#include "cels.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#define ALIGNED_SLAB(size, name) __declspec(align(64)) uint8_t name[size]
#else
#define ALIGNED_SLAB(size, name) __attribute__((aligned(64))) uint8_t name[size]
#endif

/** Failures seen so far; main returns non-zero when this is not zero. */
static int g_failureCount = 0;

/*
 * Reports and aborts the current test function rather than the process, so one
 * broken expectation does not hide the results of every later suite.
 */
#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__);      \
            fflush(stdout);                                                    \
            g_failureCount++;                                                  \
            return;                                                            \
        }                                                                      \
    } while (0)

#define KEY_WATCHER_BASE 0x5100u

/** A cell payload larger than one register, so memcmp is the compare path. */
typedef struct TestPlayerData {
    int32_t hp;
    int32_t mana;
    int32_t gold;
} TestPlayerData;

CEL_Mutable(TestScore) {
    int32_t score;
};

/**
 * Recovers a cell's header from the pointer CelsMutableStateCreate returned.
 *
 * state.h specifies the layout: the CelsMutableCell header sits immediately
 * before the value, and accessors recover it by negative offset. The tests use
 * the same route to inspect watcherCount and overflowed, which are otherwise
 * unobservable from outside the module. Every caller checks the sentinel first,
 * so a layout change fails an assertion instead of reading garbage.
 *
 * @param value Pointer returned by CelsMutableStateCreate. Non-NULL.
 * @return The owning header, read-only.
 */
static const CelsMutableCell *
CellHeaderOf(const void *value)
{
    return (const CelsMutableCell *)(const void *)
        ((const uint8_t *)value - sizeof(CelsMutableCell));
}

/**
 * Prepares a composition host backed by a caller-owned slab.
 *
 * Only the slot table and the dirty/queue bookkeeping matter here, so the host
 * is zeroed and its table initialised directly rather than going through the
 * session machinery, which would drag in a Flecs world this suite does not
 * need.
 *
 * @param host     Host to initialise. Non-NULL.
 * @param slab     64-byte aligned slab memory. Non-NULL.
 * @param slabSize Byte size of slab.
 * @return true if the slot table initialised.
 */
static bool
TestHostInit(CelsCompositionHost *host, void *slab, size_t slabSize)
{
    memset(host, 0, sizeof(*host));
    host->slabMemory = (uint8_t *)slab;
    return CelsSlotTableInit(&host->slotTable, slab, slabSize, 32) == CELS_OK;
}

/**
 * Opens a composition walk over a host: binds the composer, publishes it as
 * ambient, and publishes the host so reads can subscribe against it.
 *
 * @param cmp  Composer to drive. Non-NULL.
 * @param host Host being composed. Non-NULL.
 */
static void
TestCompositionEnter(CelsComposer *cmp, CelsCompositionHost *host)
{
    memset(cmp, 0, sizeof(*cmp));
    CelsComposerBegin(cmp, &host->slotTable);
    CelsComposerSetCurrent(cmp);
    CelsInvalidationContextSet(host);
}

/** Closes the walk opened by TestCompositionEnter. */
static void
TestCompositionLeave(void)
{
    CelsComposerSetCurrent(NULL);
    CelsInvalidationContextSet(NULL);
}

/**
 * Clears a host's recorded invalidation state so the next update can be
 * observed in isolation.
 *
 * @param host Host to clear. Non-NULL.
 */
static void
TestHostClearInvalidation(CelsCompositionHost *host)
{
    host->isDirty = false;
    host->invalidationCount = 0;
}

/**
 * Reports whether a host's invalidation queue names a composable.
 *
 * @param host       Host to inspect. Non-NULL.
 * @param composable Composable id to look for.
 * @return true if the queue holds that id.
 */
static bool
TestHostQueueContains(const CelsCompositionHost *host,
                      CelsComposableId composable)
{
    for (uint32_t i = 0; i < host->invalidationCount; i++) {
        if (host->invalidationQueue[i] == composable) {
            return true;
        }
    }
    return false;
}

static void
TestCreateAndReadRoundTrip(void)
{
    printf("Running TestCreateAndReadRoundTrip...\n");
    CelsMutableStateResetPool();

    const TestPlayerData initialA = { .hp = 100, .mana = 50, .gold = 7 };
    const TestScore initialB = { .score = 42 };

    void *cellA = NULL;
    void *cellB = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initialA, sizeof(initialA), &cellA)
                == CELS_OK);
    TEST_ASSERT(CelsMutableStateCreate(&initialB, sizeof(initialB), &cellB)
                == CELS_OK);
    TEST_ASSERT(cellA != NULL);
    TEST_ASSERT(cellB != NULL);
    TEST_ASSERT(cellA != cellB);

    TEST_ASSERT(CellHeaderOf(cellA)->sentinel == CELS_MUTABLE_CELL_SENTINEL);
    TEST_ASSERT(CellHeaderOf(cellA)->valueSize == sizeof(initialA));
    TEST_ASSERT(CellHeaderOf(cellA)->watcherCount == 0);

    TestPlayerData readA;
    memset(&readA, 0, sizeof(readA));
    TEST_ASSERT(CelsMutableStateRead(cellA, &readA, sizeof(readA)) == CELS_OK);
    TEST_ASSERT(readA.hp == 100 && readA.mana == 50 && readA.gold == 7);

    TestScore readB;
    memset(&readB, 0, sizeof(readB));
    TEST_ASSERT(CelsMutableStateRead(cellB, &readB, sizeof(readB)) == CELS_OK);
    TEST_ASSERT(readB.score == 42);

    // Independent cells must not alias: writing one leaves the other alone.
    const TestPlayerData nextA = { .hp = 1, .mana = 2, .gold = 3 };
    TEST_ASSERT(CelsMutableStateUpdate(cellA, &nextA, sizeof(nextA))
                == CELS_OK);

    memset(&readA, 0, sizeof(readA));
    memset(&readB, 0, sizeof(readB));
    TEST_ASSERT(CelsMutableStateRead(cellA, &readA, sizeof(readA)) == CELS_OK);
    TEST_ASSERT(CelsMutableStateRead(cellB, &readB, sizeof(readB)) == CELS_OK);
    TEST_ASSERT(readA.hp == 1 && readA.mana == 2 && readA.gold == 3);
    TEST_ASSERT(readB.score == 42);

    // A size that disagrees with the cell is a caller error, not a cell error.
    TEST_ASSERT(CelsMutableStateRead(cellB, &readA, sizeof(readA))
                == CELS_ERROR_INVALID_ARGUMENT);

    printf("  PASSED: TestCreateAndReadRoundTrip\n");
}

static void
TestDslRoundTrip(void)
{
    printf("Running TestDslRoundTrip...\n");
    CelsMutableStateResetPool();

    const TestScore initial = { .score = 3 };
    TestScore *const score = CEL_MutableState(TestScore, initial);
    TEST_ASSERT(score != NULL);

    const TestScore observed = CEL_Watch(score);
    TEST_ASSERT(observed.score == 3);

    TestScore next = CEL_Watch(score);
    next.score = 11;
    cel_update(score, next);

    const TestScore updated = CEL_Watch(score);
    TEST_ASSERT(updated.score == 11);

    printf("  PASSED: TestDslRoundTrip\n");
}

static void
TestPoolExhaustionAndReset(void)
{
    printf("Running TestPoolExhaustionAndReset...\n");
    CelsMutableStateResetPool();

    const TestScore initial = { .score = 0 };

    // Size 0 and oversize are rejected before a slot is ever taken.
    void *rejected = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, 0, &rejected)
                == CELS_ERROR_INVALID_ARGUMENT);
    TEST_ASSERT(CelsMutableStateCreate(&initial,
                                       CELS_MUTABLE_VALUE_CAPACITY + 1u,
                                       &rejected)
                == CELS_ERROR_INVALID_ARGUMENT);
    TEST_ASSERT(rejected == NULL);

    for (uint32_t i = 0; i < CELS_MAX_MUTABLE_CELLS; i++) {
        void *cell = NULL;
        const CelsResult result =
            CelsMutableStateCreate(&initial, sizeof(initial), &cell);
        TEST_ASSERT(result == CELS_OK);
        TEST_ASSERT(cell != NULL);
    }

    void *overflowCell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial),
                                       &overflowCell)
                == CELS_ERROR_CAPACITY_EXCEEDED);
    TEST_ASSERT(overflowCell == NULL);

    CelsMutableStateResetPool();

    void *recovered = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &recovered)
                == CELS_OK);
    TEST_ASSERT(recovered != NULL);

    printf("  PASSED: TestPoolExhaustionAndReset\n");
}

static void
TestForeignPointerIsRejected(void)
{
    printf("Running TestForeignPointerIsRejected...\n");
    CelsMutableStateResetPool();

    // A plain stack variable never came from CelsMutableStateCreate. The
    // implementation rejects it on provenance — the address is outside the
    // static pool — before it reads anything, so the error return is what this
    // asserts on and a debug build does not abort here. A pointer that DOES
    // land inside the pool but carries a broken sentinel is a genuine
    // corruption and still asserts; that path is deliberately not exercised,
    // because an aborting assertion cannot be observed from inside the suite.
    TestScore stackValue = { .score = 1234 };
    TestScore out;
    memset(&out, 0, sizeof(out));

    TEST_ASSERT(CelsMutableStateRead(&stackValue, &out, sizeof(stackValue))
                == CELS_ERROR_INVALID_STATE);
    TEST_ASSERT(CelsMutableStateReadOrZero(&stackValue, sizeof(stackValue))
                == CELS_ERROR_INVALID_STATE);

    const TestScore attempted = { .score = 777 };
    TEST_ASSERT(CelsMutableStateUpdate(&stackValue, &attempted,
                                       sizeof(attempted))
                == CELS_ERROR_INVALID_STATE);

    // Nothing was written through the bogus pointer.
    TEST_ASSERT(stackValue.score == 1234);
    TEST_ASSERT(out.score == 0);

    // A NULL cell pointer is deliberately NOT exercised here: it is a plain
    // contract violation, so CELS_ASSERT fires before the error return can be
    // observed. The suite asserts only on outcomes it can survive.

    // An interior pointer into a real cell is inside the pool but at the wrong
    // offset, so it is rejected on provenance too.
    const TestPlayerData initial = { .hp = 5, .mana = 6, .gold = 7 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    void *const interior = (void *)((uint8_t *)cell + 4);
    TEST_ASSERT(CelsMutableStateReadOrZero(interior, sizeof(initial))
                == CELS_ERROR_INVALID_STATE);

    printf("  PASSED: TestForeignPointerIsRejected\n");
}

static void
TestReadOutsideCompositionSubscribesNothing(void)
{
    printf("Running TestReadOutsideCompositionSubscribesNothing...\n");
    CelsMutableStateResetPool();

    ALIGNED_SLAB(4096, slab);
    CelsCompositionHost host;
    TEST_ASSERT(TestHostInit(&host, slab, sizeof(slab)));

    const TestScore initial = { .score = 1 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    TestScore out;
    memset(&out, 0, sizeof(out));

    // No host, no composer: a plain read, as from a network handler.
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 0);

    // A published host but no active composable is still a plain read.
    CelsInvalidationContextSet(&host);
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 0);
    CelsInvalidationContextSet(NULL);

    // An active composable but no published host also subscribes nothing:
    // there would be no queue to append an invalidation to.
    CelsComposer cmp;
    memset(&cmp, 0, sizeof(cmp));
    CelsComposerBegin(&cmp, &host.slotTable);
    CelsComposerSetCurrent(&cmp);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 0);
    CelsComposerGroupEnd(&cmp);
    CelsComposerSetCurrent(NULL);

    // And the update reaches nobody, so the host stays clean.
    TestHostClearInvalidation(&host);
    const TestScore next = { .score = 2 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &next, sizeof(next)) == CELS_OK);
    TEST_ASSERT(!host.isDirty);

    printf("  PASSED: TestReadOutsideCompositionSubscribesNothing\n");
}

static void
TestSubscriptionIsIdempotent(void)
{
    printf("Running TestSubscriptionIsIdempotent...\n");
    CelsMutableStateResetPool();

    ALIGNED_SLAB(4096, slab);
    CelsCompositionHost host;
    TEST_ASSERT(TestHostInit(&host, slab, sizeof(slab)));

    const TestScore initial = { .score = 1 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    CelsComposer cmp;
    TestCompositionEnter(&cmp, &host);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_WATCHER_BASE));

    TestScore out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 1);

    // Reading the same cell again in the same body registers nothing new.
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CelsMutableStateReadOrZero(cell, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 1);

    CelsComposerGroupEnd(&cmp);

    // A different composable on the same host is a different watcher.
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_WATCHER_BASE + 1u));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 2);
    CelsComposerGroupEnd(&cmp);

    TestCompositionLeave();
    TEST_ASSERT(!CellHeaderOf(cell)->overflowed);

    printf("  PASSED: TestSubscriptionIsIdempotent\n");
}

static void
TestUpdateQueuesOnlyOnRealChange(void)
{
    printf("Running TestUpdateQueuesOnlyOnRealChange...\n");
    CelsMutableStateResetPool();

    ALIGNED_SLAB(4096, slabA);
    ALIGNED_SLAB(4096, slabB);
    CelsCompositionHost hostA;
    CelsCompositionHost hostB;
    TEST_ASSERT(TestHostInit(&hostA, slabA, sizeof(slabA)));
    TEST_ASSERT(TestHostInit(&hostB, slabB, sizeof(slabB)));

    const TestPlayerData initial = { .hp = 100, .mana = 50, .gold = 0 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    TestPlayerData out;
    memset(&out, 0, sizeof(out));

    // Two composables on host A, one on host B — cells are session-agnostic.
    CelsComposer cmpA;
    TestCompositionEnter(&cmpA, &hostA);
    TEST_ASSERT(CelsComposerGroupStart(&cmpA, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpA);
    TEST_ASSERT(CelsComposerGroupStart(&cmpA, KEY_WATCHER_BASE + 1u));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpA);
    TestCompositionLeave();

    CelsComposer cmpB;
    TestCompositionEnter(&cmpB, &hostB);
    TEST_ASSERT(CelsComposerGroupStart(&cmpB, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpB);
    TestCompositionLeave();

    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 3);

    // An update that changes nothing queues nothing. This is what stops an
    // update storm from a writer that runs every tick.
    TestHostClearInvalidation(&hostA);
    TestHostClearInvalidation(&hostB);
    const TestPlayerData same = { .hp = 100, .mana = 50, .gold = 0 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &same, sizeof(same)) == CELS_OK);
    TEST_ASSERT(!hostA.isDirty);
    TEST_ASSERT(!hostB.isDirty);
    TEST_ASSERT(hostA.invalidationCount == 0);
    TEST_ASSERT(hostB.invalidationCount == 0);

    // A real change reaches every subscriber, on every host.
    const TestPlayerData changed = { .hp = 100, .mana = 49, .gold = 0 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &changed, sizeof(changed))
                == CELS_OK);
    TEST_ASSERT(hostA.isDirty);
    TEST_ASSERT(hostB.isDirty);

    // The per-composable queue is CelsCompositionHostInvalidate's own
    // contract; while that is still a skeleton the queue stays empty, so this
    // only asserts the ids once entries actually appear.
    TEST_ASSERT(hostA.invalidationCount == 0
                || (TestHostQueueContains(&hostA, 0)
                    && TestHostQueueContains(&hostA, 1)));
    TEST_ASSERT(hostB.invalidationCount == 0
                || TestHostQueueContains(&hostB, 0));

    // The new value is what a subsequent read observes.
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    TEST_ASSERT(out.mana == 49);

    printf("  PASSED: TestUpdateQueuesOnlyOnRealChange\n");
}

static void
TestUnsubscribeRemovesExactlyOne(void)
{
    printf("Running TestUnsubscribeRemovesExactlyOne...\n");
    CelsMutableStateResetPool();

    ALIGNED_SLAB(4096, slabA);
    ALIGNED_SLAB(4096, slabB);
    CelsCompositionHost hostA;
    CelsCompositionHost hostB;
    TEST_ASSERT(TestHostInit(&hostA, slabA, sizeof(slabA)));
    TEST_ASSERT(TestHostInit(&hostB, slabB, sizeof(slabB)));

    const TestScore initial = { .score = 0 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    TestScore out;
    memset(&out, 0, sizeof(out));

    CelsComposer cmpA;
    TestCompositionEnter(&cmpA, &hostA);
    for (uint32_t i = 0; i < 3u; i++) {
        TEST_ASSERT(CelsComposerGroupStart(&cmpA, KEY_WATCHER_BASE + i));
        TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
        CelsComposerGroupEnd(&cmpA);
    }
    TestCompositionLeave();

    CelsComposer cmpB;
    TestCompositionEnter(&cmpB, &hostB);
    TEST_ASSERT(CelsComposerGroupStart(&cmpB, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpB);
    TestCompositionLeave();

    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 4);

    // Removing composable 1 on host A leaves the other three untouched.
    CelsMutableStateUnsubscribe(&hostA, 1);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 3);

    TestHostClearInvalidation(&hostA);
    TestHostClearInvalidation(&hostB);
    const TestScore changed = { .score = 1 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &changed, sizeof(changed))
                == CELS_OK);
    TEST_ASSERT(hostA.isDirty);
    TEST_ASSERT(hostB.isDirty);

    // NULL is accepted and ignored; an id nobody holds removes nothing.
    CelsMutableStateUnsubscribe(NULL, 0);
    CelsMutableStateUnsubscribe(&hostA, 99);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 3);

    // Tearing down host A must leave host B's subscription alone — a cell
    // outlives the sessions watching it.
    CelsMutableStateUnsubscribeHost(&hostA);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 1);

    TestHostClearInvalidation(&hostA);
    TestHostClearInvalidation(&hostB);
    const TestScore changedAgain = { .score = 2 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &changedAgain,
                                       sizeof(changedAgain))
                == CELS_OK);
    TEST_ASSERT(!hostA.isDirty);
    TEST_ASSERT(hostB.isDirty);

    CelsMutableStateUnsubscribeHost(&hostB);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 0);

    printf("  PASSED: TestUnsubscribeRemovesExactlyOne\n");
}

static void
TestWatcherOverflowDegradesToHostDirty(void)
{
    printf("Running TestWatcherOverflowDegradesToHostDirty...\n");
    CelsMutableStateResetPool();

    ALIGNED_SLAB(4096, slabA);
    ALIGNED_SLAB(4096, slabB);
    CelsCompositionHost hostA;
    CelsCompositionHost hostB;
    TEST_ASSERT(TestHostInit(&hostA, slabA, sizeof(slabA)));
    TEST_ASSERT(TestHostInit(&hostB, slabB, sizeof(slabB)));

    const TestScore initial = { .score = 0 };
    void *cell = NULL;
    TEST_ASSERT(CelsMutableStateCreate(&initial, sizeof(initial), &cell)
                == CELS_OK);

    TestScore out;
    memset(&out, 0, sizeof(out));

    // Fill the individual watcher list exactly.
    CelsComposer cmpA;
    TestCompositionEnter(&cmpA, &hostA);
    for (uint32_t i = 0; i < CELS_MAX_WATCHERS_PER_CELL; i++) {
        TEST_ASSERT(CelsComposerGroupStart(&cmpA, KEY_WATCHER_BASE + i));
        TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
        CelsComposerGroupEnd(&cmpA);
    }
    TestCompositionLeave();

    TEST_ASSERT(CellHeaderOf(cell)->watcherCount
                == CELS_MAX_WATCHERS_PER_CELL);
    TEST_ASSERT(!CellHeaderOf(cell)->overflowed);

    // One more distinct watcher, on a DIFFERENT host, tips it over. The cell
    // stops tracking individuals and collapses to distinct hosts instead.
    CelsComposer cmpB;
    TestCompositionEnter(&cmpB, &hostB);
    TEST_ASSERT(CelsComposerGroupStart(&cmpB, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpB);
    TestCompositionLeave();

    TEST_ASSERT(CellHeaderOf(cell)->overflowed);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 2);

    // Both hosts still get invalidated — coarser, but nothing was dropped.
    TestHostClearInvalidation(&hostA);
    TestHostClearInvalidation(&hostB);
    const TestScore changed = { .score = 1 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &changed, sizeof(changed))
                == CELS_OK);
    TEST_ASSERT(hostA.isDirty);
    TEST_ASSERT(hostB.isDirty);

    // An overflowed cell queues whole-host invalidations, never composables.
    TEST_ASSERT(hostA.invalidationCount == 0
                || TestHostQueueContains(&hostA, CELS_COMPOSABLE_ID_INVALID));

    // Further reads from either host add nothing: the hosts are already known.
    TestCompositionEnter(&cmpA, &hostA);
    TEST_ASSERT(CelsComposerGroupStart(&cmpA, KEY_WATCHER_BASE));
    TEST_ASSERT(CelsMutableStateRead(cell, &out, sizeof(out)) == CELS_OK);
    CelsComposerGroupEnd(&cmpA);
    TestCompositionLeave();
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 2);

    // An unchanged write still queues nothing, overflowed or not.
    TestHostClearInvalidation(&hostA);
    TestHostClearInvalidation(&hostB);
    TEST_ASSERT(CelsMutableStateUpdate(cell, &changed, sizeof(changed))
                == CELS_OK);
    TEST_ASSERT(!hostA.isDirty);
    TEST_ASSERT(!hostB.isDirty);

    // Dropping every watcher lets the cell track individuals again.
    CelsMutableStateUnsubscribeHost(&hostA);
    CelsMutableStateUnsubscribeHost(&hostB);
    TEST_ASSERT(CellHeaderOf(cell)->watcherCount == 0);
    TEST_ASSERT(!CellHeaderOf(cell)->overflowed);

    printf("  PASSED: TestWatcherOverflowDegradesToHostDirty\n");
}

int
main(void)
{
    printf("====================================================\n");
    printf(" Starting Cels Reactive State Test Suite\n");
    printf("====================================================\n");

    TestCreateAndReadRoundTrip();
    TestDslRoundTrip();
    TestPoolExhaustionAndReset();
    TestForeignPointerIsRejected();
    TestReadOutsideCompositionSubscribesNothing();
    TestSubscriptionIsIdempotent();
    TestUpdateQueuesOnlyOnRealChange();
    TestUnsubscribeRemovesExactlyOne();
    TestWatcherOverflowDegradesToHostDirty();

    CelsMutableStateResetPool();

    printf("====================================================\n");
    if (g_failureCount != 0) {
        printf(" %d state test assertion(s) FAILED\n", g_failureCount);
        printf("====================================================\n");
        return 1;
    }
    printf(" All 9 test suites PASSED successfully!\n");
    printf("====================================================\n");
    return 0;
}
