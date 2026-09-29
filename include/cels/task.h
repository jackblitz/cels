#pragma once

#include "cels/slot_table.h"
#include "cels/session.h"
#include "cels/thread.h"
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#define CEL_TASK_PHASE_RUN    1
#define CEL_TASK_PHASE_CANCEL 2

typedef struct CelsTaskState {
    int      step;          /* Current step / line number (0 = initial, -1 = done, -2 = cancelled) */
    uint64_t waitTimerMs;   /* Non-blocking delay deadline */
    bool     isRunning;     /* True while task is active */
    bool     isCancelled;   /* True if task was cancelled */
    bool     isDone;        /* True if run block completed */
} CelsTaskState;

#define cancel \
    _cels_task_cancel_entry: \
    if (_cels_task_phase == CEL_TASK_PHASE_CANCEL) \
        for (int _cels_canc_once = 1; _cels_canc_once; _cels_canc_once = 0)

#define run \
    if (0) goto _cels_task_cancel_entry; \
    if (_cels_task_phase == CEL_TASK_PHASE_RUN) \
        for (int _cels_task_loop = 1; _cels_task_loop; \
             _cels_task_loop = 0, \
             _cels_task_state->isDone = true, \
             _cels_task_state->isRunning = false, \
             _cels_task_state->step = -1) \
            switch (_cels_task_state->step) case 0:

#define cel_yield() \
    do { \
        _cels_task_state->step = __LINE__; \
        CelsSessionInvalidateKey(CelsGetCurrentSession(), _cels_task_key); \
        return; \
        case __LINE__:; \
    } while(0)

#define cel_wait(ms) \
    do { \
        _cels_task_state->waitTimerMs = CelsGetTimeMs() + (uint64_t)(ms); \
        _cels_task_state->step = __LINE__; \
        CelsSessionInvalidateKey(CelsGetCurrentSession(), _cels_task_key); \
        return; \
        case __LINE__: \
            if (CelsGetTimeMs() < _cels_task_state->waitTimerMs) { \
                CelsSessionInvalidateKey(CelsGetCurrentSession(), _cels_task_key); \
                return; \
            } \
            _cels_task_state->waitTimerMs = 0; \
    } while(0)

#define cel_cancel() \
    do { \
        _cels_task_state->isCancelled = true; \
        _cels_task_state->isRunning = false; \
        _cels_task_state->step = -2; \
        _cels_task_phase = CEL_TASK_PHASE_CANCEL; \
        goto _cels_task_cancel_entry; \
    } while(0)

#define cel_task(Name, ...) Name(__VA_ARGS__)
#define cel_cancel_task(Name) _cels_task_cancel_##Name(CelsGetCurrentSession())
#define cel_is_task_running(Name) _cels_task_is_running_##Name(CelsGetCurrentSession())
#define cel_is_task_done(Name) _cels_task_is_done_##Name(CelsGetCurrentSession())
#define cel_restart_task(Name) _cels_task_restart_##Name(CelsGetCurrentSession())

/* ========================================================================= */
/* CEL_Task 0 Arguments Implementation                                       */
/* ========================================================================= */

