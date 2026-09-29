#include "cli/test_cli.h"
#include "cels/cels.h"
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Test State & Trackers                                                     */
/* ========================================================================= */

static int  s_stepCount = 0;
static bool s_cancelRan = false;
static bool s_completedRan = false;
static char s_lastHost[32] = {0};
static int  s_lastPort = 0;

/* ========================================================================= */
/* Task 1: Basic Sequential Task                                             */
/* ========================================================================= */

CEL_Task(BasicSequentialTask) {
    cancel {
        s_cancelRan = true;
    }
    run {
        s_stepCount = 1;
        cel_yield();

        s_stepCount = 2;
        cel_yield();

        s_stepCount = 3;
        s_completedRan = true;
    }
}

CEL_Composition(BasicTaskComp, void *userData) {
    (void)userData;
    cel_task(BasicSequentialTask);
}

static void TestTaskSequentialExecution(void) {
    s_stepCount = 0;
    s_cancelRan = false;
    s_completedRan = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("BasicComp"), BasicTaskComp);

    /* Frame 1: Starts and runs step 1 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(cel_is_task_running(BasicSequentialTask));
    assert(!cel_is_task_done(BasicSequentialTask));
    assert(!s_completedRan);

    /* Frame 2: Resumes at step 2 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 2);
    assert(cel_is_task_running(BasicSequentialTask));
    assert(!cel_is_task_done(BasicSequentialTask));

    /* Frame 3: Resumes and completes step 3 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 3);
    assert(!cel_is_task_running(BasicSequentialTask));
    assert(cel_is_task_done(BasicSequentialTask));
    assert(s_completedRan);
    assert(!s_cancelRan);

    /* Frame 4: Steady state, stays done */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 3);
    assert(cel_is_task_done(BasicSequentialTask));

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Task 2: Delay Wait Task                                                   */
/* ========================================================================= */

CEL_Task(WaitDelayTask) {
    cancel {
        s_cancelRan = true;
    }
    run {
        s_stepCount = 1;
        cel_wait(40); /* 40ms wait */

        s_stepCount = 2;
    }
}

CEL_Composition(WaitTaskComp, void *userData) {
    (void)userData;
    cel_task(WaitDelayTask);
}

static void TestTaskWaitNonBlocking(void) {
    s_stepCount = 0;
    s_cancelRan = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("WaitComp"), WaitTaskComp);

    /* Frame 1: Begins and enters wait */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(cel_is_task_running(WaitDelayTask));

    /* Frame 2: Immediately recompose without waiting -> should NOT advance */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);

    /* Wait 50ms so deadline passes */
    CelsSleepMs(50);

    /* Frame 3: Now time has elapsed -> advances to step 2 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 2);
    assert(cel_is_task_done(WaitDelayTask));

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Task 3: Self Cancellation                                                 */
/* ========================================================================= */

CEL_Task(SelfCancelTask) {
    cancel {
        s_cancelRan = true;
    }
    run {
        s_stepCount = 1;
        cel_yield();

        s_stepCount = 2;
        cel_cancel(); /* Cancel self! */

        s_stepCount = 3; /* Must never be reached */
    }
}

CEL_Composition(SelfCancelComp, void *userData) {
    (void)userData;
    cel_task(SelfCancelTask);
}

static void TestTaskSelfCancel(void) {
    s_stepCount = 0;
    s_cancelRan = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("SelfCancelComp"), SelfCancelComp);

    /* Frame 1: Runs step 1 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(!s_cancelRan);

    /* Frame 2: Runs step 2, calls cel_cancel() -> cancel block runs immediately */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 2);
    assert(s_cancelRan);
    assert(!cel_is_task_running(SelfCancelTask));

    /* Frame 3: Stays cancelled, step 3 never executed */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 2);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Task 4: External Cancellation                                             */
/* ========================================================================= */

CEL_Task(ExternalCancelTask) {
    cancel {
        s_cancelRan = true;
    }
    run {
        s_stepCount = 1;
        cel_yield();

        s_stepCount = 2;
        cel_yield();
    }
}

