#pragma once

/**
 * @file cels_layout.h
 * @brief Reusable Layout, Directional Navigation, and Ambient Focus Management Pattern for CELS.
 *
 * NOTE: This file is an application-level architectural pattern demonstrating how
 * developers can author custom layout engines (1D linear rows/columns, 2D grids, menus)
 * on top of core CELS framework primitives (cel_container, cel_has, cel_set_context).
 * It is NOT hard-coded into the core CELS engine library—developers can adapt and extend
 * this pattern to create custom layout archetypes (e.g. FlexBox, Docking, SplitPane).
 *
 * Core Architectural Principles:
 * 1. Component vs. State Separation:
 *    - Layout Direction (`CelsLayoutDir`) is attached as a declarative Component via cel_has().
 *      Components represent static or structural configuration, not reactive state.
 *    - Focus (`CelsFocusState`) is allocated as reactive State via cel_remember_state().
 *      Focus represents dynamic runtime user interaction that invalidates observing composables.
 *
 * 2. Ambient Context Cascading:
 *    - The layout container publishes both `CelsLayoutDir` and `CelsFocusState` down the composable
 *      subtree using `cel_set_context()`.
 *    - Child composables call `cel_focusable()` or `cel_is_focused()` to check their focus state
 *      without manual parameter drilling.
 *
 * 3. Frame-Accurate Child Settlement:
 *    - Delegates directly to `cel_container(Name, onGroupStart, onGroupEnd, layoutData)`.
 *    - On container exit, `_CelsLayoutOnGroupEnd` receives the settled `childCount` directly from
 *      the slot table engine, enabling instant focus clamping and boundary checks on frame 0
 *      with zero latency or frame guessing.
 *
 * Usage Examples:
 * @code
 *     // Example 1: 1D Vertical Menu
 *     CEL_Layout(CEL_LAYOUT_DIR_VERT) {
 *         VolumeSlider(settings);
 *         MuteToggle(settings);
 *         GraphicsSelector(settings);
 *     }
 *
 *     // Example 2: 2D Inventory Grid (4 columns, wrap navigation)
 *     CEL_Layout(InventoryGrid, CEL_LAYOUT_DIR_2D, 4, true) {
 *         for (int i = 0; i < 16; i++) {
 *             ItemSlot(items[i]);
 *         }
 *     }
 *
 *     // Example 3: Focusable Child Composable
 *     CEL_Composable(VolumeSlider, SettingsState*, settings) {
 *         if (cel_focusable()) {
 *             // Focused rendering & input handling
 *             if (cel_action_consume(ACTION_VALUE_INC)) settings->volume += 5;
 *         } else {
 *             // Unfocused rendering
 *         }
 *     }
 * @endcode
 */

#include "cels.h"
#include "cels_input.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Layout Direction Component                                                */
/* Declarative component attached to the layout entity via cel_has().        */
/* Supports vertical, horizontal, and multi-directional 2D grid navigation.  */
/* Direction is stored as a Component, NOT as reactive state.                */
/* ========================================================================= */

/**
 * @enum CelsLayoutDir
 * @brief Direction flags for layout navigation and axis alignment.
 */
typedef enum CelsLayoutDir {
    CEL_LAYOUT_DIR_VERT  = 1 << 0, /**< Vertical: Up/Down navigation (W/S, Arrows). */
    CEL_LAYOUT_DIR_HORIZ = 1 << 1, /**< Horizontal: Left/Right navigation (A/D, Arrows). */
    CEL_LAYOUT_DIR_2D    = (CEL_LAYOUT_DIR_VERT | CEL_LAYOUT_DIR_HORIZ), /**< Multi-directional: 2D Grid / Omni. */
} CelsLayoutDir;

#define CEL_LAYOUT_VERT  CEL_LAYOUT_DIR_VERT
#define CEL_LAYOUT_HORIZ CEL_LAYOUT_DIR_HORIZ
#define CEL_LAYOUT_2D    CEL_LAYOUT_DIR_2D
#define CEL_LAYOUT_GRID  CEL_LAYOUT_DIR_2D
#define CEL_LAYOUT_OMNI  CEL_LAYOUT_DIR_2D

/**
 * @struct CelsLayoutParams
 * @brief Declarative configuration parameters for a CEL_Layout container.
 */
typedef struct CelsLayoutParams {
    CelsLayoutDir dir;         /**< Layout navigation direction (vertical, horizontal, or 2D). */
    int           columns;     /**< Grid columns (0 or 1 for linear; >= 2 for 2D grid). */
    bool          wrapFocus;   /**< Whether focus navigation wraps around boundaries. */
    uint8_t       reserved[3]; /**< Structure alignment padding. */
} CelsLayoutParams;

/* ========================================================================= */
/* Reactive Focus State                                                      */
/* Clean, double-buffered reactive state managing focus indices.             */
/* Note: FocusState ONLY tracks focus indices; direction is in Component!   */
/* ========================================================================= */

/**
 * @brief Reactive state tracking active keyboard/controller focus index within a layout.
 *
 * Note: FocusState ONLY tracks dynamic runtime focus indices; structural direction is
 * stored separately as a declarative Component (CelsLayoutDir).
 */
