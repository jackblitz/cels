---
name: cels-modifications
description: Provide technical guidance on CELS declarative modifications, subtree theming and ambient context cascading (cel_context), and discrete entity component reconciliation (cel_has, cel_has_tag, cel_get, cel_is, cel_id, cel_key). Use when decorating composables with styles, passing ambient environmental context down subtrees without parameter drilling, binding composables to ECS entities (Flecs), or managing declarative component lifecycles with automatic removal diffing.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-06'
  category: modifications-styling-and-ecs
  keywords:
    - modifications
    - styling
    - theming
    - cel_context
    - cel_has
    - cel_has_tag
    - cel_get
    - cel_is
    - cel_id
    - cel_key
    - cel_call
    - ambient-context
    - ECS
    - Flecs
    - reconciliation
    - C99
---

# CELS Declarative Modifications: Ambient Theming & ECS Components

In declarative programming, decorating and customizing UI elements and simulation nodes is frequently mishandled by forcing every attribute into a bloated, monolithic "modifier" struct. In CELS, modifications are split into two precise, complementary declarative models:

1. **Ambient Context Cascading (`cel_context`)**: For environmental context (themes, palettes, typography, DPI scales, world coordinates) that cascades down a composable subtree without parameter drilling.
2. **Declarative Component Reconciliation (`cel_has`, `cel_has_tag`, `cel_get`, `cel_is`)**: For discrete, typed components attached directly to composable entities (`cel_id()`, `cel_key()`), automatically diffed and committed to external backends (like Flecs ECS) via lock-free transactions.

---

## 1. Ambient Context vs. Declarative Components: Mental Model

| Dimension | Ambient Context (`cel_context`) | Declarative Components (`cel_has` / `cel_get`) |
| :--- | :--- | :--- |
| **Primary Scope** | Entire composable subtree (parent down to all descendants) | Single composable entity node (`cel_id()`) |
| **Typical Payloads** | `UITheme`, `StyleConfig`, `DPIScale`, `InputMap` | `Position`, `Health`, `EnergyShield`, `BurnDebuff` |
| **Target Consumer** | Child composables reading styling/environment | External execution engines (Flecs ECS, Vulkan, Audio) |
| **Memory Mechanics** | Stack-allocated linked list (`CelsScopeNode`), 0 heap | Slot table tracker diffing staged transactions |
| **Reconciliation** | Restores ancestor scope on composable exit | Omissions auto-stage `CELS_OP_REMOVE`; unmount stages `DELETE` |
| **Kotlin/Flutter Parity** | `CompositionLocalProvider` / `Theme.of(context)` | ECS Entity Components / Jetpack Compose modifiers |

---

## 2. Ambient Context Guide (`cel_set_context` / `cel_get_context`)

### The Problem: Parameter Drilling
Passing theme structs, DPI scales, or UI metrics through 5 to 10 layers of composables clutters function signatures:
```c
// BAD: Parameter drilling
Button("Submit", theme, dpi, fontManager);
```

### The CELS Solution: Scoped Lexical Ambient Stack
`cel_set_context(Type, &val)` (or `cel_setContext`) attaches context to the current composable scope, automatically restoring the ancestor scope when the composable exits. Descendants can read context using either **result-protected resolution** or the **direct pointer form**:

#### 1. Result-Protected Resolution (`cel_get_context(Type, &outPtr) -> CelsResult`) [Recommended]:
Returns `CELS_OK` on success, `CELS_ERROR_NOT_FOUND` if absent, and `CELS_ERROR_INVALID_STATE` outside an active session:
```c
CEL_Composable(ThemedButton, const char*, label) {
    const UITheme *theme = NULL;
    if (cel_get_context(UITheme, &theme) == CELS_OK) {
        DrawButton(label, theme->accent);
    } else {
        DrawButton(label, 0xFF007ACC); // Fallback if no ancestor provided UITheme
    }
}
```

#### 2. Direct Pointer Form (`cel_get_context(Type) -> const Type*`):
Returns the nearest ancestor pointer, or `NULL` if absent:
```c
CEL_Composable(ThemedButtonFast, const char*, label) {
    const UITheme *theme = cel_get_context(UITheme);
    uint32_t accent = theme ? theme->accent : 0xFF007ACC;
    DrawButton(label, accent);
}
```

