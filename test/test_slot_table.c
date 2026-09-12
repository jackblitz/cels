#include "cels/slot_table.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define ALIGNED_SLAB(size, name) __declspec(align(64)) uint8_t name[size]
#else
#define ALIGNED_SLAB(size, name) __attribute__((aligned(64))) uint8_t name[size]
#endif

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "Assertion failed: %s at %s:%d\n",                 \
                    #cond, __FILE__, __LINE__);                                \
            assert(cond);                                                      \
        }                                                                      \
    } while (0)

static void
TestInitAndReset(void)
{
    printf("Running TestInitAndReset...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;

    // Unaligned memory should fail
    const CelsResult unalignedRes =
        CelsSlotTableInit(&table, (void *)((uintptr_t)slab + 1u), 4095, 10);
    TEST_ASSERT(unalignedRes == CELS_ERROR_INVALID_ARGUMENT);

    // Insufficient slab memory for requested groups
    const CelsResult tooSmallRes =
        CelsSlotTableInit(&table, slab, 64, 10);
    TEST_ASSERT(tooSmallRes == CELS_ERROR_OUT_OF_MEMORY);

    // Valid init
    const CelsResult okRes = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(okRes == CELS_OK);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 0);
    TEST_ASSERT(CelsSlotTableSlotCount(&table) == 0);
    TEST_ASSERT(CelsSlotTableGroupCapacity(&table) == 16);
    TEST_ASSERT(CelsSlotTableSlotCapacity(&table) > 0);

    // Reset empty table
    const CelsResult resetRes = CelsSlotTableReset(&table);
    TEST_ASSERT(resetRes == CELS_OK);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 0);
    TEST_ASSERT(CelsSlotTableSlotCount(&table) == 0);

    printf("  PASSED: TestInitAndReset\n");
}

static void
TestWriterAndReaderBasic(void)
{
    printf("Running TestWriterAndReaderBasic...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    CelsResult res = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(res == CELS_OK);

    CelsSlotWriter writer;
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_OK);

    // Start Root Group (key = 0x1000, entity = 101)
    uint32_t rootIdx = 0;
    res = CelsSlotWriterGroupStart(&writer, 0x1000, 101, &rootIdx);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(rootIdx == 0);

    // Write 2 slots for Root
    uint32_t slotIdx1 = 0;
    uint32_t slotIdx2 = 0;
    res = CelsSlotWriterSlotWrite(&writer, 0xAAAA, &slotIdx1);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterSlotWrite(&writer, 0xBBBB, &slotIdx2);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(slotIdx1 == 0);
    TEST_ASSERT(slotIdx2 == 1);

    // Emit 1 node for Root
    res = CelsSlotWriterNodeEmit(&writer, 1);
    TEST_ASSERT(res == CELS_OK);

    // Start Child Group (key = 0x2000, entity = 202)
    uint32_t childIdx = 0;
    res = CelsSlotWriterGroupStart(&writer, 0x2000, 202, &childIdx);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(childIdx == 1);

    // Write 1 slot for Child
    uint32_t slotIdx3 = 0;
    res = CelsSlotWriterSlotWrite(&writer, 0xCCCC, &slotIdx3);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(slotIdx3 == 2);

    // Emit 1 node for Child
    res = CelsSlotWriterNodeEmit(&writer, 1);
    TEST_ASSERT(res == CELS_OK);

    // End Child Group
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    // End Root Group
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    // Close Writer
    res = CelsSlotWriterClose(&writer);
    TEST_ASSERT(res == CELS_OK);

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);
    TEST_ASSERT(CelsSlotTableSlotCount(&table) == 3);

    // Read Back via CelsSlotReader
    CelsSlotReader reader;
    res = CelsSlotTableReaderOpen(&table, &reader);
    TEST_ASSERT(res == CELS_OK);

    // Inspect Root Group
    CelsSlotGroup rootGroup;
    res = CelsSlotReaderGroupStart(&reader, &rootGroup);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(rootGroup.key == 0x1000);
    TEST_ASSERT(rootGroup.userData == 101);
    TEST_ASSERT(rootGroup.parentIndex == UINT32_MAX);
    TEST_ASSERT(rootGroup.groupSize == 1);
    TEST_ASSERT(rootGroup.nodeCount == 2);
    TEST_ASSERT(rootGroup.slotCount == 2);

    // Read Root's 2 slots
    CelsSlotValue v1 = 0;
    CelsSlotValue v2 = 0;
    res = CelsSlotReaderSlotRead(&reader, &v1);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(v1 == 0xAAAA);
    res = CelsSlotReaderSlotRead(&reader, &v2);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(v2 == 0xBBBB);

    // Next read on Root's slots should fail (exhausted)
    CelsSlotValue vExtra = 0;
    res = CelsSlotReaderSlotRead(&reader, &vExtra);
    TEST_ASSERT(res == CELS_ERROR_INDEX_OUT_OF_BOUNDS);

    // Inspect Child Group
    CelsSlotGroup childGroup;
    res = CelsSlotReaderGroupStart(&reader, &childGroup);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(childGroup.key == 0x2000);
    TEST_ASSERT(childGroup.userData == 202);
    TEST_ASSERT(childGroup.parentIndex == 0);
    TEST_ASSERT(childGroup.groupSize == 0);
    TEST_ASSERT(childGroup.nodeCount == 1);
    TEST_ASSERT(childGroup.slotCount == 1);

    // Read Child's slot
    CelsSlotValue v3 = 0;
    res = CelsSlotReaderSlotRead(&reader, &v3);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(v3 == 0xCCCC);

    // End groups and close reader
    res = CelsSlotReaderGroupEnd(&reader);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotReaderGroupEnd(&reader);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotReaderClose(&reader);
    TEST_ASSERT(res == CELS_OK);

    printf("  PASSED: TestWriterAndReaderBasic\n");
}

