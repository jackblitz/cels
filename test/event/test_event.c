#include "cli/test_cli.h"
#include "cels/cels.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Test Event / Signal / Broadcast Payloads                                  */
/* ========================================================================= */

typedef struct ClickEvent {
    int button;
    int mouseX, mouseY;
} ClickEvent;

typedef struct DamageSignal {
    int targetId;
    float amount;
} DamageSignal;

typedef struct AudioBroadcast {
    const char *soundName;
    float volume;
} AudioBroadcast;

typedef struct HandshakeEvent {
    int code;
} HandshakeEvent;

/* ========================================================================= */
/* Test 1: Tree-Local Bubbling (cel_event & cel_listen)                     */
/* ========================================================================= */

static int s_parentReceivedClicks = 0;
static int s_lastClickX = 0;
static int s_parentTriggerOnlyClicks = 0;

static bool s_buttonTriggered = false;

CEL_Composable(ButtonChild) {
    /* Deep child emits an event up the tree when triggered */
    if (!s_buttonTriggered) {
        s_buttonTriggered = true;
        cel_event(ClickEvent, { .button = 1, .mouseX = 100, .mouseY = 200 });
    }
}

CEL_Composable(ModalTriggerChild) {
    /* 1-argument form: trigger-only, no payload variable needed */
    cel_listen(ClickEvent) {
        s_parentTriggerOnlyClicks++;
    }
}

CEL_Composable(ModalParent) {
    /* Parent listens for ClickEvent */
    cel_listen(ClickEvent, ev) {
        s_parentReceivedClicks++;
        s_lastClickX = ev->mouseX;
    }
    ModalTriggerChild();
    ButtonChild();
}

CEL_Composition(TreeEventApp, void *userData) {
    (void)userData;
    ModalParent();
}

static void TestEventTreeLocalBubbling(void)
{
    s_parentReceivedClicks = 0;
    s_lastClickX = 0;
    s_parentTriggerOnlyClicks = 0;
    s_buttonTriggered = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TreeEventApp);

    /* First recompose executes ModalParent and ButtonChild, which emits ClickEvent.
     * The drain loop immediately invalidates ModalParent and re-evaluates within the same frame! */
    assert(CelsSessionRecompose(&session) == CELS_OK);

    assert(s_parentReceivedClicks == 1);
    assert(s_lastClickX == 100);
    assert(s_parentTriggerOnlyClicks == 1);

    /* Subsequent quiet recompose skips since no new events */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_parentReceivedClicks == 1);
    assert(s_parentTriggerOnlyClicks == 1);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 2: Targeted Session Signals (cel_signal & cel_connect)               */
/* ========================================================================= */

static int s_hudReceivedDamage = 0;
static float s_lastDamageAmount = 0.0f;
static int s_hudTriggerOnlyCount = 0;
static int s_hudUnusedVarCount = 0;

CEL_Composable(HudWidget) {
    cel_connect(DamageSignal, sig) {
        s_hudReceivedDamage++;
        s_lastDamageAmount = sig->amount;
    }
}

CEL_Composable(HudTriggerWidget) {
    /* 1-argument form: trigger-only, no payload variable needed */
    cel_connect(DamageSignal) {
        s_hudTriggerOnlyCount++;
    }
}

CEL_Composable(HudUnusedVarWidget) {
    /* 2-argument form with unused variable: no (void)sig needed */
    cel_connect(DamageSignal, sig) {
        s_hudUnusedVarCount++;
    }
}

CEL_Composition(HudSessionApp, void *userData) {
    (void)userData;
    HudWidget();
    HudTriggerWidget();
    HudUnusedVarWidget();
}

static void TestSignalTargetedSession(void)
{
    s_hudReceivedDamage = 0;
    s_lastDamageAmount = 0.0f;
    s_hudTriggerOnlyCount = 0;
    s_hudUnusedVarCount = 0;

    CelsSession hudSession;
    CelsSessionInit(&hudSession, NULL);
    cel_attach(&hudSession, HudSessionApp);

    /* Mount HUD */
    assert(CelsSessionRecompose(&hudSession) == CELS_OK);
    assert(s_hudReceivedDamage == 0);
    assert(s_hudTriggerOnlyCount == 0);
    assert(s_hudUnusedVarCount == 0);

    /* External Game Session or caller sends a targeted signal to hudSession */
    cel_signal(&hudSession, DamageSignal, { .targetId = 7, .amount = 45.5f });

    /* Next recompose of HUD processes the targeted signal */
    assert(CelsSessionRecompose(&hudSession) == CELS_OK);
    assert(s_hudReceivedDamage == 1);
    assert(s_lastDamageAmount == 45.5f);
    assert(s_hudTriggerOnlyCount == 1);
    assert(s_hudUnusedVarCount == 1);

    /* Subsequent recompose skips */
    assert(CelsSessionRecompose(&hudSession) == CELS_OK);
    assert(s_hudReceivedDamage == 1);
    assert(s_hudTriggerOnlyCount == 1);
    assert(s_hudUnusedVarCount == 1);

    CelsSessionDestroy(&hudSession);
}

