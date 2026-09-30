#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>

static void TestDefaultSlab(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);

    assert(session.slab != NULL);
    assert(session.slabSize == CELS_DEFAULT_SLAB_SIZE);
    assert(session.slabSize == CELS_SLAB_512K);
    assert(session.maxGroups == 4096);
    assert(session.maxSlots == 4096);
    assert(session.ownsSlab == true);
    assert(((uintptr_t)session.slab % CELS_CACHE_LINE_SIZE) == 0);

    /* Verify size of groups, slots, and arena */
    assert(session.dataArenaSize == (CELS_SLAB_512K - 4096 * sizeof(CelsSlotGroup) - 4096 * sizeof(CelsSlotAllocation)));

    CelsSessionDestroy(&session);
    assert(session.slab == NULL);
}

static void TestL1Profiles(void) {
    CelsSession s;

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_16K });
    assert(s.slabSize == CELS_SLAB_16K);
    assert(s.maxGroups == 128);
    assert(s.maxSlots == 128);
    assert(((uintptr_t)s.slab % CELS_CACHE_LINE_SIZE) == 0);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_32K });
    assert(s.slabSize == CELS_SLAB_32K);
    assert(s.maxGroups == 256);
    assert(s.maxSlots == 256);
    assert(((uintptr_t)s.slab % CELS_CACHE_LINE_SIZE) == 0);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_48K });
    assert(s.slabSize == CELS_SLAB_48K);
    assert(s.maxGroups == 384);
    assert(s.maxSlots == 384);
    assert(((uintptr_t)s.slab % CELS_CACHE_LINE_SIZE) == 0);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_64K });
    assert(s.slabSize == CELS_SLAB_64K);
    assert(s.maxGroups == 512);
    assert(s.maxSlots == 512);
    assert(((uintptr_t)s.slab % CELS_CACHE_LINE_SIZE) == 0);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_128K });
    assert(s.slabSize == CELS_SLAB_128K);
    assert(s.maxGroups == 1024);
    assert(s.maxSlots == 1024);
    assert(CelsGetSlabSize(&s) == CELS_SLAB_128K);
    assert(CelsGetMaxGroups(&s) == 1024);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_256K });
    assert(s.slabSize == CELS_SLAB_256K);
    assert(s.maxGroups == 2048);
    assert(s.maxSlots == 2048);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .slabSize = CELS_SLAB_1M });
    assert(s.slabSize == CELS_SLAB_1M);
    assert(s.maxGroups == 8192);
    assert(s.maxSlots == 8192);
    CelsSessionDestroy(&s);
}

static void TestZeroAllocUserSlab(void) {
    CEL_SLAB(userBuffer, CELS_SLAB_32K);
    assert(((uintptr_t)userBuffer % CELS_CACHE_LINE_SIZE) == 0);

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .slab = userBuffer,
        .slabSize = sizeof(userBuffer),
    });

    assert(session.slab == (void *)userBuffer);
    assert(session.ownsSlab == false);
    assert(session.slabSize == sizeof(userBuffer));
    assert(session.maxGroups == 256);

    CelsSessionDestroy(&session);
    /* userBuffer is untouched, not freed */
    assert(session.slab == NULL);
}

static void SmallChild(int i) {
    CelsSession *sess = CelsGetCurrentSession();
    if (CelsEnterComposable(sess, CelsKeyIndex(CEL_ID("Child"), (uint64_t)i))) {
        int *val = cel_remember(int, i);
        (void)val;
    }
    CelsExitGroup(sess);
}

CEL_Composition(SmallTreeApp, void *userData) {
    (void)userData;
    for (int i = 0; i < 15; ++i) {
        SmallChild(i);
    }
}

static void TestGroupCapacityExceeded(void) {
    /* Create a session with maxGroups = 16 (1 root + 15 children fills it completely) */
    CEL_SLAB(buf, 4096);
    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){
        .slab = buf,
        .slabSize = sizeof(buf),
        .maxGroups = 16
    });
    cel_attach(&s, SmallTreeApp);

    assert(s.maxGroups == 16);
    CelsResult res = CelsSessionRecompose(&s);
    assert(res == CELS_OK);
    assert(CelsGetLogicalGroupCount(&s) == 16);

    CelsSessionDestroy(&s);
}

