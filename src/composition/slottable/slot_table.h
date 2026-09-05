#pragma once

/**
 * @file slot_table.h
 * @brief Cache-aligned hierarchical dual gap buffer for composition state.
 *
 * Typical usage:
 * @code
 *     #if defined(_MSC_VER)
 *     __declspec(align(64)) uint8_t slab[4096];
 *     #else
 *     __attribute__((aligned(64))) uint8_t slab[4096];
 *     #endif
 *
 *     CelsSlotTable table;
 *     CelsResult result = CelsSlotTableInit(&table, slab, sizeof(slab), 64);
 *     if (result != CELS_OK) {
 *         fprintf(stderr, "%s\n", CelsResultToString(result));
 *         return result;
 *     }
 *
 *     CelsSlotWriter writer;
 *     result = CelsSlotTableWriterOpen(&table, &writer);
 *     if (result != CELS_OK) return result;
 *
 *     uint32_t rootGroup = 0;
 *     result = CelsSlotWriterGroupStart(&writer, 0x1001, 0, &rootGroup);
 *     if (result != CELS_OK) return result;
 *
 *     result = CelsSlotWriterSlotWrite(&writer, 42, NULL);
 *     if (result != CELS_OK) return result;
 *
 *     result = CelsSlotWriterGroupEnd(&writer);
 *     if (result != CELS_OK) return result;
 *
 *     result = CelsSlotWriterClose(&writer);
 *     if (result != CELS_OK) return result;
 * @endcode
 *
 * Thread safety: CelsSlotTable is not internally synchronised. Concurrent
 * reads using multiple CelsSlotReader instances are safe; any mutation through
 * a CelsSlotWriter requires exclusive access.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CELS_CACHE_LINE_SIZE 64u
#define CELS_SLOT_WRITER_MAX_DEPTH 64u

/**
 * Result codes for all fallible operations in the composition module.
 */
typedef enum CelsResult {
    CELS_OK = 0,
    CELS_ERROR_INVALID_ARGUMENT,
    CELS_ERROR_OUT_OF_MEMORY,
    CELS_ERROR_CAPACITY_EXCEEDED,
    CELS_ERROR_INDEX_OUT_OF_BOUNDS,
    CELS_ERROR_INVALID_STATE
} CelsResult;

/**
 * Returns a static string description of a CelsResult code.
 *
 * @param result Any CelsResult value.
 * @return Static, never-NULL null-terminated string.
 */
const char *CelsResultToString(CelsResult result);

/**
 * 64-bit word slot value. Can hold an integer, double, handle, or pointer.
 */
typedef uint64_t CelsSlotValue;

/**
 * Structural metadata for a single group in the composition hierarchy.
 * Ordered largest to smallest: 64-bit -> 32-bit -> 16-bit to eliminate padding.
 * Total size is exactly 32 bytes (2 groups per 64-byte cache line).
 */
typedef struct CelsSlotGroup {
    uint64_t entityId;     // Associated ECS / Flecs entity ID (0 if none)
    uint32_t key;          // Stable callsite key / hash
    uint32_t parentIndex;  // Logical index of parent group (UINT32_MAX if root)
    uint32_t slotIndex;    // Physical/anchored index in slots array
    uint32_t aux;          // Auxiliary user tag / flags
    uint16_t slotCount;    // Number of word slots owned directly by this group
    uint16_t groupSize;    // Transitive child groups in subtree (for O(1) skip)
    uint16_t nodeCount;    // Materialized ECS/UI nodes in subtree
    uint16_t flags;        // Reserved lifecycle / dirty flags
} CelsSlotGroup;

/**
 * Dual gap buffer carved out of a single contiguous, cache-aligned memory slab.
 */
typedef struct CelsSlotTable {
    CelsSlotGroup *groups;
    CelsSlotValue *slots;
    uint32_t groupCapacity;
    uint32_t groupGapStart;
    uint32_t groupGapLen;
    uint32_t slotCapacity;
    uint32_t slotGapStart;
    uint32_t slotGapLen;
    uint32_t readerCount;
    bool writerActive;
} CelsSlotTable;

