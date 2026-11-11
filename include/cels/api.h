/** @file api.h
 * @brief Immutable authoring services for the Cels composition runtime.
 * Define CELS_NO_SERVICE_DSL before including Cels headers when an embedding
 * framework provides its own Cel service table. Runtime symbols remain available.
 */
#ifndef CELS_API_H
#define CELS_API_H
#include <stddef.h>
#include <stdint.h>

/** @brief Reactive subscriptions to composition-owned state. */
typedef struct CelsStateApi {
    /** @brief Subscribes the current composition to an existing state instance.
     * @param[in] state Borrowed live front-buffer pointer in the current session.
     * The expression is evaluated once. The runtime obtains size and ownership
     * from its registry; watching does not dereference the caller's pointer.
     * @return The same read-only pointer, or NULL for invalid context, unregistered
     * storage or exhausted subscription capacity. NULL input is a no-op.
     * @note Call on the owning composition thread. Duplicate watches are
     * idempotent. Watching grants neither mutation rights nor extra lifetime.
     * Do not retain pointers after owner unmount, session destruction or reuse.
     */
    const void *(*Watch)(const void *state);
} CelsStateApi;

/** @brief Fixed layout for service ABI 1; changes require a new getter symbol. */
typedef struct CelsApi {
    uint32_t abiVersion; /**< Service table ABI, currently 1; independent of release version. */
    size_t structSize; /**< Size of this exact table. */
    CelsStateApi State; /**< State subscriptions. */
} CelsApi;

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Gets the immutable services belonging to the linked Cels runtime.
 * @return Non-NULL table, borrowed for that runtime's lifetime. Never free or modify it.
 * @note Normally accessed through Cel. This is the explicit embedding ABI.
 */
const CelsApi *CelsApiV1(void);
#ifdef __cplusplus
}
#endif

#ifndef CELS_NO_SERVICE_DSL
/** Authoring services, e.g. Cel.State.Watch(settings). */
#define Cel (*CelsApiV1())
#endif
#endif
