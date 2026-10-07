#include "cli/test_cli.h"
#include "cels/cels.h"
#include "common_events.h"
#include "cels_input.h"
#include "cels_layout.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @file test_input.c
 * @brief Unit tests for PdcursesInputContext, action mapping, and layout focus routing.
 */

/* ========================================================================= */
/* Mock Terminal Poll Function for Headless Testing                          */
/* ========================================================================= */

static int s_mockPolledKey = 0;

int CelsTerminalPollKey(void)
{
    int key = s_mockPolledKey;
    s_mockPolledKey = 0;
    return key;
}

/* ========================================================================= */
/* Test 1: Driver Context Lifecycle & Staged Keys                            */
/* ========================================================================= */

static void TestPdcursesInputLifecycle(void)
{
    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    assert(ctx.currentKey == 0);
    assert(ctx.frameId == 0);
    assert(ctx.pulseActive == false);

    /* Explicit key staging via setKey */
    PdcursesInputSetKey(&ctx, 'w');
    assert(ctx.currentKey == 'w');

    /* Hardware/terminal polling via readKey with mock backend */
    s_mockPolledKey = 'a';
    int polled = PdcursesInputReadKey(&ctx);
    assert(polled == 'a');
    assert(ctx.currentKey == 'a');

    /* Reading when no key is available returns 0 without overwriting pending key */
    s_mockPolledKey = 0;
    polled = PdcursesInputReadKey(&ctx);
    assert(polled == 0);
    assert(ctx.currentKey == 'a');
}

/* ========================================================================= */
/* Test 2: Double-Buffered State Publication & 1-Frame Pulse Decay           */
/* ========================================================================= */

CEL_Composable(InputPulseWidget) {
    cel_watch_state(CelsInputState);
}

static void TestPdcursesInputPublishPulseAndDecay(void)
{
    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, InputPulseWidget);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Initial mount pass */
    assert(CelsSessionRecompose(&session) == CELS_OK);

    /* Frame 1: Stage 'W' and publish to CELS */
    PdcursesInputSetKey(&ctx, 'W');
    bool mutated = PdcursesInputPublishKey(&ctx, &session);
    assert(mutated == true);
    assert(ctx.pulseActive == true);
    assert(ctx.currentKey == 0);
    assert(ctx.frameId == 1);

    /* Recompose frame: publishes double-buffered state to front buffer */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    const CelsInputState *input = (const CelsInputState*)CelsStateGet(&session, CelsHashKey("CelsInputState"), sizeof(CelsInputState));
    assert(input != NULL);
    assert(input->rawKey == 'W');
    assert(input->handled == false);
    assert(input->frameId == 1);

    /* Frame 2: Pulse decay (no key pressed, pulseActive is true) */
    mutated = PdcursesInputPublishKey(&ctx, &session);
    assert(mutated == true);
    assert(ctx.pulseActive == false);

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(input->rawKey == 0);
    assert(input->handled == false);

    /* Frame 3: Resting/idle state (no key staged, no active pulse) */
    mutated = PdcursesInputPublishKey(&ctx, &session);
    assert(mutated == false);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 3: Action Mapping, cel_action_pressed & Selective Recomposition     */
/* ========================================================================= */

static const CelsKeyBinding kActionBindings[] = {
    { 'w', ACTION_MOVE_UP },
    { 's', ACTION_MOVE_DOWN },
    { ' ', ACTION_SUBMIT }
};
static const CelsInputMap kActionMap = CELS_INPUT_MAP("ActionMap", kActionBindings);

static int s_watcherRunCount = 0;
static bool s_moveUpFired = false;
static bool s_submitFired = false;

CEL_Composable(ActionObserverWidget) {
    s_watcherRunCount++;
    if (cel_action_pressed(ACTION_MOVE_UP)) {
        s_moveUpFired = true;
    }
    if (cel_action_pressed(ACTION_SUBMIT)) {
        s_submitFired = true;
    }
}

CEL_Composable(ActionTestApp) {
    cel_set_context(CelsInputMap, &kActionMap);
    ActionObserverWidget();
}

