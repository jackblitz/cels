#include "cli/test_cli.h"
#include "cels/cels.h"
#include <stdio.h>
#include <math.h>

/* ========================================================================= */
/* Test State & Composable                                                   */
/* ========================================================================= */

CEL_State(TargetState) {
    float target;
};

static float s_lastTransitionVal = 0.0f;

CEL_Composable(TransitionWidget) {
    const TargetState *ts = cel_watch_state(TargetState);
    const float target = ts ? ts->target : 0.0f;
    s_lastTransitionVal = cel_transition(target, 50, CEL_EASE_LINEAR);
}

CEL_Composition(TransitionComp, void *userData) {
    (void)userData;
    cel_remember_state(TargetState, ((TargetState){ .target = 0.0f }));
    TransitionWidget();
}

static void TestTransitionInitialValue(void)
{
    s_lastTransitionVal = -1.0f;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TransitionComp);

    // First frame initializes to initial target (0.0) immediately
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal == 0.0f);

    CelsSessionDestroy(&session);
}

static void TestTransitionStepAndSettle(void)
{
    s_lastTransitionVal = -1.0f;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TransitionComp);

    // Initial mount at 0.0
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal == 0.0f);

    // Mutate target reactively to 100.0
    cels_session_mutate(&session, TargetState) {
        this->target = 100.0f;
    }

    // First recompose after mutation initiates transition
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal >= 0.0f && s_lastTransitionVal <= 100.0f);

    // Wait out the 50ms duration
    CelsSleepMs(60);

    // Next recompose completes and settles at target
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal == 100.0f);

    CelsSessionDestroy(&session);
}

static void TestTransitionRetargetMidFlight(void)
{
    s_lastTransitionVal = -1.0f;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TransitionComp);

    // Initial mount at 0.0
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal == 0.0f);

    // Mutate to 100.0
    cels_session_mutate(&session, TargetState) {
        this->target = 100.0f;
    }
    CelsSessionRecompose(&session);

    // Sleep 15ms so it's in flight
    CelsSleepMs(15);
    CelsSessionRecompose(&session);
    const float midVal = s_lastTransitionVal;

    // Retarget to 500.0 mid-flight
    cels_session_mutate(&session, TargetState) {
        this->target = 500.0f;
    }
    CelsSessionRecompose(&session);

    // Seamless retarget starts from midVal, never snaps to 0 or 100
    assert(s_lastTransitionVal >= midVal);

    // Wait out remaining duration
    CelsSleepMs(60);
    CelsSessionRecompose(&session);
    assert(s_lastTransitionVal == 500.0f);

    CelsSessionDestroy(&session);
}

static void TestTransitionEasingCurves(void)
{
    assert(CelsEaseLinear(0.0f) == 0.0f);
    assert(CelsEaseLinear(1.0f) == 1.0f);
    assert(CelsEaseLinear(0.5f) == 0.5f);

    assert(CelsEaseInQuad(0.0f) == 0.0f);
    assert(CelsEaseInQuad(1.0f) == 1.0f);
    assert(CelsEaseInQuad(0.5f) == 0.25f);

    assert(CelsEaseOutQuad(0.0f) == 0.0f);
    assert(CelsEaseOutQuad(1.0f) == 1.0f);
    assert(CelsEaseOutQuad(0.5f) == 0.75f);
}

/* ========================================================================= */
/* Test Suite Registration                                                   */
/* ========================================================================= */

static const TestCase s_transTests[] = {
    { "TestTransitionInitialValue",      "Immediate initialization to initial target", TestTransitionInitialValue },
    { "TestTransitionStepAndSettle",     "Linear progression and settlement at target", TestTransitionStepAndSettle },
    { "TestTransitionRetargetMidFlight", "Smooth retargeting without snapping",        TestTransitionRetargetMidFlight },
    { "TestTransitionEasingCurves",      "Math validation for standard easing curves", TestTransitionEasingCurves }
};

static const TestSuite s_transSuite = {
    .name = "transition",
    .description = "cel_transition declarative temporal value convergence and easing",
    .tests = s_transTests,
    .testCount = sizeof(s_transTests) / sizeof(s_transTests[0])
};

const TestSuite *GetTransitionTestSuite(void)
{
    return &s_transSuite;
}
