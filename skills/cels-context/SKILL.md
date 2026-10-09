---
name: cels-context
description: Provide technical guidance on CELS ambient context mechanics, two-tier scoping (session vs composable subtree), multi-context consumption, and scoped layouts. Use when eliminating parameter drilling, cascading themes/styles down subtrees (UITheme), injecting platform services (PdcursesInputContext), managing layout directions (CEL_Layout), introspecting child metrics, or authoring custom DSL archetypes.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: ambient-context-and-containers
  keywords:
    - ambient-context
    - cel_context
    - cel_set_context
    - cel_get_context
    - theming
    - styling
    - layout
    - CEL_Layout
    - child-metrics
    - archetypes
    - C99
---

# CELS Ambient Context & Scoped Containers Guide

Passing configuration structs, themes, DPI scales, native device handles, or layout metrics through 5 to 10 layers of composables clutters function signatures and creates brittle parameter drilling.

CELS solves this using **Ambient Context (`cel_context`)**: a lexically scoped, zero-allocation mechanism that routes immutable pointers down the composable call tree and restores previous scopes automatically upon exit.

---

## 1. Core Mechanics: How Ambient Context Works

### The Stack-Allocated Scope Chain
Under the hood in CELS, ambient context is implemented as a stack-allocated linked list of [`CelsScopeNode`](file:///D:/cels-workspace/library/cels/include/cels/runtime/slot_table.h):

```
[ Root Session Context ] (Platform Services: Input, Renderer, Audio)
         ▲
         │ (inherited down tree)
[ Subtree Scope Node ]   (UITheme: Light Theme)
         ▲
         │ (inherited down subtree)
[ Nested Scope Node ]    (Modal Dialog Override: UITheme -> Dark Theme)
         ▲
         │ (active resolution)
[ Child Composable ]     -> cel_context(UITheme)            => Dark Theme
                         -> cel_context(PdcursesInputContext) => Platform Input
```

### Key Mechanical Invariants:
1. **Zero Heap Allocation**: Scope nodes reside on the C execution stack or in slot memory. Zero `malloc` overhead.
2. **Type-Keyed by 64-Bit FNV-1a Hash**: Each type `#TypeName` is hashed at compile time. Lookups traverse the scope chain and match type keys in $\mathcal{O}(\text{depth})$ without string comparisons.
3. **Lexical Restoration**: When a composable function returns, the stack frame unwinds and the previous parent scope is automatically restored for sibling composables.
4. **Subtree Inheritance**: Any child or descendant composable can resolve context provided by any ancestor.

---

## 2. Two-Tier Context Scoping Model

CELS provides two distinct scopes for ambient context:

```
┌────────────────────────────────────────────────────────────────────────┐
│ TIER 1: ROOT SESSION SCOPE (cel_set_context(session, Type, &instance))  │
│ - Attached at depth 0 by host executable at startup                    │
│ - Available to EVERY composable across the entire session universe     │
│ - Ideal for: Platform input drivers, renderers, audio devices, loggers │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ inherited
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ TIER 2: COMPOSABLE SUBTREE SCOPE (cel_set_context(Type, &instance))    │
│ - Attached inside any composable body                                  │
│ - Lexically scoped strictly to that composable's descendants           │
│ - Automatically pops and restores ancestor context on return           │
│ - Ideal for: Themes (UITheme), layout directions (CelsLayoutDir), modal│
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Multi-Context Consumption in a Single Composable

Because ambient context is type-keyed, a single composable can consume **multiple unrelated contexts simultaneously**. Overriding one context type down a subtree does not hide or interfere with other types:

```c
CEL_Composable(ControlPanel) {
    /* 1. Platform driver from root session scope */
    const PdcursesInputContext *input  = cel_context(PdcursesInputContext);

    /* 2. Visual styling from nearest ancestor theme provider */
    const UITheme              *theme  = cel_context(UITheme);

    /* 3. Orientation from parent layout container */
    const CelsLayoutDir        *layout = cel_context(CelsLayoutDir);

    if (input && cel_action_pressed(input, "Select")) {
        DrawHighlightedButton("Submit", theme ? theme->accent : 0xFFFFFFFF, layout->direction);
    }
}
```

---

## 4. Context APIs & Result Protection

CELS provides two ways to consume context:

### 1. Result-Protected Form (`cel_get_context`) [Recommended]
Returns a `CelsResult` status code. Safely differentiates between absence, success, and out-of-session errors:
```c
CEL_Composable(ThemedButton, const char*, label) {
    const UITheme *theme = NULL;
    if (cel_get_context(UITheme, &theme) == CELS_OK) {
        DrawButton(label, theme->accent);
    } else {
        DrawButton(label, 0xFF007ACC); /* Fallback default */
    }
}
```
- `CELS_OK`: Found in current subtree or root session.
- `CELS_ERROR_NOT_FOUND`: Type not provided by any ancestor.
- `CELS_ERROR_INVALID_STATE`: Invoked outside an active session.

### 2. Pointer Form (`cel_context`)
Convenient shorthand returning `const Type*` or `NULL`:
```c
CEL_Composable(ThemedButtonFast, const char*, label) {
    const UITheme *theme = cel_context(UITheme);
    uint32_t accent = theme ? theme->accent : 0xFF007ACC;
    DrawButton(label, accent);
}
```

---

## 5. The 3-Way Rule: Props vs. State vs. Context

Developers often confuse when to pass function arguments (props), when to create reactive state, and when to publish ambient context:

| Mechanism | Where it Lives | Primary Purpose | How Updates Happen |
| :--- | :--- | :--- | :--- |
| **Props (Arguments)** | Function parameters | Explicit inputs for an immediate child widget (e.g. `label`, `width`). | Parent re-evaluates and passes new values. |
| **State (`cel_state`)** | Double-buffered slot memory | Dynamic data that mutates over time and triggers recomposition. | Staged through `cel_mutate(ptr)`. |
| **Context (`cel_context`)** | Ambient call stack chain | Passes environmental pointers down an entire subtree without parameter drilling. | Non-reactive courier; restored automatically on exit. |

> [!IMPORTANT]
> **Context is Non-Reactive**: Context holds no double-buffers and has no dirty tracking. Never attempt to write to context directly (`((UITheme*)cel_context(UITheme))->bg = ...` is forbidden). Always store dynamic data in `cel_state`, and use context merely to route pointers down the call stack.

---

## 6. Practical Example 1: Theming & Visual Styling Cascades

Theming is the classic application of ambient context. Top-level screens provide a global palette, while modals or sub-panels can override the palette for their entire subtree:

```c
typedef struct UITheme {
    uint32_t bg;
    uint32_t text;
    uint32_t accent;
    float cornerRadius;
} UITheme;

static const UITheme g_lightTheme = { .bg = 0xFFF5F5F5, .text = 0xFF222222, .accent = 0xFF0066CC, .cornerRadius = 6.0f };
static const UITheme g_darkTheme  = { .bg = 0xFF1E1E1E, .text = 0xFFEEEEEE, .accent = 0xFF9C27B0, .cornerRadius = 10.0f };

/* Subtree Override: Everything inside this modal inherits dark theme */
CEL_Composable(ModalDialog, const char*, title) {
    cel_set_context(UITheme, &g_darkTheme);

    RenderModalFrame(title);
    ThemedButton("Confirm"); // Uses Dark Theme!
    ThemedButton("Cancel");  // Uses Dark Theme!
}

/* Root Screen: Provides light theme to application */
CEL_Composable(AppScreen, bool, showModal) {
    cel_set_context(UITheme, &g_lightTheme);

    ThemedButton("Main Screen Button"); // Uses Light Theme!

    if (showModal) {
        ModalDialog("Warning"); // Children use Dark Theme!
    }

    ThemedButton("Footer Button"); // Automatically restored to Light Theme!
}
```

---

## 7. Practical Example 2: Layout Containers & Sibling Introspection

In CELS, layout engines use ambient context to propagate orientation and spacing down their child hierarchies:

### Directional Layouts (`CEL_Layout`)
- `CEL_Layout(dir)`: Application-level layout archetype (`examples/common/cels_layout.h`). Publishes `CelsLayoutDir` ambient context, manages reactive focus state (`CelsFocusState`), and automatically routes directional input navigation across child composables.

```c
// 1. Direct inline layout invocation (Zero #define required!)
CEL_Composable(SettingsMenu, SettingsState*, settings) {
    CEL_Layout(CEL_LAYOUT_DIR_VERT) {
        VolumeSlider(settings);
        MuteToggle(settings);
        GraphicsSelector(settings);
    }
}

// 2. Responsive Orientation: Direction passed as a parameter
CEL_Layout(isPortrait ? CEL_LAYOUT_DIR_VERT : CEL_LAYOUT_DIR_HORIZ) {
    SidebarPanel();
    ContentPanel();
}

// 3. Multi-directional 2D Game Grid (Named container in slot table)
CEL_Layout(InventoryGrid, CEL_LAYOUT_DIR_2D, 4 /* columns */, true /* wrapFocus */) {
    for (int i = 0; i < 16; ++i) {
        InventorySlot(i);
    }
}
```

### Child Introspection APIs (`CelsResult` Protected)
Child composables inside containers can discover their position safely without parent parameter drilling:
- `cel_child_info(&info)`: Resolves `CelsChildInfo` (`index`, `totalCount`, `parentId`, `childId`, `isFirst`, `isLast`).
- `cel_child_index(&outIdx)`: 0-based sibling index among container children.
- `cel_child_count(&outCount)`: Total direct children inside container.
- `cel_is_first_child(&outFirst)` / `cel_is_last_child(&outLast)`: Sibling position predicates for visual dividers or rounded corners.

```c
CEL_Composable(ListItem, const char*, text) {
    bool isFirst = false, isLast = false;
    cel_is_first_child(&isFirst);
    cel_is_last_child(&isLast);

    RenderRow(text, isFirst /* roundTop */, isLast /* roundBottom */);
}
```

---

## 8. Practical Example 3: The Container-State Synergy Pattern

Containers (like `LazyColumn` or `TabContainer`) combine reactive state with ambient context:
1. **Container owns reactive state**:
   ```c
   LazyListState *st = cel_remember_state(LazyListState, { .firstVisibleIndex = 0 });
   ```
2. **Container publishes context**:
   ```c
   cel_set_context(LazyListState, st);
   ```
3. **Descendants consume & mutate**:
   ```c
   const LazyListState *st = NULL;
   if (cel_get_context(LazyListState, &st) == CELS_OK) {
       cel_mutate(st) { this->firstVisibleIndex += 5; }
   }
   ```

---

## 9. Practical Example 4: Host Platform Services Injection

The host executable uses root session ambient context to provide platform drivers to all composables without global variables:

```c
/* In host main.c: */
static PdcursesInputContext g_inputCtx;
cel_set_context(&engine.session, PdcursesInputContext, &g_inputCtx);

/* In any deeply nested composable: */
CEL_Composable(PlayerControls) {
    const PdcursesInputContext *input = cel_context(PdcursesInputContext);
    if (input && cel_action_pressed(input, "Jump")) {
        // Handle jump
    }
}
```

---

## 10. Advanced Recipe: Authoring Container Archetypes

Framework and UI library authors can create custom container archetypes (like `CEL_SplitPane`, `CEL_Docking`, `CEL_ScrollView`).

### The C99 Mechanical Reality
In ANSI C99, **function calls cannot be followed by a trailing code block `{ ... }`**:
```c
// ILLEGAL C99 SYNTAX:
MyContainer() { // Compiler Error: expected ';' before '{' token!
    ChildWidget();
}
```
In C, the only statements that can be followed by a compound statement `{ ... }` are `for`, `if`, `while`, and `switch`. Therefore, declarative container archetypes **must expand to a scoped `for` loop**.

### Foundation: Container Archetypes with `CEL_Lifecycle`
Custom block archetypes (like `CEL_Layout` in `examples/common/cels_layout.h`) delegate subtree setup and child settlement directly to `CEL_Lifecycle`:
- **`before_children { ... }`**: Publishes ambient context and initializes reactive state before children execute.
- **`after_children(childCount) { ... }`**: Automatically receives the settled `childCount` directly from `CelsExitGroup` without manual stack introspection.

#### Clean Archetype Definition Example:
```c
typedef struct ToolbarParams {
    bool isVertical;
    float padding;
} ToolbarParams;

CEL_Lifecycle(ToolbarLifecycle, ToolbarParams*, params) {
    before_children {
        /* Publish context down subtree */
        cel_set_context(ToolbarParams, params);
    }
    after_children(childCount) {
        /* Frame-accurate child metrics without manual stack reading */
        printf("Toolbar completed with %u items\n", childCount);
    }
}

static inline bool _CelsToolbarEnter(uint64_t key, ToolbarParams params) {
    CelsSession *s = CelsGetCurrentSession();
    if (!s || !CelsEnterComposable(s, key)) return false;
    ToolbarParams *slot = cel_remember(ToolbarParams, params);
    if (slot) *slot = params;
    cel_lifecycle(ToolbarLifecycle, slot);
    return true;
}

#define CEL_Toolbar(...) \
    for (int _cels_tonce = 1; \
         _cels_tonce && _CelsToolbarEnter(CelsHashKey("Toolbar"), (ToolbarParams){ __VA_ARGS__ }); \
         CelsExitGroup(CelsGetCurrentSession()), _cels_tonce = 0)
```

> [!NOTE]
> **The Golden Rule of CELS Development**:
> - `#define` macros are reserved exclusively for **framework-level archetypes** (DSL primitives like `CEL_Composition`, `CEL_Composable`, `CEL_Layout`).
> - Application developers **never write `#define` for application components**; they write standard C functions with `CEL_Composable`.

---

## 11. Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Mutating Context Directly
```c
// BAD: Attempting to write to ambient context
UITheme *theme = (UITheme*)cel_context(UITheme);
theme->bg = 0xFF0000; // Violates immutable top-down cascade!
```
**Fix**: Use `cel_set_context(UITheme, &newTheme)` in a parent composable to establish an override, or use reactive `cel_state` if the theme dynamically changes over time.

### ❌ Anti-Pattern 2: Storing Volatile Dynamic Data in Context
```c
// BAD: Using context for state that changes every tick
cel_set_context(PlayerHealth, &playerHp); // Context has no dirty tracking or double buffers!
```
**Fix**: Use reactive state (`cel_state`, `cel_watch`, `cel_mutate`).

### ❌ Anti-Pattern 3: Writing `#define` for Application Components
```c
// BAD: Application developer creating macro wrappers for widgets
#define MyCard(title) CEL_Layout(CEL_LAYOUT_DIR_VERT) // Bad! Clutters codebase and hides C types
```
**Fix**: Author standard composables with `CEL_Composable(MyCard, const char*, title)`.
