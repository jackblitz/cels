#include "cli/test_cli.h"
#include "cels/cels.h"
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Test Data Structures                                                      */
/* ========================================================================= */

typedef struct Position {
    float x;
    float y;
    float z;
} Position;

typedef struct Health {
    int hp;
    int maxHp;
} Health;

typedef struct CapturedOp {
    CelsOpCode opCode;
    uint64_t targetId;
    uint64_t typeKey;
    size_t size;
    uint8_t payload[64];
} CapturedOp;

#define MAX_CAPTURED 32
static CapturedOp s_capturedOps[MAX_CAPTURED];
static uint32_t s_capturedCount = 0;

static void TestCommitHandler(CelsOpCode opCode,
                              uint64_t targetId,
                              uint64_t typeKey,
                              const void *data,
                              size_t size,
                              void *userData)
{
    (void)userData;
    if (s_capturedCount < MAX_CAPTURED) {
        CapturedOp *cap = &s_capturedOps[s_capturedCount++];
        cap->opCode = opCode;
        cap->targetId = targetId;
        cap->typeKey = typeKey;
        cap->size = size;
        if (size > 0 && data != NULL) {
            memcpy(cap->payload, data, size < 64 ? size : 64);
        }
    }
}

/* ========================================================================= */
/* Test 1: Basic Staging & Commit                                            */
/* ========================================================================= */

static void TestTransactionStagingAndCommit(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);
    s_capturedCount = 0;

    /* Stage set, remove, delete, and custom ops */
    bool ok1 = cel_stage_set(1001, Position, { .x = 1.0f, .y = 2.0f, .z = 3.0f });
    bool ok2 = cel_stage_set(1001, Health, { .hp = 80, .maxHp = 100 });
    bool ok3 = cel_stage_remove(1001, Position);
    bool ok4 = cel_stage_delete(1001);
    bool ok5 = cel_stage_custom(CELS_OP_CUSTOM + 10, 2002, Health, { .hp = 50, .maxHp = 50 });

    assert(ok1 && ok2 && ok3 && ok4 && ok5);

    /* Commit */
    uint32_t committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 5);
    assert(s_capturedCount == 5);

    /* Check Set Position */
    assert(s_capturedOps[0].opCode == CELS_OP_SET);
    assert(s_capturedOps[0].targetId == 1001);
    assert(s_capturedOps[0].typeKey == CelsHashKey("Position"));
    assert(s_capturedOps[0].size == sizeof(Position));
    Position *p = (Position *)s_capturedOps[0].payload;
    assert(p->x == 1.0f && p->y == 2.0f && p->z == 3.0f);

    /* Check Set Health */
    assert(s_capturedOps[1].opCode == CELS_OP_SET);
    assert(s_capturedOps[1].targetId == 1001);
    assert(s_capturedOps[1].typeKey == CelsHashKey("Health"));
    Health *h = (Health *)s_capturedOps[1].payload;
    assert(h->hp == 80 && h->maxHp == 100);

    /* Check Remove */
    assert(s_capturedOps[2].opCode == CELS_OP_REMOVE);
    assert(s_capturedOps[2].targetId == 1001);
    assert(s_capturedOps[2].typeKey == CelsHashKey("Position"));

    /* Check Delete */
    assert(s_capturedOps[3].opCode == CELS_OP_DELETE);
    assert(s_capturedOps[3].targetId == 1001);

    /* Check Custom */
    assert(s_capturedOps[4].opCode == (CelsOpCode)(CELS_OP_CUSTOM + 10));
    assert(s_capturedOps[4].targetId == 2002);

    /* Commit again on empty batch should do nothing */
    uint32_t emptyCommitted = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(emptyCommitted == 0);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 2: Double Buffering & Cross-Thread Batch Handoff                     */
/* ========================================================================= */

static void TestTransactionDoubleBuffering(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);

    /* Stage operations in active batch */
    cel_stage_set(42, Position, { .x = 10.0f, .y = 20.0f, .z = 30.0f });
    cel_stage_delete(99);

    /* Swap batches (simulating end of recomposition on main thread) */
    CelsSessionSwapTransactionBatches(&s);

    /* Worker thread consumes ready batch locklessly */
    const CelsTransactionBatch *ready = CelsSessionGetReadyBatch(&s);
    assert(ready != NULL);
    assert(ready->opCount == 2);
    assert(ready->ops[0].opCode == CELS_OP_SET);
    assert(ready->ops[0].targetId == 42);
    assert(ready->ops[1].opCode == CELS_OP_DELETE);
    assert(ready->ops[1].targetId == 99);

    /* Main thread can concurrently stage new ops in active batch */
    cel_stage_set(555, Health, { .hp = 10, .maxHp = 10 });
    assert(ready->opCount == 2); /* Ready batch unmodified */

    /* Clear ready batch */
    CelsSessionClearReadyBatch(&s);
    ready = CelsSessionGetReadyBatch(&s);
    assert(ready->opCount == 0);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 3: Session UserData & PostRecompose Hook                             */
/* ========================================================================= */

static bool s_postRecomposeFired = false;
static void *s_receivedUserData = NULL;

static void DummyPostRecomposeHook(CelsSession *session, void *userData)
{
    (void)session;
    s_postRecomposeFired = true;
    s_receivedUserData = userData;
}

CEL_State(DummyState) {
    int counter;
};

CEL_Composition(UserDataTestComp, void *userData) {
    (void)userData;
    int *testContext = cel_user_data(int);
    assert(testContext != NULL);
    assert(*testContext == 777);

    const DummyState *st = cel_watch(DummyState, CEL_ID("DummyState"));
    (void)st;
}

