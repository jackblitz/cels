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

static void SmallTreeApp(CelsSession *s) {
    CEL_Composition(s, CEL_KEY("Root")) {
        for (int i = 0; i < 15; ++i) {
            CEL_Composable(s, CEL_KeyIndex(CEL_KEY("Child"), (uint64_t)i)) {
                cel_remember(s, int, i);
            } CEL_Close(s);
        }
    } CEL_Close(s);
}

static void TestGroupCapacityExceeded(void) {
    /* Create a session with maxGroups = 16 (1 root + 15 children fills it completely) */
    CEL_SLAB(buf, 4096);
    CelsSession s;
    CelsSessionInit(&s, &(CelsSessionConfig){
        .slab = buf,
        .slabSize = sizeof(buf),
        .maxGroups = 16,
        .root = SmallTreeApp
    });

    assert(s.maxGroups == 16);
    CelsResult res = CelsSessionRecompose(&s);
    assert(res == CELS_OK);
    assert(CelsGetLogicalGroupCount(&s) == 16);

    CelsSessionDestroy(&s);
}

static const TestCase s_slabTests[] = {
    { "TestDefaultSlab", "Default 32 KiB slab initialization and 64-byte alignment", TestDefaultSlab },
    { "TestL1Profiles", "L1 cache profiles (16 KiB, 48 KiB, 64 KiB)", TestL1Profiles },
    { "TestZeroAllocUserSlab", "CEL_SLAB zero-heap stack buffer mode", TestZeroAllocUserSlab },
    { "TestGroupCapacityExceeded", "Capacity boundary verification (16/16 groups filled safely)", TestGroupCapacityExceeded }
};

static const TestSuite s_slabSuite = {
    .name = "slab",
    .description = "Slab memory allocation, L1 sizing profiles and boundary verification",
    .tests = s_slabTests,
    .testCount = sizeof(s_slabTests) / sizeof(s_slabTests[0])
};

const TestSuite *GetSlabTestSuite(void) {
    return &s_slabSuite;
}
