#include "cels/slot_table.h"

#include <assert.h>
#include <string.h>

#define CELS_ASSERT(condition) assert(condition)

/**
 * Returns a static, never-NULL human-readable description of a CelsResult.
 *
 * @param result Any CelsResult value.
 * @return Static description string.
 */
const char *
CelsResultToString(CelsResult result)
{
    switch (result) {
    case CELS_OK:
        return "Operation completed successfully";
    case CELS_ERROR_INVALID_ARGUMENT:
        return "Invalid argument supplied";
    case CELS_ERROR_OUT_OF_MEMORY:
        return "Insufficient memory in slab allocation";
    case CELS_ERROR_CAPACITY_EXCEEDED:
        return "Fixed capacity limit exceeded";
    case CELS_ERROR_INDEX_OUT_OF_BOUNDS:
        return "Index is out of range";
    case CELS_ERROR_INVALID_STATE:
        return "Operation invalid for current table/writer state";
    default:
        return "Unknown error code";
    }
}

/**
 * Carves groups and slots buffers out of a single contiguous memory slab.
 *
 * @param table      Target table to initialize. Non-NULL.
 * @param slabMemory Pointer to 64-byte aligned slab. Non-NULL.
 * @param slabSize   Total byte capacity of the slab buffer.
 * @param maxGroups  Maximum groups reserved at the base of the slab.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_OUT_OF_MEMORY.
 */