static void TestSessionUserDataAndPostRecomposeHook(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);

    int myContext = 777;
    CelsSessionSetUserData(&s, &myContext);
    assert(CelsSessionGetUserData(&s) == &myContext);

    s_postRecomposeFired = false;
    s_receivedUserData = NULL;
    CelsSessionSetPostRecomposeHook(&s, DummyPostRecomposeHook, &myContext);

    CelsSessionRememberState(&s, CEL_ID("DummyState"), sizeof(DummyState), &((DummyState){ .counter = 1 }));
    cel_attach(&s, UserDataTestComp);

    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_postRecomposeFired == true);
    assert(s_receivedUserData == &myContext);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 4: cel_remember with unmount callback                                */
/* ========================================================================= */

static int s_unmountCount = 0;
static uint64_t s_lastUnmountedVal = 0;

static void OnCleanupCallback(void *ptr, CelsSession *session)
{
    (void)session;
    uint64_t *val = (uint64_t *)ptr;
    ++s_unmountCount;
    s_lastUnmountedVal = *val;
}

CEL_State(GateState) {
    bool open;
};

CEL_Composable(ChildWithCleanup, uint64_t, entityId) {
    /* Using cel_remember with unmount callback */
    uint64_t *slot = cel_remember(uint64_t, entityId, OnCleanupCallback);
    assert(*slot == entityId);

    /* Using cel_remember with explicit NULL (no unmount callback) */
    int *noCleanup = cel_remember(int, 123, NULL);
    assert(*noCleanup == 123);
}

CEL_Composition(GateComposition, void *userData) {
    (void)userData;
    const GateState *gate = cel_watch(GateState, CEL_ID("GateState"));
    if (gate && gate->open) {
        ChildWithCleanup(9999);
    }
}

static void TestCelRememberWithUnmount(void)
{
    s_unmountCount = 0;
    s_lastUnmountedVal = 0;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("GateState"), sizeof(GateState), &((GateState){ .open = true }));
    cel_attach(&s, GateComposition);

    /* Frame 1: Child mounts */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_unmountCount == 0);

    /* Frame 2: Child remains mounted */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_unmountCount == 0);

    /* Frame 3: Gate closes -> Child unmounts */
    cels_session_mutate(&s, CEL_ID("GateState"), GateState) {
        this->open = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_unmountCount == 1);
    assert(s_lastUnmountedVal == 9999);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 5: CEL_Lifecycle Staging Operations (Mount: Add/Set, Unmount: Delete)*/
/* ========================================================================= */

CEL_Lifecycle(EntityTransactionLifecycle, uint64_t, entityId) {
    mount {
        cel_stage_set(entityId, Position, { .x = 100.0f, .y = 200.0f, .z = 300.0f });
        cel_stage_set(entityId, Health, { .hp = 50, .maxHp = 100 });
    }
    unmount {
        cel_stage_delete(entityId);
    }
}

CEL_Composable(LivingEntityNode, uint64_t, entityId) {
    cel_lifecycle(EntityTransactionLifecycle, entityId);
}

CEL_Composition(LifecycleEntityComposition, void *userData) {
    (void)userData;
    const GateState *gate = cel_watch(GateState, CEL_ID("GateState"));
    if (gate && gate->open) {
        LivingEntityNode(4242);
    }
}

static void TestLifecycleTransactionStaging(void)
{
    s_capturedCount = 0;

    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSessionRememberState(&s, CEL_ID("GateState"), sizeof(GateState), &((GateState){ .open = true }));
    cel_attach(&s, LifecycleEntityComposition);

    /* Frame 1: Mount -> mount block stages Set Position and Set Health */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    uint32_t committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2);
    assert(s_capturedCount == 2);
    assert(s_capturedOps[0].opCode == CELS_OP_SET && s_capturedOps[0].targetId == 4242);
    assert(s_capturedOps[1].opCode == CELS_OP_SET && s_capturedOps[1].targetId == 4242);

    /* Frame 2: Steady state -> no new mount/unmount operations */
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 0);
    assert(s_capturedCount == 0);

    /* Frame 3: Gate closes -> node is pruned from tree -> unmount stages Delete */
    cels_session_mutate(&s, CEL_ID("GateState"), GateState) {
        this->open = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 1);
    assert(s_capturedCount == 1);
    assert(s_capturedOps[0].opCode == CELS_OP_DELETE);
    assert(s_capturedOps[0].targetId == 4242);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test Suite Registration                                                   */
/* ========================================================================= */

static const TestCase s_transactionTests[] = {
    { "TestTransactionStagingAndCommit", "Stage and commit operations via CelsTransactionBatch", TestTransactionStagingAndCommit },
    { "TestTransactionDoubleBuffering", "Double-buffered batch swapping for zero-lock cross-thread handoff", TestTransactionDoubleBuffering },
    { "TestSessionUserDataAndPostRecomposeHook", "Session user context pointer and post-recompose commit callback", TestSessionUserDataAndPostRecomposeHook },
    { "TestCelRememberWithUnmount", "cel_remember with unmount destructor (or NULL) invocation during slot pruning", TestCelRememberWithUnmount },
    { "TestLifecycleTransactionStaging", "CEL_Lifecycle staging add/set on mount and delete on unmount", TestLifecycleTransactionStaging },
};

static const TestSuite s_transactionSuite = {
    .name = "transaction",
    .description = "Transaction batch staging, cross-thread bridging, and unmount cleanup hooks",
    .tests = s_transactionTests,
    .testCount = sizeof(s_transactionTests) / sizeof(s_transactionTests[0])
};

const TestSuite *GetTransactionTestSuite(void)
{
    return &s_transactionSuite;
}