static void
TestSubtreeSkipInO1(void)
{
    printf("Running TestSubtreeSkipInO1...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    CelsResult res = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(res == CELS_OK);

    CelsSlotWriter writer;
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_OK);

    // Tree layout:
    // [0] Subtree A (has 2 children: [1] Child A1, [2] Child A2) -> groupSize = 2
    // [3] Subtree B (no children) -> groupSize = 0
    res = CelsSlotWriterGroupStart(&writer, 0x0A00, 0, NULL);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotWriterGroupStart(&writer, 0x0A01, 0, NULL);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotWriterGroupStart(&writer, 0x0A02, 0, NULL);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotWriterGroupStart(&writer, 0x0B00, 0, NULL);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    res = CelsSlotWriterClose(&writer);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    // Reader skips Subtree A in O(1)
    CelsSlotReader reader;
    res = CelsSlotTableReaderOpen(&table, &reader);
    TEST_ASSERT(res == CELS_OK);

    // Skip Subtree A
    res = CelsSlotReaderGroupSkip(&reader);
    TEST_ASSERT(res == CELS_OK);

    // Reader should now be positioned directly at Subtree B (key 0x0B00)
    CelsSlotGroup bGroup;
    res = CelsSlotReaderGroupStart(&reader, &bGroup);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(bGroup.key == 0x0B00);

    res = CelsSlotReaderGroupEnd(&reader);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotReaderClose(&reader);
    TEST_ASSERT(res == CELS_OK);

    printf("  PASSED: TestSubtreeSkipInO1\n");
}

static void
TestGapMovementAndRecompositionWithoutCorruption(void)
{
    printf("Running TestGapMovementAndRecompositionWithoutCorruption...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    CelsResult res = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(res == CELS_OK);

    // 1. Initial composition: 3 sequential groups: AAA (100), BBB (200), CCC (300)
    CelsSlotWriter writer;
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_OK);

    const uint32_t initialKeys[] = { 0xAAA, 0xBBB, 0xCCC };
    const CelsSlotValue initialValues[] = { 100, 200, 300 };

    for (uint32_t i = 0; i < 3; i++) {
        res = CelsSlotWriterGroupStart(&writer, initialKeys[i], 0, NULL);
        TEST_ASSERT(res == CELS_OK);
        res = CelsSlotWriterSlotWrite(&writer, initialValues[i], NULL);
        TEST_ASSERT(res == CELS_OK);
        res = CelsSlotWriterGroupEnd(&writer);
        TEST_ASSERT(res == CELS_OK);
    }

    // 2. Recomposition: Shift gap back to index 1 (between AAA and BBB)
    res = CelsSlotWriterGapMoveTo(&writer, 1);
    TEST_ASSERT(res == CELS_OK);

    // 3. Insert new group DDD (payload 999) into the gap
    res = CelsSlotWriterGroupStart(&writer, 0xDDD, 0, NULL);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterSlotWrite(&writer, 999, NULL);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterGroupEnd(&writer);
    TEST_ASSERT(res == CELS_OK);

    // 4. Move gap to the end (index 4) and close writer
    res = CelsSlotWriterGapMoveTo(&writer, 4);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterClose(&writer);
    TEST_ASSERT(res == CELS_OK);

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);
    TEST_ASSERT(CelsSlotTableSlotCount(&table) == 4);

    // 5. Read back all groups and verify that BBB and CCC were NEVER corrupted!
    CelsSlotReader reader;
    res = CelsSlotTableReaderOpen(&table, &reader);
    TEST_ASSERT(res == CELS_OK);

    const uint32_t expectedKeys[] = { 0xAAA, 0xDDD, 0xBBB, 0xCCC };
    const CelsSlotValue expectedValues[] = { 100, 999, 200, 300 };

    for (uint32_t i = 0; i < 4; i++) {
        CelsSlotGroup g;
        res = CelsSlotReaderGroupStart(&reader, &g);
        TEST_ASSERT(res == CELS_OK);
        TEST_ASSERT(g.key == expectedKeys[i]);

        CelsSlotValue val = 0;
        res = CelsSlotReaderSlotRead(&reader, &val);
        TEST_ASSERT(res == CELS_OK);
        TEST_ASSERT(val == expectedValues[i]);

        res = CelsSlotReaderGroupEnd(&reader);
        TEST_ASSERT(res == CELS_OK);
    }

    res = CelsSlotReaderClose(&reader);
    TEST_ASSERT(res == CELS_OK);

    printf("  PASSED: TestGapMovementAndRecompositionWithoutCorruption\n");
}