CEL_State(CelsFocusState) {
    int     activeIndex; /**< Index of currently focused child widget (0-based). */
    int     totalCount;  /**< Total focusable widgets registered in this layout. */
    bool    wrapFocus;   /**< Whether focus navigation wraps around edges. */
    uint8_t reserved[3]; /**< Structure alignment padding. */
};

/* ========================================================================= */
/* Layout Setup & Child-Aware Focus Routing Hooks                            */
/* ========================================================================= */

/**
 * @brief Internal lifecycle hook executed when entering a CEL_Layout container.
 *
 * Declaratively attaches the layout direction component to the container entity,
 * publishes direction and focus state down the subtree via ambient context, and
 * allocates or resolves the reactive focus state.
 *
 * @param layoutData Pointer to caller-allocated CelsLayoutParams.
 */
static inline void _CelsLayoutOnGroupStart(void *layoutData)
{
    CelsLayoutParams *params = (CelsLayoutParams*)layoutData;
    if (params == NULL) return;

    /* 1. Declaratively attach direction as a Component to the layout entity */
    cel_has(CelsLayoutDir, { params->dir });

    /* 2. Publish direction down the subtree via ambient context */
    cel_set_context(CelsLayoutDir, &params->dir);

    /* 3. Allocate/resolve reactive focus state */
    CelsFocusState *focus = cel_remember_state(CelsFocusState, {
        .activeIndex = 0,
        .totalCount = 0,
        .wrapFocus = params->wrapFocus
    });
    cel_watch(focus);

    /* 4. Publish focus state down subtree via clean ambient context */
    if (focus != NULL) {
        cel_set_context(CelsFocusState, focus);
    }
}

/**
 * @brief Internal lifecycle hook executed when exiting a CEL_Layout container.
 *
 * Receives the exact settled child count from the slot table engine. Automatically
 * clamps the active focus index to valid child bounds, updates total count, and
 * processes directional input navigation actions (Up/Down/Left/Right/2D Grid).
 *
 * @param childCount Settled count of direct child composables executed within this container.
 * @param layoutData Pointer to caller-allocated CelsLayoutParams.
 */
static inline void _CelsLayoutOnGroupEnd(uint32_t childCount, void *layoutData)
{
    CelsLayoutParams *params = (CelsLayoutParams*)layoutData;
    if (params == NULL || childCount == 0) return;

    const CelsFocusState *focusRO = NULL;
    if (cel_get_context(CelsFocusState, &focusRO) != CELS_OK || focusRO == NULL) {
        return;
    }

    CelsFocusState *focus = (CelsFocusState*)focusRO;
    int count = (int)childCount;
    focus->totalCount = count;

    /* Clamp focus if activeIndex is out of range */
    if (focus->activeIndex >= count) {
        cel_mutate(focus) {
            this->activeIndex = (count > 0) ? (count - 1) : 0;
            this->totalCount = count;
        }
    }

    /* Directional navigation handling: dynamically read direction */
    const CelsLayoutDir *dirComp = cel_get(CelsLayoutDir);
    CelsLayoutDir dir = dirComp ? *dirComp : params->dir;
    int cols = (params->columns > 1) ? params->columns : 1;

    /* Vertical navigation: W/S or Up/Down */
    if (dir & CEL_LAYOUT_DIR_VERT) {
        if (cel_action_consume(ACTION_NAV_NEXT)) {
            cel_mutate(focus) {
                this->totalCount = count;
                if (cols > 1) {
                    /* In a 2D grid, Down moves down a row (+cols) */
                    int next = this->activeIndex + cols;
                    if (next < count) {
                        this->activeIndex = next;
                    } else if (params->wrapFocus) {
                        this->activeIndex = (this->activeIndex % cols);
                    }
                } else {
                    this->activeIndex = (this->activeIndex + 1) % count;
                }
            }
        } else if (cel_action_consume(ACTION_NAV_PREV)) {
            cel_mutate(focus) {
                this->totalCount = count;
                if (cols > 1) {
                    /* In a 2D grid, Up moves up a row (-cols) */
                    int prev = this->activeIndex - cols;
                    if (prev >= 0) {
                        this->activeIndex = prev;
                    } else if (params->wrapFocus) {
                        int col = this->activeIndex % cols;
                        int lastRowStart = ((count - 1) / cols) * cols;
                        int target = lastRowStart + col;
                        this->activeIndex = (target < count) ? target : target - cols;
                    }
                } else {
                    this->activeIndex = (this->activeIndex + count - 1) % count;
                }
            }
        }
    }

    /* Horizontal navigation: A/D or Left/Right */
    if (dir & CEL_LAYOUT_DIR_HORIZ) {
        if (cel_action_consume(ACTION_VALUE_INC)) {
            cel_mutate(focus) {
                this->totalCount = count;
                if (cols > 1) {
                    /* In a 2D grid, Right moves +1 within row or wraps row */
                    if ((this->activeIndex + 1) % cols != 0 && (this->activeIndex + 1) < count) {
                        this->activeIndex++;
                    } else if (params->wrapFocus) {
                        this->activeIndex = this->activeIndex - (this->activeIndex % cols);
                    }
                } else {
                    this->activeIndex = (this->activeIndex + 1) % count;
                }
            }
        } else if (cel_action_consume(ACTION_VALUE_DEC)) {
            cel_mutate(focus) {
                this->totalCount = count;
                if (cols > 1) {
                    /* In a 2D grid, Left moves -1 within row or wraps row */
                    if (this->activeIndex % cols != 0) {
                        this->activeIndex--;
                    } else if (params->wrapFocus) {
                        int rowEnd = this->activeIndex + cols - 1;
                        this->activeIndex = (rowEnd < count) ? rowEnd : count - 1;
                    }
                } else {
                    this->activeIndex = (this->activeIndex + count - 1) % count;
                }
            }
        }
    }
}