CelsResult
CelsSlotTableInit(CelsSlotTable *table,
                  void *slabMemory,
                  size_t slabSize,
                  uint32_t maxGroups)
{
    if (table == NULL || slabMemory == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (maxGroups == 0 || slabSize == 0) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    if (((uintptr_t)slabMemory % CELS_CACHE_LINE_SIZE) != 0) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    const size_t groupBytes = maxGroups * sizeof(CelsSlotGroup);
    if (slabSize <= (groupBytes + sizeof(CelsSlotValue))) {
        return CELS_ERROR_OUT_OF_MEMORY;
    }

    const size_t slotBytes = slabSize - groupBytes;
    const uint32_t slotWords = (uint32_t)(slotBytes / sizeof(CelsSlotValue));
    if (slotWords == 0) {
        return CELS_ERROR_OUT_OF_MEMORY;
    }

    memset(slabMemory, 0, slabSize);

    table->groups = (CelsSlotGroup *)slabMemory;
    table->slots = (CelsSlotValue *)((uint8_t *)slabMemory + groupBytes);
    table->groupCapacity = maxGroups;
    table->groupGapStart = 0;
    table->groupGapLen = maxGroups;
    table->slotCapacity = slotWords;
    table->slotGapStart = 0;
    table->slotGapLen = slotWords;
    table->readerCount = 0;
    table->writerActive = false;

    return CELS_OK;
}

/**
 * Resets table to empty state without releasing underlying slab storage.
 *
 * @param table Target table. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotTableReset(CelsSlotTable *table)
{
    CELS_ASSERT(table != NULL);

    if (table->readerCount > 0 || table->writerActive) {
        return CELS_ERROR_INVALID_STATE;
    }

    table->groupGapStart = 0;
    table->groupGapLen = table->groupCapacity;
    table->slotGapStart = 0;
    table->slotGapLen = table->slotCapacity;

    const size_t groupBytes = table->groupCapacity * sizeof(CelsSlotGroup);
    const size_t slotBytes = table->slotCapacity * sizeof(CelsSlotValue);

    memset(table->groups, 0, groupBytes);
    memset(table->slots, 0, slotBytes);

    return CELS_OK;
}

/**
 * Returns active group count.
 *
 * @param table Target table. Non-NULL.
 * @return Active group count.
 */
uint32_t
CelsSlotTableGroupCount(const CelsSlotTable *table)
{
    CELS_ASSERT(table != NULL);
    return table->groupCapacity - table->groupGapLen;
}

/**
 * Returns active slot word count.
 *
 * @param table Target table. Non-NULL.
 * @return Active slot word count.
 */
uint32_t
CelsSlotTableSlotCount(const CelsSlotTable *table)
{
    CELS_ASSERT(table != NULL);
    return table->slotCapacity - table->slotGapLen;
}

/**
 * Returns maximum group capacity.
 *
 * @param table Target table. Non-NULL.
 * @return Group capacity.
 */
uint32_t
CelsSlotTableGroupCapacity(const CelsSlotTable *table)
{
    CELS_ASSERT(table != NULL);
    return table->groupCapacity;
}

/**
 * Returns maximum slot word capacity.
 *
 * @param table Target table. Non-NULL.
 * @return Slot capacity.
 */
uint32_t
CelsSlotTableSlotCapacity(const CelsSlotTable *table)
{
    CELS_ASSERT(table != NULL);
    return table->slotCapacity;
}

/**
 * Translates a logical group index to its physical array index.
 *
 * @param table          Target table. Non-NULL.
 * @param logicalIndex   Logical group index.
 * @param outPhysicalIdx Receives physical index. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotTableGroupToPhysicalIdx(const CelsSlotTable *table,
                                uint32_t logicalIndex,
                                uint32_t *outPhysicalIdx)
{
    CELS_ASSERT(table != NULL);
    CELS_ASSERT(outPhysicalIdx != NULL);

    const uint32_t total = CelsSlotTableGroupCount(table);
    if (logicalIndex >= total) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    const uint32_t physical = (logicalIndex < table->groupGapStart)
        ? logicalIndex
        : (logicalIndex + table->groupGapLen);

    *outPhysicalIdx = physical;
    return CELS_OK;
}

/* ========================================================================= */
/* Invalidation flags                                                        */
/* ========================================================================= */

/**
 * Resolves a logical group index to its mutable CelsSlotGroup.
 *
 * Shared by every flag accessor below so the gap-buffer translation and the
 * bounds check live in exactly one place.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param composable Logical group index.
 * @return Pointer to the group, or NULL if the index is out of range.
 */
static CelsSlotGroup *
GroupAt(const CelsSlotTable *table, CelsComposableId composable)
{
    CELS_ASSERT(table != NULL);

    uint32_t physical = 0;
    if (CelsSlotTableGroupToPhysicalIdx(table, composable, &physical)
        != CELS_OK) {
        return NULL;
    }
    return &table->groups[physical];
}

/**
 * Reads the invalidation and lifecycle flags for a group.
 *
 * @param table      Pointer to the slot table. May be NULL.
 * @param composable Logical group index.
 * @return The group's CelsGroupFlags bitmask, or CELS_GROUP_FLAG_NONE if invalid.
 */
uint16_t
CelsSlotTableGroupFlags(const CelsSlotTable *table, CelsComposableId composable)
{
    if (table == NULL) {
        return (uint16_t)CELS_GROUP_FLAG_NONE;
    }

    const CelsSlotGroup *const group = GroupAt(table, composable);
    return (group != NULL) ? group->flags : (uint16_t)CELS_GROUP_FLAG_NONE;
}

/**
 * Marks a group as invalidated and propagates dirty flags upward to the root.
 *
 * Sets CELS_GROUP_FLAG_INVALIDATED on the targeted group and sets
 * CELS_GROUP_FLAG_CONTAINS_INVALIDATED on every ancestor group by climbing parentIndex.
 * Halts at the root, at an ancestor already marked, or if cycle protection limit is reached.
 *
 * @param table      Pointer to the slot table. Non-NULL.
 * @param composable Logical group index to invalidate.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotTableGroupInvalidate(CelsSlotTable *table, CelsComposableId composable)
{
    if (table == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    CelsSlotGroup *const target = GroupAt(table, composable);
    if (target == NULL) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    target->flags |= (uint16_t)CELS_GROUP_FLAG_INVALIDATED;

    // Carry the signal upward so no ancestor can O(1)-skip past this subtree.
    // Bounded by groupCount: a corrupt parentIndex cycle must not hang the
    // caller, and an ancestor already marked has marked its own ancestors.
    const uint32_t limit = CelsSlotTableGroupCount(table);
    uint32_t parent = target->parentIndex;

    for (uint32_t step = 0; step < limit && parent != UINT32_MAX; ++step) {
        CelsSlotGroup *const ancestor = GroupAt(table, parent);
        if (ancestor == NULL) {
            break;
        }
        if ((ancestor->flags & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED)
            != 0u) {
            break;
        }
        ancestor->flags |= (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED;
        parent = ancestor->parentIndex;
    }

    return CELS_OK;
}

/**
 * Clears invalidation flags on a specific group.
 *
 * Called when a recomposition walk commits to descending into and executing a group.
 *
 * @param table      Pointer to the slot table. May be NULL.
 * @param composable Logical group index to clear.
 */
void
CelsSlotTableGroupClearFlags(CelsSlotTable *table, CelsComposableId composable)
{
    if (table == NULL) {
        return;
    }

    CelsSlotGroup *const group = GroupAt(table, composable);
    if (group != NULL) {
        group->flags = (uint16_t)CELS_GROUP_FLAG_NONE;
    }
}

/**
 * Clears invalidation flags across all active groups in the table.
 *
 * @param table Pointer to the slot table. May be NULL.
 */
void
CelsSlotTableClearAllFlags(CelsSlotTable *table)
{
    if (table == NULL) {
        return;
    }

    const uint32_t total = CelsSlotTableGroupCount(table);
    for (uint32_t i = 0; i < total; ++i) {
        CelsSlotGroup *const group = GroupAt(table, i);
        if (group != NULL) {
            group->flags = (uint16_t)CELS_GROUP_FLAG_NONE;
        }
    }
}

/**
 * Adjusts logical parentIndex references affected by group insertion or deletion.
 *
 * Walks active groups physically (skipping the gap) and shifts any parentIndex
 * at or above threshold by delta. Ensures that invalidation chains remain intact
 * when groups shift in the gap buffer.
 *
 * @param table     Pointer to the slot table. May be NULL.
 * @param threshold Lowest logical parent index affected by the shift.
 * @param delta     Signed offset to add to affected parent indices.
 */
void
CelsSlotTableGroupsShiftParents(CelsSlotTable *table,
                                uint32_t threshold,
                                int32_t delta)
{
    if (table == NULL || delta == 0) {
        return;
    }

    // Walk physically rather than logically: the caller is mid-renumbering, so
    // logical indices are exactly the thing that cannot be trusted right now.
    // Groups inside the gap are skipped, which is what excludes the group being
    // inserted and the groups being removed.
    const uint32_t gapEnd = table->groupGapStart + table->groupGapLen;
    for (uint32_t physical = 0; physical < table->groupCapacity; ++physical) {
        if (physical >= table->groupGapStart && physical < gapEnd) {
            continue;
        }

        CelsSlotGroup *const group = &table->groups[physical];
        if (group->parentIndex == UINT32_MAX
            || group->parentIndex < threshold) {
            continue;
        }
        group->parentIndex =
            (uint32_t)((int64_t)group->parentIndex + (int64_t)delta);
    }
}

/**
 * Searches active groups in the table for a group matching the given key.
 *
 * @param table         Pointer to the CelsSlotTable. Non-NULL.
 * @param key           Key or hashed name to locate.
 * @param outLogicalIdx Optional destination to receive the logical group index.
 * @return Pointer to matching CelsSlotGroup in table, or NULL if not found.
 */
CelsSlotGroup *
CelsSlotTableFindGroup(const CelsSlotTable *table,
                       uint64_t key,
                       uint32_t *outLogicalIdx)
{
    if (table == NULL) {
        return NULL;
    }

    const uint32_t count = CelsSlotTableGroupCount(table);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = 0;
        if (CelsSlotTableGroupToPhysicalIdx(table, i, &phys) == CELS_OK) {
            if (table->groups[phys].key == key) {
                if (outLogicalIdx != NULL) {
                    *outLogicalIdx = i;
                }
                return &table->groups[phys];
            }
        }
    }
    return NULL;
}