/**
 * Read-only cursor for traversing composition hierarchy and cached state.
 */
typedef struct CelsSlotReader {
    const CelsSlotTable *table;
    uint32_t currentGroup;
    uint32_t currentSlot;
    uint32_t currentParent;
    uint32_t groupEnd;
} CelsSlotReader;

/**
 * Mutating cursor for inserting, updating, and removing composition state.
 */
typedef struct CelsSlotWriter {
    CelsSlotTable *table;
    uint32_t insertIndex;
    uint32_t depth;
    uint32_t parentStack[CELS_SLOT_WRITER_MAX_DEPTH];
} CelsSlotWriter;

/**
 * Carves a CelsSlotTable out of a single contiguous, 64-byte aligned slab.
 * Performs zero heap allocations.
 *
 * @param table Pointer to the CelsSlotTable struct to initialize. Non-NULL.
 * @param slabMemory Pointer to 64-byte aligned memory. Non-NULL.
 * @param slabSize Total size of the slab buffer in bytes.
 * @param maxGroups Number of group slots to allocate at base of slab.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_OUT_OF_MEMORY.
 */
CelsResult CelsSlotTableInit(CelsSlotTable *table,
                             void *slabMemory,
                             size_t slabSize,
                             uint32_t maxGroups);

/**
 * Resets an existing CelsSlotTable to empty without reallocating slab memory.
 * Fails if any reader or writer is currently active.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsSlotTableReset(CelsSlotTable *table);

/**
 * Returns the total active group count (excluding gap space).
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @return Total number of active groups.
 */
uint32_t CelsSlotTableGroupCount(const CelsSlotTable *table);

/**
 * Returns the total active slot count (excluding gap space).
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @return Total number of active slots.
 */
uint32_t CelsSlotTableSlotCount(const CelsSlotTable *table);

/**
 * Returns the total group capacity allocated in the slab.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @return Total group capacity.
 */
uint32_t CelsSlotTableGroupCapacity(const CelsSlotTable *table);

/**
 * Returns the total word slot capacity allocated in the slab.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @return Total slot capacity in 64-bit words.
 */
uint32_t CelsSlotTableSlotCapacity(const CelsSlotTable *table);

/**
 * Translates a continuous logical group index into its physical array index.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param logicalIndex Logical element index [0, groupCount).
 * @param outPhysicalIndex Receives the translated physical array index.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotTableGroupToPhysicalIdx(const CelsSlotTable *table,
                                          uint32_t logicalIndex,
                                          uint32_t *outPhysicalIndex);

/**
 * Searches active groups in the table for a group matching the given key.
 *
 * @param table         Pointer to the CelsSlotTable. Non-NULL.
 * @param key           Key or hashed name to locate.
 * @param outLogicalIdx Optional destination to receive the logical group index.
 * @return Pointer to matching CelsSlotGroup in table, or NULL if not found.
 */
CelsSlotGroup *CelsSlotTableFindGroup(const CelsSlotTable *table,
                                     uint32_t key,
                                     uint32_t *outLogicalIdx);

/**
 * Opens a read-only reader on the table.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param outReader Receives the initialized reader cursor. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if a writer is active.
 */
CelsResult CelsSlotTableReaderOpen(const CelsSlotTable *table,
                                   CelsSlotReader *outReader);

/**
 * Closes an active reader.
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsSlotReaderClose(CelsSlotReader *reader);

/**
 * Enters the next group in traversal order.
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @param outGroup Optional pointer to receive group metadata.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS if at end.
 */
CelsResult CelsSlotReaderGroupStart(CelsSlotReader *reader,
                                    CelsSlotGroup *outGroup);

/**
 * Leaves the current group, restoring reader boundary to enclosing parent.
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsSlotReaderGroupEnd(CelsSlotReader *reader);

/**
 * Skips the current group and all its transitive children in O(1).
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS if at end.
 */