static void TestInputActionPressedAndWatchInvalidation(void)
{
    s_watcherRunCount = 0;
    s_moveUpFired = false;
    s_submitFired = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, ActionTestApp);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Initial mount pass */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_watcherRunCount == 1);
    assert(s_moveUpFired == false);
    assert(s_submitFired == false);

    /* Frame 1: Stage and publish 'w' (mapped to ACTION_MOVE_UP) */
    PdcursesInputSetKey(&ctx, 'w');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_watcherRunCount == 2);
    assert(s_moveUpFired == true);
    assert(s_submitFired == false);

    /* Frame 2: Pulse decay restores state to 0 */
    s_moveUpFired = false;
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_watcherRunCount == 3);
    assert(s_moveUpFired == false);

    /* Frame 3: Publish ' ' (mapped to ACTION_SUBMIT) */
    PdcursesInputSetKey(&ctx, ' ');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_watcherRunCount == 4);
    assert(s_submitFired == true);

    /* Frame 4: Pulse decay restores state to 0 */
    s_submitFired = false;
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_watcherRunCount == 5);
    assert(s_submitFired == false);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 4: cel_action_consume Event Swallowing                              */
/* ========================================================================= */

static bool s_firstConsumerHit = false;
static bool s_secondConsumerHit = false;

CEL_Composable(FirstConsumer) {
    if (cel_action_consume(ACTION_SUBMIT)) {
        s_firstConsumerHit = true;
    }
}

CEL_Composable(SecondConsumer) {
    if (cel_action_pressed(ACTION_SUBMIT)) {
        s_secondConsumerHit = true;
    }
}

CEL_Composable(ConsumeApp) {
    cel_set_context(CelsInputMap, &kActionMap);
    FirstConsumer();
    SecondConsumer();
}

static void TestInputActionConsume(void)
{
    s_firstConsumerHit = false;
    s_secondConsumerHit = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, ConsumeApp);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Initial mount */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(!s_firstConsumerHit && !s_secondConsumerHit);

    /* Frame 1: Stage and publish ' ' */
    PdcursesInputSetKey(&ctx, ' ');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);

    /* First consumer must match and swallow; second consumer must be blocked */
    assert(s_firstConsumerHit == true);
    assert(s_secondConsumerHit == false);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 5: Case Insensitivity                                                */
/* ========================================================================= */

static bool s_uppercaseTriggered = false;

CEL_Composable(CaseWidget) {
    if (cel_action_pressed(ACTION_MOVE_UP)) {
        s_uppercaseTriggered = true;
    }
}

CEL_Composable(CaseApp) {
    cel_set_context(CelsInputMap, &kActionMap);
    CaseWidget();
}

static void TestInputCaseInsensitivity(void)
{
    s_uppercaseTriggered = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CaseApp);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Mount */
    assert(CelsSessionRecompose(&session) == CELS_OK);

    /* Send uppercase 'W' while map is bound to lowercase 'w' */
    PdcursesInputSetKey(&ctx, 'W');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_uppercaseTriggered == true);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 6: 1D Linear Layout Focus Navigation (Vertical & Wrapping)           */
/* ========================================================================= */

static int s_activeFocusItem = -1;

CEL_Composable(FocusItem, int, itemIndex) {
    if (cel_focusable()) {
        s_activeFocusItem = itemIndex;
    }
}

static const CelsKeyBinding kNavBindings[] = {
    { 'w', ACTION_NAV_PREV },
    { 's', ACTION_NAV_NEXT },
    { 'a', ACTION_VALUE_DEC },
    { 'd', ACTION_VALUE_INC },
    { ' ', ACTION_SUBMIT }
};
static const CelsInputMap kNavMap = CELS_INPUT_MAP("NavMap", kNavBindings);

CEL_Composable(FocusMenuApp) {
    cel_set_context(CelsInputMap, &kNavMap);
    CEL_Layout(CEL_LAYOUT_DIR_VERT) {
        FocusItem(0);
        FocusItem(1);
        FocusItem(2);
    }
}

