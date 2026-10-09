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
| **Framework Archetype** ("Base Class") | Engine / UI Library Developer | `cel_scope` + `CEL_Composable` + inline hooks (`on_exit`, `on_attach`, `on_detach`) / `CEL_Lifecycle` | [`CEL_Composition`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_scope`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`CEL_Layout`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h), `CelsColumn`, `CelsRow` |
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

## 3. The Core Foundation: Inline Lifecycle Hooks & Code-to-Code Execution

CELS slot tables track hierarchical composable boundaries through [`CelsEnterComposable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) and [`CelsExitGroup`](file:///D:/cels-workspace/library/cels/include/cels/cels.h). When authoring a custom container archetype (like `CelsColumn`, `CelsRow`, `CEL_Layout`, or a custom `CEL_FlexBox`), framework authors rely on **Code-to-Code Execution** rather than abstract frame ticks:

### Architectural Mental Model: Code Execution Order vs. Presence in the Tree

```
[Presence in Tree: Lifetime]
  on_attach (Fresh Mount) ───────────────► on_detach (Omitted / Pruned)
      │                                            ▲
      ▼                                            │
[Code Execution Order: Every Evaluation Pass]      │
  Scope Entry (Composable body / enter)           │
      │                                            │
      ├─── Sets ambient context (cel_set_context)  │
      ├─── Attaches components (cel_has)          │
      └─── Allocates / reads state (cel_state)     │
      │                                            │
      ▼                                            │
  Children Execute ({ ... } child composables)     │
      │                                            │
      ▼                                            │
  Scope Exit (on_exit(childCount) at closing }) ───┘
```

1. **Code Execution Order (Every Recomposition Pass)**:
   - **Scope Entry (Composable Body)**: The composable function body executes *before* any children run. This is where you configure declarative ECS components (`cel_has`), publish ambient context down the subtree (`cel_set_context`), and initialize reactive state (`cel_remember_state`).
   - **Children Execution**: Direct child composables execute sequentially within the caller's `{ ... }` block. Sibling counts and indexes are tracked in $\mathcal{O}(1)$.
   - **Scope Exit (`on_exit(childCount) { ... }`)**: Automatically triggers at the closing `}` of the container scope when [`CelsExitGroup`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) closes the group. Receives the settled, exact `childCount` without manual stack tracking.
2. **Presence in the Tree (Topology Lifetime)**:
   - **`on_attach { ... }`**: Runs once on the very first pass when the node is mounted into the session hierarchy.
   - **`on_detach { ... }`**: Runs once when the composable is omitted or pruned from the tree (or on session teardown).

---

### Primary Pattern: Inline Hooks inside `CEL_Composable`

Inline lifecycle hooks eliminate intermediate boilerplate structs and let you write declarative container primitives directly:

```c
typedef struct Toolbar {
    CelsLayoutDir dir;
    int           cols;
} Toolbar;

// 1. Declare container directly as a standard CEL_Composable
CEL_Composable(CelsToolbar, CelsLayoutDir, dir, int, cols) {
    on_attach {
        // Runs once when this container is first mounted
        printf("Toolbar mounted\n");
    }

    // 2. Scope Entry (Body): Runs BEFORE children execute
    Toolbar config = { .dir = dir, .cols = cols };
    cel_has(Toolbar, config);
    cel_set_context(Toolbar, &config);

    // 3. Scope Exit: Runs AFTER children finish (at the closing '}')
    on_exit(childCount) {
        // childCount is settled, exact, and frame-accurate!
        printf("Toolbar finished rendering %u buttons!\n", childCount);
    }

    on_detach {
        // Runs once when this container is removed from the active tree
        printf("Toolbar unmounted\n");
    }
}

// 4. One-line macro mapping to cel_scope for clean syntax
#define Toolbar(...) cel_scope(CelsToolbar, ##__VA_ARGS__)
```

---

### Decoupled Pattern: Standalone `CEL_Lifecycle`

For complex engines where lifecycle controllers must be shared across multiple composables or decoupled across translation units, CELS also supports standalone `CEL_Lifecycle` definitions:

```c
CEL_Lifecycle(ToolbarLifecycle) {
    mount {
        printf("Toolbar mounted!\n");
    }
    before_children {
        const Toolbar *params = cel_get(Toolbar);
        if (!params) return;
        cel_set_context(Toolbar, params);
    }
    after_children(childCount) {
        printf("Toolbar finished rendering %u buttons!\n", childCount);
    }
    unmount {
        printf("Toolbar unmounted!\n");
    }
}

CEL_Composable(CelsToolbarDecoupled, CelsLayoutDir, dir, int, cols) {
    cel_has(Toolbar, { .dir = dir, .cols = cols });
    cel_lifecycle(ToolbarLifecycle);
}
```

---

## 4. The Archetype Design Patterns

### Pattern 1: Scoped Container Composables (`cel_scope`)
To allow application developers to write natural Compose-like container calls such as `Toolbar(LAYOUT_HORIZ, 3) { ... }` or `LazyColumn() { ... }`:

1. Declare the container logic as a standard `CEL_Composable(Toolbar, CelsLayoutDir, dir, int, cols)`.
2. Add a 1-line macro:
   ```c
   #define Toolbar(...) cel_scope(Toolbar, ##__VA_ARGS__)
   ```
3. Application developers can now call the container directly with trailing braces:
   ```c
   Toolbar(LAYOUT_HORIZ, 3) {
       Button("New");
       Button("Open");
       Button("Save");
   }
   ```
   Or for zero-parameter containers:
   ```c
   Column() {
       Title();
       Content();
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

Below is a complete, real-world custom layout primitive using `cel_scope` and `CEL_Composable`:

```c
#pragma once
#include "cels.h"

typedef enum CelsFlexAlign {
    FLEX_ALIGN_START,
    FLEX_ALIGN_CENTER,
    FLEX_ALIGN_END
} CelsFlexAlign;

typedef struct CelsFlex {
    CelsLayoutDir dir;
    CelsFlexAlign align;
    float         gap;
} CelsFlex;

CEL_State(CelsFlexFocusState) {
    int activeIndex;
    int totalItems;
};

/* Container Composable: Standard CEL_Composable with inline on_exit(childCount) */
CEL_Composable(CelsFlexBox, CelsLayoutDir, dir, CelsFlexAlign, align) {
    /* 1. Scope Entry (Composable Body): Runs BEFORE children execute */
    CelsFlex config = { .dir = dir, .align = align, .gap = 0.0f };
    cel_has(CelsFlex, config);
    cel_set_context(CelsFlex, &config);

    /* Allocate and publish reactive focus state */
    CelsFlexFocusState *focus = cel_remember_state(CelsFlexFocusState, {0});
    cel_watch(focus);
    if (focus != NULL) {
        cel_set_context(CelsFlexFocusState, focus);
    }

    /* 2. Scope Exit: Runs AFTER children finish (at closing '}') */
    on_exit(childCount) {
        const CelsFlexFocusState *focusRO = NULL;
        if (cel_get_context(CelsFlexFocusState, &focusRO) == CELS_OK && focusRO) {
            CelsFlexFocusState *f = (CelsFlexFocusState*)focusRO;
            f->totalItems = (int)childCount;
        }
    }
}

/* 1-line alias macro mapping directly to cel_scope */
#define CEL_FlexBox(...) cel_scope(CelsFlexBox, __VA_ARGS__)
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

1. **Clean Scope Invariants**: Always encapsulate `CelsEnterComposable` and `CelsExitGroup` inside a single-iteration `for` loop scope. This guarantees slot groups are reliably exited even under complex nesting.
2. **Zero Heap Allocation**: All context and state allocations must use stack structs, `cel_remember_state`, or ECS transaction buffers.
3. **Component vs. State Separation**: Orientation, alignment, and static layout attributes belong in `cel_has` components and ambient context (`cel_set_context`). Only temporal dynamic values (like active focus index) belong in reactive `cel_state`.
4. **Result Protection**: All child introspection queries ([`cel_child_info`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_child_index`](file:///D:/cels-workspace/library/cels/include/cels/cels.h), [`cel_child_count`](file:///D:/cels-workspace/library/cels/include/cels/cels.h)) return `CelsResult` to guard against usage outside of containers.
5. **C99 Portability**: Always explicitly cast compound literals: `((Type){ ... })`. Avoid non-standard GCC statement expressions (`({ ... })`).
