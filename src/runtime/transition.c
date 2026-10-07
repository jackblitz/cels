#include "cels/runtime/transition.h"
#include "cels/runtime/thread.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* Standard Built-In Easing Functions                                        */
/* ========================================================================= */

/**
 * Linear easing curve with constant velocity throughout the transition.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Unmodified normalized progress value identical to t.
 */
float CelsEaseLinear(float t)
{
    return t;
}

/**
 * Quadratic ease-in curve that accelerates from zero initial velocity.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor computed as t^2.
 */
float CelsEaseInQuad(float t)
{
    return t * t;
}

/**
 * Quadratic ease-out curve that decelerates towards zero terminal velocity.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor computed as t * (2 - t).
 */
float CelsEaseOutQuad(float t)
{
    return t * (2.0f - t);
}

/**
 * Quadratic ease-in-out curve with acceleration up to midpoint and deceleration thereafter.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor smoothly transitioning between 0.0 and 1.0.
 */
float CelsEaseInOutQuad(float t)
{
    return (t < 0.5f) ? (2.0f * t * t) : (-1.0f + (4.0f - 2.0f * t) * t);
}

/**
 * Cubic ease-in curve that accelerates sharply from zero initial velocity.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor computed as t^3.
 */
float CelsEaseInCubic(float t)
{
    return t * t * t;
}

/**
 * Cubic ease-out curve that smoothly decelerates to rest at the target value.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor computed via inverted cubic curve.
 */
float CelsEaseOutCubic(float t)
{
    const float f = t - 1.0f;
    return f * f * f + 1.0f;
}

/**
 * Cubic ease-in-out curve accelerating until the halfway point and decelerating to the target.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor between 0.0 and 1.0.
 */
float CelsEaseInOutCubic(float t)
{
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    const float f = 2.0f * t - 2.0f;
    return 0.5f * f * f * f + 1.0f;
}

/**
 * Sinusoidal ease-in curve accelerating smoothly using trigonometric quarter-cosine.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor.
 */
float CelsEaseInSine(float t)
{
    return 1.0f - (float)cos(t * (M_PI / 2.0));
}

/**
 * Sinusoidal ease-out curve decelerating smoothly using trigonometric quarter-sine.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor.
 */
float CelsEaseOutSine(float t)
{
    return (float)sin(t * (M_PI / 2.0));
}

/**
 * Sinusoidal ease-in-out curve with symmetric trigonometric acceleration and deceleration.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Eased interpolation factor.
 */
float CelsEaseInOutSine(float t)
{
    return -0.5f * (float)(cos(M_PI * t) - 1.0);
}

/**
 * Bounce ease-out curve simulating decaying physical rebounds against the target value.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Bouncing interpolation factor settling cleanly at 1.0.
 */
float CelsEaseOutBounce(float t)
{
    const float n1 = 7.5625f;
    const float d1 = 2.75f;

    if (t < 1.0f / d1) {
        return n1 * t * t;
    }
    if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

/**
 * Back ease-out curve that overshoots the target value slightly before settling back.
 *
 * @param t Normalized elapsed time in the range [0.0, 1.0].
 * @return Overshooting interpolation factor.
 */
float CelsEaseOutBack(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float f = t - 1.0f;
    return 1.0f + c3 * f * f * f + c1 * f * f;
}

/* ========================================================================= */
/* Transition Step Evaluation                                                */
/* ========================================================================= */

/**
 * Updates or advances a temporal state transition for the current frame.
 *
 * Evaluates the elapsed monotonic time against the transition duration:
 * - On first mount (uninitialized): snaps current to targetVal and marks settled.
 * - On retarget mid-flight: resets startVal to current value, adjusts timestamp, and un-settles.
 * - While active: samples easing curve and computes linearly interpolated current value.
 * - Automatically registers session invalidation on group key until settled to drive frames.
 *
 * @param session    Owning or active session. Safe if NULL (resolves current session).
 * @param state      Pointer to persistent transition state storage. Non-NULL.
 * @param key        Unique 64-bit composable slot group key used for frame invalidation.
 * @param target     Target floating-point value to converge towards.
 * @param durationMs Transition duration in milliseconds.
 * @param easing     Easing function pointer. Defaults to CelsEaseOutQuad if NULL.
 * @return Current interpolated floating-point value for the active frame.
 */
float CelsTransitionStep(
    CelsSession *session,
    CelsTransitionState *state,
    uint64_t key,
    float target,
    uint32_t durationMs,
    CelsEasingFn easing)
{
    if (state == NULL) {
        return target;
    }
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (easing == NULL) {
        easing = CelsEaseOutQuad;
    }

    const uint64_t now = CelsGetTimeMs();

    if (!state->isInitialized) {
        state->current = target;
        state->startVal = target;
        state->targetVal = target;
        state->startTimeMs = now;
        state->durationMs = durationMs;
        state->easing = easing;
        state->isInitialized = true;
        state->isSettled = true;
        return target;
    }

    // Detect retargeting mid-flight
    if (state->targetVal != target) {
        state->startVal = state->current;
        state->targetVal = target;
        state->startTimeMs = now;
        state->durationMs = durationMs;
        state->easing = easing;
        state->isSettled = false;
    }

    if (state->isSettled) {
        return state->current;
    }

    const uint64_t elapsed = (now >= state->startTimeMs) ? (now - state->startTimeMs) : 0;
    if (state->durationMs == 0 || elapsed >= (uint64_t)state->durationMs) {
        state->current = state->targetVal;
        state->isSettled = true;
        return state->current;
    }

    float t = (float)elapsed / (float)state->durationMs;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    const float eased = state->easing(t);
    state->current = state->startVal + (state->targetVal - state->startVal) * eased;

    // Invalidation triggers recomposition pass on subsequent frame
    if (session != NULL && key != 0) {
        CelsSessionInvalidateKey(session, key);
    }

    return state->current;
}
