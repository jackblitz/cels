#pragma once

/**
 * @file cels_input.h
 * @brief Unity-style Declarative Input Manager, Action Mapping, and Event Routing for CELS.
 *
 * NOTE: This file is an application-level pattern demonstrating how developers can build
 * semantic action mapping and input routing on top of CELS reactive state (CelsInputState)
 * and ambient context (cel_context / cel_set_context).
 *
 * Core Architectural Principles:
 * 1. Semantic Action Abstraction:
 *    - Key bindings decouple hardware key codes from semantic actions (e.g. 'w' -> ACTION_NAV_PREV).
 *    - Context-dependent scoping allows Gameplay and UI to reuse the same keys (W/A/S/D)
 *      with completely different behaviors depending on the ambient CelsInputMap in scope.
 *
 * 2. Ambient Context Cascading:
 *    - Subtrees declare their active input mapping with cel_set_context(CelsInputMap, &myMap).
 *    - Child composables query actions with cel_action_pressed() or cel_action_consume()
 *      without passing input state or action maps down through function parameters.
 *
 * 3. Reactive Event Wakeup:
 *    - CelsInputState is a double-buffered CEL_State. When the host mutates it on key press,
 *      only composables observing input are recomposed.
 *
 * Usage Example:
 * @code
 *     // 1. Declare key bindings table
 *     static const CelsKeyBinding kUiBindings[] = {
 *         { 'w', ACTION_NAV_PREV }, { 's', ACTION_NAV_NEXT },
 *         { ' ', ACTION_SUBMIT },   { '\r', ACTION_SUBMIT }
 *     };
 *     static const CelsInputMap kUiMap = CELS_INPUT_MAP("UiMap", kUiBindings);
 *
 *     // 2. Publish map down menu composable
 *     CEL_Composable(MainMenu) {
 *         cel_set_context(CelsInputMap, &kUiMap);
 *
 *         CEL_Layout(CEL_LAYOUT_DIR_VERT) {
 *             MenuItem("Start Game");
 *             MenuItem("Options");
 *         }
 *     }
 *
 *     // 3. Child composable consumes action
 *     CEL_Composable(MenuItem, const char *title) {
 *         if (cel_focusable()) {
 *             if (cel_action_consume(ACTION_SUBMIT)) {
 *                 // Trigger menu item
 *             }
 *         }
 *     }
 * @endcode
 */

#include "cels.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Reactive Input State                                                      */
/* Double-buffered, automatically wakes observing composables upon mutation. */
/* ========================================================================= */

/**
 * @brief Reactive state tracking current keyboard/controller raw input for a session.
 */
CEL_State(CelsInputState) {
    uint32_t frameId;      /**< Monotonically increasing tick ID (4 bytes). */
    int      rawKey;       /**< ASCII or key code, 0 = no key (4 bytes). */
    bool     handled;      /**< True if an active composable consumed this key (1 byte). */
    uint8_t  reserved[3];  /**< Alignment padding (3 bytes). */
};

/* ========================================================================= */
/* Declarative Action Mapping (Unity-style Input Manager)                    */
/* ========================================================================= */

/**
 * @struct CelsKeyBinding
 * @brief Maps a physical key code to a semantic action identifier.
 */
typedef struct CelsKeyBinding {
    int key;               /**< Key code (e.g. 'w', 's', ' ', '\t', 27). */
    int action;            /**< Semantic action identifier (e.g. ACTION_NAV_PREV). */
} CelsKeyBinding;

/**
 * @struct CelsInputMap
 * @brief Named collection of key bindings cascaded down composable trees via ambient context.
 */
typedef struct CelsInputMap {
    const char           *name;         /**< Human-readable name of this action map. */
    const CelsKeyBinding *bindings;     /**< Array of key-action bindings. */
    size_t                bindingCount; /**< Number of bindings in the array. */
} CelsInputMap;

/**
 * @def CELS_INPUT_MAP
 * @brief Helper macro for declaring a static CelsInputMap with automatic element counting.
 */
#define CELS_INPUT_MAP(nameStr, bindingsArray) \
    { (nameStr), (bindingsArray), sizeof(bindingsArray) / sizeof((bindingsArray)[0]) }

/* ========================================================================= */
/* Semantic Action Constants                                                 */
/* ========================================================================= */

/* Gameplay Actions */
#define ACTION_MOVE_UP      101
#define ACTION_MOVE_DOWN    102
#define ACTION_MOVE_LEFT    103
#define ACTION_MOVE_RIGHT   104
#define ACTION_ATTACK       105
#define ACTION_INTERACT     106

