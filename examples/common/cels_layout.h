#pragma once

/**
 * @file cels_layout.h
 * @brief Reusable Layout, Directional Navigation, and Ambient Focus Management Pattern for CELS.
 *
 * NOTE: This file is an application-level architectural pattern demonstrating how
 * developers can author custom layout engines (1D linear rows/columns, 2D grids, menus)
 * on top of core CELS framework primitives (CelsEnterComposable, cel_has, cel_set_context).
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
 *    - Managed natively by `CEL_Lifecycle(CelsLayoutLifecycle)`: `before_children` sets up direction & focus
 *      before children run; `after_children(childCount)` receives the settled `childCount` directly from `CelsExitGroup`,
 *      enabling instant focus clamping and boundary checks on frame 0 with zero latency or manual scope boilerplate.
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
#include <stdio.h>
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
 * @struct CelsLayout
 * @brief Declarative layout configuration component attached to container composables via cel_has().
 */
typedef struct CelsLayout {
    CelsLayoutDir dir;         /**< Layout navigation direction (vertical, horizontal, or 2D). */
    int           columns;     /**< Grid columns (0 or 1 for linear; >= 2 for 2D grid). */
    bool          wrapFocus;   /**< Whether focus navigation wraps around boundaries. */
    uint8_t       reserved[3]; /**< Structure alignment padding. */
} CelsLayout;

/* ========================================================================= */
/* Reactive Focus State                                                      */
/* Clean, double-buffered reactive state managing focus indices.             */
/* Note: FocusState ONLY tracks focus indices; direction is in Component!   */
/* ========================================================================= */

/**
 * @brief Reactive state tracking active keyboard/controller focus index within a layout.
 *
 * Note: FocusState ONLY tracks dynamic runtime focus indices; structural direction is
 * stored separately as a declarative Component (CelsLayout).
 */
CEL_State(CelsFocusState) {
    int     activeIndex; /**< Index of currently focused child widget (0-based). */
    int     totalCount;  /**< Total focusable widgets registered in this layout. */
    bool    wrapFocus;   /**< Whether focus navigation wraps around edges. */
    uint8_t reserved[3]; /**< Structure alignment padding. */
};

/* ========================================================================= */
/* Layout Setup & Child-Aware Focus Routing (CEL_Lifecycle)                  */
/* ========================================================================= */

/**
 * @brief Internal child-settlement focus processor executed during the end phase.
 *
 * Receives the exact settled child count from the slot table engine. Automatically
 * clamps the active focus index to valid child bounds, updates total count, and
 * processes directional input navigation actions (Up/Down/Left/Right/2D Grid).
 *
 * @param childCount Settled count of direct child composables executed within this container.
 * @param layout     Pointer to layout configuration component.
 */