static void TestEngineNamedSessionLifecycleAndSignal(void)
{
    s_hudReceivedDamage = 0;
    s_lastDamageAmount = 0.0f;
    s_hudTriggerOnlyCount = 0;
    s_hudUnusedVarCount = 0;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    /* 1. Main / root session retrieval */
    CelsSession *mainSession = cel_get_session(&engine, "main");
    assert(mainSession == &engine.session);
    assert(cel_get_session(&engine, "root") == &engine.session);
    assert(cel_get_session(&engine, NULL) == &engine.session);

    /* 2. Secondary session creation and retrieval */
    CelsSession *hudSession = cel_create_session(&engine, "hud", CELS_PROFILE_256);
    assert(hudSession != NULL);
    assert(hudSession != &engine.session);
    assert(cel_get_session(&engine, "hud") == hudSession);

    /* 3. Re-requesting existing named session returns same pointer */
    assert(cel_create_session(&engine, "hud", CELS_PROFILE_256) == hudSession);

    /* 4. Attach composition to named secondary session */
    cel_attach(hudSession, HudSessionApp);
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_hudReceivedDamage == 0);

    /* 5. Dispatch targeted signal via named session retrieval */
    cel_signal(cel_get_session(&engine, "hud"), DamageSignal, { .targetId = 99, .amount = 88.5f });

    /* 6. Engine-wide recompose executes secondary sessions and delivers signal */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_hudReceivedDamage == 1);
    assert(s_lastDamageAmount == 88.5f);
    assert(s_hudTriggerOnlyCount == 1);
    assert(s_hudUnusedVarCount == 1);

    CelsEngineDestroy(&engine);
}

/* ========================================================================= */
/* Test 3: Engine-Wide Broadcast (cel_broadcast & cel_bind)                  */
/* ========================================================================= */

static int s_audioReceivedBinds = 0;
static float s_lastVolume = 0.0f;
static int s_audioTriggerOnlyBinds = 0;

CEL_Composable(AudioListenerWidget) {
    cel_bind(AudioBroadcast, bcast) {
        s_audioReceivedBinds++;
        s_lastVolume = bcast->volume;
    }
}

CEL_Composable(AudioTriggerWidget) {
    /* 1-argument form: trigger-only, no payload variable needed */
    cel_bind(AudioBroadcast) {
        s_audioTriggerOnlyBinds++;
    }
}

CEL_Composition(AudioSessionApp, void *userData) {
    (void)userData;
    AudioListenerWidget();
    AudioTriggerWidget();
}

static void TestBroadcastEngineWide(void)
{
    s_audioReceivedBinds = 0;
    s_lastVolume = 0.0f;
    s_audioTriggerOnlyBinds = 0;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);
    cel_attach(&engine.session, AudioSessionApp);

    /* Initial mount */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_audioReceivedBinds == 0);
    assert(s_audioTriggerOnlyBinds == 0);

    /* Broadcast from external thread or system */
    cel_broadcast(AudioBroadcast, { .soundName = "explosion.wav", .volume = 0.95f });

    /* Engine recompose drains broadcast and executes cel_bind */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_audioReceivedBinds == 1);
    assert(s_lastVolume == 0.95f);
    assert(s_audioTriggerOnlyBinds == 1);

    /* Subsequent recompose skips */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_audioReceivedBinds == 1);
    assert(s_audioTriggerOnlyBinds == 1);

    CelsEngineDestroy(&engine);
}

/* ========================================================================= */
/* Test 4: Task Awaiting Events (cel_wait_for)                               */
/* ========================================================================= */

static int s_taskReceivedCode = 0;
static bool s_taskDone = false;

CEL_Task(AwaitEventTask) {
    cancel {}
    run {
        HandshakeEvent ev;
        cel_wait_for(HandshakeEvent, &ev);
        s_taskReceivedCode = ev.code;
        s_taskDone = true;
    }
}

CEL_Composition(TaskEventApp, void *userData) {
    (void)userData;
    cel_task(AwaitEventTask);
}

static void TestTaskWaitForEvent(void)
{
    s_taskReceivedCode = 0;
    s_taskDone = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TaskEventApp);

    /* Frame 1: Task starts, suspends at cel_wait_for */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(!s_taskDone);
    assert(s_taskReceivedCode == 0);

    /* Emit event */
    cel_event(HandshakeEvent, { .code = 777 });

    /* Frame 2: Task receives event, completes */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_taskDone);
    assert(s_taskReceivedCode == 777);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 5: Task Awaiting Signal (cel_wait_signal)                            */
/* ========================================================================= */

static float s_taskSignalAmount = 0.0f;
static bool s_taskSignalDone = false;

