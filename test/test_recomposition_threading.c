#include "cels.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flecs.h"

#define NUM_COMPLEX_HOSTS 20u
#define NUM_LIGHT_HOSTS 40u
#define TOTAL_HOSTS (NUM_COMPLEX_HOSTS + NUM_LIGHT_HOSTS)
#define SLAB_SIZE 4096u
#define WORKER_COUNT 4u

#define COMPLEX_CHILD_COUNT 10u
#define LIGHT_CHILD_COUNT 1u

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr,                                                    \
                    "Assertion failed: %s at %s:%d\n",                         \
                    #cond,                                                     \
                    __FILE__,                                                  \
                    __LINE__);                                                 \
            assert(cond);                                                      \
        }                                                                      \
    } while (0)

/**
 * Component attached to child entities spawned deferred by composable functions.
 * Merged into the canonical archetype table during PostRecomposeSync.
 */
typedef struct ChildNodeComponent {
    ecs_entity_t parentHost;
    uint32_t nodeIndex;
} ChildNodeComponent;

ECS_COMPONENT_DECLARE(ChildNodeComponent);
ecs_entity_t ecs_id(ChildNodeComponent);

/**
 * User props passed to dummy composables to configure hierarchy depth.
 */
typedef struct HostProps {
    ecs_entity_t hostEntity;
    uint32_t childNodeCount;
    uint32_t baseKey;
} HostProps;

static pthread_mutex_t g_entityMutex = PTHREAD_MUTEX_INITIALIZER;

static inline ecs_entity_t
StageNewEntity(ecs_world_t *stage)
{
    pthread_mutex_lock(&g_entityMutex);
    const ecs_entity_t e = ecs_new(stage);
    pthread_mutex_unlock(&g_entityMutex);
    return e;
}

/**
 * Composable root function executed by worker threads during OnRecompose.
 * Creates a root composition group and child composables, staging deferred
 * entity creations into the worker thread's isolated ecs_stage_t.
 *
 * @param cmp   Transient worker composer. Non-NULL.
 * @param props Pointer to HostProps. Non-NULL.
 */
