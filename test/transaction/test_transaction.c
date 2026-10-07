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
/* Test 6: Ambient Context (cel_context)                                     */
/* ========================================================================= */

typedef struct AppTheme {
    uint32_t color;
    int fontSize;
} AppTheme;

typedef struct InputFilter {
    int mask;
} InputFilter;

static void TestAmbientContextScope(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);

    /* 1. Outside any scope -> returns NULL or CELS_ERROR_NOT_FOUND */
    assert(cel_context(AppTheme) == NULL);
    assert(cel_context(InputFilter) == NULL);

    const AppTheme *outTheme = (const AppTheme *)0x1234;
    assert(cel_get_context(AppTheme, &outTheme) == CELS_ERROR_NOT_FOUND);
    assert(outTheme == NULL);
    assert(cel_getContext(AppTheme, &outTheme) == CELS_ERROR_NOT_FOUND);
    assert(outTheme == NULL);
    assert(cel_get_context(AppTheme, NULL) == CELS_ERROR_INVALID_ARGUMENT);

    /* Test invalid session state */
    CelsSetCurrentSession(NULL);
    assert(cel_get_context(AppTheme, &outTheme) == CELS_ERROR_INVALID_STATE);
    assert(outTheme == NULL);
    CelsSetCurrentSession(&s);

    AppTheme darkTheme = { .color = 0x111111, .fontSize = 14 };
    InputFilter filter = { .mask = 42 };

    /* 2. Top-level composition ambient scope */
    assert(CelsEnterComposition(&s, 0x1000));
    cel_context(AppTheme, &darkTheme);

    const AppTheme *theme = cel_context(AppTheme);
    assert(theme != NULL);
    assert(theme->color == 0x111111);
    assert(theme->fontSize == 14);

    const AppTheme *resTheme = NULL;
    assert(cel_get_context(AppTheme, &resTheme) == CELS_OK);
    assert(resTheme == theme);
    assert(cel_getContext(AppTheme, &resTheme) == CELS_OK);
    assert(resTheme == theme);

    /* Distinct type is still NULL / NOT_FOUND */
    assert(cel_context(InputFilter) == NULL);
    const InputFilter *missingFilter = (const InputFilter *)0x5678;
    assert(cel_get_context(InputFilter, &missingFilter) == CELS_ERROR_NOT_FOUND);
    assert(missingFilter == NULL);

    /* 3. Nested child composable scope */
    assert(CelsEnterComposable(&s, 0x2000));
    cel_context(InputFilter, &filter);

    const AppTheme *nestedTheme = cel_context(AppTheme);
    const InputFilter *nestedFilter = cel_context(InputFilter);

    assert(nestedTheme != NULL && nestedTheme->color == 0x111111);
    assert(nestedFilter != NULL && nestedFilter->mask == 42);

    /* 4. Subtree shadowing: inner child composable overrides outer theme */
    assert(CelsEnterComposable(&s, 0x3000));
    AppTheme lightTheme = { .color = 0xFFFFFF, .fontSize = 18 };
    cel_context(AppTheme, &lightTheme);

    const AppTheme *shadowedTheme = cel_context(AppTheme);
    assert(shadowedTheme != NULL);
    assert(shadowedTheme->color == 0xFFFFFF);
    assert(shadowedTheme->fontSize == 18);

    /* 5. Exiting grandchild composable restores previous theme */
    CelsExitGroup(&s);
    const AppTheme *restoredTheme = cel_get_context(AppTheme);
    assert(restoredTheme != NULL);
    assert(restoredTheme->color == 0x111111);
    assert(cel_getContext(AppTheme) == restoredTheme);

    /* 6. Exiting child composable restores parent composition scope */
    CelsExitGroup(&s);
    assert(cel_get_context(InputFilter) == NULL);
    assert(cel_getContext(InputFilter) == NULL);
    assert(cel_get_context(AppTheme) != NULL);
    assert(cel_getContext(AppTheme) != NULL);

    /* 7. Verify explicit cel_set_context / cel_setContext */
    assert(CelsEnterComposable(&s, 0x4000));
    AppTheme explicitTheme = { .color = 0x222222, .fontSize = 24 };
    cel_set_context(AppTheme, &explicitTheme);
    assert(cel_get_context(AppTheme)->color == 0x222222);

    assert(CelsEnterComposable(&s, 0x5000));
    AppTheme camelTheme = { .color = 0x333333, .fontSize = 32 };
    cel_setContext(AppTheme, &camelTheme);
    assert(cel_getContext(AppTheme)->color == 0x333333);
    CelsExitGroup(&s);

    assert(cel_get_context(AppTheme)->color == 0x222222);
    CelsExitGroup(&s);

    /* 8. Exiting composition -> all scopes popped */
    CelsExitGroup(&s);
    assert(cel_get_context(AppTheme) == NULL);
    assert(cel_getContext(AppTheme) == NULL);
    assert(cel_get_context(InputFilter) == NULL);
    assert(cel_getContext(InputFilter) == NULL);

    outTheme = NULL;
    assert(cel_get_context(AppTheme, &outTheme) == CELS_ERROR_NOT_FOUND);
    assert(outTheme == NULL);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 7: Declarative Entity & Components (CEL_Entity & cel_has)            */
