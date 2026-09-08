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
    CELS_ERROR_INVALID_STATE,
    /** Recompose hit its drain-iteration bound; queue left intact. */
    CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE
} CelsResult;

/**
 * Returns a static string description of a CelsResult code.
 *
 * @param result Any CelsResult value.
 * @return Static, never-NULL null-terminated string.
 */
const char *CelsResultToString(CelsResult result);

/**
 * Stable identity of one composable, equal to its logical group index in the
 * owning CelsSlotTable.
 *
 * Ids are dense and small so callers can use them directly as array indices.
 * They are also REUSED: pruning reclaims a group's space immediately, so an id
 * held across recomposition passes may refer to a different composable later.
 * Anything retaining an id beyond the pass that produced it must tolerate that.
 */
typedef uint32_t CelsComposableId;

/** Sentinel for "no composable" / invalid identity. */
#define CELS_COMPOSABLE_ID_INVALID UINT32_MAX

/**
 * Owning composition host, defined in session.h.
 *
 * The typedef lives here, in the header every module already includes, because
 * C99 forbids repeating a typedef — declaring it in each header that needs the
 * incomplete type is a -Wpedantic error.
 */
typedef struct CelsCompositionHost CelsCompositionHost;

/**
 * Callbacks through which a Session reports composition changes.
 *
 * CELS models no component data, tags or payload, and does not decide when a
 * composable dies (Lifecycle does). It reports exactly two things, at the
 * moment they happen: a composable mounted, or a composable was pruned.
 * Assign either, both or neither — an unset callback is skipped.
 *
 * Declared in this header, rather than beside the Session that owns one,
 * because both the composer (which fires them) and the session (which holds
 * them) need the type, and neither should include the other's header.
 */
typedef struct CelsTransactionContext {
    /** Fires when a composable mounts, BEFORE that composable's body runs. */
    void (*onCreate)(CelsComposableId composable,
                     CelsComposableId parent,
                     uint32_t key,
                     void *userdata);
    /** Fires when a composable is pruned because it was not visited. */
    void (*onDestroy)(CelsComposableId composable, void *userdata);
    void *userdata;
} CelsTransactionContext;

/**
 * Per-group invalidation flags stored in CelsSlotGroup.flags.
 *
 * Composition walks DOWN from a host's root while invalidation arrives at a
 * LEAF from outside, so an invalidated composable must be able to defeat an
 * ancestor's O(1) subtree skip. CONTAINS_INVALIDATED is what carries that
 * signal upward; it is set on every group between an invalidated node and the
 * root when the invalidation queue is drained.
 */
typedef enum CelsGroupFlags {
    CELS_GROUP_FLAG_NONE = 0u,
    /** This composable's own body must re-run this pass. */
    CELS_GROUP_FLAG_INVALIDATED = 1u << 0,
    /** Some descendant is invalidated — this group must not be O(1)-skipped. */
    CELS_GROUP_FLAG_CONTAINS_INVALIDATED = 1u << 1
} CelsGroupFlags;

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
    uint64_t userData;     // Opaque caller word; CELS never interprets it
    uint32_t key;          // Stable callsite key / hash
    uint32_t parentIndex;  // Logical index of parent group (UINT32_MAX if root)
    uint32_t slotIndex;    // Physical/anchored index in slots array
    uint32_t aux;          // Auxiliary user tag / flags
    uint16_t slotCount;    // Number of word slots owned directly by this group
    uint16_t groupSize;    // Transitive child groups in subtree (for O(1) skip)
    uint16_t nodeCount;    // Caller-defined node tally for the subtree
    uint16_t flags;        // CelsGroupFlags invalidation bits
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

/* ========================================================================= */
/* Invalidation flags                                                        */
/* ========================================================================= */

/**
 * Reads the invalidation flags of one group.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param composable Logical group index [0, groupCount).
 * @return The group's CelsGroupFlags bitmask, or CELS_GROUP_FLAG_NONE if the
 *         table is NULL or the index is out of range.
 */
uint16_t CelsSlotTableGroupFlags(const CelsSlotTable *table,
                                 CelsComposableId composable);

/**
 * Marks one composable's body as needing to re-run, and marks every group
 * between it and the root as containing an invalidation.
 *
 * This is the whole of the upward-propagation step: CELS_GROUP_FLAG_INVALIDATED
 * on the target, CELS_GROUP_FLAG_CONTAINS_INVALIDATED on each ancestor reached
 * by following parentIndex. The cost is charged here, on the invalidating side,
 * precisely so the composition walk can stay a pure O(1) skip everywhere the
 * invalidation did not reach.
 *
 * Walking stops at a root (parentIndex == UINT32_MAX), at an ancestor that
 * already carries CONTAINS_INVALIDATED (its own ancestors are already marked),
 * or after groupCount steps as a cycle guard.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param composable Logical group index [0, groupCount).
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or
 *         CELS_ERROR_INDEX_OUT_OF_BOUNDS.
 */
CelsResult CelsSlotTableGroupInvalidate(CelsSlotTable *table,
                                        CelsComposableId composable);

/**
 * Clears both invalidation flags on one group.
 *
 * Called by the composition walk on entering a group whose body it is about to
 * run — the flags have served their purpose once the walk has committed to
 * descending.
 *
 * @param table Pointer to the CelsSlotTable. Non-NULL.
 * @param composable Logical group index [0, groupCount).
 */
void CelsSlotTableGroupClearFlags(CelsSlotTable *table,
                                  CelsComposableId composable);

/**
 * Clears the invalidation flags of every group in the table.
 *
 * @param table Pointer to the CelsSlotTable. NULL is accepted and ignored.
 */
void CelsSlotTableClearAllFlags(CelsSlotTable *table);

/**
 * Renumbers every parentIndex at or above a threshold by a signed delta.
 *
 * parentIndex names a LOGICAL group index, and inserting or removing groups
 * renumbers every logical index after the change point. A parentIndex left
 * naming its old number silently points at an unrelated group.
 *
 * That is not a cosmetic inconsistency: CelsSlotTableGroupInvalidate climbs
 * this chain to place CONTAINS_INVALIDATED, so a stale link puts the flag on
 * the wrong group, the walk O(1)-skips past the composable that actually
 * changed, and the state updates while the tree does not. It is precisely the
 * silent failure the flags exist to prevent. Every structural change must call
 * this — an inserting caller with delta +1, a removing caller with -count.
 *
 * The threshold is expressed in the numbering being left behind. Insertions
 * should call before advancing the gap; removals after widening it. In both
 * cases groups sitting inside the gap are skipped, so the group being inserted
 * and the groups being removed are naturally excluded.
 *
 * @param table     Pointer to the CelsSlotTable. NULL is accepted and ignored.
 * @param threshold Lowest logical parent index affected by the renumbering.
 * @param delta     Amount to add to each affected parentIndex. Zero is a no-op.
 */
void CelsSlotTableGroupsShiftParents(CelsSlotTable *table,
                                     uint32_t threshold,
                                     int32_t delta);

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
 * @param userData Opaque caller word stored verbatim on the group (or 0).
 * @param outGroupIndex Optional pointer receiving new logical group index.
 * @return CELS_OK or CELS_ERROR_CAPACITY_EXCEEDED.
 */
CelsResult CelsSlotWriterGroupStart(CelsSlotWriter *writer,
                                    uint32_t key,
                                    uint64_t userData,
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
