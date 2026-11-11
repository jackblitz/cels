/** @file api.c @brief One immutable service table for the linked Cels runtime. */
#include "cels/api.h"
#include "cels/runtime/session.h"
#include "cels/runtime/state.h"

static const void *WatchState(const void *state)
{
    return CelsWatchStateInstance(CelsGetCurrentSession(), state);
}

static const CelsApi s_api = {
    .abiVersion = 1u,
    .structSize = sizeof(CelsApi),
    .State = { .Watch = WatchState }
};

const CelsApi *CelsApiV1(void)
{
    return &s_api;
}