/* ========================================================================= */

CEL_State(PlayerLogicState) {
    bool exists;
    bool hasBuff;
    float posX;
    float posY;
};

CEL_Composition(DeclarativeEntityComposition) {
    const PlayerLogicState *logic = cel_watch(PlayerLogicState, CEL_ID("PlayerLogicState"));
    if (logic && logic->exists) {
        CEL_Entity(9999) {
            assert(cel_entity_id() == 9999);
            cel_has(Position, { .x = logic->posX, .y = logic->posY, .z = 0.0f });

            if (logic->hasBuff) {
                cel_has(Health, { .hp = 100, .maxHp = 100 });
            }
        }
    }
}

static void TestDeclarativeEntityAndComponents(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);

    CelsSessionRememberState(&s, CEL_ID("PlayerLogicState"), sizeof(PlayerLogicState),
        &((PlayerLogicState){ .exists = true, .hasBuff = true, .posX = 10.0f, .posY = 20.0f }));
    cel_attach(&s, DeclarativeEntityComposition);

    /* Frame 1: Entity exists with Position AND Health */
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    uint32_t committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2);
    assert(s_capturedCount == 2);
    assert(s_capturedOps[0].opCode == CELS_OP_SET && s_capturedOps[0].targetId == 9999);
    assert(s_capturedOps[0].typeKey == CelsHashKey("Position"));
    assert(s_capturedOps[1].opCode == CELS_OP_SET && s_capturedOps[1].targetId == 9999);
    assert(s_capturedOps[1].typeKey == CelsHashKey("Health"));

    /* Frame 2: Steady state with position update -> both still present */
    s_capturedCount = 0;
    cels_session_mutate(&s, CEL_ID("PlayerLogicState"), PlayerLogicState) {
        this->posX = 15.0f;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2);
    assert(s_capturedCount == 2);
    Position *p = (Position *)s_capturedOps[0].payload;
    assert(p->x == 15.0f);

    /* Frame 3: hasBuff becomes FALSE -> cel_has(Health) is omitted!
     * CELS MUST AUTOMATICALLY STAGE CELS_OP_REMOVE FOR HEALTH! */
    s_capturedCount = 0;
    cels_session_mutate(&s, CEL_ID("PlayerLogicState"), PlayerLogicState) {
        this->hasBuff = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2); /* 1 SET (Position) + 1 REMOVE (Health) */
    assert(s_capturedCount == 2);
    assert(s_capturedOps[0].opCode == CELS_OP_SET && s_capturedOps[0].typeKey == CelsHashKey("Position"));
    assert(s_capturedOps[1].opCode == CELS_OP_REMOVE && s_capturedOps[1].typeKey == CelsHashKey("Health"));
    assert(s_capturedOps[1].targetId == 9999);

    /* Frame 4: Entity exists becomes FALSE -> entity unmounts!
     * CELS MUST AUTOMATICALLY STAGE CELS_OP_DELETE FOR ENTITY 9999! */
    s_capturedCount = 0;
    cels_session_mutate(&s, CEL_ID("PlayerLogicState"), PlayerLogicState) {
        this->exists = false;
    }
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 1);
    assert(s_capturedCount == 1);
    assert(s_capturedOps[0].opCode == CELS_OP_DELETE && s_capturedOps[0].targetId == 9999);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 8: Composable Identity, cel_id, cel_has, cel_get, cel_is             */
/* ========================================================================= */

typedef struct TodoItem {
    uint64_t id;
    float posX;
    float posY;
    int priority;
} TodoItem;

static uint64_t s_observedId = 0;
static float s_observedX = 0.0f;
static bool s_hasPriority = false;