static void
TestWriterGroupSkipRecomposition(void)
{
    printf("Running TestWriterGroupSkipRecomposition...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    CelsResult res = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(res == CELS_OK);

    CelsSlotWriter writer;
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_OK);

    // Create 3 groups
    for (uint32_t i = 0; i < 3; i++) {
        res = CelsSlotWriterGroupStart(&writer, 0x10 + i, 0, NULL);
        TEST_ASSERT(res == CELS_OK);
        res = CelsSlotWriterSlotWrite(&writer, 1000 + i, NULL);
        TEST_ASSERT(res == CELS_OK);
        res = CelsSlotWriterGroupEnd(&writer);
        TEST_ASSERT(res == CELS_OK);
    }

    // Move gap to index 0
    res = CelsSlotWriterGapMoveTo(&writer, 0);
    TEST_ASSERT(res == CELS_OK);

    // Skip the first group (0x10)
    uint32_t skipped = 0;
    res = CelsSlotWriterGroupSkip(&writer, &skipped);
    TEST_ASSERT(res == CELS_OK);
    TEST_ASSERT(skipped == 1);

    // Gap should now be at index 1
    res = CelsSlotWriterGapMoveTo(&writer, 3);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotWriterClose(&writer);
    TEST_ASSERT(res == CELS_OK);

    printf("  PASSED: TestWriterGroupSkipRecomposition\n");
}

static void
TestConcurrencyContract(void)
{
    printf("Running TestConcurrencyContract...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    CelsResult res = CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(res == CELS_OK);

    CelsSlotReader r1;
    CelsSlotReader r2;
    res = CelsSlotTableReaderOpen(&table, &r1);
    TEST_ASSERT(res == CELS_OK);

    // Multiple concurrent readers permitted
    res = CelsSlotTableReaderOpen(&table, &r2);
    TEST_ASSERT(res == CELS_OK);

    // Writer cannot open while readers are active
    CelsSlotWriter writer;
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_ERROR_INVALID_STATE);

    res = CelsSlotReaderClose(&r1);
    TEST_ASSERT(res == CELS_OK);
    res = CelsSlotReaderClose(&r2);
    TEST_ASSERT(res == CELS_OK);

    // Now writer can open
    res = CelsSlotTableWriterOpen(&table, &writer);
    TEST_ASSERT(res == CELS_OK);

    // Reader cannot open while writer is active
    res = CelsSlotTableReaderOpen(&table, &r1);
    TEST_ASSERT(res == CELS_ERROR_INVALID_STATE);

    res = CelsSlotWriterClose(&writer);
    TEST_ASSERT(res == CELS_OK);

    printf("  PASSED: TestConcurrencyContract\n");
}

int
main(void)
{
    printf("====================================================\n");
    printf(" Starting Cels SlotTable Test Suite\n");
    printf("====================================================\n");

    TestInitAndReset();
    TestWriterAndReaderBasic();
    TestSubtreeSkipInO1();
    TestGapMovementAndRecompositionWithoutCorruption();
    TestWriterGroupSkipRecomposition();
    TestConcurrencyContract();

    printf("====================================================\n");
    printf(" All 6 test suites PASSED successfully!\n");
    printf("====================================================\n");
    return 0;
}