```c
// 1. Define theme struct
typedef struct UITheme {
    uint32_t bg;
    uint32_t text;
    uint32_t accent;
    float cornerRadius;
} UITheme;

static const UITheme g_lightTheme = { .bg = 0xFFF5F5F5, .text = 0xFF222222, .accent = 0xFF0066CC, .cornerRadius = 8.0f };
static const UITheme g_darkTheme  = { .bg = 0xFF1E1E1E, .text = 0xFFEEEEEE, .accent = 0xFF9C27B0, .cornerRadius = 10.0f };

// 2. Child Composable with Overridden Theme:
CEL_Composable(ModalDialog, const char*, title) {
    // Subtree Override: Everything inside this modal inherits dark theme!
    cel_set_context(UITheme, &g_darkTheme);
    ThemedButton("Modal Confirm"); // Uses Dark Theme!
}

// 3. Provider & Root Composable:
CEL_Composable(AppScreen, bool, showDarkModal) {
    // Provide light theme to the application:
    cel_set_context(UITheme, &g_lightTheme);
    ThemedButton("Main Screen Button"); // Uses Light Theme!

    if (showDarkModal) {
        ModalDialog("Warning");
    }
}
```

### Layout Containers & Child Tracking (`cel_container` & `CEL_Layout`)
In CELS, **every composable naturally acts as a container for its children**. Child composables invoked sequentially within a parent composable, container, or layout block are automatically registered as nested slot groups in $\mathcal{O}(1)$ time.

