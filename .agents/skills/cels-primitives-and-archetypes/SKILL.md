---
name: cels-primitives-and-archetypes
description: Provide technical guidance on authoring CELS framework-level primitives, base classes, and DSL archetypes using C99 macros, RAII-like scoped for-loops, and slot table integration (cel_container, cel_key, cel_has, cel_set_context). Use when authoring custom layout engines (e.g. flexbox, grid, split pane, docking, scroll views), domain-specific declarative DSL wrappers, or extending the CELS framework.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: dsl-and-framework-authoring
  keywords:
    - primitives
    - base-classes
    - archetypes
    - dsl
    - cel_container
    - cel_key
    - cel_has
    - cel_set_context
    - macros
    - C99
    - slot-table
---

# Authoring CELS Primitives, Base Classes & DSL Archetypes

CELS is designed around a clean separation of roles: **Framework Archetypes** (primitives or "base classes") provide language-level scoping and container syntax, while **Application Developers** compose standard C functions using those archetypes.

This skill guides library authors, engine developers, and tooling architects on how to build custom DSL primitives and container archetypes (like `CEL_Layout`, `CEL_FlexBox`, `CEL_SplitPane`, `CEL_ScrollView`, or custom ECS binders) that feel native to CELS.

---

## 1. The Core Philosophy: Archetypes vs. Composables

### The Golden Rule of CELS Development
> **Preprocessor `#define` macros are reserved exclusively for framework-level archetypes (the "base classes" and DSL constructs of the library or engine). Application developers should NEVER write `#define` for everyday widgets, menus, or screens.**

| Role | Target Author | Primary Tools | Examples |
| :--- | :--- | :--- | :--- |
| **Framework Archetype** ("Base Class") | Engine / UI Library Developer | `#define` + `for (...)` + `cel_container` | `CEL_Composition`, `CEL_Composable`, `CEL_Layout`, `cel_container`, `cel_key` |
| **Application Component** | Game / App Developer | Standard C functions (`CEL_Composable`) | `PauseMenu`, `InventoryGrid`, `VolumeSlider`, `HealthBar` |

### Why This Rule Exists:
1. **Composables Are the Universal Base Class**: In CELS, `CEL_Composable` is already the universal unit of composition, lifecycle, and reactive state. Application components should not invent competing macro wrappers.
2. **Predictable Tooling**: Application code remains standard C functions with full IDE autocomplete, static analysis, and debugger symbol inspection.
3. **Ergonomic DSLs**: Framework authors can provide beautiful declarative syntax (like `CEL_Layout(...) { ... }`) without leaking macro plumbing into application logic.

---

## 2. The C99 Mechanical Reality: How Trailing Blocks `{ ... }` Work

In modern languages (Swift, Kotlin, Rust), trailing closures or block syntax are language features:
```kotlin
// Kotlin trailing lambda:
Column {
    Text("Hello")
}
```

In ANSI C99, **function calls cannot be followed by a trailing code block**:
```c
// ILLEGAL C99 SYNTAX:
MyContainer() { // Compiler Error: expected ';' before '{' token!
    ChildWidget();
}
```

In C, the **only** statements that can be immediately followed by a compound statement `{ ... }` are:
- `for (init; cond; step) { ... }`
- `if (cond) { ... }`
- `while (cond) { ... }`
- `switch (expr) { ... }`

Therefore, to create a declarative container archetype that accepts a `{ ... }` block of child composables, the archetype **must** expand to a loop statement.

### The Foundation: `cel_container` with Lifecycle Hooks
CELS provides `cel_container` in `include/cels/cels.h` as the core primitive for all container archetypes. It natively supports optional **`onGroupStart`** and **`onGroupEnd`** lifecycle hooks:

```c
typedef void (*CelsContainerStartHook)(void *userData);
typedef void (*CelsContainerEndHook)(uint32_t childCount, void *userData);
```

#### Hook Execution Model:
1. **On Enter (`onGroupStart(userData)`)**: Executes immediately after `CelsEnterComposable(sess, key)` *inside* the active group. Used to attach components (`cel_has`), publish ambient context (`cel_set_context`), and allocate reactive focus state (`cel_remember_state`).
2. **Body**: The caller's `{ ... }` child composables execute sequentially within the group. Sibling metrics are recorded in $\mathcal{O}(1)$.
3. **On Exit (`onGroupEnd(childCount, userData)`)**: Executes immediately after children finish. Receives the **exact settled `childCount`**! Ideal for post-layout geometry, focus index clamping, or directional navigation.
4. **Group Exit**: Calls `CelsExitGroup(sess)`, closing the slot group.

---

## 3. The 3 Archetype Design Patterns

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

Like `CEL_Composable`, a declaration archetype emits a forward declaration and a static inline wrapper function:

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

```c
// Developer defines custom audio node:
CEL_AudioNode(OscillatorNode, float frequency) {
    cel_has(OscillatorComponent, { .freq = frequency });
}

// Developer calls it:
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

## 4. Step-by-Step Tutorial: Building `CEL_FlexBox`

Let's build a complete, production-grade custom layout archetype: `CEL_FlexBox`.

### Step 1: Define Components & Context Types
```c
typedef enum CelsFlexAlign {
    FLEX_ALIGN_START,
    FLEX_ALIGN_CENTER,
    FLEX_ALIGN_END,
    FLEX_ALIGN_STRETCH
} CelsFlexAlign;