/* ========================================================================= */
/* Reusable CEL_Layout Macro                                                 */
/* Clean 1-line definitions delegating directly to child-aware cel_container!*/
/* ========================================================================= */

#define _CEL_LAYOUT_4(Name, dirVal, colsVal, wrapVal) \
    cel_container(Name, _CelsLayoutOnGroupStart, _CelsLayoutOnGroupEnd, \
                  &((CelsLayoutParams){ .dir = (dirVal), .columns = (colsVal), .wrapFocus = (wrapVal) }))

#define _CEL_LAYOUT_3(Name, dirVal, colsVal) \
    _CEL_LAYOUT_4(Name, dirVal, colsVal, true)

#define _CEL_LAYOUT_2(Name, dirVal) \
    _CEL_LAYOUT_4(Name, dirVal, 0, true)

#define _CEL_LAYOUT_1(dirVal) \
    _CEL_LAYOUT_4(CelsLayout, dirVal, 0, true)

/**
 * @def CEL_Layout
 * @brief Child-aware layout container with directional focus routing and ambient context.
 *
 * What it does:
 * Creates an inline container group delegating directly to cel_container. Attaches
 * the layout direction as an ECS component (cel_has(CelsLayoutDir)), manages reactive
 * focus state (CelsFocusState), publishes both down the subtree via ambient context,
 * and automatically routes directional input navigation across settled child composables.
 *
 * Signatures:
 * - CEL_Layout(dir): Anonymous linear layout (vertical, horizontal, or 2D grid).
 * - CEL_Layout(Name, dir): Named 1D layout container in slot table.
 * - CEL_Layout(Name, dir, cols): Named 2D grid container with specified column count.
 * - CEL_Layout(Name, dir, cols, wrapFocus): Full configuration with wrap behavior.
 *
 * Example:
 * @code
 *     CEL_Layout(CEL_LAYOUT_DIR_VERT) {
 *         VolumeSlider(settings);
 *         MuteToggle(settings);
 *         GraphicsSelector(settings);
 *     }
 * @endcode
 */
#define CEL_Layout(...) \
    _CEL_GET_MACRO_4(__VA_ARGS__, _CEL_LAYOUT_4, _CEL_LAYOUT_3, _CEL_LAYOUT_2, _CEL_LAYOUT_1)(__VA_ARGS__)

/* ========================================================================= */
/* Child Composable Focus Query Helpers                                      */
/* ========================================================================= */

/**
 * @brief Registers the calling composable as a focusable element in the ambient layout.
 *
 * Queries the ambient CelsFocusState context and the caller's sibling child index.
 * Returns true if the caller currently holds active keyboard/controller focus.
 * Protected by CelsResult against missing ambient context or outside-container calls.
 *
 * @return True if this element has active focus; false otherwise.
 */
static inline bool cel_focusable(void)
{
    const CelsFocusState *focus = NULL;
    uint32_t myIdx = 0;
    if (cel_get_context(CelsFocusState, &focus) != CELS_OK || focus == NULL) {
        return false;
    }
    if (cel_child_index(&myIdx) != CELS_OK) {
        return false;
    }
    return (focus->activeIndex == (int)myIdx);
}

/**
 * @brief Queries whether the currently executing focusable element has active focus.
 *
 * Alias for cel_focusable().
 *
 * @return True if focused; false otherwise.
 */
static inline bool cel_is_focused(void)
{
    return cel_focusable();
}

/**
 * @brief Queries the active layout direction from ambient context.
 *
 * @param[out] outDir Pointer to destination CelsLayoutDir variable.
 * @return CELS_OK on success; CELS_ERROR_INVALID_ARGUMENT if outDir is NULL;
 *         CELS_ERROR_NOT_FOUND if called outside a layout scope.
 */
static inline CelsResult cel_layout_direction(CelsLayoutDir *outDir)
{
    if (outDir == NULL) return CELS_ERROR_INVALID_ARGUMENT;
    const CelsLayoutDir *dir = NULL;
    if (cel_get_context(CelsLayoutDir, &dir) == CELS_OK && dir != NULL) {
        *outDir = *dir;
        return CELS_OK;
    }
    return CELS_ERROR_NOT_FOUND;
}

#ifdef __cplusplus
}
#endif
