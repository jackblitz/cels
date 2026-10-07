#include "cels.h"
#include "theming_state.h"
#include "cels_input.h"
#include "tui_renderer.h"
#include "common_events.h"
#include <stdio.h>

/* ========================================================================= */
/* Pre-defined Theme Palettes                                                */
/* ========================================================================= */

static const UITheme g_themes[3] = {
    { .name = "Light Mode",       .bg = 0xFFF5F5F5, .text = 0xFF222222, .accent = 0xFF0066CC, .cornerRadius = 8.0f,  .padding = 12.0f },
    { .name = "Dark Charcoal",    .bg = 0xFF1E1E1E, .text = 0xFFEEEEEE, .accent = 0xFF9C27B0, .cornerRadius = 10.0f, .padding = 16.0f },
    { .name = "Cyberpunk Neon",   .bg = 0xFF0A0A12, .text = 0xFF00FFCC, .accent = 0xFFFF0055, .cornerRadius = 2.0f,  .padding = 14.0f }
};

static const UITheme g_modalSubtreeTheme = {
    .name = "Midnight Modal",
    .bg = 0xFF0D1B2A,
    .text = 0xFFE0E1DD,
    .accent = 0xFF415A77,
    .cornerRadius = 12.0f,
    .padding = 18.0f
};

static const UITheme g_criticalAlertTheme = {
    .name = "High-Contrast Caution",
    .bg = 0xFF000000,
    .text = 0xFFFFFF00,
    .accent = 0xFFFF0000,
    .cornerRadius = 0.0f,
    .padding = 20.0f
};

/* ========================================================================= */
/* Leaf Composables: Zero Theme Parameters (Reads cel_get_context)           */
/* ========================================================================= */

/**
 * ThemedButton has NO theme parameter!
 * It reads the ambient theme dynamically via result-protected cel_get_context(UITheme, &theme).
 * Also introspects its sibling index within the enclosing cel_container.
 */
CEL_Composable(ThemedButton, const char*, label)
{
    const UITheme *theme = NULL;
    CelsResult res = cel_get_context(UITheme, &theme);
    const char *themeName = (res == CELS_OK && theme != NULL) ? theme->name : "Default";
    unsigned int accent = (unsigned int)((res == CELS_OK && theme != NULL) ? (theme->accent & 0xFFFFFF) : 0x007ACC);

    uint32_t childIdx = 0;
    if (cel_child_index(&childIdx) == CELS_OK) {
        TuiPrint("    [%u. Btn: %-16s] Accent=#%06X (Inherited: '%s')", (unsigned)(childIdx + 1), label, accent, themeName);
    } else {
        TuiPrint("    [Btn: %-16s] Accent=#%06X (Inherited: '%s')", label, accent, themeName);
    }
}

/**
 * Nested Alert Composable: Sets its own overridden theme via cel_set_context.
 */
CEL_Composable(CriticalAlertModal)
{
    /* Override ambient theme for this modal subtree */
    cel_set_context(UITheme, &g_criticalAlertTheme);

    const UITheme *alertTheme = cel_get_context(UITheme);
    TuiPrint("    *** Nested Alert Override (Theme: '%s', Text=#%06X) ***",
             alertTheme ? alertTheme->name : "None",
             (unsigned int)(alertTheme ? (alertTheme->text & 0xFFFFFF) : 0));
    ThemedButton("CRITICAL PURGE");
}

/**
 * Modal Subtree: Pushes an overridden ambient theme for this dialog branch.
 */
