# Authoring Primitives, Base Classes & DSL Archetypes

This guide explains how framework architects, engine authors, and UI library developers build custom declarative primitives and container archetypes (like [`CEL_Layout`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h), `CEL_FlexBox`, `CEL_SplitPane`, or custom ECS binders) in **CELS**.

---

## 1. Architectural Mental Model: Archetypes vs. Composables

In object-oriented GUI frameworks, developers frequently derive from base classes (`Widget`, `Container`, `LayoutGroup`). In modern declarative frameworks (Jetpack Compose, SwiftUI, React), the fundamental mental model is functional composition: **the Composable is the universal building block**.

In CELS, this distinction is enforced by a single architectural rule:

### The Golden Rule of CELS Development
> **Preprocessor `#define` macros are reserved exclusively for framework-level archetypes (the "base classes" and DSL constructs of the library or engine). Application developers should NEVER write `#define` for everyday widgets, menus, or screens.**

| Role | Target Author | Primary Tools | Examples |
| :--- | :--- | :--- | :--- |
| **Framework Archetype** ("Base Class") | Engine / UI Library Developer | `#define` + `for (...)` + [`cel_container`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) | [`CEL_Composition`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`CEL_Layout`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h), [`cel_container`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_key`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) |
| **Application Component** | Game / App Developer | Standard C functions ([`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h)) | `PauseMenu`, `InventoryGrid`, `VolumeSlider`, `HealthBar` |

### Why This Rule Exists:
1. **Composables Are the Universal Base**: Application developers create reusable components simply by writing [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h). They never write `#define MyWidget(...)`.
2. **Tooling & Debuggability**: Standard C functions preserve IDE autocomplete, static analysis, parameter hints, and clean callstacks during debugging.
3. **Ergonomic Declarative Syntax**: Framework developers encapsulate complex slot table entry/exit, double-buffered state allocation, and ambient context cascading inside clean, language-like archetypes.

---

## 2. C99 Grammar Constraints: How Trailing Blocks `{ ... }` Work

In languages like Swift or Kotlin, trailing closures or lambdas allow functions to accept code blocks:
```kotlin
// Kotlin trailing lambda
Column {
    Text("Hello")
}
```

In ANSI C99, **a function call cannot be followed by a trailing code block**:
```c
// ILLEGAL IN C99:
MyContainer() { // Compiler Error: expected ';' before '{' token!
    ChildWidget();
}
```

In C, the **only** language statements that can immediately precede a compound statement `{ ... }` are:
- `for (init; cond; step) { ... }`
- `if (cond) { ... }`
- `while (cond) { ... }`
- `switch (expr) { ... }`

Therefore, any declarative container archetype that accepts an inline block of child composables `{ ... }` **must expand to a control loop**.

---

## 3. The Core Foundation: `cel_container` with Lifecycle Hooks

CELS provides [`cel_container`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) as the zero-heap, RAII-like slot grouping primitive. It natively supports optional **`onGroupStart`** and **`onGroupEnd`** lifecycle hooks:

```c
typedef void (*CelsContainerStartHook)(void *userData);
typedef void (*CelsContainerEndHook)(uint32_t childCount, void *userData);
```

### Signatures Supported:
- `cel_container(Name)` - Simple grouping without hooks.
- `cel_container(Name, onGroupStart)` - Runs setup on enter.
- `cel_container(Name, onGroupStart, onGroupEnd)` - Runs setup on enter and teardown/settlement on exit.
- `cel_container(Name, onGroupStart, onGroupEnd, userData)` - Full configuration with typed user context.

### Lifecycle Execution:
1. **Scope Enter (`onGroupStart`)**: `CelsContainerScopeEnter` calls `CelsEnterComposable(sess, key)` to establish a new group in the slot table. It then immediately executes `onGroupStart(userData)` *inside* the active group scope, allowing you to attach components (`cel_has`), publish ambient context (`cel_set_context`), and allocate reactive state (`cel_remember_state`).
2. **Block Execution**: The caller's `{ ... }` child composables execute sequentially within the group. Sibling indexes and counts are recorded in $\mathcal{O}(1)$.
3. **Scope Exit (`onGroupEnd`)**: When the block completes, `CelsContainerScopeExit` reads the exact settled child count from the slot engine and invokes `onGroupEnd(childCount, userData)`. You can perform post-layout geometry settlement, index clamping, or focus navigation with full awareness of all children!
4. **Group Finalization**: `CelsExitGroup(sess)` is called, closing the slot group.

---

## 4. The 3 Archetype Design Patterns

### Pattern 1: Scoped Container Archetypes (Clean 1-Line Macros)
With `cel_container`'s hook API, authoring a custom layout container requires **zero nested loops** and **zero comma-operator hacks**. The macro is a clean, readable one-liner:

```c
// 1. Setup hook: Runs before children execute
static inline void _ToolbarStart(void *userData) {
    ToolbarParams *params = (ToolbarParams*)userData;
    cel_has(ToolbarParams, *params);
    cel_set_context(ToolbarParams, params);
}

// 2. Settlement hook: Runs after children execute with settled childCount!
static inline void _ToolbarEnd(uint32_t childCount, void *userData) {
    printf("Toolbar finished rendering %u buttons!\n", childCount);
}

// 3. Clean 1-line macro archetype!
#define CEL_Toolbar(Name, dir, cols) \
    cel_container(Name, _ToolbarStart, _ToolbarEnd, &((ToolbarParams){ dir, cols }))
```

```c
// Caller usage: Zero #define required by application developers!
CEL_Toolbar(MainToolbar, LAYOUT_HORIZ, 3) {
    Button("New");
    Button("Open");
    Button("Save");
}
```

---