> [!NOTE]
> **The Golden Rule: Framework Archetypes vs. Application Composables**:
> - `#define` macros are reserved exclusively for **framework-level archetypes** (DSL primitives like `CEL_Composition`, `CEL_Composable`, `cel_container`, `CEL_Layout`). Application developers **never** write `#define` for application components; they write standard C functions with `CEL_Composable`.
> - `cel_container(Name)` and `cel_container(Name, id)` are **core CELS library primitives** (`include/cels/cels.h`). They manage slot group entry and exit using RAII-like `for` loops, completely eliminating boilerplate calls to `CelsEnterComposable` and `CelsExitGroup`.
> - `CEL_Layout` is an **application-level helper recipe** (`examples/common/cels_layout.h`). Developers use `CEL_Layout(dir) { ... }` directly with zero `#define`, or author reusable components with `CEL_Composable`.
> - For full technical guidance on building custom container primitives or DSL base classes, consult the **`cels-primitives-and-archetypes`** skill and [`docs/guides/10-authoring-primitives-and-archetypes.md`](file:///D:/cels-workspace/library/cels/docs/guides/10-authoring-primitives-and-archetypes.md).

#### 1. Core Container Primitive (`cel_container`)
Wrap any block of child composables to establish an isolated slot group with tracked sibling metrics:
```c
cel_container(TabPanel) {
    ContentHeader("Settings");
    ContentBody();
}
```

#### 2. Direct Inline Layouts & Reusable Composables (`CEL_Layout`)
Developers use `CEL_Layout` directly inside composable bodies with **zero `#define` boilerplate**:
```c
// Direct linear vertical menu:
CEL_Layout(CEL_LAYOUT_DIR_VERT) {
    VolumeSlider(settings);
    MuteToggle(settings);
    GraphicsSelector(settings);
    ResumeButton(state);
}

// Multi-directional 2D Game Grid layout (named container in slot table):
CEL_Layout(InventoryGrid, CEL_LAYOUT_DIR_2D, 4 /* columns */, true /* wrapFocus */) {
    for (int i = 0; i < 16; ++i) {
        InventorySlotItem(inventory[i]);
    }
}

// Authoring a reusable layout composable (Composable IS the universal base):
CEL_Composable(InventoryGridWidget, InventoryState*, inv) {
    CEL_Layout(CEL_LAYOUT_DIR_2D, 4) {
        for (int i = 0; i < 16; ++i) {
            InventorySlotItem(inv->items[i]);
        }
    }
}
// Callers invoke: InventoryGridWidget(inv);
```

#### 3. Responsive Direction & Multi-Directional Navigation
Direction (`CelsLayoutDir`) is passed as a **parameter**, allowing orientation and modes to adapt dynamically across recomposition:
```c
/* Responsive / Dynamic Direction: Adapts immediately on recomposition */
CEL_Layout(isPortrait ? CEL_LAYOUT_DIR_VERT : CEL_LAYOUT_DIR_HORIZ) {
    SidebarPanel();
    ContentPanel();
}
```

> [!NOTE]
> **Component vs. State Separation in Layouts**:
> Direction (`CelsLayoutDir`) is attached as a declarative ECS **Component** via `cel_has(CelsLayoutDir, { params.dir })` and published via ambient context `cel_set_context(CelsLayoutDir, &params.dir)`. It is NOT stored inside reactive state! Reactive state (`CelsFocusState`) only tracks focus indices (`activeIndex`, `totalCount`, `wrapFocus`).

#### Child Introspection APIs (`CelsResult` Protected):
Children inside any container or layout block query their sibling metrics safely:
- `cel_child_info(&info)`: Resolves `CelsChildInfo` (`index`, `totalCount`, `parentId`, `childId`, `isFirst`, `isLast`).
- `cel_child_index(&outIdx)`: 0-based sibling index among container children.
- `cel_child_count(&outCount)`: Total child count inside parent container.
- `cel_is_first_child(&outFirst)` / `cel_is_last_child(&outLast)`: Sibling position predicates for visual styling or dividers.

Returns `CELS_ERROR_INVALID_STATE` when called standalone outside a container or session.

### State vs. Context: Architectural Mental Model
- **State (`cel_state`, `cel_remember_state`, `cel_mutate`)**: Authoritative, double-buffered reactive data that changes over time. When state mutates via `cel_mutate`, all observing composables automatically invalidate and recompose. State is owned by a composable or session.
- **Context (`cel_set_context`, `cel_get_context`)**: Ambient scoping mechanism to pass pointers down the composable call tree without parameter drilling. Context holds no state of its own; it merely routes pointers down the active call tree and pops them automatically when the composable exits.
- **The Container Pattern**: A container composable (e.g. `CEL_Layout` or `LazyColumn`) owns reactive state via `cel_remember_state`, attaches the pointer to the ambient call tree via `cel_set_context(MyState, state)`, and child composables read it via `cel_get_context(MyState, &state)` and stage updates via `cel_mutate`.

### Best Practices for Ambient Context:
- **Explicit Read vs. Write**: Always prefer `cel_set_context(Type, ptr)` / `cel_setContext(Type, ptr)` when establishing context down a subtree, and `cel_get_context(Type, &ptr)` / `cel_getContext(Type, &ptr)` when consuming it with result safety.
- **Always handle absence or provide a fallback**: Check `CelsResult == CELS_OK` or use a default static struct.
- **Zero Heap Cost**: Pointers point to stack or slot memory. Do NOT `malloc` temporary themes unless lifecycle-managed.
- **Automatic Restoration**: When the composable exits, the previous parent theme is restored automatically.

---

## 3. Declarative Component Attachment (`cel_has` & Composable Identity)

### Composable Identity (`cel_id`, `cel_key`, `cel_call`)
Every composable in CELS is an identified node in the slot table with a 64-bit ID matching Flecs `ecs_entity_t`:
1. **Auto-Generated ID**: Left unspecified (unkeyed `CEL_Composable(MyWidget)`), CELS assigns a unique stable 64-bit ID based on slot address and callsite hash (`cel_id()`).
2. **Explicit Composable ID Parameter**: Declared directly in the signature: `CEL_Composable(MyWidget, id, ...)`. Callers invoke `MyWidget(entityId, ...)`. Binds the node's `cel_id()` to `id`. Developers never set IDs imperatively; `cel_id()` is strictly a query/getter macro.
3. **Caller-Directed Identity (Dynamic Lists)**:
   - Keyed composables: `ItemRow(items[i].id, &items[i]);`
   - Scoped block: `cel_key(items[i].id) { ItemRow(&items[i]); }`
   - Single-line invocation: `cel_call(ItemRow, items[i].id, &items[i]);`

### The Declarative Reconciliation Invariant (`cel_has`)
When you declare components using `cel_has(Type, { ... })` or tags using `cel_has_tag(Type)`:
1. **Mount / First Declaration**: Stages a `CELS_OP_SET` transaction.
2. **Automatic Omission Removal**: If a component was declared in frame $N$, but is **omitted** in frame $N+1$ (e.g. an `if` condition evaluates to `false`), CELS **automatically stages a `CELS_OP_REMOVE` transaction** on scope exit!
3. **Automatic Unmount Cleanup**: If the composable or keyed entity leaves the slot table, CELS **automatically stages a `CELS_OP_DELETE` transaction**.

```c
CEL_Composable(PlayerNode, id, const PlayerState*, player) {
    cel_watch(player);

    // Baseline components:
    cel_has(Position, { .x = player->x, .y = player->y, .z = player->z });
    cel_has(Health,   { .current = player->hp, .max = player->maxHp });

    // Conditional Debuff:
    // When true, stages SET. When it becomes false, CELS AUTO-REMOVES it!
    if (player->isBurning) {
        cel_has(BurnDebuff, { .dps = 25.0f, .duration = 3.0f });
    }

    if (player->hasShield) {
        cel_has(EnergyShield, { .capacity = 100.0f });
    }
}
```

---

## 4. Intra-Frame Queries & ECS Fallback (`cel_get` & `cel_is`)

During recomposition, a composable or child may need to inspect attached components before transactions are committed to the external ECS.

CELS provides **2-Tier Resolution**:
- **Tier 1 (Intra-Frame)**: Checks the active frame's staged transaction batch. If a component was staged in this frame, returns the staged payload immediately.
- **Tier 2 (External ECS Fallback)**: If not present in the staged batch, queries the external ECS via `CelsSessionSetEcsLookupHook`.

```c
// Intra-frame query on current node (implicit cel_id()):
const Health *hp = cel_get(Health);
if (hp && hp->current < 25) {
    cel_has_tag(LowHealthAlert);
}

// Intra-frame query on explicit entity ID:
const Position *targetPos = cel_get(targetEntityId, Position);

// Boolean tag check:
if (cel_is(BossTag)) {
    cel_has(BossEnrageMeter, { .rage = 100.0f });
}
```

---

## 5. Complete Decision Matrix: Which API To Use?

| Requirement | Recommended API | Why |
| :--- | :--- | :--- |
| Pass visual styling or theme down a subtree | `cel_context(UITheme, &theme)` | Zero parameter drilling, stack-allocated, auto-restoring |
| Read active ambient style in a widget | `cel_context(UITheme)` | Accesses nearest ancestor theme |
| Attach game data or physics to a node | `cel_has(Type, { ... })` | Declarative, diffed on recomposition, staged for Flecs |
| Attach zero-sized flags (tags) | `cel_has_tag(Type)` | Zero payload, maps to ECS tag/pair |
| Query a component during recomposition | `cel_get(Type)` or `cel_is(Type)` | 2-tier resolution (staged batch -> Flecs fallback) |
| Preserving slot state in a dynamic list | `cel_key(id) { ... }` or `cel_call(...)` | Preserves `cel_remember` state across reordering |
| Storing private local widget memory | `cel_remember(Type, { ... })` | Retained across frames in the slot table |
| Triggering recomposition on data changes | `cel_state` + `cel_watch` + `cel_mutate` | Double-buffered reactive cache |

---

## 6. Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Monolithic Modifier Structs
```c
// BAD: Monolithic struct trying to hold everything
typedef struct WidgetMod {
    float pad;
    uint32_t color;
    Position pos;
    bool isBurning;
} WidgetMod;
```
**Fix**: Split into `cel_context` for cascading styling, and `cel_has` for discrete ECS data.

### ❌ Anti-Pattern 2: Manual Imperative `ecs_remove` Glue
```c
// BAD: Manually writing imperative removal branches
if (player->isBurning) {
    ecs_set(world, e, BurnDebuff, ...);
} else {
    ecs_remove(world, e, BurnDebuff); // Easily forgotten!
}
```
**Fix**: Use `cel_has`. If the condition evaluates to `false`, CELS automatically stages `CELS_OP_REMOVE` for you!

### ❌ Anti-Pattern 3: Mutating `cel_context` Directly
```c
// BAD: Attempting to write to ambient context
UITheme *theme = (UITheme*)cel_context(UITheme);
theme->bg = 0xFF0000; // Violates immutable top-down cascade!
```
**Fix**: Use `cel_context(UITheme, &newTheme)` in a parent or wrapper composable to scope a new theme, or use reactive `cel_state` if the theme changes dynamically over time.