static void TestLayout1DFocusNavigation(void)
{
    s_activeFocusItem = -1;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, FocusMenuApp);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Mount pass; item 0 gets initial focus */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_activeFocusItem == 0);

    /* Navigate down: send 's' (ACTION_MOVE_DOWN / ACTION_NAV_NEXT) */
    PdcursesInputSetKey(&ctx, 's');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    /* Pulse decay pass */
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_activeFocusItem == 1);

    /* Navigate down again: item 2 */
    PdcursesInputSetKey(&ctx, 's');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_activeFocusItem == 2);

    /* Navigate down again with wrap: wraps back to item 0 */
    PdcursesInputSetKey(&ctx, 's');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_activeFocusItem == 0);

    /* Navigate up with wrap: wraps to item 2 */
    PdcursesInputSetKey(&ctx, 'w');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_activeFocusItem == 2);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 7: 2D Grid Layout Focus Navigation                                   */
/* ========================================================================= */

static const CelsKeyBinding kGridBindings[] = {
    { 'w', ACTION_NAV_PREV },  /* Up */
    { 's', ACTION_NAV_NEXT },  /* Down */
    { 'a', ACTION_VALUE_DEC }, /* Left */
    { 'd', ACTION_VALUE_INC }  /* Right */
};
static const CelsInputMap kGridMap = CELS_INPUT_MAP("GridMap", kGridBindings);

static int s_gridActiveItem = -1;

CEL_Composable(GridSlot, int, slotIndex) {
    if (cel_focusable()) {
        s_gridActiveItem = slotIndex;
    }
}

CEL_Composable(GridApp) {
    cel_set_context(CelsInputMap, &kGridMap);
    /* 2 columns, 4 items: row 0 = [0, 1], row 1 = [2, 3] */
    CEL_Layout(TestGrid, CEL_LAYOUT_GRID, 2, true) {
        GridSlot(0);
        GridSlot(1);
        GridSlot(2);
        GridSlot(3);
    }
}

static void TestLayout2DFocusNavigation(void)
{
    s_gridActiveItem = -1;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, GridApp);

    PdcursesInputContext ctx;
    PdcursesInputInit(&ctx);

    /* Frame 0: Initial mount -> Slot 0 (0, 0) */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_gridActiveItem == 0);

    /* Right ('d'): Move from (0,0) to (1,0) -> Slot 1 */
    PdcursesInputSetKey(&ctx, 'd');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_gridActiveItem == 1);

    /* Down ('s'): Move from (1,0) to (1,1) -> Slot 3 */
    PdcursesInputSetKey(&ctx, 's');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_gridActiveItem == 3);

    /* Left ('a'): Move from (1,1) to (0,1) -> Slot 2 */
    PdcursesInputSetKey(&ctx, 'a');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_gridActiveItem == 2);

    /* Up ('w'): Move from (0,1) to (0,0) -> Slot 0 */
    PdcursesInputSetKey(&ctx, 'w');
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    PdcursesInputPublishKey(&ctx, &session);
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_gridActiveItem == 0);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test Suite Registration                                                   */
/* ========================================================================= */

static const TestCase s_inputTests[] = {
    { "TestPdcursesInputLifecycle", "Driver context lifecycle and staged keys", TestPdcursesInputLifecycle },
    { "TestPdcursesInputPublishPulseAndDecay", "Double-buffered state publication and 1-frame pulse decay", TestPdcursesInputPublishPulseAndDecay },
    { "TestInputActionPressedAndWatchInvalidation", "cel_action_pressed dependency watching and selective invalidation", TestInputActionPressedAndWatchInvalidation },
    { "TestInputActionConsume", "cel_action_consume event swallowing across siblings", TestInputActionConsume },
    { "TestInputCaseInsensitivity", "Case-insensitive ASCII key mapping", TestInputCaseInsensitivity },
    { "TestLayout1DFocusNavigation", "1D vertical layout focus navigation and boundary wrapping", TestLayout1DFocusNavigation },
    { "TestLayout2DFocusNavigation", "2D grid layout focus navigation across rows and columns", TestLayout2DFocusNavigation }
};

static const TestSuite s_inputSuite = {
    .name = "input",
    .description = "Declarative action mapping, PdcursesInputContext driver, and layout focus routing",
    .tests = s_inputTests,
    .testCount = sizeof(s_inputTests) / sizeof(s_inputTests[0])
};

const TestSuite *GetInputTestSuite(void)
{
    return &s_inputSuite;
}