### Pattern 2: Declaration Archetypes (Declaring Components & Functions)
Use this pattern when introducing a specialized type of composable or root lifecycle node (e.g. `CEL_AudioNode`, `CEL_PhysicsBody`, or `CEL_CustomWindow`).

Like [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), a declaration archetype emits a forward declaration and a static inline wrapper function:

```c
#define CEL_AudioNode(Name, ...) \
    static void _audio_body_##Name(__VA_ARGS__); \
    static inline void Name(__VA_ARGS__) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        if (CelsEnterComposable(sess, CelsHashKey(#Name))) { \
            _SetupAudioContext(); \
            _audio_body_##Name(__VA_ARGS__); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _audio_body_##Name(__VA_ARGS__)
```

Application developers then declare and call components naturally:
```c
// Declaration:
CEL_AudioNode(OscillatorNode, float frequency) {
    cel_has(OscillatorComponent, { .freq = frequency });
}

// Call:
OscillatorNode(440.0f);
```

---

### Pattern 3: Scoped Identity / Entity Archetypes (`cel_key`)
Use this pattern when establishing unique identity for item reconciliation in dynamic lists.

```c
#define cel_key(id) \
    for (CelsEntityScope _cels_key_scope = { 0 }; \
         !_cels_key_scope.isActive && CelsEnterEntityScope(CelsGetCurrentSession(), (uint64_t)(id), NULL, &_cels_key_scope); \
         CelsExitEntityScope(CelsGetCurrentSession(), &_cels_key_scope))
```

---

## 5. Complete Worked Example: Authoring `CEL_FlexBox`

Below is a complete, real-world custom layout primitive:

```c
#pragma once
#include "cels.h"

typedef enum CelsFlexAlign {
    FLEX_ALIGN_START,
    FLEX_ALIGN_CENTER,
    FLEX_ALIGN_END
} CelsFlexAlign;

typedef struct CelsFlexParams {
    CelsLayoutDir dir;
    CelsFlexAlign align;
    float         gap;
} CelsFlexParams;

CEL_State(CelsFlexFocusState) {
    int activeIndex;
    int totalItems;
};

static inline void _CelsFlexOnStart(void *layoutData) {
    CelsFlexParams *params = (CelsFlexParams*)layoutData;
    if (params == NULL) return;

    /* 1. Attach declarative ECS component */
    cel_has(CelsFlexParams, { params->dir, params->align, params->gap });

    /* 2. Publish ambient context down subtree */
    cel_set_context(CelsFlexParams, params);

    /* 3. Allocate/resolve reactive focus state */
    CelsFlexFocusState *focus = cel_remember_state(CelsFlexFocusState, {0});
    cel_watch(focus);

    cel_set_context(CelsFlexFocusState, focus);
}

static inline void _CelsFlexOnEnd(uint32_t childCount, void *layoutData) {
    (void)layoutData;
    /* 4. Child-aware settlement hook: childCount is exact and settled! */
    const CelsFlexFocusState *focusRO = NULL;
    if (cel_get_context(CelsFlexFocusState, &focusRO) == CELS_OK && focusRO) {
        CelsFlexFocusState *focus = (CelsFlexFocusState*)focusRO;
        focus->totalItems = (int)childCount;
    }
}

/* Overloaded Macro Archetype: Clean 1-liners! */
#define _CEL_FLEX_3(Name, dirVal, alignVal) \
    cel_container(Name, _CelsFlexOnStart, _CelsFlexOnEnd, \
                  &((CelsFlexParams){ .dir = (dirVal), .align = (alignVal), .gap = 0.0f }))

#define _CEL_FLEX_2(dirVal, alignVal) \
    _CEL_FLEX_3(CelsFlex, dirVal, alignVal)

#define _CEL_FLEX_1(dirVal) \
    _CEL_FLEX_2(dirVal, FLEX_ALIGN_START)

#define CEL_FlexBox(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_FLEX_3, _CEL_FLEX_2, _CEL_FLEX_1)(__VA_ARGS__)
```

### How Application Developers Consume It:
Application developers use `CEL_FlexBox` directly without writing any `#define`:

```c
CEL_Composable(HeaderBar, const char *title) {
    CEL_FlexBox(CEL_LAYOUT_DIR_HORIZ, FLEX_ALIGN_CENTER) {
        AppLogo();
        TitleLabel(title);
        UserAvatar();
    }
}
```

Or they wrap it in a reusable composable:
```c
CEL_Composable(ActionRow, void *ctx) {
    CEL_FlexBox(CEL_LAYOUT_DIR_HORIZ, FLEX_ALIGN_START) {
        Button("Cancel");
        Button("Apply");
        Button("OK");
    }
}

// Call: ActionRow(NULL);
```

---

## 6. Architectural Rules & Invariants

1. **Always delegate to `cel_container`**: Never implement manual `CelsEnterComposable` and `CelsExitGroup` calls for container blocks. `cel_container` ensures slot groups are properly closed even under complex nesting.
2. **Zero Heap Allocation**: All context and state allocations must use stack structs, `cel_remember_state`, or ECS transaction buffers.
3. **Component vs. State Separation**: Orientation, alignment, and static layout attributes belong in `cel_has` components and ambient context (`cel_set_context`). Only temporal dynamic values (like active focus index) belong in reactive `cel_state`.
4. **Result Protection**: All child introspection queries ([`cel_child_info`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_child_index`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_child_count`](file:///D:/cels-workspace/library/cels/include/cels/cels.h)) return `CelsResult` to guard against usage outside of containers.
5. **C99 Portability**: Always explicitly cast compound literals: `((Type){ ... })`. Avoid non-standard GCC statement expressions (`({ ... })`).