/**
 * Opens a read-only reader cursor on the table.
 *
 * @param table     Target table. Non-NULL.
 * @param outReader Receives the initialized reader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotTableReaderOpen(const CelsSlotTable *table,
                        CelsSlotReader *outReader)
{
    if (table == NULL || outReader == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (table->writerActive) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsSlotTable *mutableTable = (CelsSlotTable *)table;
    mutableTable->readerCount++;

    outReader->table = table;
    outReader->currentGroup = 0;
    outReader->currentSlot = 0;
    outReader->currentParent = UINT32_MAX;
    outReader->groupEnd = CelsSlotTableGroupCount(table);

    return CELS_OK;
}

/**
 * Closes an active reader cursor.
 *
 * @param reader Target reader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotReaderClose(CelsSlotReader *reader)
{
    if (reader == NULL || reader->table == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (reader->table->readerCount == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsSlotTable *mutableTable = (CelsSlotTable *)reader->table;
    mutableTable->readerCount--;
    reader->table = NULL;

    return CELS_OK;
}

/**
 * Enters the next group in reader traversal order.
 *
 * @param reader   Target reader. Non-NULL.
 * @param outGroup Optional pointer receiving group metadata.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotReaderGroupStart(CelsSlotReader *reader,
                         CelsSlotGroup *outGroup)
{
    CELS_ASSERT(reader != NULL);
    CELS_ASSERT(reader->table != NULL);

    if (reader->currentGroup >= reader->groupEnd) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        reader->table, reader->currentGroup, &phys);
    if (res != CELS_OK) {
        return res;
    }

    const CelsSlotGroup *group = &reader->table->groups[phys];
    if (outGroup != NULL) {
        *outGroup = *group;
    }

    reader->currentParent = reader->currentGroup;
    reader->currentSlot = 0;
    reader->currentGroup++;
    reader->groupEnd = reader->currentGroup + (uint32_t)group->groupSize;

    return CELS_OK;
}

/**
 * Leaves the current group, restoring parent bounds.
 *
 * @param reader Target reader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotReaderGroupEnd(CelsSlotReader *reader)
{
    CELS_ASSERT(reader != NULL);
    CELS_ASSERT(reader->table != NULL);

    if (reader->currentParent == UINT32_MAX) {
        return CELS_ERROR_INVALID_STATE;
    }

    uint32_t parentPhys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        reader->table, reader->currentParent, &parentPhys);
    if (res != CELS_OK) {
        return res;
    }

    const CelsSlotGroup *parent = &reader->table->groups[parentPhys];
    reader->currentParent = parent->parentIndex;

    if (parent->parentIndex == UINT32_MAX) {
        reader->groupEnd = CelsSlotTableGroupCount(reader->table);
    } else {
        uint32_t grandPhys = 0;
        const CelsResult grandRes = CelsSlotTableGroupToPhysicalIdx(
            reader->table, parent->parentIndex, &grandPhys);
        if (grandRes != CELS_OK) {
            return grandRes;
        }
        const CelsSlotGroup *grand = &reader->table->groups[grandPhys];
        reader->groupEnd = parent->parentIndex + 1u + (uint32_t)grand->groupSize;
    }

    return CELS_OK;
}

/**
 * Skips the current group and all transitive children in O(1).
 *
 * @param reader Target reader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotReaderGroupSkip(CelsSlotReader *reader)
{
    CELS_ASSERT(reader != NULL);
    CELS_ASSERT(reader->table != NULL);

    const uint32_t total = CelsSlotTableGroupCount(reader->table);
    if (reader->currentGroup >= total) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        reader->table, reader->currentGroup, &phys);
    if (res != CELS_OK) {
        return res;
    }

    const CelsSlotGroup *group = &reader->table->groups[phys];
    reader->currentGroup += 1u + (uint32_t)group->groupSize;
    return CELS_OK;
}

/**
 * Reads the next slot word from the currently entered group.
 *
 * @param reader   Target reader. Non-NULL.
 * @param outValue Receives the 64-bit slot value. Non-NULL.
 * @return CELS_OK, CELS_ERROR_INDEX_OUT_OF_BOUNDS, or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotReaderSlotRead(CelsSlotReader *reader,
                       CelsSlotValue *outValue)
{
    CELS_ASSERT(reader != NULL);
    CELS_ASSERT(reader->table != NULL);
    CELS_ASSERT(outValue != NULL);

    if (reader->currentParent == UINT32_MAX) {
        return CELS_ERROR_INVALID_STATE;
    }

    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        reader->table, reader->currentParent, &phys);
    if (res != CELS_OK) {
        return res;
    }

    const CelsSlotGroup *group = &reader->table->groups[phys];
    if (reader->currentSlot >= group->slotCount) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    *outValue = reader->table->slots[group->slotIndex + reader->currentSlot];
    reader->currentSlot++;
    return CELS_OK;
}

/**
 * Reads group metadata by logical group index.
 *
 * @param reader       Target reader. Non-NULL.
 * @param logicalIndex Logical group index.
 * @param outGroup     Receives group metadata. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotReaderGroupGet(const CelsSlotReader *reader,
                       uint32_t logicalIndex,
                       CelsSlotGroup *outGroup)
{
    CELS_ASSERT(reader != NULL);
    CELS_ASSERT(reader->table != NULL);
    CELS_ASSERT(outGroup != NULL);

    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        reader->table, logicalIndex, &phys);
    if (res != CELS_OK) {
        return res;
    }

    *outGroup = reader->table->groups[phys];
    return CELS_OK;
}

/**
 * Opens an exclusive mutating writer cursor on the table.
 *
 * @param table     Target table. Non-NULL.
 * @param outWriter Receives initialized writer. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotTableWriterOpen(CelsSlotTable *table,
                        CelsSlotWriter *outWriter)
{
    if (table == NULL || outWriter == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (table->writerActive || table->readerCount > 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    table->writerActive = true;

    outWriter->table = table;
    outWriter->insertIndex = table->groupGapStart;
    outWriter->depth = 0;
    memset(outWriter->parentStack, 0, sizeof(outWriter->parentStack));

    return CELS_OK;
}

/**
 * Closes an active writer cursor.
 *
 * @param writer Target writer. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if unclosed groups remain.
 */