CEL_Composable(ConfirmationModal, const ThemingAppState*, state)
{
    cel_watch(state);

    /* Push the Midnight Modal theme onto the ambient scope stack */
    cel_set_context(UITheme, &g_modalSubtreeTheme);

    const UITheme *theme = cel_get_context(UITheme);
    TuiDivider();
    TuiPrint("  >>> Modal Dialog Subtree Override (Theme: '%s', BG=#%06X) <<<",
             theme ? theme->name : "None",
             (unsigned int)(theme ? (theme->bg & 0xFFFFFF) : 0));

    cel_container(ModalActions) {
        ThemedButton("Confirm Action");
        ThemedButton("Dismiss");
    }

    /* Multi-Level Nested Override: Critical alert pushes yet another theme */
    if (state->showCriticalAlert) {
        CriticalAlertModal();
    }
}

/**
 * Dashboard HUD Composable.
 */
CEL_Composable(ThemingHUD, const ThemingAppState*, state)
{
    cel_watch(state);

    const UITheme *theme = cel_get_context(UITheme);

    TuiWindow("CELS Scoped Ambient Context & Theming (cel_set_context / cel_get_context)", "Profile: 512");
    TuiPrint("Active Root Theme:  '%s'", theme ? theme->name : "Default");
    TuiPrint("Accent Color:       #%06X", (unsigned int)(theme ? (theme->accent & 0xFFFFFF) : 0));
    TuiPrint("Corner Radius:      %.1f px", theme ? theme->cornerRadius : 0.0f);
    TuiPrint("Modal Subtree:      %s", state->showDarkModal ? "[ MOUNTED / ACTIVE ]" : "[ CLOSED ]");
    TuiPrint("Critical Alert:     %s", state->showCriticalAlert ? "[ MOUNTED / ACTIVE ]" : "[ OFF ]");
    TuiDivider();
    TuiLine("Root-Level Composable Buttons (Reading cel_get_context inside cel_container):");
    cel_container(RootToolbar) {
        ThemedButton("Save Preferences");
        ThemedButton("Refresh View");
        ThemedButton("System Settings");
    }

    /* Render modal with ambient subtree overriding */
    if (state->showDarkModal) {
        ConfirmationModal(state);
    }

    TuiDivider();
    TuiFooter("Controls: [T] Cycle Theme | [M] Toggle Modal | [A] Toggle Alert | [Q] Quit");
}

typedef enum ThemingAction {
    THEME_ACTION_CYCLE = 1,
    THEME_ACTION_MODAL,
    THEME_ACTION_ALERT
} ThemingAction;

static const CelsKeyBinding g_themingBindings[] = {
    { 't', THEME_ACTION_CYCLE },
    { 'm', THEME_ACTION_MODAL },
    { 'a', THEME_ACTION_ALERT }
};
static const CelsInputMap g_themingMap = CELS_INPUT_MAP("ThemingMap", g_themingBindings);

/* ========================================================================= */
/* Root Composition & Application Definition                                 */
/* ========================================================================= */

CEL_Composition(ThemingAppComposition)
{
    ThemingAppState *state = cel_remember_state(ThemingAppState, {
        .currentThemeIndex = 0,
        .showDarkModal = true,
        .showCriticalAlert = false,
        .actionCount = 0
    });

    /* Explicitly attach ambient input map and root theme down the entire tree */
    cel_set_context(CelsInputMap, &g_themingMap);
    cel_set_context(UITheme, &g_themes[state->currentThemeIndex]);

    if (cel_action_consume(THEME_ACTION_CYCLE)) {
        cel_mutate(state) {
            this->currentThemeIndex = (this->currentThemeIndex + 1) % 3;
            this->actionCount++;
        }
    } else if (cel_action_consume(THEME_ACTION_MODAL)) {
        cel_mutate(state) {
            this->showDarkModal = !this->showDarkModal;
            this->actionCount++;
        }
    } else if (cel_action_consume(THEME_ACTION_ALERT)) {
        cel_mutate(state) {
            this->showCriticalAlert = !this->showCriticalAlert;
            this->actionCount++;
        }
    }

    ThemingHUD(state);
}

/**
 * Declarative Application Module Definition.
 */
CEL_App(ThemingApp, ThemingAppComposition);
