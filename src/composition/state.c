#include "composition/state.h"

#include <assert.h>
#include <string.h>

#include "composition/recomposition_dispatcher.h"

#define CELS_ASSERT(condition) assert(condition)

/*
 * IMPLEMENTATION STATUS: skeleton.
 *
 * The ambient composition context below is real — the composer and the
 * recompose walk both depend on it, so it is implemented here rather than
 * stubbed. Everything else is a signature-complete placeholder that reports
 * failure rather than pretending to work: a half-working cell that silently
 * drops subscriptions would fail exactly the way §3 of CELS.md warns about,
 * where the state changes and the tree quietly does not.
 */

/** Host currently composing, or NULL outside a composition walk. */
static CelsCompositionHost *s_invalidationHost = NULL;

void
CelsInvalidationContextSet(CelsCompositionHost *host)
{
    s_invalidationHost = host;
}

CelsCompositionHost *
CelsInvalidationContextGet(void)
{
    return s_invalidationHost;
}

CelsResult
CelsMutableStateCreate(const void *initialValue,
                       size_t valueSize,
                       void **outValue)
{
    (void)initialValue;
    (void)valueSize;
    (void)outValue;
    return CELS_ERROR_INVALID_STATE;
}

CelsResult
CelsMutableStateRead(const void *value, void *outValue, size_t valueSize)
{
    (void)value;
    (void)outValue;
    (void)valueSize;
    return CELS_ERROR_INVALID_STATE;
}

CelsResult
CelsMutableStateUpdate(void *value, const void *newValue, size_t valueSize)
{
    (void)value;
    (void)newValue;
    (void)valueSize;
    return CELS_ERROR_INVALID_STATE;
}

void
CelsMutableStateUnsubscribe(CelsCompositionHost *host,
                            CelsComposableId composable)
{
    (void)host;
    (void)composable;
}

void
CelsMutableStateUnsubscribeHost(CelsCompositionHost *host)
{
    (void)host;
}

void
CelsMutableStateResetPool(void)
{
}

void *
CelsMutableStateCreateOrNull(const void *initialValue, size_t valueSize)
{
    void *value = NULL;
    if (CelsMutableStateCreate(initialValue, valueSize, &value) != CELS_OK) {
        return NULL;
    }
    return value;
}

CelsResult
CelsMutableStateReadOrZero(const void *value, size_t valueSize)
{
    (void)value;
    (void)valueSize;
    return CELS_ERROR_INVALID_STATE;
}