CelsResult CelsSlotReaderGroupSkip(CelsSlotReader *reader);

/**
 * Reads the next slot value belonging to the current group.
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @param outValue Receives the 64-bit slot value. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS if group slots exhausted.
 */
CelsResult CelsSlotReaderSlotRead(CelsSlotReader *reader,
                                  CelsSlotValue *outValue);

/**
 * Random-access read of a group by its logical index.
 *
 * @param reader Pointer to the CelsSlotReader. Non-NULL.
 * @param logicalIndex Logical group index.
 * @param outGroup Receives group metadata. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotReaderGroupGet(const CelsSlotReader *reader,
                                  uint32_t logicalIndex,
                                  CelsSlotGroup *outGroup);

/**
 * Opens a mutating writer on the table.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param outWriter Receives the initialized writer cursor. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if readers or writer active.
 */
CelsResult CelsSlotTableWriterOpen(CelsSlotTable *table,
                                   CelsSlotWriter *outWriter);

/**
 * Closes an active writer, verifying all groups were closed properly.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if open groups remain.
 */
CelsResult CelsSlotWriterClose(CelsSlotWriter *writer);

/**
 * Shifts the group gap and slot gap in lockstep to targetLogicalIndex.
 *
 * Table-level primitive shared by CelsSlotWriterGapMoveTo (which additionally
 * enforces the no-open-group writer invariant) and the composer's diffing
 * engine, which moves the gap directly without a CelsSlotWriter.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param targetLogicalIndex Target logical group index.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotTableMoveGapTo(CelsSlotTable *table,
                                  uint32_t targetLogicalIndex);

/**
 * Shifts the group gap and slot gap to the specified logical index.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param targetLogicalIndex Target logical group index.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotWriterGapMoveTo(CelsSlotWriter *writer,
                                   uint32_t targetLogicalIndex);

/**
 * Begins insertion of a new group at the current writer gap.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param key Stable callsite key.
 * @param entityId Associated ECS entity ID (or 0).
 * @param outGroupIndex Optional pointer receiving new logical group index.
 * @return CELS_OK or CELS_ERROR_CAPACITY_EXCEEDED.
 */
CelsResult CelsSlotWriterGroupStart(CelsSlotWriter *writer,
                                    uint32_t key,
                                    uint64_t entityId,
                                    uint32_t *outGroupIndex);

/**
 * Closes the currently open group, backfilling subtree groupSize.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if no open group.
 */
CelsResult CelsSlotWriterGroupEnd(CelsSlotWriter *writer);

/**
 * Emits materialized node counts into current open group and its ancestors.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param count Number of nodes emitted.
 * @return CELS_OK or CELS_ERROR_INVALID_STATE if no open group.
 */
CelsResult CelsSlotWriterNodeEmit(CelsSlotWriter *writer,
                                  uint16_t count);

/**
 * Writes a 64-bit slot value into the slot gap for current group.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param value 64-bit word to write.
 * @param outSlotIndex Optional pointer receiving logical slot index.
 * @return CELS_OK, CELS_ERROR_CAPACITY_EXCEEDED, or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsSlotWriterSlotWrite(CelsSlotWriter *writer,
                                   CelsSlotValue value,
                                   uint32_t *outSlotIndex);

/**
 * In-place update of an existing slot value.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param logicalSlotIndex Target logical slot index.
 * @param value New 64-bit word value.
 * @return CELS_OK or CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotWriterSlotSet(CelsSlotWriter *writer,
                                 uint32_t logicalSlotIndex,
                                 CelsSlotValue value);

/**
 * Skips the group at current writer position and all its children.
 *
 * @param writer Pointer to the CelsSlotWriter. Non-NULL.
 * @param outSkippedGroups Optional pointer receiving count of skipped groups.
 * @return CELS_OK, CELS_ERROR_INDEX_OUT_OF_BOUNDS, or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsSlotWriterGroupSkip(CelsSlotWriter *writer,
                                   uint32_t *outSkippedGroups);
