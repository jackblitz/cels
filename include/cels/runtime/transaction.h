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
 * @def cel_stage_set
 * @brief Stages a component or payload assignment transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_SET operation to the session's active transaction batch. Serializes
 * the provided struct value into the batch's linear byte arena without heap allocations
 * or mutex locks.
 *
 * Expected outcome:
 * The operation is buffered until the frame sync point or commit hook, where it will be
 * processed by external consumers (ECS world, render backend, physics engine).
 *
 * Where to use:
 * Call from within any CEL_Composable, CEL_Composition, or CEL_Task.
 *
 * Example:
 * @code
 *     cel_stage_set(entityId, Position, { .x = 10.0f, .y = 20.0f });
 * @endcode
 */
#ifndef cel_stage_set
#define cel_stage_set(targetId, Type, ...) \
    CelsSessionStageSet(CelsGetCurrentSession(), \
                        (uint64_t)(targetId), \
                        CelsHashKey(#Type), \
                        sizeof(Type), \
                        &(Type)__VA_ARGS__)
#endif

/**
 * @def cel_stage_remove
 * @brief Stages a component removal or tag detachment transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_REMOVE operation identifying the targetId and component type key
 * to the session's transaction staging batch.
 *
 * Expected outcome:
 * At the transaction commit point, the handler is notified to remove or unbind the
 * component from the target entity.
 *
 * Where to use:
 * Call inside composables or tasks when conditionally shedding components or tags.
 *
 * Example:
 * @code
 *     cel_stage_remove(entityId, RigidBody);
 * @endcode
 */
#ifndef cel_stage_remove
#define cel_stage_remove(targetId, Type) \
    CelsSessionStageRemove(CelsGetCurrentSession(), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type))
#endif

/**
 * @def cel_stage_delete
 * @brief Stages a target deletion or destruction transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_DELETE operation targeting targetId to the session's transaction batch.
 *
 * Expected outcome:
 * At the frame sync point, the backend consumer destroys the associated entity or resource.
 *
 * Where to use:
 * Inside composables or tasks when an entity should be despawned or destroyed.
 *
 * Example:
 * @code
 *     cel_stage_delete(deadEnemyId);
 * @endcode
 */
#ifndef cel_stage_delete
#define cel_stage_delete(targetId) \
    CelsSessionStageDelete(CelsGetCurrentSession(), (uint64_t)(targetId))
#endif

/**
 * @def cel_stage_custom
 * @brief Stages a custom user-defined transaction operation with a typed payload.
 *
 * What it does:
 * Enqueues an application-defined opcode with a custom struct payload into the transaction batch.
 *
 * Expected outcome:
 * Allows arbitrary cross-thread or cross-subsystem messages (e.g. audio triggers, networking
 * packets, draw commands) to be staged locklessly during recomposition.
 *
 * Where to use:
 * Inside composables or tasks for domain-specific operations beyond SET/REMOVE/DELETE.
 *
 * Example:
 * @code
 *     cel_stage_custom(OP_PLAY_AUDIO, soundId, AudioParams, { .volume = 0.8f, .pitch = 1.0f });
 * @endcode
 */
#ifndef cel_stage_custom
#define cel_stage_custom(opCode, targetId, Type, ...) \
    CelsSessionStageCustom(CelsGetCurrentSession(), \
                           (uint32_t)(opCode), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type), \
                           sizeof(Type), \
                           &(Type)__VA_ARGS__)
#endif

/**
 * @def cel_user_data
 * @brief Retrieves the ambient session's attached user context pointer.
 *
 * What it does:
 * Queries CelsGetCurrentSession() and retrieves its arbitrary userData pointer,
 * casting it to `Type*`.
 *
 * Expected outcome:
 * Returns the application-level context object configured during session initialization
 * or composition attachment.
 *
 * Where to use:
 * Inside composables needing access to application-wide non-reactive context (e.g. host pointers,
 * native window handles, asset managers).
 *
 * Example:
 * @code
 *     AppContext *ctx = cel_user_data(AppContext);
 * @endcode
 */
#ifndef cel_user_data
#define cel_user_data(Type) \
    ((Type*)CelsSessionGetUserData(CelsGetCurrentSession()))
#endif

/**
 * @def cel_session_user_data
 * @brief Retrieves an explicit session's attached user context pointer.
 *
 * What it does:
 * Reads the userData pointer from a specific CelsSession instance and casts it to `Type*`.
 *
 * Expected outcome:
 * Returns the userData pointer associated with the given session.
 *
 * Where to use:
 * In multi-session hosts, background tasks, or callback hooks where the session pointer
 * is already available.
 */
#ifndef cel_session_user_data
#define cel_session_user_data(session, Type) \
    ((Type*)CelsSessionGetUserData(session))
#endif

#ifdef __cplusplus
}
#endif