static void
DummyComposableRoot(CelsComposer *cmp, void *props)
{
    const HostProps *const p = (const HostProps *)props;
    assert(cmp != NULL);
    assert(p != NULL);

    const uint32_t childCount = p->childNodeCount;
    const uint32_t baseKey = p->baseKey;
    const ecs_entity_t parentHost = p->hostEntity;

    CEL_Composition(cmp, cmp->table, CEL_Key(baseKey)) {
        for (uint32_t i = 0; i < childCount; i++) {
            CEL_Compose(CEL_Key(baseKey + 1u + i)) {
                // Stage deferred entity creation into the worker's private stage
                ecs_world_t *const stage = CelsComposerGetStage(cmp);
                if (stage != NULL) {
                    const ecs_entity_t child = StageNewEntity(stage);
                    const ChildNodeComponent comp = {
                        .parentHost = parentHost,
                        .nodeIndex = i
                    };
                    ecs_set_ptr(stage, child, ChildNodeComponent, &comp);
                }
            }
        }
    }
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("==============================================================\n");
    printf("  CELS Task 3: Multi-Threaded Recomposition Dispatcher Test   \n");
    printf("==============================================================\n\n");

    // 1. Bootstrap Flecs world
    printf("[Step 1] Bootstrapping Flecs ECS world...\n");
    ecs_world_t *const world = ecs_init();
    TEST_ASSERT(world != NULL);

    // Register test component
    ECS_COMPONENT_DEFINE(world, ChildNodeComponent);
    {
        const ecs_entity_t dummy = ecs_new(world);
        ChildNodeComponent comp = { .parentHost = 0, .nodeIndex = 0 };
        ecs_set_ptr(world, dummy, ChildNodeComponent, &comp);
        ecs_delete(world, dummy);
    }

    // 2. Initialize RecompositionDispatcher with 4 worker threads
    printf("[Step 2] Initializing RecompositionDispatcher with %u workers...\n",
           WORKER_COUNT);
    RecompositionDispatcher disp;
    const CelsResult initRes = dispatcher_init(&disp, world, WORKER_COUNT);
    TEST_ASSERT(initRes == CELS_OK);
    TEST_ASSERT(disp.threadCount == WORKER_COUNT);
    TEST_ASSERT(disp.phaseOnRecompose != 0);
    TEST_ASSERT(disp.phasePostRecomposeSync != 0);

    // 3. Register dummy entity hierarchy:
    //    20 complex hosts with 10 child nodes each (weight 10)
    //    40 lightweight hosts with 1 child node each (weight 1)
    printf("[Step 3] Registering entity hierarchy (%u complex, %u light)...\n",
           NUM_COMPLEX_HOSTS,
           NUM_LIGHT_HOSTS);

    uint8_t *rawSlabs[TOTAL_HOSTS];
    uint8_t *alignedSlabs[TOTAL_HOSTS];
    ecs_entity_t hostEntities[TOTAL_HOSTS];

    for (uint32_t i = 0; i < TOTAL_HOSTS; i++) {
        // Allocate 64-byte aligned 4KB slab buffer
        rawSlabs[i] = (uint8_t *)malloc(SLAB_SIZE + 64u);
        TEST_ASSERT(rawSlabs[i] != NULL);
        const uintptr_t addr = (uintptr_t)rawSlabs[i];
        const uintptr_t aligned = (addr + 63u) & ~(uintptr_t)63u;
        alignedSlabs[i] = (uint8_t *)aligned;

        const bool isComplex = (i < NUM_COMPLEX_HOSTS);
        const uint32_t childCount =
            isComplex ? COMPLEX_CHILD_COUNT : LIGHT_CHILD_COUNT;
        const uint32_t baseKey =
            isComplex ? (0x1000u + i * 100u) : (0x5000u + i * 10u);

        // Create Flecs host entity
        char nameBuf[32];
        snprintf(nameBuf,
                 sizeof(nameBuf),
                 isComplex ? "Host_Complex_%02u" : "Host_Light_%02u",
                 i);
        hostEntities[i] = ecs_entity(world, { .name = nameBuf });
        TEST_ASSERT(hostEntities[i] != 0);

        // Initialize CompositionHost
        CompositionHost host;
        memset(&host, 0, sizeof(host));
        const CelsResult hostRes =
            CelsCompositionHostInit(&host,
                                    alignedSlabs[i],
                                    SLAB_SIZE,
                                    32u,
                                    DummyComposableRoot);
        TEST_ASSERT(hostRes == CELS_OK);

        HostProps *const props = (HostProps *)host.props;
        props->hostEntity = hostEntities[i];
        props->childNodeCount = childCount;
        props->baseKey = baseKey;

        // Pre-populate slot table hierarchy to establish structural group counts
        CelsComposer seedCmp;
        memset(&seedCmp, 0, sizeof(seedCmp));
        CelsComposerBegin(&seedCmp, &host.slotTable);
        DummyComposableRoot(&seedCmp, host.props);

        // Verify slot table populated
        const uint32_t groupCount = CelsSlotTableGroupCount(&host.slotTable);
        TEST_ASSERT(groupCount == (childCount + 1u));

        // 4. Flag all hosts as is_dirty = true
        host.isDirty = true;

        // Set host component on entity
        ecs_set_ptr(world, hostEntities[i], CompositionHost, &host);
    }

    printf("  Total hosts created: %u\n", TOTAL_HOSTS);
    printf("  Total expected child entities: %u\n",
           (NUM_COMPLEX_HOSTS * COMPLEX_CHILD_COUNT) +
               (NUM_LIGHT_HOSTS * LIGHT_CHILD_COUNT));

    // 5. Step the pipeline using ecs_progress
    printf("\n[Step 4] Stepping Flecs pipeline (ecs_progress 16ms)...\n");
    printf("  -> OnRecompose: Workers execute parallel recomposition\n");
    printf("  -> PostRecomposeSync: Main thread sequentially merges stages\n");

    const bool progressOk = ecs_progress(world, 0.016f);
    TEST_ASSERT(progressOk);

    // 6. Asserts and prints:
    //    A. Distribution of workloads across worker threads (LPT confirmation)
    printf("\n[Step 5] Workload Balancing Verification (LPT Bin-Packing):\n");
    printf("  +--------+-------------+--------------+\n");
    printf("  | Worker | Hosts Count | Total Weight |\n");
    printf("  +--------+-------------+--------------+\n");

    uint32_t totalAssignedHosts = 0;
    uint32_t totalAssignedWeight = 0;

    for (uint32_t w = 0; w < WORKER_COUNT; w++) {
        const uint32_t count = disp.workers[w].slice.hostCount;
        const uint32_t weight = disp.lastDispatchedWeight[w];
        totalAssignedHosts += count;
        totalAssignedWeight += weight;

        printf("  |   #%u   |     %2u      |      %3u     |\n",
               w,
               count,
               weight);

        // LPT Invariant: 20 complex hosts (weight 11) + 40 light hosts (weight 2)
        // Total weight = 20*11 + 40*2 = 300.
        // Each of the 4 workers must receive exactly 5 complex hosts and 10 light hosts,
        // totaling 15 hosts and exactly 75 weight!
        TEST_ASSERT(count == 15u);
        TEST_ASSERT(weight == 75u);
    }
    printf("  +--------+-------------+--------------+\n");
    printf("  | Total  |     %2u      |      %3u     |\n",
           totalAssignedHosts,
           totalAssignedWeight);
    printf("  +--------+-------------+--------------+\n");

    TEST_ASSERT(totalAssignedHosts == TOTAL_HOSTS);
    printf("  CONFIRMED: LPT bin-packing perfectly balanced all workers!\n");

    // B. Assert all deferred entity creations merged into ecs_world_t
    printf("\n[Step 6] Staging & Merging Verification:\n");
    ecs_query_t *const childQuery = ecs_query(world, {
        .terms = {
            { .id = ecs_id(ChildNodeComponent) }
        }
    });
    TEST_ASSERT(childQuery != NULL);

    uint32_t totalMergedChildren = 0;
    ecs_iter_t it = ecs_query_iter(world, childQuery);
    while (ecs_query_next(&it)) {
        totalMergedChildren += (uint32_t)it.count;
    }
    ecs_query_fini(childQuery);

    const uint32_t expectedChildren =
        (NUM_COMPLEX_HOSTS * COMPLEX_CHILD_COUNT) +
        (NUM_LIGHT_HOSTS * LIGHT_CHILD_COUNT);

    printf("  Canonical world child entities found: %u (expected: %u)\n",
           totalMergedChildren,
           expectedChildren);
    TEST_ASSERT(totalMergedChildren == expectedChildren);

    // C. Assert that all hosts have is_dirty == false
    for (uint32_t i = 0; i < TOTAL_HOSTS; i++) {
        const CompositionHost *const host =
            ecs_get(world, hostEntities[i], CompositionHost);
        TEST_ASSERT(host != NULL);
        TEST_ASSERT(host->isDirty == false);
    }
    printf("  CONFIRMED: All hosts marked clean (isDirty == false)!\n");

    // D. Step pipeline a second time: verify clean pass (0 hosts dispatched)
    printf("\n[Step 7] Clean frame verification (second ecs_progress)...\n");
    disp.totalDispatchedHosts = 0;
    ecs_progress(world, 0.016f);

    for (uint32_t w = 0; w < WORKER_COUNT; w++) {
        TEST_ASSERT(disp.workers[w].slice.hostCount == 0);
    }
    printf("  CONFIRMED: Clean frame dispatched 0 hosts in O(1) time!\n");

    // 7. Cleanup & Shutdown
    printf("\n[Step 8] Shutting down RecompositionDispatcher and Flecs world...\n");
    dispatcher_destroy(&disp);

    for (uint32_t i = 0; i < TOTAL_HOSTS; i++) {
        free(rawSlabs[i]);
    }

    ecs_fini(world);

    printf("\n==============================================================\n");
    printf("  ALL RECOMPOSITION DISPATCHER TESTS PASSED SUCCESSFULLY!    \n");
    printf("==============================================================\n");

    return 0;
}