CEL_Task(AwaitSignalTask) {
    cancel {}
    run {
        DamageSignal sig;
        cel_wait_signal(DamageSignal, &sig);
        s_taskSignalAmount = sig.amount;
        s_taskSignalDone = true;
    }
}

CEL_Composition(TaskSignalApp, void *userData) {
    (void)userData;
    cel_task(AwaitSignalTask);
}

static void TestTaskWaitForSignal(void)
{
    s_taskSignalAmount = 0.0f;
    s_taskSignalDone = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TaskSignalApp);

    /* Frame 1: Task starts and suspends */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(!s_taskSignalDone);

    /* Send signal to session */
    cel_signal(&session, DamageSignal, { .targetId = 1, .amount = 99.0f });

    /* Frame 2: Task wakes up */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_taskSignalDone);
    assert(s_taskSignalAmount == 99.0f);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test 6: Task Awaiting Broadcast (cel_wait_broadcast)                      */
/* ========================================================================= */

static float s_taskBcastVolume = 0.0f;
static bool s_taskBcastDone = false;

CEL_Task(AwaitBroadcastTask) {
    cancel {}
    run {
        AudioBroadcast bcast;
        cel_wait_broadcast(AudioBroadcast, &bcast);
        s_taskBcastVolume = bcast.volume;
        s_taskBcastDone = true;
    }
}

CEL_Composition(TaskBroadcastApp, void *userData) {
    (void)userData;
    cel_task(AwaitBroadcastTask);
}

static void TestTaskWaitForBroadcast(void)
{
    s_taskBcastVolume = 0.0f;
    s_taskBcastDone = false;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);
    cel_attach(&engine.session, TaskBroadcastApp);

    /* Frame 1: Task starts and suspends */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(!s_taskBcastDone);

    /* Send broadcast */
    cel_broadcast(AudioBroadcast, { .soundName = "horn.wav", .volume = 0.77f });

    /* Frame 2: Engine drains broadcast and resumes task */
    assert(CelsEngineRecompose(&engine) == CELS_OK);
    assert(s_taskBcastDone);
    assert(s_taskBcastVolume == 0.77f);

    CelsEngineDestroy(&engine);
}

/* ========================================================================= */
/* Test 7: Task Timeout (cel_wait_for_timeout)                               */
/* ========================================================================= */

static bool s_timeoutReceived = true;
static bool s_timeoutFinished = false;

CEL_Task(TimeoutTask) {
    cancel {}
    run {
        HandshakeEvent ev;
        /* Wait up to 30ms for event that never arrives */
        s_timeoutReceived = cel_wait_for_timeout(HandshakeEvent, &ev, 30);
        s_timeoutFinished = true;
    }
}

CEL_Composition(TaskTimeoutApp, void *userData) {
    (void)userData;
    cel_task(TimeoutTask);
}

static void TestTaskWaitForTimeout(void)
{
    s_timeoutReceived = true;
    s_timeoutFinished = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, TaskTimeoutApp);

    /* Frame 1: Starts waiting */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(!s_timeoutFinished);

    /* Sleep 40ms to exceed 30ms timeout */
    CelsSleepMs(40);

    /* Frame 2: Resumes on timeout */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_timeoutFinished);
    assert(!s_timeoutReceived); /* Timeout occurred without receiving event */

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test Suite Registration                                                   */
/* ========================================================================= */

static const TestCase s_eventTests[] = {
    { "TestEventTreeLocalBubbling", "Local tree event bubbling via cel_event & cel_listen", TestEventTreeLocalBubbling },
    { "TestSignalTargetedSession",  "Targeted session delivery via cel_signal & cel_connect", TestSignalTargetedSession },
    { "TestEngineNamedSessionLifecycleAndSignal", "Named session creation, retrieval, and targeted signals", TestEngineNamedSessionLifecycleAndSignal },
    { "TestBroadcastEngineWide",   "Global engine bus broadcast via cel_broadcast & cel_bind", TestBroadcastEngineWide },
    { "TestTaskWaitForEvent",      "Task fiber awaiting event via cel_wait_for", TestTaskWaitForEvent },
    { "TestTaskWaitForSignal",     "Task fiber awaiting signal via cel_wait_signal", TestTaskWaitForSignal },
    { "TestTaskWaitForBroadcast",  "Task fiber awaiting broadcast via cel_wait_broadcast", TestTaskWaitForBroadcast },
    { "TestTaskWaitForTimeout",    "Task fiber awaiting with timeout via cel_wait_for_timeout", TestTaskWaitForTimeout }
};

static const TestSuite s_eventSuite = {
    .name = "event",
    .description = "Discrete events, targeted signals, engine broadcasts, and task fiber awaiting",
    .tests = s_eventTests,
    .testCount = sizeof(s_eventTests) / sizeof(s_eventTests[0])
};

const TestSuite *GetEventTestSuite(void)
{
    return &s_eventSuite;
}
