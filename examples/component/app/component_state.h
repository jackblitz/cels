#pragma once

/**
 * @file component_state.h
 * @brief Declarative ECS Components, Tags, and Reactive State for Entity Reconciliation.
 */

#include "cels.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Declarative ECS Components (Pure C99 data payloads)                       */
/* ========================================================================= */

/**
 * @struct Position
 * @brief Spatial coordinates in 3D world space.
 */
typedef struct Position {
    float x; /**< World X coordinate (4 bytes). */
    float y; /**< World Y coordinate (4 bytes). */
    float z; /**< World Z coordinate (4 bytes). */
} Position;

/**
 * @struct Health
 * @brief Authoritative actor vitality points.
 */
typedef struct Health {
    int current; /**< Current HP points (4 bytes). */
    int max;     /**< Maximum HP points (4 bytes). */
} Health;

/**
 * @struct EnergyShield
 * @brief Temporary rechargeable damage absorption barrier.
 */
typedef struct EnergyShield {
    float capacity;     /**< Total absorption points remaining (4 bytes). */
    float rechargeRate; /**< Points restored per second (4 bytes). */
} EnergyShield;

/**
 * @struct BurnDebuff
 * @brief Periodic damage-over-time status condition.
 */
typedef struct BurnDebuff {
    float dps;              /**< Damage inflicted per second (4 bytes). */
    float remainingSeconds; /**< Duration remaining before expiry (4 bytes). */
} BurnDebuff;

/* ========================================================================= */
/* Zero-Sized Tags                                                           */
/* ========================================================================= */

/**
 * @struct BossTag
 * @brief Zero-sized tag identifying boss entities.
 */
typedef struct BossTag { char _unused; } BossTag;

/**
 * @struct LowHealthAlert
 * @brief Zero-sized tag attached conditionally during intra-frame query (<25% HP).
 */
typedef struct LowHealthAlert { char _unused; } LowHealthAlert;

/* ========================================================================= */
/* Reactive Application State                                                */
/* Fields ordered in descending alignment (8B -> 4B -> 1B) for zero padding.  */
/* ========================================================================= */

/**
 * @brief Reactive application state driving the entity component reconciliation demo.
 */
CEL_State(CombatEntityState) {
    uint64_t entityId;       /**< 64-bit entity identity (8 bytes). */
    float    x;              /**< World X coordinate (4 bytes). */
    float    y;              /**< World Y coordinate (4 bytes). */
    float    z;              /**< World Z coordinate (4 bytes). */
    int      hp;             /**< Current health points (4 bytes). */
    int      maxHp;          /**< Max health points (4 bytes). */
    float    shieldCapacity; /**< Shield capacity (4 bytes). */
    float    burnDps;        /**< Burn damage per second (4 bytes). */
    bool     hasShield;      /**< Whether shield component should be attached (1 byte). */
    bool     isBurning;      /**< Whether burn debuff component should be attached (1 byte). */
    bool     isBoss;         /**< Whether boss tag should be attached (1 byte). */
    bool     isAlive;        /**< Whether actor is alive and mounted in hierarchy (1 byte). */
};

#ifdef __cplusplus
}
#endif