static void TestSessionProfiles(void) {
    /* Verify helper sizing mappings */
    assert(CelsSlabSizeFromProfile(CELS_PROFILE_DEFAULT) == CELS_SLAB_512K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_DEFAULT) == 4096);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_128) == CELS_SLAB_16K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_128) == 128);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_256) == CELS_SLAB_32K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_256) == 256);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_512) == CELS_SLAB_64K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_512) == 512);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_1K) == CELS_SLAB_128K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_1K) == 1024);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_2K) == CELS_SLAB_256K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_2K) == 2048);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_4K) == CELS_SLAB_512K);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_4K) == 4096);

    assert(CelsSlabSizeFromProfile(CELS_PROFILE_8K) == CELS_SLAB_1M);
    assert(CelsMaxComposablesFromProfile(CELS_PROFILE_8K) == 8192);

    /* Test CelsSessionInit with profile in config */
    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .profile = CELS_PROFILE_1K });
    assert(s.slabSize == CELS_SLAB_128K);
    assert(s.maxGroups == 1024);
    assert(s.maxSlots == 1024);
    assert(((uintptr_t)s.slab % CELS_CACHE_LINE_SIZE) == 0);
    CelsSessionDestroy(&s);

    /* Test CelsSessionInitWithProfile helper */
    CelsSessionInitWithProfile(&s, CELS_PROFILE_512);
    assert(s.slabSize == CELS_SLAB_64K);
    assert(s.maxGroups == 512);
    assert(s.maxSlots == 512);
    CelsSessionDestroy(&s);

    /* Test CelSessionCreateWithProfile */
    CEL_Session *heapSess = CelSessionCreateWithProfile(CEL_ID("ProfileSession"), CELS_PROFILE_256);
    assert(heapSess != NULL);
    assert(heapSess->slabSize == CELS_SLAB_32K);
    assert(heapSess->maxGroups == 256);
    CelSessionDestroy(heapSess);
}

static void TestSessionCapacityAutoSizing(void) {
    /* Test CelsSessionCapacityConfig */
    CelsSessionConfig cfg1 = CelsSessionCapacityConfig(100);
    assert(cfg1.profile == CELS_PROFILE_128);
    assert(cfg1.slabSize == CELS_SLAB_16K);
    assert(cfg1.maxGroups == 128);

    CelsSessionConfig cfg2 = CelsSessionCapacityConfig(350);
    assert(cfg2.profile == CELS_PROFILE_512);
    assert(cfg2.slabSize == CELS_SLAB_64K);
    assert(cfg2.maxGroups == 512);

    /* Test auto-sizing in CelsSessionInit via maxComposables */
    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){ .maxComposables = 350 });
    assert(s.slabSize == CELS_SLAB_64K);
    assert(s.maxGroups == 512);
    CelsSessionDestroy(&s);

    CelsSessionInit(&s, &(CelsSessionConfig){ .maxComposables = 1500 });
    assert(s.slabSize == CELS_SLAB_256K);
    assert(s.maxGroups == 2048);
    CelsSessionDestroy(&s);

    /* Test CelSessionCreate with profile option */
    CelSessionOptions opts = { .profile = CELS_PROFILE_512 };
    CEL_Session *sess = CelSessionCreate(CEL_ID("CapacityOptSess"), &opts);
    assert(sess != NULL);
    assert(sess->slabSize == CELS_SLAB_64K);
    assert(sess->maxGroups == 512);
    CelSessionDestroy(sess);
}

static const TestCase s_slabTests[] = {
    { "TestDefaultSlab", "Default slab initialization and 64-byte alignment", TestDefaultSlab },
    { "TestL1Profiles", "L1 cache profiles (16 KiB, 48 KiB, 64 KiB)", TestL1Profiles },
    { "TestZeroAllocUserSlab", "CEL_SLAB zero-heap stack buffer mode", TestZeroAllocUserSlab },
    { "TestGroupCapacityExceeded", "Capacity boundary verification (16/16 groups filled safely)", TestGroupCapacityExceeded },
    { "TestSessionProfiles", "Named workload capacity profiles (CELS_PROFILE_128 through 8K)", TestSessionProfiles },
    { "TestSessionCapacityAutoSizing", "Intent-driven composable count auto-sizing (maxComposables)", TestSessionCapacityAutoSizing }
};

static const TestSuite s_slabSuite = {
    .name = "slab",
    .description = "Slab memory allocation, workload capacity profiles and boundary verification",
    .tests = s_slabTests,
    .testCount = sizeof(s_slabTests) / sizeof(s_slabTests[0])
};

const TestSuite *GetSlabTestSuite(void) {
    return &s_slabSuite;
}