#define _CEL_TASK_0(Name) \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        CelsTaskState *state = (CelsTaskState*)instance; \
        if (state != NULL && !state->isDone && !state->isCancelled) { \
            state->isCancelled = true; \
            state->isRunning = false; \
            state->step = -2; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, state, CelsHashKey(#Name)); \
        } \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isRunning && !_cels_task_last_##Name->isCancelled && !_cels_task_last_##Name->isDone); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isDone); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            _cels_task_last_##Name->step = 0; \
            _cels_task_last_##Name->waitTimerMs = 0; \
            _cels_task_last_##Name->isRunning = true; \
            _cels_task_last_##Name->isCancelled = false; \
            _cels_task_last_##Name->isDone = false; \
            CelsSessionInvalidateKey(sess, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(void) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            CelsTaskState *state = cel_remember(CelsTaskState, \
                ((CelsTaskState){ .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }), \
                _cels_task_clean_##Name \
            ); \
            _cels_task_last_##Name = state; \
            if (CelsIsFreshMount(sess)) { \
                state->step = 0; \
                state->isRunning = true; \
                state->isCancelled = false; \
                state->isDone = false; \
                state->waitTimerMs = 0; \
                CelsSessionRegisterLifecycle(sess, state, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, state, _cels_task_clean_##Name); \
            } \
            if (!state->isCancelled && !state->isDone) { \
                if (state->waitTimerMs > 0 && CelsGetTimeMs() < state->waitTimerMs) { \
                    CelsSessionInvalidateKey(sess, key); \
                    CelsExitGroup(sess); \
                    return; \
                } \
                state->waitTimerMs = 0; \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, state, key); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key)

/* ========================================================================= */
/* CEL_Task 1 Argument Implementation (Type1, Arg1)                          */
/* ========================================================================= */

#define _CEL_TASK_1(Name, Type1, Arg1) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState state; \
        Type1 Arg1; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->state.isDone && !box->state.isCancelled) { \
            box->state.isCancelled = true; \
            box->state.isRunning = false; \
            box->state.step = -2; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->state, CelsHashKey(#Name), box->Arg1); \
        } \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isRunning && !_cels_task_last_##Name->isCancelled && !_cels_task_last_##Name->isDone); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isDone); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            _cels_task_last_##Name->step = 0; \
            _cels_task_last_##Name->waitTimerMs = 0; \
            _cels_task_last_##Name->isRunning = true; \
            _cels_task_last_##Name->isCancelled = false; \
            _cels_task_last_##Name->isDone = false; \
            CelsSessionInvalidateKey(sess, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            _CelsTaskBox_##Name *box = cel_remember(_CelsTaskBox_##Name, \
                ((_CelsTaskBox_##Name){ .state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }, .Arg1 = Arg1 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            _cels_task_last_##Name = &box->state; \
            if (CelsIsFreshMount(sess)) { \
                box->state.step = 0; \
                box->state.isRunning = true; \
                box->state.isCancelled = false; \
                box->state.isDone = false; \
                box->state.waitTimerMs = 0; \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            if (!box->state.isCancelled && !box->state.isDone) { \
                if (box->state.waitTimerMs > 0 && CelsGetTimeMs() < box->state.waitTimerMs) { \
                    CelsSessionInvalidateKey(sess, key); \
                    CelsExitGroup(sess); \
                    return; \
                } \
                box->state.waitTimerMs = 0; \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->state, key, Arg1); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1)

/* ========================================================================= */
/* CEL_Task 2 Arguments Implementation (Type1, Arg1, Type2, Arg2)            */
/* ========================================================================= */

#define _CEL_TASK_2(Name, Type1, Arg1, Type2, Arg2) \
    typedef struct _CelsTaskBox_##Name { \
        CelsTaskState state; \
        Type1 Arg1; \
        Type2 Arg2; \
    } _CelsTaskBox_##Name; \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2); \
    static CELS_THREAD_LOCAL CelsTaskState *_cels_task_last_##Name = NULL; \
    static void _cels_task_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _CelsTaskBox_##Name *box = (_CelsTaskBox_##Name*)instance; \
        if (box != NULL && !box->state.isDone && !box->state.isCancelled) { \
            box->state.isCancelled = true; \
            box->state.isRunning = false; \
            box->state.step = -2; \
            _cels_task_body_##Name(CEL_TASK_PHASE_CANCEL, &box->state, CelsHashKey(#Name), box->Arg1, box->Arg2); \
        } \
    } \
    static inline void _cels_task_cancel_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL && !_cels_task_last_##Name->isDone && !_cels_task_last_##Name->isCancelled) { \
            _cels_task_clean_##Name(_cels_task_last_##Name, sess); \
        } \
    } \
    static inline bool _cels_task_is_running_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isRunning && !_cels_task_last_##Name->isCancelled && !_cels_task_last_##Name->isDone); \
    } \
    static inline bool _cels_task_is_done_##Name(CelsSession *sess) { \
        (void)sess; \
        return (_cels_task_last_##Name != NULL && _cels_task_last_##Name->isDone); \
    } \
    static inline void _cels_task_restart_##Name(CelsSession *sess) { \
        if (sess == NULL) sess = CelsGetCurrentSession(); \
        if (_cels_task_last_##Name != NULL) { \
            _cels_task_last_##Name->step = 0; \
            _cels_task_last_##Name->waitTimerMs = 0; \
            _cels_task_last_##Name->isRunning = true; \
            _cels_task_last_##Name->isCancelled = false; \
            _cels_task_last_##Name->isDone = false; \
            CelsSessionInvalidateKey(sess, CelsHashKey(#Name)); \
        } \
    } \
    static inline void Name(Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #Name " called outside of an active CelsSession"); \
        const uint64_t key = CelsHashKey(#Name); \
        if (CelsEnterComposable(sess, key)) { \
            _CelsTaskBox_##Name *box = cel_remember(_CelsTaskBox_##Name, \
                ((_CelsTaskBox_##Name){ .state = { .step = 0, .waitTimerMs = 0, .isRunning = true, .isCancelled = false, .isDone = false }, .Arg1 = Arg1, .Arg2 = Arg2 }), \
                _cels_task_clean_##Name \
            ); \
            box->Arg1 = Arg1; \
            box->Arg2 = Arg2; \
            _cels_task_last_##Name = &box->state; \
            if (CelsIsFreshMount(sess)) { \
                box->state.step = 0; \
                box->state.isRunning = true; \
                box->state.isCancelled = false; \
                box->state.isDone = false; \
                box->state.waitTimerMs = 0; \
                CelsSessionRegisterLifecycle(sess, box, NULL, _cels_task_clean_##Name); \
            } else { \
                CelsSessionUpdateLifecycle(sess, box, _cels_task_clean_##Name); \
            } \
            if (!box->state.isCancelled && !box->state.isDone) { \
                if (box->state.waitTimerMs > 0 && CelsGetTimeMs() < box->state.waitTimerMs) { \
                    CelsSessionInvalidateKey(sess, key); \
                    CelsExitGroup(sess); \
                    return; \
                } \
                box->state.waitTimerMs = 0; \
                _cels_task_body_##Name(CEL_TASK_PHASE_RUN, &box->state, key, Arg1, Arg2); \
            } \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_task_body_##Name(int _cels_task_phase, CelsTaskState *_cels_task_state, uint64_t _cels_task_key, Type1 Arg1, Type2 Arg2)

/* ========================================================================= */
/* CEL_Task Macro Dispatcher                                                 */
/* ========================================================================= */

#define _CEL_GET_TASK_MACRO(_1, _2, _3, _4, _5, NAME, ...) NAME

#define CEL_Task(...) \
    _CEL_GET_TASK_MACRO(__VA_ARGS__, _CEL_TASK_2, _UNUSED, _CEL_TASK_1, _UNUSED, _CEL_TASK_0)(__VA_ARGS__)