typedef struct CelsFlexParams {
    CelsLayoutDir dir;
    CelsFlexAlign align;
    float         gap;
} CelsFlexParams;
```

### Step 2: Define Reactive State (If the Layout Tracks State)
```c
CEL_State(CelsFlexFocusState) {
    int activeIndex;
    int totalItems;
};
```

### Step 3: Implement onGroupStart and onGroupEnd Hooks
```c
static inline void _CelsFlexOnStart(void *layoutData) {
    CelsFlexParams *params = (CelsFlexParams*)layoutData;
    if (params == NULL) return;

    // 1. Attach declarative ECS component:
    cel_has(CelsFlexParams, { params->dir, params->align, params->gap });

    // 2. Publish ambient context for descendants:
    cel_set_context(CelsFlexParams, params);

    // 3. Allocate / resolve reactive focus:
    CelsFlexFocusState *focus = cel_remember_state(CelsFlexFocusState, {0});
    cel_watch(focus);

    // 4. Expose focus context:
    cel_set_context(CelsFlexFocusState, focus);
}

static inline void _CelsFlexOnEnd(uint32_t childCount, void *layoutData) {
    (void)layoutData;
    // 5. Sibling metrics are settled! childCount is accurate and immediate:
    const CelsFlexFocusState *focusRO = NULL;
    if (cel_get_context(CelsFlexFocusState, &focusRO) == CELS_OK && focusRO) {
        CelsFlexFocusState *focus = (CelsFlexFocusState*)focusRO;
        focus->totalItems = (int)childCount;
    }
}
```

### Step 4: Author the Macro Archetype (Clean 1-Liners!)
Support both anonymous and named container invocations using clean delegation to `cel_container`:

```c
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

### Step 5: How Application Developers Use It
Application developers simply invoke it directly with children—**zero `#define` needed**:

```c
CEL_Composable(UserProfileHeader, const UserProfile *profile) {
    // Inline flex layout:
    CEL_FlexBox(CEL_LAYOUT_DIR_HORIZ, FLEX_ALIGN_CENTER) {
        AvatarImage(profile->avatarUrl);
        UserNameLabel(profile->name);
        StatusBadge(profile->status);
    }
}
```

---

## 5. Architectural Rules & Invariants

| Invariant | Requirement | Why |
| :--- | :--- | :--- |
| **Always delegate to `cel_container`** | Use `cel_container(Name)` for the outer scope | Eliminates manual enter/exit slot plumbing and guarantees exit on loop termination. |
| **Zero Heap Allocations** | Allocate state via `cel_remember_state`, pass context from stack/slot memory | CELS guarantees deterministic memory footprints with 0 dynamic heap churn during recomposition. |
| **Component vs. State Separation** | Static/layout parameters belong in `cel_has` components; only temporal dynamic values (e.g. focus index) belong in `cel_remember_state` | Preserves reactivity boundaries and prevents unnecessary subtree invalidations. |
| **C99 Compound Literals** | Always cast compound literals: `((MyType){ .field = val })` | Strict ANSI C99 compliance across MSVC, Clang, and GCC. |
| **Result-Protected Introspection** | Always use `CelsResult` returning APIs (`cel_child_index`, `cel_child_info`) | Protects against invalid invocations outside of containers or sessions. |

---

## 6. Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Application Developers Creating `#define` Wrappers
```c
// WRONG: Defining a macro for an application component
#define InventoryMenu(...) CEL_Layout(InventoryMenu, __VA_ARGS__)

InventoryMenu(CEL_LAYOUT_DIR_VERT) {
    SlotItem(1);
    SlotItem(2);
}
```
**Fix**: Author as a standard `CEL_Composable` containing `CEL_Layout`, or invoke `CEL_Layout` directly:
```c
// CORRECT: Composable is the universal base!
CEL_Composable(InventoryMenu, InventoryState *inv) {
    CEL_Layout(CEL_LAYOUT_DIR_VERT) {
        for (int i = 0; i < inv->count; ++i) {
            SlotItem(&inv->items[i]);
        }
    }
}
// Caller invokes:
InventoryMenu(inv);
```

### ❌ Anti-Pattern 2: Manual `CelsEnterComposable` / `CelsExitGroup` Scopes
```c
// WRONG: Imperative enter/exit helper functions
bool EnterMyScope(...) { CelsEnterComposable(...); }
void ExitMyScope(...) { CelsExitGroup(...); }
```
**Fix**: Delegate directly to `cel_container(Name)`. It uses an RAII-style `for` loop that automatically executes `CelsExitGroup` on scope exit.

### ❌ Anti-Pattern 3: Mutating Context
```c
// WRONG: Modifying context pointers
CelsFlexParams *p = (CelsFlexParams*)cel_get_context(CelsFlexParams);
p->gap = 10.0f; // Violates immutable top-down cascade!
```
**Fix**: Context is strictly an ambient read-only cascade. Use reactive `cel_state` + `cel_mutate` if layout metrics need to mutate over time.
