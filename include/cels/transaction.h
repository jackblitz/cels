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
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef CELS_MAX_TRANSACTIONS
#define CELS_MAX_TRANSACTIONS 1024u
#endif

#ifndef CELS_TRANSACTION_DATA_SIZE
#define CELS_TRANSACTION_DATA_SIZE (32u * 1024u)
#endif

/**
 * Common operation codes for staged transactions.
 * Developers can define custom opcodes beyond CELS_OP_CUSTOM.
 */
typedef enum CelsOpCode {
    CELS_OP_SET    = 1, /**< Set or add a component/data payload on a target */
    CELS_OP_REMOVE = 2, /**< Remove a component/tag from a target */
    CELS_OP_DELETE = 3, /**< Destroy/delete a target entity or resource */
    CELS_OP_CUSTOM = 4  /**< Application-defined custom transaction op */
} CelsOpCode;

/**
 * Single staged transaction record.
 */
typedef struct CelsTransactionOp {
    uint32_t opCode;     /**< CelsOpCode or user-defined opcode */
    uint32_t size;       /**< Payload size in bytes */
    uint64_t targetId;   /**< Target entity or resource ID */
    uint64_t typeKey;    /**< 64-bit component or payload type identifier */
    uint32_t dataOffset; /**< Offset in batch data arena */
} CelsTransactionOp;

/**
 * Linear byte-arena transaction batch.
 */
typedef struct CelsTransactionBatch {
    CelsTransactionOp ops[CELS_MAX_TRANSACTIONS];
    uint32_t opCount;
    uint8_t  data[CELS_TRANSACTION_DATA_SIZE];
    uint32_t dataSize;
} CelsTransactionBatch;

/**
 * Callback invoked for each transaction during batch commit.
 */
typedef void (*CelsTransactionHandler)(CelsOpCode opCode,
                                       uint64_t targetId,
                                       uint64_t typeKey,
                                       const void *data,
                                       size_t size,
                                       void *userData);

/* ========================================================================= */
/* High-Level Staging DSL                                                    */
/* ========================================================================= */

/**
 * Stages a component/data set operation on targetId.
 *
 * Example:
 * @code
 *     cel_stage_set(it, Position, { .x = 10.0f, .y = 20.0f });
 * @endcode
 */
#define cel_stage_set(targetId, Type, ...) \
    CelsSessionStageSet(CelsGetCurrentSession(), \
                        (uint64_t)(targetId), \
                        CelsHashKey(#Type), \
                        sizeof(Type), \
                        &(Type)__VA_ARGS__)

/**
 * Stages a component removal operation from targetId.
 *
 * Example:
 * @code
 *     cel_stage_remove(it, RigidBody);
 * @endcode
 */
#define cel_stage_remove(targetId, Type) \
    CelsSessionStageRemove(CelsGetCurrentSession(), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type))

/**
 * Stages a target deletion/destruction operation.
 *
 * Example:
 * @code
 *     cel_stage_delete(it);
 * @endcode
 */
#define cel_stage_delete(targetId) \
    CelsSessionStageDelete(CelsGetCurrentSession(), (uint64_t)(targetId))

/**
 * Stages a custom opcode transaction on targetId with typed payload.
 */
#define cel_stage_custom(opCode, targetId, Type, ...) \
    CelsSessionStageCustom(CelsGetCurrentSession(), \
                           (uint32_t)(opCode), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type), \
                           sizeof(Type), \
                           &(Type)__VA_ARGS__)

/**
 * Retrieves ambient session userData pointer cast to Type*.
 */
#define cel_user_data(Type) \
    ((Type*)CelsSessionGetUserData(CelsGetCurrentSession()))

/**
 * Retrieves session userData pointer from an explicit session.
 */
#define cel_session_user_data(session, Type) \
    ((Type*)CelsSessionGetUserData(session))
