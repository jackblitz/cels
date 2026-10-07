#pragma once

/**
 * @file theming_state.h
 * @brief Ambient Environmental Context Types and Reactive State for Theme Cascading.
 */

#include "cels.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Ambient Environmental Context Types (Cascaded via cel_context)            */
/* ========================================================================= */

/**
 * @struct UITheme
 * @brief Ambient style design tokens propagated down composable subtrees.
 */
typedef struct UITheme {
    const char *name;         /**< Human-readable theme label (8 bytes). */
    uint32_t    bg;           /**< Background color ARGB/HEX (4 bytes). */
    uint32_t    text;         /**< Primary text color ARGB/HEX (4 bytes). */
    uint32_t    accent;       /**< Accent highlight color ARGB/HEX (4 bytes). */
    float       cornerRadius; /**< Rounded corner radius in pixels (4 bytes). */
    float       padding;      /**< Inner content padding in pixels (4 bytes). */
} UITheme;

/* ========================================================================= */
/* Reactive Application State                                                */
/* Fields ordered in descending alignment (4B -> 1B) for zero padding.        */
/* ========================================================================= */

/**
 * @brief Reactive state driving the ambient theming and modal override demo.
 */
CEL_State(ThemingAppState) {
    int     currentThemeIndex; /**< Active theme palette index: 0=Light, 1=Dark, 2=Cyberpunk (4 bytes). */
    int     actionCount;       /**< Total user actions executed (4 bytes). */
    bool    showDarkModal;     /**< Whether modal dialog subtree is mounted (1 byte). */
    bool    showCriticalAlert; /**< Whether nested high-contrast alert is mounted (1 byte). */
    uint8_t reserved[2];       /**< Alignment padding (2 bytes). */
};

#ifdef __cplusplus
}
#endif