CelsResult
CelsSlotWriterClose(CelsSlotWriter *writer)
{
    if (writer == NULL || writer->table == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (writer->depth > 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    writer->table->writerActive = false;
    writer->table = NULL;

    return CELS_OK;
}

/**
 * Shifts both group and slot gaps in lockstep to targetLogicalIndex.
 *
 * @param table              Target table. Non-NULL.
 * @param targetLogicalIndex Target logical group index.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotTableMoveGapTo(CelsSlotTable *table,
                       uint32_t targetLogicalIndex)
{
    CELS_ASSERT(table != NULL);

    const uint32_t totalGroups = CelsSlotTableGroupCount(table);
    if (targetLogicalIndex > totalGroups) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    if (targetLogicalIndex == table->groupGapStart) {
        return CELS_OK;
    }

    if (targetLogicalIndex < table->groupGapStart) {
        // Gap moves LEFT: elements in [target, groupGapStart) shift to the right
        const uint32_t shiftGroups = table->groupGapStart - targetLogicalIndex;
        CelsSlotGroup *srcGroups = &table->groups[targetLogicalIndex];
        CelsSlotGroup *dstGroups =
            &table->groups[targetLogicalIndex + table->groupGapLen];

        // Find the target slot index corresponding to targetLogicalIndex
        const uint32_t targetSlotIndex = srcGroups[0].slotIndex;
        const uint32_t shiftSlots = table->slotGapStart - targetSlotIndex;
        CelsSlotValue *srcSlots = &table->slots[targetSlotIndex];
        CelsSlotValue *dstSlots =
            &table->slots[targetSlotIndex + table->slotGapLen];

        memmove(dstGroups, srcGroups, shiftGroups * sizeof(CelsSlotGroup));
        memmove(dstSlots, srcSlots, shiftSlots * sizeof(CelsSlotValue));

        // Adjust slotIndex for shifted groups moving to the right of the slot gap
        for (uint32_t i = 0; i < shiftGroups; i++) {
            dstGroups[i].slotIndex += table->slotGapLen;
        }

        memset(srcGroups, 0, shiftGroups * sizeof(CelsSlotGroup));
        memset(srcSlots, 0, shiftSlots * sizeof(CelsSlotValue));

        table->groupGapStart = targetLogicalIndex;
        table->slotGapStart = targetSlotIndex;
    } else {
        // Gap moves RIGHT: elements in [gapEnd, gapEnd + shiftGroups) shift to the left
        const uint32_t shiftGroups = targetLogicalIndex - table->groupGapStart;
        const uint32_t gapEnd = table->groupGapStart + table->groupGapLen;
        CelsSlotGroup *srcGroups = &table->groups[gapEnd];
        CelsSlotGroup *dstGroups = &table->groups[table->groupGapStart];

        // Total slots belonging to the shifted groups
        uint32_t shiftSlots = 0;
        for (uint32_t i = 0; i < shiftGroups; i++) {
            shiftSlots += (uint32_t)srcGroups[i].slotCount;
        }

        const uint32_t slotGapEnd = table->slotGapStart + table->slotGapLen;
        CelsSlotValue *srcSlots = &table->slots[slotGapEnd];
        CelsSlotValue *dstSlots = &table->slots[table->slotGapStart];

        memmove(dstGroups, srcGroups, shiftGroups * sizeof(CelsSlotGroup));
        memmove(dstSlots, srcSlots, shiftSlots * sizeof(CelsSlotValue));

        // Adjust slotIndex for shifted groups moving to the left of the slot gap
        for (uint32_t i = 0; i < shiftGroups; i++) {
            dstGroups[i].slotIndex -= table->slotGapLen;
        }

        memset(srcGroups, 0, shiftGroups * sizeof(CelsSlotGroup));
        memset(srcSlots, 0, shiftSlots * sizeof(CelsSlotValue));

        table->groupGapStart = targetLogicalIndex;
        table->slotGapStart += shiftSlots;
    }

    return CELS_OK;
}

/**
 * Shifts both group and slot gaps in lockstep to targetLogicalIndex.
 *
 * @param writer             Target writer. Non-NULL.
 * @param targetLogicalIndex Target logical group index.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotWriterGapMoveTo(CelsSlotWriter *writer,
                        uint32_t targetLogicalIndex)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth > 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    const CelsResult res =
        CelsSlotTableMoveGapTo(writer->table, targetLogicalIndex);
    if (res != CELS_OK) {
        return res;
    }

    writer->insertIndex = targetLogicalIndex;
    return CELS_OK;
}

/**
 * Starts a new group at the current writer insertion gap.
 *
 * @param writer        Target writer. Non-NULL.
 * @param key           Stable callsite key.
 * @param userData      Opaque caller word stored verbatim (or 0).
 * @param outGroupIndex Optional pointer receiving logical group index.
 * @return CELS_OK, CELS_ERROR_CAPACITY_EXCEEDED, or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotWriterGroupStart(CelsSlotWriter *writer,
                         uint64_t key,
                         uint64_t userData,
                         uint32_t *outGroupIndex)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth >= CELS_SLOT_WRITER_MAX_DEPTH) {
        return CELS_ERROR_CAPACITY_EXCEEDED;
    }

    CelsSlotTable *table = writer->table;
    if (table->groupGapLen == 0) {
        return CELS_ERROR_CAPACITY_EXCEEDED;
    }

    const uint32_t logicalIndex = table->groupGapStart;
    const uint32_t parentIndex = (writer->depth == 0)
        ? UINT32_MAX
        : writer->parentStack[writer->depth - 1];

    const CelsSlotGroup newGroup = {
        .key = key,
        .userData = userData,
        .parentIndex = parentIndex,
        .slotIndex = table->slotGapStart,
        .slotCount = 0,
        .groupSize = 0,
        .nodeCount = 0,
        .flags = 0
    };

    table->groups[table->groupGapStart] = newGroup;
    writer->parentStack[writer->depth] = logicalIndex;
    writer->depth++;

    table->groupGapStart++;
    table->groupGapLen--;
    writer->insertIndex = table->groupGapStart;

    if (outGroupIndex != NULL) {
        *outGroupIndex = logicalIndex;
    }

    return CELS_OK;
}

/**
 * Closes the currently active group, backfilling groupSize and propagating nodes.
 *
 * @param writer Target writer. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotWriterGroupEnd(CelsSlotWriter *writer)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    writer->depth--;
    const uint32_t groupIndex = writer->parentStack[writer->depth];

    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        writer->table, groupIndex, &phys);
    if (res != CELS_OK) {
        return res;
    }

    // groupSize is the number of groups nested inside this group
    const uint16_t subtreeGroups =
        (uint16_t)(writer->table->groupGapStart - groupIndex - 1u);
    writer->table->groups[phys].groupSize = subtreeGroups;

    // Propagate nodeCount up to the enclosing parent
    if (writer->depth > 0) {
        const uint32_t parentIndex = writer->parentStack[writer->depth - 1];
        uint32_t parentPhys = 0;
        const CelsResult parentRes = CelsSlotTableGroupToPhysicalIdx(
            writer->table, parentIndex, &parentPhys);
        if (parentRes != CELS_OK) {
            return parentRes;
        }
        writer->table->groups[parentPhys].nodeCount +=
            writer->table->groups[phys].nodeCount;
    }

    return CELS_OK;
}

/**
 * Emits materialized node counts into current open group.
 *
 * @param writer Target writer. Non-NULL.
 * @param count  Number of nodes emitted.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotWriterNodeEmit(CelsSlotWriter *writer,
                       uint16_t count)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    const uint32_t currentGroup = writer->parentStack[writer->depth - 1];
    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        writer->table, currentGroup, &phys);
    if (res != CELS_OK) {
        return res;
    }

    writer->table->groups[phys].nodeCount += count;
    return CELS_OK;
}

/**
 * Writes a 64-bit word slot value for the currently open group.
 *
 * @param writer       Target writer. Non-NULL.
 * @param value        64-bit word value.
 * @param outSlotIndex Optional pointer receiving logical slot index.
 * @return CELS_OK, CELS_ERROR_CAPACITY_EXCEEDED, or CELS_ERROR_INVALID_STATE.
 */
CelsResult
CelsSlotWriterSlotWrite(CelsSlotWriter *writer,
                        CelsSlotValue value,
                        uint32_t *outSlotIndex)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsSlotTable *table = writer->table;
    if (table->slotGapLen == 0) {
        return CELS_ERROR_CAPACITY_EXCEEDED;
    }

    const uint32_t currentGroup = writer->parentStack[writer->depth - 1];
    uint32_t phys = 0;
    const CelsResult res = CelsSlotTableGroupToPhysicalIdx(
        table, currentGroup, &phys);
    if (res != CELS_OK) {
        return res;
    }

    const uint32_t logicalSlot = table->slotGapStart;
    table->slots[table->slotGapStart] = value;
    table->slotGapStart++;
    table->slotGapLen--;

    table->groups[phys].slotCount++;

    if (outSlotIndex != NULL) {
        *outSlotIndex = logicalSlot;
    }

    return CELS_OK;
}

/**
 * Sets an existing slot value at logicalSlotIndex.
 *
 * @param writer           Target writer. Non-NULL.
 * @param logicalSlotIndex Target logical slot index.
 * @param value            New 64-bit word value.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotWriterSlotSet(CelsSlotWriter *writer,
                      uint32_t logicalSlotIndex,
                      CelsSlotValue value)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    CelsSlotTable *table = writer->table;
    const uint32_t totalSlots = CelsSlotTableSlotCount(table);
    if (logicalSlotIndex >= totalSlots) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    const uint32_t phys = (logicalSlotIndex < table->slotGapStart)
        ? logicalSlotIndex
        : (logicalSlotIndex + table->slotGapLen);

    table->slots[phys] = value;
    return CELS_OK;
}

/**
 * Skips the group at writer position and all its transitive children in O(1).
 *
 * @param writer           Target writer. Non-NULL.
 * @param outSkippedGroups Optional pointer receiving skipped group count.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult
CelsSlotWriterGroupSkip(CelsSlotWriter *writer,
                        uint32_t *outSkippedGroups)
{
    CELS_ASSERT(writer != NULL);
    CELS_ASSERT(writer->table != NULL);

    if (writer->depth > 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsSlotTable *table = writer->table;
    const uint32_t totalGroups = CelsSlotTableGroupCount(table);
    if (table->groupGapStart >= totalGroups) {
        return CELS_ERROR_INDEX_OUT_OF_BOUNDS;
    }

    const uint32_t gapEnd = table->groupGapStart + table->groupGapLen;
    const CelsSlotGroup *nextGroup = &table->groups[gapEnd];
    const uint32_t skipCount = 1u + (uint32_t)nextGroup->groupSize;

    const CelsResult res = CelsSlotWriterGapMoveTo(
        writer, table->groupGapStart + skipCount);
    if (res != CELS_OK) {
        return res;
    }

    if (outSkippedGroups != NULL) {
        *outSkippedGroups = skipCount;
    }

    return CELS_OK;
}