/* UI Menu & Navigation Actions */
#define ACTION_NAV_PREV     201
#define ACTION_NAV_NEXT     202
#define ACTION_VALUE_DEC    203
#define ACTION_VALUE_INC    204
#define ACTION_SUBMIT       205

/* Global Actions */
#define ACTION_TOGGLE_MENU  301

/* Backward Compatibility Aliases */
#define ACTION_UP           ACTION_NAV_PREV
#define ACTION_DOWN         ACTION_NAV_NEXT
#define ACTION_TOGGLE       ACTION_SUBMIT
#define ACTION_SPECIAL      ACTION_ATTACK

/* ========================================================================= */
/* Composable Query APIs                                                     */
/* ========================================================================= */

/**
 * @brief Checks if the specified semantic action was triggered by the current input.
 *
 * Matches the raw key in CelsInputState against the active ambient CelsInputMap
 * in scope (via cel_context). Performs case-insensitive matching for ASCII letters.
 *
 * @param action Semantic action integer to test.
 * @return True if action matches active unhandled input; false otherwise.
 */
static inline bool cel_action_pressed(int action) {
    const CelsInputState *input = cel_watch_state(CelsInputState);
    if (!input || input->handled || input->rawKey == 0) {
        return false;
    }

    const CelsInputMap *map = cel_context(CelsInputMap);
    if (!map || !map->bindings) {
        return false; /* No active input map in this subtree scope */
    }

    for (size_t i = 0; i < map->bindingCount; ++i) {
        int bKey = map->bindings[i].key;
        int inKey = input->rawKey;
        bool match = (bKey == inKey);
        /* Case-insensitive matching for alphabetical ASCII */
        if (!match && bKey >= 'a' && bKey <= 'z' && inKey == (bKey - 32)) match = true;
        if (!match && bKey >= 'A' && bKey <= 'Z' && inKey == (bKey + 32)) match = true;

        if (match && map->bindings[i].action == action) {
            CelsSession *sess = CelsGetCurrentSession();
            if (sess != NULL) {
                sess->isHandlingEvent = true;
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if the action was pressed, and if so, consumes it immediately.
 *
 * Consuming the input marks CelsInputState.handled = true, ensuring subsequent
 * sibling or parent composables will not react to the same key event in this frame.
 *
 * @param action Semantic action integer to test and consume.
 * @return True if the action was matched and consumed; false otherwise.
 */
static inline bool cel_action_consume(int action) {
    if (cel_action_pressed(action)) {
        CelsInputState *input = (CelsInputState*)cel_get_state(CelsInputState);
        if (input) {
            input->handled = true;
        }
        CelsSession *sess = CelsGetCurrentSession();
        if (sess != NULL) {
            sess->isHandlingEvent = true;
        }
        return true;
    }
    return false;
}

/**
 * @brief Direct physical key query without action map lookup.
 *
 * Useful for global key handling like [Tab] mode toggling or [Escape] menus.
 *
 * @param key Physical ASCII or key code to test.
 * @return True if key is pressed and unhandled; false otherwise.
 */
static inline bool cel_key_pressed(int key) {
    const CelsInputState *input = cel_watch_state(CelsInputState);
    if (!input || input->handled || input->rawKey == 0) {
        return false;
    }
    bool match = (input->rawKey == key);
    if (!match && key >= 'a' && key <= 'z' && input->rawKey == (key - 32)) match = true;
    if (!match && key >= 'A' && key <= 'Z' && input->rawKey == (key + 32)) match = true;
    if (match) {
        CelsSession *sess = CelsGetCurrentSession();
        if (sess != NULL) {
            sess->isHandlingEvent = true;
        }
        return true;
    }
    return false;
}

/**
 * @brief Direct physical key query with immediate consumption.
 *
 * Tests the physical key and marks it handled if matched.
 *
 * @param key Physical ASCII or key code to test and consume.
 * @return True if key was matched and consumed; false otherwise.
 */
static inline bool cel_key_consume(int key) {
    if (cel_key_pressed(key)) {
        CelsInputState *input = (CelsInputState*)cel_get_state(CelsInputState);
        if (input) {
            input->handled = true;
        }
        CelsSession *sess = CelsGetCurrentSession();
        if (sess != NULL) {
            sess->isHandlingEvent = true;
        }
        return true;
    }
    return false;
}

#ifdef __cplusplus
}
#endif