CEL_Composition(ExternalCancelComp, void *userData) {
    (void)userData;
    cel_task(ExternalCancelTask);
}

static void TestTaskExternalCancel(void) {
    s_stepCount = 0;
    s_cancelRan = false;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("ExtCancelComp"), ExternalCancelComp);

    /* Frame 1: Step 1 runs */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(!s_cancelRan);

    /* External cancellation triggered before frame 2 */
    cel_cancel_task(ExternalCancelTask);
    assert(s_cancelRan);
    assert(!cel_is_task_running(ExternalCancelTask));

    /* Frame 2: Does not continue step 2 */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Task 5: Structural Unmount Cancellation                                    */
/* ========================================================================= */

CEL_Task(UnmountCleanupTask) {
    cancel {
        s_cancelRan = true;
    }
    run {
        s_stepCount = 1;
        cel_yield();

        s_stepCount = 2;
        cel_yield();
    }
}

static bool s_enableTask = true;

CEL_Composition(UnmountComp, void *userData) {
    (void)userData;
    if (s_enableTask) {
        cel_task(UnmountCleanupTask);
    }
}

static void TestTaskUnmountCancel(void) {
    s_stepCount = 0;
    s_cancelRan = false;
    s_enableTask = true;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("UnmountComp"), UnmountComp);

    /* Frame 1: Task mounted and executing */
    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(!s_cancelRan);

    /* Disable task in parent composition -> triggers unmount */
    s_enableTask = false;
    CelsSessionRecompose(&session);

    /* Reconciliation detected missing task group and automatically ran cancel! */
    assert(s_cancelRan);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Task 6: Parameterized Task with Arguments                                  */
/* ========================================================================= */

CEL_Task(ParameterizedTask, const char*, host, int, port) {
    cancel {
        s_cancelRan = true;
        strncpy(s_lastHost, host, sizeof(s_lastHost) - 1);
        s_lastPort = port;
    }
    run {
        strncpy(s_lastHost, host, sizeof(s_lastHost) - 1);
        s_lastPort = port;
        s_stepCount = 1;
        cel_yield();

        s_stepCount = 2;
    }
}

CEL_Composition(ParamComp, void *userData) {
    (void)userData;
    cel_task(ParameterizedTask, "127.0.0.1", 8080);
}

static void TestTaskWithArguments(void) {
    s_stepCount = 0;
    s_cancelRan = false;
    memset(s_lastHost, 0, sizeof(s_lastHost));
    s_lastPort = 0;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, CEL_ID("ParamComp"), ParamComp);

    CelsSessionRecompose(&session);
    assert(s_stepCount == 1);
    assert(strcmp(s_lastHost, "127.0.0.1") == 0);
    assert(s_lastPort == 8080);

    /* Cancel and verify parameters are preserved in cancel handler */
    cel_cancel_task(ParameterizedTask);
    assert(s_cancelRan);
    assert(strcmp(s_lastHost, "127.0.0.1") == 0);
    assert(s_lastPort == 8080);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* Test Suite Registration                                                   */
/* ========================================================================= */

static const TestCase s_taskTests[] = {
    { "TestTaskSequentialExecution", "Sequential step progression and completion", TestTaskSequentialExecution },
    { "TestTaskWaitNonBlocking",     "Non-blocking timer delay with cel_wait",     TestTaskWaitNonBlocking },
    { "TestTaskSelfCancel",          "Self cancellation with cel_cancel",          TestTaskSelfCancel },
    { "TestTaskExternalCancel",      "External cancellation via cel_cancel_task",  TestTaskExternalCancel },
    { "TestTaskUnmountCancel",       "Automatic cancellation on unmount",          TestTaskUnmountCancel },
    { "TestTaskWithArguments",       "Parameterized task execution and cleanup",   TestTaskWithArguments }
};

static const TestSuite s_taskSuite = {
    .name = "task",
    .description = "CEL_Task coroutine state machine, timing, and cancellation",
    .tests = s_taskTests,
    .testCount = sizeof(s_taskTests) / sizeof(s_taskTests[0])
};

const TestSuite *GetTaskTestSuite(void) {
    return &s_taskSuite;
}
