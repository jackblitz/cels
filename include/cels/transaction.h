#pragma once

/**
 * @file transaction.h
 * @brief Transaction Batch and Cross-Thread Command Buffer for CELS.
 *
 * Provides a high-performance staging pipeline for external mutations (ECS,
 * Vulkan command buffers, audio queues, physics impulses). Composables and
 * tasks stage mutations during recomposition without taking locks or causing
 * cache thrashing. At pipeline sync points (or post-recomposition hooks), the
 * batch is atomically committed or swapped to worker threads with zero contention.
 *
 * Typical usage:
 * @code
 *     // 1. Stage mutations inside composables without acquiring locks
 *     CEL_Composable(SpawnEnemies, int count) {
 *         for (int i = 0; i < count; ++i) {
 *             cel_stage_set(enemyId, Position, { .x = 100.0f, .y = 50.0f });
 *         }
 *     }
 *
 *     // 2. Consume transactions at sync point in host event loop
 *     void FrameCommitHook(CelsSession *session, void *userData) {
 *         CelsSessionCommitTransactions(session, DispatchToBackend, userData);
 *     }
 * @endcode
 *
 * Thread safety: Transaction staging (`cel_stage_*`) is executed from the
 * session's composition thread. Double-buffered batches (`CelsSessionSwapTransactionBatches`)
 * permit worker threads to process ready batches concurrently while the composition
 * thread populates the active batch.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of operations per transaction batch. */
#ifndef CELS_MAX_TRANSACTIONS
#define CELS_MAX_TRANSACTIONS 1024u
#endif

/** Byte capacity of the transaction payload arena per batch. */
#ifndef CELS_TRANSACTION_DATA_SIZE
#define CELS_TRANSACTION_DATA_SIZE (32u * 1024u)
#endif

/**
 * Operation codes for staged transactions.
 * Values above CELS_OP_CUSTOM are reserved for user-defined operations.
 */
typedef enum CelsOpCode {
    CELS_OP_SET    = 1, /**< Set or add a component/data payload on a target */
    CELS_OP_REMOVE = 2, /**< Remove a component/tag from a target */
    CELS_OP_DELETE = 3, /**< Destroy/delete a target entity or resource */
    CELS_OP_CUSTOM = 4  /**< Base opcode for application-defined custom transactions */
} CelsOpCode;

/**
 * Single staged transaction record within a batch.
 */
typedef struct CelsTransactionOp {
    uint32_t opCode;     /**< CelsOpCode or user-defined opcode identifier */
    uint32_t size;       /**< Size of payload data in bytes (0 if none) */
    uint64_t targetId;   /**< Target entity or resource identifier */
    uint64_t typeKey;    /**< 64-bit component or payload type identifier */
    uint32_t dataOffset; /**< Byte offset into batch linear data arena */
} CelsTransactionOp;

/**
 * Linear byte-arena transaction batch staging buffer.
 */
typedef struct CelsTransactionBatch {
    CelsTransactionOp ops[CELS_MAX_TRANSACTIONS];  /**< Array of staged operation records */
    uint32_t opCount;                             /**< Number of operations currently staged */
    uint8_t  data[CELS_TRANSACTION_DATA_SIZE];    /**< Contiguous arena for payload bytes */
    uint32_t dataSize;                            /**< Number of payload bytes currently stored */
} CelsTransactionBatch;

/**
 * Callback function invoked for each staged operation during batch commit.
 *
 * @param opCode   Operation code (CELS_OP_SET, CELS_OP_REMOVE, CELS_OP_DELETE, etc.).
 * @param targetId Target entity or resource identifier.
 * @param typeKey  64-bit component or payload type key (e.g. FNV-1a hash of type name).
 * @param data     Pointer to payload data bytes in the batch arena, or NULL if size is 0.
 * @param size     Payload size in bytes.
 * @param userData Context pointer passed to CelsSessionCommitTransactions.
 */
typedef void (*CelsTransactionHandler)(CelsOpCode opCode,
                                       uint64_t targetId,
                                       uint64_t typeKey,
                                       const void *data,
                                       size_t size,
                                       void *userData);

/* ========================================================================= */
/* High-Level Staging DSL Macros                                             */
/* ========================================================================= */

/**
 * Stages a component/data set operation on targetId in the active session.
 *
 * Example:
 * @code
 *     cel_stage_set(entityId, Position, { .x = 10.0f, .y = 20.0f });
 * @endcode
 */
#define cel_stage_set(targetId, Type, ...) \
    CelsSessionStageSet(CelsGetCurrentSession(), \
                        (uint64_t)(targetId), \
                        CelsHashKey(#Type), \
                        sizeof(Type), \
                        &(Type)__VA_ARGS__)

/**
 * Stages a component removal operation from targetId in the active session.
 *
 * Example:
 * @code
 *     cel_stage_remove(entityId, RigidBody);
 * @endcode
 */
#define cel_stage_remove(targetId, Type) \
    CelsSessionStageRemove(CelsGetCurrentSession(), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type))

/**
 * Stages a target deletion/destruction operation in the active session.
 *
 * Example:
 * @code
 *     cel_stage_delete(entityId);
 * @endcode
 */
#define cel_stage_delete(targetId) \
    CelsSessionStageDelete(CelsGetCurrentSession(), (uint64_t)(targetId))

/**
 * Stages a custom opcode transaction on targetId with a typed payload.
 *
 * Example:
 * @code
 *     cel_stage_custom(OP_PLAY_AUDIO, soundId, AudioParams, { .volume = 0.8f });
 * @endcode
 */
#define cel_stage_custom(opCode, targetId, Type, ...) \
    CelsSessionStageCustom(CelsGetCurrentSession(), \
                           (uint32_t)(opCode), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type), \
                           sizeof(Type), \
                           &(Type)__VA_ARGS__)

/**
 * Retrieves the ambient session's userData pointer cast to Type*.
 */
#define cel_user_data(Type) \
    ((Type*)CelsSessionGetUserData(CelsGetCurrentSession()))

/**
 * Retrieves an explicit session's userData pointer cast to Type*.
 */
#define cel_session_user_data(session, Type) \
    ((Type*)CelsSessionGetUserData(session))

#ifdef __cplusplus
}
#endif