CEL_Composable(TodoItemCard, id, const TodoItem*, item) {
    s_observedId = cel_id();
    assert(s_observedId == id);

    cel_has(Position, { .x = item->posX, .y = item->posY, .z = 0.0f });
    if (item->priority > 0) {
        cel_has(Health, { .hp = item->priority, .maxHp = 100 });
    }

    const Position *pos = cel_get(Position);
    if (pos) {
        s_observedX = pos->x;
    }
    s_hasPriority = cel_is(Health);
}

CEL_State(TodoListState) {
    TodoItem item;
    bool exists;
};

CEL_Composition(TodoComposition, void *userData) {
    (void)userData;
    const TodoListState *state = cel_watch(TodoListState, CEL_ID("TodoListState"));
    if (state && state->exists) {
        TodoItemCard(state->item.id, &state->item);
    }
}

static void TestComposableIdentityAndDirectHas(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);
    s_capturedCount = 0;

    cel_remember_state_keyed(CEL_ID("TodoListState"), TodoListState, {
        .item = { .id = 5555, .posX = 12.5f, .posY = 34.5f, .priority = 10 },
        .exists = true
    });

    cel_attach(&s, TodoComposition);

    /* Frame 1: Initial composition */
    assert(CelsSessionRecompose(&s) == CELS_OK);
    uint32_t committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2); /* SET Position + SET Health */
    assert(s_observedId == 5555);
    assert(s_observedX == 12.5f);
    assert(s_hasPriority == true);

    /* Frame 2: Priority = 0 -> Health is omitted */
    cels_session_mutate(&s, CEL_ID("TodoListState"), TodoListState) {
        this->item.priority = 0;
        this->item.posX = 50.0f;
    }
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 2); /* SET Position (updated) + REMOVE Health (omitted) */
    assert(s_observedX == 50.0f);
    assert(s_hasPriority == false);
    assert(s_capturedOps[1].opCode == CELS_OP_REMOVE && s_capturedOps[1].typeKey == CelsHashKey("Health"));

    /* Frame 3: exists = false -> unmount */
    cels_session_mutate(&s, CEL_ID("TodoListState"), TodoListState) {
        this->exists = false;
    }
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    committed = CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    assert(committed == 1); /* DELETE 5555 */
    assert(s_capturedOps[0].opCode == CELS_OP_DELETE && s_capturedOps[0].targetId == 5555);

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 9: Keyed List Reconciliation & Slot Memory (cel_key / cel_call)      */
/* ========================================================================= */

typedef struct ListItem {
    uint64_t id;
    int initialVal;
} ListItem;

static int s_renderedVals[4];
static uint64_t s_renderedIds[4];
static uint32_t s_renderCount = 0;

CEL_Composable(KeyedItemRow, const ListItem*, item) {
    /* Pinned slot memory for this keyed group */
    int *persistentCount = cel_remember(int, item->initialVal);
    (*persistentCount) += 10;

    s_renderedIds[s_renderCount] = cel_id();
    s_renderedVals[s_renderCount] = *persistentCount;
    s_renderCount++;

    cel_has(Position, { .x = (float)*persistentCount, .y = 0, .z = 0 });
}

CEL_State(KeyedListModel) {
    ListItem items[4];
    int count;
};

CEL_Composition(KeyedListComposition, void *userData) {
    (void)userData;
    const KeyedListModel *m = cel_watch(KeyedListModel, CEL_ID("KeyedListModel"));
    if (m != NULL) {
        for (int i = 0; i < m->count; ++i) {
            cel_call(KeyedItemRow, m->items[i].id, &m->items[i]);
        }
    }
}