static inline void _CelsLayoutOnGroupEnd(uint32_t childCount, const CelsLayout *layout)
{
    if (layout == NULL || childCount == 0) return;

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
    CelsLayoutDir dir = layout->dir;
    int cols = (layout->columns > 1) ? layout->columns : 1;

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
                    } else if (layout->wrapFocus) {
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
                    } else if (layout->wrapFocus) {
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
                    } else if (layout->wrapFocus) {
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
                    } else if (layout->wrapFocus) {
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

/**
 * @brief Native CEL_Lifecycle managing layout direction, ambient focus, and child-count settlement.
 *
 * Reads the CelsLayout component attached to the active entity via cel_get(CelsLayout).
 * - before_children block: publishes ambient context and initializes reactive focus state.
 * - after_children(childCount) block: receives settled childCount on group exit and routes directional focus.
 */
CEL_Lifecycle(CelsLayoutLifecycle) {
    before_children {
        const CelsLayout *layout = cel_get(CelsLayout);
        if (layout == NULL) {
            cel_get_context(CelsLayout, &layout);
        }
        if (layout == NULL) return;

        /* 1. Subscribe to input events so key presses invalidate and route focus */
        cel_watch_state(CelsInputState);

        /* 2. Publish layout configuration down the subtree via ambient context */
        cel_set_context(CelsLayout, layout);
        cel_set_context(CelsLayoutDir, &layout->dir);

        /* 3. Allocate/resolve reactive focus state */
        CelsFocusState *focus = cel_remember_state(CelsFocusState, {
            .activeIndex = 0,
            .totalCount = 0,
            .wrapFocus = layout->wrapFocus
        });
        cel_watch(focus);

        /* 4. Publish focus state down subtree via clean ambient context */
        if (focus != NULL) {
            cel_set_context(CelsFocusState, focus);
        }
    }
    after_children(childCount) {
        const CelsLayout *layout = cel_get(CelsLayout);
        if (layout == NULL) {
            cel_get_context(CelsLayout, &layout);
        }
        _CelsLayoutOnGroupEnd(childCount, layout);
    }
}

/* ========================================================================= */
/* Layout Container Composables                                              */
/* Standard CEL_Composable nodes usable with cel_scope(Name, ...) { ... }    */
/* ========================================================================= */

static inline void _CelsLayoutSetup(CelsLayout layout) {
    CelsEntityRegisterComponent(CelsGetCurrentSession(), CelsHashKey("CelsLayout"), sizeof(CelsLayout), &layout);
    cel_watch_state(CelsInputState);

    const CelsLayout *l = cel_get(CelsLayout);
    if (l == NULL) l = &layout;
    cel_set_context(CelsLayout, l);
    cel_set_context(CelsLayoutDir, &l->dir);

    CelsFocusState *focus = cel_remember_state(CelsFocusState, {
        .activeIndex = 0,
        .totalCount = 0,
        .wrapFocus = layout.wrapFocus
    });
    cel_watch(focus);
    if (focus != NULL) {
        cel_set_context(CelsFocusState, focus);
    }
}

/**
 * @brief 1D Linear Layout Container Composable (Vertical or Horizontal).
 */
CEL_Composable(CelsLayoutLinear, CelsLayoutDir, dir) {
    CelsLayout layout = { .dir = dir, .columns = 0, .wrapFocus = true };
    _CelsLayoutSetup(layout);
    on_exit(childCount) {
        const CelsLayout *l = cel_get(CelsLayout);
        _CelsLayoutOnGroupEnd(childCount, l ? l : &layout);
    }
}

/**
 * @brief 2D Grid Layout Container Composable with column count and wrap behavior.
 */
CEL_Composable(CelsLayoutGrid, int, columns, bool, wrapFocus) {
    CelsLayout layout = { .dir = CEL_LAYOUT_DIR_2D, .columns = columns, .wrapFocus = wrapFocus };
    _CelsLayoutSetup(layout);
    on_exit(childCount) {
        const CelsLayout *l = cel_get(CelsLayout);
        _CelsLayoutOnGroupEnd(childCount, l ? l : &layout);
    }
}

/**
 * @brief Keyed Layout Container Composable for explicit slot table identity.
 */
CEL_Composable(CelsLayoutKeyed, id, CelsLayoutDir, dir, int, columns, bool, wrapFocus) {
    CelsLayout layout = { .dir = dir, .columns = columns, .wrapFocus = wrapFocus };
    _CelsLayoutSetup(layout);
    on_exit(childCount) {
        const CelsLayout *l = cel_get(CelsLayout);
        _CelsLayoutOnGroupEnd(childCount, l ? l : &layout);
    }
}

/**
 * @brief Vertical Column Container Composable (equivalent to Jetpack Compose Column).
 */
CEL_Composable(CelsColumn) {
    CelsLayout layout = { .dir = CEL_LAYOUT_DIR_VERT, .columns = 0, .wrapFocus = true };
    _CelsLayoutSetup(layout);
    on_exit(childCount) {
        const CelsLayout *l = cel_get(CelsLayout);
        _CelsLayoutOnGroupEnd(childCount, l ? l : &layout);
    }
}

/**
 * @brief Horizontal Row Container Composable (equivalent to Jetpack Compose Row).
 */
CEL_Composable(CelsRow) {
    CelsLayout layout = { .dir = CEL_LAYOUT_DIR_HORIZ, .columns = 0, .wrapFocus = true };
    _CelsLayoutSetup(layout);
    on_exit(childCount) {
        const CelsLayout *l = cel_get(CelsLayout);
        _CelsLayoutOnGroupEnd(childCount, l ? l : &layout);
    }
}

/* ========================================================================= */
/* Reusable CEL_Layout Macro                                                 */
/* Clean zero-plumbing alias mapping directly to cel_scope().                */
/* ========================================================================= */

#define _CEL_LAYOUT_4(Name, dirVal, colsVal, wrapVal) \
    cel_scope(CelsLayoutKeyed, CelsHashKey(#Name), (dirVal), (colsVal), (wrapVal))

#define _CEL_LAYOUT_3(Name, dirVal, colsVal) \
    cel_scope(CelsLayoutKeyed, CelsHashKey(#Name), (dirVal), (colsVal), true)

#define _CEL_LAYOUT_2(Name, dirVal) \
    cel_scope(CelsLayoutKeyed, CelsHashKey(#Name), (dirVal), 0, true)

#define _CEL_LAYOUT_1(dirVal) \
    cel_scope(CelsLayoutLinear, (dirVal))

/**
 * @def CEL_Layout
 * @brief Child-aware layout container with directional focus routing and ambient context.
 *
 * Built directly on top of cel_scope() and standard CEL_Composable nodes.
 *
 * Signatures:
 * - CEL_Layout(dir): Linear layout (vertical, horizontal, or 2D grid).
 * - CEL_Layout(Name, dir): Named 1D layout container in slot table.
 * - CEL_Layout(Name, dir, cols): Named 2D grid container with specified column count.
 * - CEL_Layout(Name, dir, cols, wrapFocus): Full configuration with wrap behavior.
 *
 * Alternatively, use cel_scope directly:
 * - cel_scope(CelsLayoutLinear, CEL_LAYOUT_DIR_VERT) { ... }
 * - cel_scope(CelsLayoutGrid, 4, true) { ... }
 * - cel_scope(CelsColumn) { ... }
 * - cel_scope(CelsRow) { ... }
 */
#define CEL_Layout(...) \
    _CEL_GET_MACRO_4(__VA_ARGS__, _CEL_LAYOUT_4, _CEL_LAYOUT_3, _CEL_LAYOUT_2, _CEL_LAYOUT_1)(__VA_ARGS__)

#ifndef Column
/**
 * @def Column
 * @brief Vertical linear layout container block (equivalent to Jetpack Compose Column).
 */
#define Column(...) cel_scope(CelsColumn, ##__VA_ARGS__)
#endif

#ifndef Row
/**
 * @def Row
 * @brief Horizontal linear layout container block (equivalent to Jetpack Compose Row).
 */
#define Row(...) cel_scope(CelsRow, ##__VA_ARGS__)
#endif

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
