#include "cels/runtime/transition.h"
#include "cels/runtime/thread.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* Standard Built-In Easing Functions                                        */
/* ========================================================================= */

/** Linear easing: constant velocity. */
float CelsEaseLinear(float t)
{
    return t;
}

/** Quadratic ease-in: accelerating from zero velocity. */
float CelsEaseInQuad(float t)
{
    return t * t;
}

/** Quadratic ease-out: decelerating to zero velocity. */
float CelsEaseOutQuad(float t)
{
    return t * (2.0f - t);
}

/** Quadratic ease-in-out: acceleration until midpoint, then deceleration. */
float CelsEaseInOutQuad(float t)
{
    return (t < 0.5f) ? (2.0f * t * t) : (-1.0f + (4.0f - 2.0f * t) * t);
}

/** Cubic ease-in: accelerating from zero velocity with cubic curve. */
float CelsEaseInCubic(float t)
{
    return t * t * t;
}

/** Cubic ease-out: decelerating to zero velocity with cubic curve. */
float CelsEaseOutCubic(float t)
{
    const float f = t - 1.0f;
    return f * f * f + 1.0f;
}

/** Cubic ease-in-out: acceleration then deceleration with cubic curve. */
float CelsEaseInOutCubic(float t)
{
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    const float f = 2.0f * t - 2.0f;
    return 0.5f * f * f * f + 1.0f;
}

/** Sinusoidal ease-in: accelerating using trigonometric sine. */
float CelsEaseInSine(float t)
{
    return 1.0f - (float)cos(t * (M_PI / 2.0));
}

/** Sinusoidal ease-out: decelerating using trigonometric sine. */
float CelsEaseOutSine(float t)
{
    return (float)sin(t * (M_PI / 2.0));
}

/** Sinusoidal ease-in-out: acceleration then deceleration using trigonometric sine. */
float CelsEaseInOutSine(float t)
{
    return -0.5f * (float)(cos(M_PI * t) - 1.0);
}

/** Bounce ease-out: decelerating with decaying rebounds off target value. */
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

/** Back ease-out: overshooting target value slightly then snapping back. */
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
 * Updates or advances a transition in the active session.
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