static void TestKeyedListReconciliation(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);
    s_capturedCount = 0;

    cel_remember_state_keyed(CEL_ID("KeyedListModel"), KeyedListModel, {
        .items = {
            { .id = 101, .initialVal = 1 },
            { .id = 102, .initialVal = 2 },
            { .id = 103, .initialVal = 3 }
        },
        .count = 3
    });

    cel_attach(&s, KeyedListComposition);

    /* Frame 1: items [101, 102, 103] -> values [1+10=11, 2+10=12, 3+10=13] */
    s_renderCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_renderCount == 3);
    assert(s_renderedIds[0] == 101 && s_renderedVals[0] == 11);
    assert(s_renderedIds[1] == 102 && s_renderedVals[1] == 12);
    assert(s_renderedIds[2] == 103 && s_renderedVals[2] == 13);
    CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);

    /* Frame 2: Reorder items to [103, 101, 102]
     * In slot table, slot state must travel with the key:
     * 103 was 13, now 13+10 = 23!
     * 101 was 11, now 11+10 = 21!
     * 102 was 12, now 12+10 = 22! */
    cels_session_mutate(&s, CEL_ID("KeyedListModel"), KeyedListModel) {
        this->items[0] = (ListItem){ .id = 103, .initialVal = 3 };
        this->items[1] = (ListItem){ .id = 101, .initialVal = 1 };
        this->items[2] = (ListItem){ .id = 102, .initialVal = 2 };
        this->count = 3;
    }
    s_renderCount = 0;
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_renderCount == 3);
    assert(s_renderedIds[0] == 103 && s_renderedVals[0] == 23);
    assert(s_renderedIds[1] == 101 && s_renderedVals[1] == 21);
    assert(s_renderedIds[2] == 102 && s_renderedVals[2] == 22);

    /* Frame 3: Remove item 101 -> items [103, 102].
     * 101 must be unmounted and deleted! */
    cels_session_mutate(&s, CEL_ID("KeyedListModel"), KeyedListModel) {
        this->items[0] = (ListItem){ .id = 103, .initialVal = 3 };
        this->items[1] = (ListItem){ .id = 102, .initialVal = 2 };
        this->count = 2;
    }
    s_renderCount = 0;
    s_capturedCount = 0;
    assert(CelsSessionRecompose(&s) == CELS_OK);
    assert(s_renderCount == 2);
    assert(s_renderedIds[0] == 103 && s_renderedVals[0] == 33);
    assert(s_renderedIds[1] == 102 && s_renderedVals[1] == 32);

    CelsSessionCommitTransactions(&s, TestCommitHandler, NULL);
    /* Should have DELETE for 101 */
    bool foundDelete101 = false;
    for (uint32_t i = 0; i < s_capturedCount; ++i) {
        if (s_capturedOps[i].opCode == CELS_OP_DELETE && s_capturedOps[i].targetId == 101) {
            foundDelete101 = true;
            break;
        }
    }
    assert(foundDelete101 && "Item 101 must be deleted on removal from list");

    CelsSessionDestroy(&s);
}

/* ========================================================================= */
/* Test 10: ECS Lookup Hook Fallback & Tiered Read (cel_get / cel_is)         */
/* ========================================================================= */

static Position s_externalEcsPos = { .x = 999.0f, .y = 888.0f, .z = 777.0f };

static const void *MockFlecsLookup(uint64_t entityId, uint64_t typeKey, void *userData)
{
    (void)userData;
    if (entityId == 7777 && typeKey == CelsHashKey("Position")) {
        return &s_externalEcsPos;
    }
    return NULL;
}

CEL_Composable(EcsObserverWidget) {
    /* 1. Query external entity 7777 before anything staged in CELS:
     * Should hit Tier 2 ECS lookup hook and find 999.0f! */
    const Position *extPos = cel_get(7777, Position);
    assert(extPos != NULL && extPos->x == 999.0f);
    assert(cel_is(7777, Position) == true);
    assert(cel_is(7777, Health) == false);

    /* 2. Now stage a component for 7777 inside CELS in this frame: */
    cel_has(Position, { .x = 42.0f, .y = 42.0f, .z = 42.0f });

    /* Querying 7777 again must now return Tier 1 staged data (42.0f), overriding the stale ECS data! */
    const Position *stagedPos = cel_get(Position);
    assert(stagedPos != NULL && stagedPos->x == 42.0f);
}

CEL_Composition(EcsComposition, void *userData) {
    (void)userData;
    cel_key(7777) {
        EcsObserverWidget();
    }
}

static void TestEcsLookupHookFallback(void)
{
    CelsSession s;
    CelsSessionInit(&s, NULL);
    CelsSetCurrentSession(&s);

    CelsSessionSetEcsLookupHook(&s, MockFlecsLookup, NULL);
    cel_attach(&s, EcsComposition);

    assert(CelsSessionRecompose(&s) == CELS_OK);

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
    { "TestAmbientContextScope", "cel_context composable cascade, shadowing, and zero-allocation restore", TestAmbientContextScope },
    { "TestDeclarativeEntityAndComponents", "CEL_Entity and cel_has automatic reconciliation, removal, and deletion", TestDeclarativeEntityAndComponents },
    { "TestComposableIdentityAndDirectHas", "Composable identity, cel_id, cel_has, cel_get, cel_is", TestComposableIdentityAndDirectHas },
    { "TestKeyedListReconciliation", "List reconciliation by cel_key and cel_call preserving slot memory across reorder", TestKeyedListReconciliation },
    { "TestEcsLookupHookFallback", "Tier 1 staged data and Tier 2 external ECS lookup hook fallback", TestEcsLookupHookFallback },
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
