#pragma once

/**
 * @file transition.h
 * @brief Declarative Temporal Value Transitions & Convergence for CELS.
 *
 * Provides slot-memory backed temporal value transitions for CELS composables:
 * - Continuous, eased convergence towards target values over time.
 * - Ephemeral presentation smoothing without polluting authoritative game state.
 * - Automatic session recomposition invalidation while transitioning.
 * - Zero CPU overhead once settled (drops to 0% idle when converged).
 * - Automatic slot memory cleanup when composables unmount.
 *
 * Typical usage:
 * @code
 *     CEL_Composable(HealthBar, void *ctx) {
 *         const PlayerState *p = cel_watch(PlayerState, CEL_Player);
 *         if (!p) return;
 *
 *         // Smoothly transition display health to target over 300ms
 *         float displayHealth = cel_transition(p->health, 300, CEL_EASE_OUT_QUAD);
 *         RenderHealthBar(displayHealth, p->maxHealth);
 *     }
 * @endcode
 *
 * Thread safety: Transitions operate strictly on session slot memory and must
 * execute on the session's active recomposition thread. Concurrent reads or
 * mutations across threads must be mediated by CelsSession transactions.
 */

#include "cels/runtime/session.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Easing function prototype.
 *
 * @param t Normalized progress in [0.0, 1.0].
 * @return Eased progress, typically in [0.0, 1.0].
 */
typedef float (*CelsEasingFn)(float t);

/* ========================================================================= */
/* Standard Built-In Easing Functions                                        */
/* ========================================================================= */

float CelsEaseLinear(float t);
float CelsEaseInQuad(float t);
float CelsEaseOutQuad(float t);
float CelsEaseInOutQuad(float t);
float CelsEaseInCubic(float t);
float CelsEaseOutCubic(float t);
float CelsEaseInOutCubic(float t);
float CelsEaseInSine(float t);
float CelsEaseOutSine(float t);
float CelsEaseInOutSine(float t);
float CelsEaseOutBounce(float t);
float CelsEaseOutBack(float t);

#define CEL_EASE_LINEAR       CelsEaseLinear
#define CEL_EASE_IN_QUAD      CelsEaseInQuad
#define CEL_EASE_OUT_QUAD     CelsEaseOutQuad
#define CEL_EASE_IN_OUT_QUAD  CelsEaseInOutQuad
#define CEL_EASE_IN_CUBIC     CelsEaseInCubic
#define CEL_EASE_OUT_CUBIC    CelsEaseOutCubic
#define CEL_EASE_IN_OUT_CUBIC CelsEaseInOutCubic
#define CEL_EASE_IN_SINE      CelsEaseInSine
#define CEL_EASE_OUT_SINE     CelsEaseOutSine
#define CEL_EASE_IN_OUT_SINE  CelsEaseInOutSine
#define CEL_EASE_OUT_BOUNCE   CelsEaseOutBounce
#define CEL_EASE_OUT_BACK     CelsEaseOutBack

/* ========================================================================= */
/* Transition State & Evaluation API                                         */
/* ========================================================================= */

/**
 * State struct retained in CELS slot memory for a single transition.
 * Fields ordered largest-to-smallest for optimal alignment and 32-byte packing.
 */
typedef struct CelsTransitionState {
    uint64_t     startTimeMs;    /**< Timestamp when transition was initiated (8 B) */
    CelsEasingFn easing;         /**< Active easing curve callback (8 B pointer) */
    float        current;        /**< Current interpolated value (4 B) */
    float        startVal;       /**< Value when current transition started (4 B) */
    float        targetVal;      /**< Destination target value (4 B) */
    uint32_t     durationMs;     /**< Transition duration in milliseconds (4 B) */
    bool         isInitialized;  /**< True once initial value has been established (1 B) */
    bool         isSettled;      /**< True when current == targetVal and idle (1 B) */
} CelsTransitionState;

/**
 * Updates or advances a transition in the active session.
 *
 * Where to use: Called internally by the cel_transition macro or directly when
 * managing an explicit session and slot key.
 *
 * @param session    Active session. Safe if NULL (falls back to current).
 * @param state      Pointer to persistent state struct in slot memory. Non-NULL.
 * @param key        Unique 64-bit key of the enclosing composable for invalidation.
 * @param target     Target destination value.
 * @param durationMs Duration in milliseconds to reach target.
 * @param easing     Easing function (defaults to CEL_EASE_OUT_QUAD if NULL).
 * @return Current interpolated value.
 */
float CelsTransitionStep(
    CelsSession *session,
    CelsTransitionState *state,
    uint64_t key,
    float target,
    uint32_t durationMs,
    CelsEasingFn easing
);

/* ========================================================================= */
/* Declarative cel_transition DSL Macros                                     */
/* ========================================================================= */

#define _CEL_TRANS_2(target, durationMs) \
    CelsTransitionStep( \
        CelsGetCurrentSession(), \
        cel_remember(CelsTransitionState, ((CelsTransitionState){0})), \
        CelsSessionGetCurrentGroupKey(CelsGetCurrentSession()), \
        (float)(target), \
        (uint32_t)(durationMs), \
        CelsEaseOutQuad \
    )

#define _CEL_TRANS_3(target, durationMs, easing) \
    CelsTransitionStep( \
        CelsGetCurrentSession(), \
        cel_remember(CelsTransitionState, ((CelsTransitionState){0})), \
        CelsSessionGetCurrentGroupKey(CelsGetCurrentSession()), \
        (float)(target), \
        (uint32_t)(durationMs), \
        (easing) \
    )

#define _CEL_GET_TRANS_MACRO(_1, _2, _3, NAME, ...) NAME

/**
 * @def cel_transition
 * @brief Declaratively transitions a scalar value toward a target over time.
 *
 * 1. What it does exactly:
 * Allocates or retrieves a persistent CelsTransitionState struct inside the
 * calling composable's slot-table memory via cel_remember(). Computes the
 * current interpolated value based on monotonic elapsed time and the selected
 * easing curve. Automatically invalidates the enclosing composable group on
 * subsequent frames while in motion, and automatically ceases invalidation
 * once the target value settles. Supports seamless in-flight retargeting with
 * zero visual pops.
 *
 * 2. Expected outcome:
 * Returns the smoothed float value for the current frame. While converging,
 * schedules the session for continuous next-frame recomposition. Once converged,
 * drops to 0% CPU idle without triggering further recompositions.
 *
 * 3. Where to use it:
 * Inside any active CEL_Composable body during a recomposition pass.
 *
 * 4. Usage example:
 * @code
 *     CEL_Composable(PlayerHealthHUD) {
 *         const PlayerState *p = cel_watch(PlayerState, CEL_Player);
 *         if (!p) return;
 *
 *         // Smoothly ease visual bar toward authoritative health over 400ms
 *         float visualHealth = cel_transition(p->currentHealth, 400, CEL_EASE_OUT_QUAD);
 *         DrawHealthBar(visualHealth, p->maxHealth);
 *     }
 * @endcode
 */
#ifndef cel_transition
#define cel_transition(...) \
    _CEL_GET_TRANS_MACRO(__VA_ARGS__, _CEL_TRANS_3, _CEL_TRANS_2, _UNUSED)(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
