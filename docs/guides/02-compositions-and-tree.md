# Compositions & the Tree Hierarchy

In CELS, your entire user interface or simulation hierarchy is declared as a tree of pure C functions. Unlike traditional object-oriented UI systems that allocate heavy widget objects on the heap, CELS represents nodes directly in an efficient **Slot Table** (a cache-aligned gap buffer).

This guide explains how to construct trees, pass parameters (props), handle conditional rendering, and leverage CELS's slot-skipping optimizations.

---

## 1. Compositions vs. Composables

CELS divides tree nodes into two tiers:

| Concept | Macro | Purpose | Typical Scope |
| :--- | :--- | :--- | :--- |
| **Composition** | `CEL_Composition` | Top-level subtree root. Controls root lifecycle and attachment. | 1 per window, screen, or independent scene. |
| **Composable** | `CEL_Composable` | Reusable building block. Mounts under an active parent. | Buttons, cards, toolbars, panels, layouts. |

```
Session (CelsSession)
 └── Composition: WindowComposition (Root Node)
      ├── Composable: HeaderBar("Dashboard")
      └── Composable: MainPanel(contentData)
           ├── Composable: UserCard(user)
           └── if (showBadge) Composable: StatusBadge()
```

---

## 2. Declaring and Attaching Compositions

A **Composition** is the root entry point of an independent tree. It is attached to a `CelsSession` and can define an evaluation predicate (`CEL_Evaluation`) to determine when the entire subtree should close.

Here is the pattern used in [`examples/window/app/composition/window.c`](file:///D:/cels-workspace/library/cels/examples/window/app/composition/window.c):

```c
#include "cels.h"
#include <stdio.h>

/* Evaluation predicate: runs before every recomposition tick (clean zero-parameter form) */
CEL_Evaluation(WindowEval) {
    const WindowState *state = cel_get_state(WindowState);
    /* If isOpen is false, teardown the entire tree */
    return (state == NULL || state->isOpen);
}

/* Root composition function */
CEL_Composition(WindowComposition) {
    WindowState *win = cel_remember_state(WindowState, {
        .isOpen = true,
        .showBadge = true,
        .width = 800,
        .height = 600
    });

    WindowContent(win);
}

/* Attach directly to the session in onStart */
CEL_OnStart(App_OnStart) {
    cel_attach(session, WindowComposition, WindowEval);
}
```

> [!NOTE]
> If `lifecycleEval` returns `false`, CELS tears down every child composable, unmounts all resources in reverse order, and notifies the host engine that this composition has finished.

---

## 3. Root Compositions & `userData`: How, When, and Why

Root compositions are the boundary between the external host engine (or operating system) and the reactive CELS component hierarchy. Understanding how and when to pass external context is essential for clean native architecture.

### The Three Signature Styles of `CEL_Composition`

CELS provides three flexible signatures for `CEL_Composition`:

```c
/* 1. Zero-Parameter (Standard) - Zero boilerplate, eliminates unused parameter warnings */
CEL_Composition(MainWindow) {
    AppHeader();
    DashboardView();
}

/* 2. Type-Safe Parameter Injection - Injects typed host context or configuration */
CEL_Composition(ViewportWindow, const ViewportConfig*, cfg) {
    printf("Mounting viewport for display: %s (Scale: %.1f)\n", cfg->displayName, cfg->dpiScale);
    ViewportCanvas(cfg);
}

/* 3. Generic Pointer (Legacy / Universal) */
CEL_Composition(PluginView, void *userData) {
    PluginContext *ctx = (PluginContext*)userData;
    ParameterPanel(ctx);
}
```

### Passing Context into Root Compositions

#### 1. In Application Modules (`CEL_App`)
When declaring an application module, bind your root composition directly via `CEL_App`:

```c
/* Standard zero-parameter composition */
CEL_App(MyApp, MainWindow);

/* With an evaluation lifecycle predicate */
CEL_App(MyApp, MainWindow, WindowEval);

/* With custom instance userData */
CEL_App(MyApp, ViewportWindow, NULL, &g_ViewportConfig);

/* With both lifecycle predicate and instance userData */
CEL_App(MyApp, ViewportWindow, WindowEval, &g_ViewportConfig);
```

#### 2. In Multi-Composition Host Tick Loops
If your host dynamically spawns and manages multiple composition roots in a single session:

```c
/* Attach multiple independent window roots with distinct instance data */
cel_attach(session, CEL_ID("Window_Left"), ViewportWindow, &configLeft);
cel_attach(session, CEL_ID("Window_Right"), ViewportWindow, &configRight);
```

---

### Architectural Choice: Ambient Session Data vs. Composition `userData`

CELS provides two distinct mechanisms for external host context. Choose the right tool based on architectural scope:

| Mechanism | API | Scope | Best For |
| :--- | :--- | :--- | :--- |
| **Ambient Session Context** | `CelsSessionSetUserData` / `cel_user_data(Type)` | Session-wide (accessible anywhere in tree) | Global engine subsystems, Vulkan/DirectX devices, Flecs ECS world, asset caches. |
| **Composition `userData`** | `cel_root_with_data` / `cel_attach` | Per-Composition instance | Instance-specific configs, multi-window handles, plugin host state, test mocks. |

#### Pattern A: Ambient Session Context (`cel_user_data`)
Use ambient context when an external service is a singleton for the session. Any composable anywhere in the subtree can resolve it without prop-drilling:

```c
// Host setup
CelsSessionSetUserData(session, &renderEngine);

// Any composable deep in the tree:
CEL_Composable(MeshRenderer, ModelHandle, model) {
    RenderEngine *engine = cel_user_data(RenderEngine);
    engine->drawMesh(model);
}
```

#### Pattern B: Composition `userData`
Use composition `userData` when the context is unique to a specific composition root instance.

### Five Real-World Scenarios for `userData`

1. **Multi-Window & Multi-Viewport Rendering**  
   The same composition code drives multiple native windows or split-screen viewports. Each root receives its specific viewport rect, display DPI scaling, and target render texture.
2. **Native OS & Platform Handle Injection**  
   The host passes platform-specific display or event loop handles (e.g. Win32 `HWND`, Wayland `wl_surface*`, macOS `NSWindow*`) into the composition without polluting global engine state.
3. **External ECS & Physics Simulations**  
   The host simulation loop passes an isolated simulation instance (e.g. Flecs `ecs_world_t*` or Box2D `b2WorldId`) directly to the composition root.
4. **Plugin Embeddings (Audio VST / CLAP / Game Mods)**  
   When CELS is embedded inside a DAW host or game engine, each plugin instance receives its private host processor state or parameter bus through `userData`.
5. **Automated Testing & Headless Mocks**  
   Unit tests inject mock network transports, mock audio backends, or virtual file systems into the root composition without modifying production application code.

---

## 4. Declaring Composables & Passing Props

Composables are standard C functions wrapped in the `CEL_Composable` macro. When invoked, CELS automatically:
1. Pushes a group node into the slot table using a hash of the function name.
2. Evaluates the body if dirty or mounting.
3. Automatically pops the group node upon exit.

### Supported Signatures (0 to 4 Typed Arguments)

Pass props down just like standard C function arguments:

```c
/* 0 arguments */
CEL_Composable(AppFooter) {
    printf("  [Footer] System Ready.\n");
}

/* 1 argument */
CEL_Composable(CounterDisplay, int, count) {
    printf("  [Counter] Current count: %d\n", count);
}

/* 2 arguments */
CEL_Composable(UserBadge, const char*, username, int, level) {
    printf("  [Badge] User: %s (Lvl %d)\n", username, level);
}
```

Calling them in a parent composition looks identical to normal C:

```c
CEL_Composition(MainView) {
    UserBadge("Alice", 42);
    CounterDisplay(10);
    AppFooter();
}
```

---

## 5. Multi-File Composable Architecture

In CELS, child composables compile down to standard C functions that return `void`. You can organize them across files with zero friction:

- **In Header Files (`.h`)**: Write a standard C function prototype. Zero macro overhead, zero header pollution, and immediate IDE/LSP autocomplete:
  ```c
  // user_card.h
  #pragma once

  /* Standard C function prototype */
  void UserCard(const char *name, int score);
  ```

- **In Source Files (`.c`)**: Implement the function with `CEL_ComposableDef`:
  ```c
  // user_card.c
  #include "user_card.h"
  #include "cels.h"
  #include <stdio.h>

  /* Implements linkable composable with automatic slot caching */
  CEL_ComposableDef(UserCard, const char*, name, int, score) {
      printf("  User: %s | Score: %d\n", name, score);
  }
  ```

> [!TIP]
> For header-only or local single-file composables, simply use `CEL_Composable(Name, ...)` which defines a `static inline` function. Use `CEL_ComposableDef` only when separating declarations across `.h` and `.c` translation units.

---

## 6. Component Decomposition: How Composables Have Children

A common question from developers arriving from HTML/JSX or Kotlin Compose is:
> *"Can composables have children? Why isn't there a `cel_container(Card) { ... }` macro?"*

The answer is: **Composables in CELS already have children naturally!**

### Why CELS Avoids Synthetic Container Macros

Frameworks that introduce wrapper macros like `cel_container(Card) { ... }` or `cel(Card(...)) { ... }` introduce severe friction in C:
1. **Dual Calling Conventions**: Developers have to memorize whether a widget is a "container" (requiring `cel_container`) or a "leaf" (called like `Widget(...)`).
2. **Degraded IDE Support**: Macro-generated loop constructs obscure parameter tooltips, disable autocomplete, and generate confusing compiler error messages in C99.
3. **Loss of Architectural Clarity**: Generic container blocks encourage massive, tightly coupled monolithic functions instead of clean, reusable components.

In CELS, **every composable is called identically**: `Name(...)` or `Name(id, ...)`. There are no special cases, no wrapper macros, and no syntax changes between containers and leaves.

### Pattern: Natural Component Decomposition

In modern declarative UI (the same architectural pattern embraced by SwiftUI and Jetpack Compose), the canonical way to build hierarchies is **Component Decomposition**:
- A parent composable *is* the container.
- Inside its body, the parent invokes focused child composables sequentially.
- The CELS slot table engine automatically nests each child group under the parent group in the gap buffer with $\mathcal{O}(1)$ performance.

```c
/* 1. Focused child composables with single responsibilities */
CEL_Composable(CardHeader, const char*, title, const char*, subtitle) {
    printf("  [Header] %s (%s)\n", title, subtitle);
}

CEL_Composable(UserAvatar, const char*, avatarUrl) {
    printf("  [Avatar] %s\n", avatarUrl);
}

CEL_Composable(UserBio, const char*, bio) {
    printf("  [Bio] %s\n", bio);
}

CEL_Composable(CardFooter, int, userId) {
    printf("  [Footer] Actions for user #%d\n", userId);
}

/* 2. Parent composable naturally nests its children */
CEL_Composable(UserProfileCard, const UserProfile*, user) {
    CardHeader(user->displayName, user->role);
    UserAvatar(user->avatarUrl);
    UserBio(user->bio);
    CardFooter(user->id);
}
```

### What Happens in the Slot Table?

Because `CardHeader`, `UserAvatar`, `UserBio`, and `CardFooter` are invoked within the execution scope of `UserProfileCard`, CELS automatically records them as nested child groups in the slot table:

```
[ Group: UserProfileCard ]
  ├── [ Group: CardHeader ]
  ├── [ Group: UserAvatar ]
  ├── [ Group: UserBio ]
  └── [ Group: CardFooter ]
```

When `UserProfileCard` recomposes, CELS navigates the hierarchy in cache-aligned slot order. If the user's role or bio changes, only the affected child composable re-executes. If the entire card is omitted, CELS tears down the entire subtree and runs all destructors in reverse order.

### Reusable Layout Containers

When creating reusable layout framing (such as dialogs, panels, or modal frames), decompose the layout and pass contextual children or data models directly:

```c
/* Layout wrapper framing */
CEL_Composable(DialogFrame, const char*, title, const DialogData*, data) {
    DialogTitleBar(title);
    DialogContent(data);
    DialogButtonBar(data->canSubmit);
}
```

If children require shared environment or styling from their container, pair decomposition with **ambient context** (`cel_set_context` / `cel_get_context`):
```c
CEL_Composable(ThemedPanel, const PanelData*, data) {
    cel_set_context(PanelTheme, &data->theme);
    PanelHeader(data->title);
    PanelBody(data->content);
    PanelFooter();
}
```

---

## 7. Conditional Rendering (`if/else`)

One of CELS's greatest strengths is **native C conditional logic**. You don't need ternary wrappers, virtual DOM diffing, or custom templating engines. Just use standard `if`, `else`, and `switch` statements:

```c
CEL_Composable(WindowContent, WindowState*, win) {
    cel_watch(win);

    printf("Window: %dx%d\n", win->width, win->height);

    /* Conditional branch */
    if (win->showBadge) {
        StatusBadge();
    } else {
        PlaceholderNotice();
    }
}
```

### What Happens in the Slot Table?

CELS uses an internal **Gap Buffer** to track structure across frames:

```
Frame 1: showBadge == true
[ Group: WindowContent ]
  ├── [ Slot: win watch ]
  └── [ Group: StatusBadge ] <── Mounted!

Frame 2: showBadge == false
[ Group: WindowContent ]
  ├── [ Slot: win watch ]
  └── [ Group: PlaceholderNotice ] <── StatusBadge unmounted (destructors run!)
```

1. **Mounting**: When `StatusBadge()` executes for the first time, CELS inserts a group into the slot table and invokes any attached `mount` blocks.
2. **Skipping**: On frames where nothing changed, CELS navigates the slot table in $O(1)$ without re-evaluating unchanged subtrees.
3. **Unmounting**: When `win->showBadge` becomes `false`, CELS skips calling `StatusBadge()`. The gap buffer reconciles the omission, detects the missing group, immediately runs its registered `unmount` cleanup handlers in reverse order, and reclaims slot memory.

---

## 8. Loops and Keyed Dynamic Lists

In slot-table architectures, child groups are matched sequentially by execution index. In dynamic lists where items can be inserted, deleted, or reordered, indexing alone leads to bugs (state leaking to the wrong row, exit transitions failing to fire). 

CELS offers two clean ways to key dynamic children:

### Approach A: Keyed Composables (Recommended)
Declare the child composable with an explicit `id` parameter. The first parameter is automatically bound as the stable 64-bit slot key and active entity ID:

```c
typedef struct TodoItem {
    uint64_t id;
    const char *title;
} TodoItem;

/* Declares a keyed composable: TodoItemCard(id, item) */
CEL_Composable(TodoItemCard, id, const TodoItem*, item) {
    // Local persistent state preserved across reordering!
    int *editCount = cel_remember(int, &(int){ 0 });
    printf("  - [%llu] %s (edited %d times)\n", (unsigned long long)cel_id(), item->title, *editCount);
}

CEL_Composable(TodoList, const TodoItem*, items, int, count) {
    for (int i = 0; i < count; ++i) {
        /* Call directly with id: clean, uniform, zero wrapper macros */
        TodoItemCard(items[i].id, &items[i]);
    }
}
```

### Approach B: Scoped Key Block (`cel_key`)
For composables that are already declared without an `id` parameter, wrap the call site in `cel_key(id)`:

```c
CEL_Composable(TodoItemCardUnkeyed, const TodoItem*, item) {
    int *editCount = cel_remember(int, &(int){ 0 });
    printf("  - %s (edited %d times)\n", item->title, *editCount);
}

CEL_Composable(TodoListScoped, const TodoItem*, items, int, count) {
    for (int i = 0; i < count; ++i) {
        cel_key(items[i].id) {
            TodoItemCardUnkeyed(&items[i]);
        }
    }
}
```

---

## 9. Declarative Modifications & Ambient Context: Theming, Inputs & Components

CELS provides two modern paradigms for modifying composables without monolithic struct "modifiers" or parameter drilling:
1. **Ambient Context (`cel_set_context` / `cel_get_context`)**: Cascades environment, themes, inputs, or container state pointers down a subtree without parameter drilling.
2. **Declarative Component Reconciliation (`cel_has`, `cel_has_tag`, `cel_get`, `cel_is`)**: Attaches typed data components directly to composable entities (`cel_id()`, `cel_key()`).

---

### When and Where to Use Context: The 3-Way Decision Matrix

Developers often wonder: *Should this be a parameter (prop), reactive state, or ambient context?*

```
                             Is the data passed from parent to child?
                                                │
                       ┌────────────────────────┴────────────────────────┐
                       ▼                                                 ▼
             Direct 1-to-1 relationship                        Shared across an entire
             (e.g., label, row model, item id)                subtree (3+ levels deep)
                       │                                                 │
                       ▼                                                 ▼
               Use **PROPS**                                  Does it change over time?
           (Function Parameters)                                         │
                                                       ┌─────────────────┴─────────────────┐
                                                       ▼                                   ▼
                                                  YES (Dynamic)                       NO (Static / Read-only)
                                                       │                                   │
                                                       ▼                                   ▼
                                           Own as **STATE**                    Attach as **CONTEXT**
                                           (`cel_remember_state`)               (`cel_set_context`)
                                           Publish pointer via **CONTEXT**      (Theme, DPI, InputMap)
                                           (`cel_set_context`)
```

| Decision Metric | **Props** (Function Arguments) | **State** (`cel_state` / `cel_remember_state`) | **Context** (`cel_set_context` / `cel_get_context`) |
| :--- | :--- | :--- | :--- |
| **Primary Purpose** | Explicit inputs specific to an individual composable. | Authoritative, double-buffered data that mutates and drives rendering. | Environmental data or state pointers that cascade down an entire subtree. |
| **Typical Data** | `label`, `width`, `const User *user`, `itemId`. | `activeIndex`, `currentHealth`, `volume`, `isOpen`. | `UITheme`, `CelsInputMap`, `CelsFocusState`, `LazyListState`. |
| **Reactivity** | Passive input. | **Active**: Calling `cel_mutate(ptr)` schedules recomposition. | Non-reactive courier: routes pointers down the active call tree. |
| **Lifecycle** | Call-frame argument. | Persists across recomposition frames in slot memory. | Bound to the composable call duration; automatically popped on exit. |
| **When to Use** | Direct parent-child relationships, composable-specific values. | Any data whose modification must re-evaluate composables. | Cross-cutting environment or state pointers needed by descendants 3+ tiers deep. |

---

### 1. Ambient Context Theming (`cel_set_context` / `cel_get_context`)
Passes visual styling or environment down an entire composable subtree with zero parameter drilling and zero heap allocations.

#### Publishing Context:
```c
CEL_Composable(SettingsContainer) {
    /* Provide dark theme down this entire subtree */
    cel_set_context(UITheme, &darkTheme);
    SettingsScreen(); // Descendants consume via cel_get_context(UITheme)
}
```

#### Consuming Context (Two Styles):

1. **Result-Protected Form (`cel_get_context(Type, &outPtr) -> CelsResult`) [Recommended for Safety]**:
   Returns `CELS_OK` on success, `CELS_ERROR_NOT_FOUND` if no ancestor provides the type, or `CELS_ERROR_INVALID_STATE` if called outside an active session. Enables rigorous error handling and eliminates accidental null dereferences:
   ```c
   CEL_Composable(SettingsItem, const char*, title) {
       const UITheme *theme = NULL;
       if (cel_get_context(UITheme, &theme) == CELS_OK) {
           RenderTextWithColor(title, theme->textColor);
       } else {
           RenderTextDefault(title);
       }
   }
   ```

2. **Pointer Form (`cel_get_context(Type) -> const Type*`)**:
   Returns the nearest ancestor context pointer, or `NULL` if absent:
   ```c
   CEL_Composable(SettingsItemFast, const char*, title) {
       const UITheme *theme = cel_get_context(UITheme);
       uint32_t color = theme ? theme->textColor : 0xFFFFFFFF;
       RenderTextWithColor(title, color);
   }
   ```

---

### 2. Layouts & Containers with Children (`cel_container` & `CEL_Layout`)

In CELS, **every composable is naturally a container for its children** via the slot table. There is no need for manual `CelsEnterComposable` / `CelsExitGroup` calls or boilerplate plumbing—when a composable invokes other composables in its body, CELS automatically tracks sibling metrics, parent-child hierarchies, and settled child counts in $\mathcal{O}(1)$.

#### Core Primitive: `cel_container(Name)`
CELS core provides [`cel_container(Name)`](file:///D:/cels-workspace/library/cels/include/cels/cels.h#L1982) to establish an inline container group in the slot table with zero heap allocations:

```c
cel_container(SectionContainer) {
    HeaderItem("Section 1");
    SettingsRow("Volume");
    SettingsRow("Graphics");
}
```

#### Direct Inline Layouts (`CEL_Layout`):
To manage keyboard/gamepad focus navigation, the example application pattern in [`cels_layout.h`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h) builds on `cel_container` to provide scoped directional layouts with zero `#define` boilerplate:

```c
/* 1. Linear Vertical Menu: W/S or Up/Down cycles focus */
CEL_Layout(CEL_LAYOUT_DIR_VERT) {
    VolumeSlider(settings);
    MuteToggle(settings);
    GraphicsSelector(settings);
    ResumeButton(state);
}

/* 2. Responsive Direction: Dynamic orientation across recomposition */
CEL_Layout(isPortrait ? CEL_LAYOUT_DIR_VERT : CEL_LAYOUT_DIR_HORIZ) {
    SidebarPanel();
    ContentPanel();
}

/* 3. Multi-directional 2D Game Grid: 4 columns with Up/Down/Left/Right 2D navigation */
CEL_Layout(InventoryGrid, CEL_LAYOUT_DIR_2D, 4 /* columns */, true /* wrapFocus */) {
    for (int i = 0; i < 16; ++i) {
        InventorySlotItem(inventory[i]);
    }
}
```

#### Reusable Layout Composables (Composable is the Universal Base):
If developers want a reusable named layout component like `InventoryGrid`, they simply author a standard [`CEL_Composable`](file:///D:/cels-workspace/library/cels/include/cels/cels.h) containing `CEL_Layout`:

```c
CEL_Composable(InventoryGrid, InventoryState*, inv) {
    CEL_Layout(CEL_LAYOUT_DIR_2D, 4) {
        for (int i = 0; i < 16; ++i) {
            InventorySlotItem(inv->items[i]);
        }
    }
}

// Callers use it identically to all other composables:
InventoryGrid(inv);
```

> [!NOTE]
> **Application-Level Pattern vs. Core CELS Library**:
> `cel_container` is a **core CELS primitive** in [`include/cels/cels.h`](file:///D:/cels-workspace/library/cels/include/cels/cels.h). `CEL_Layout` is an **application-level helper pattern** in [`examples/common/cels_layout.h`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h) demonstrating how developers combine:
> - `cel_container`: Enters/exits slot groups with zero custom scope plumbing.
> - `cel_remember_state`: Allocates double-buffered focus state in slot memory.
> - `cel_has`: Declaratively attaches ECS components (`cel_has(CelsLayoutDir, { dir })`).
> - `cel_set_context` / `cel_get_context`: Ambient context passing down subtrees.
> - `cel_child_index` / `cel_child_count`: Safe $\mathcal{O}(1)$ child metrics.
>
> Developers can use `cels_layout.h` as an example to implement their own custom layout engines, flexbox wrappers, or game UI containers.

> [!NOTE]
> **Component vs. State Separation in Layouts**:
> In [`cels_layout.h`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h), layout direction (`CelsLayoutDir`) is attached as a declarative ECS **Component** via `cel_has(CelsLayoutDir, { params.dir })` and published down the subtree via ambient context `cel_set_context(CelsLayoutDir, &params.dir)`. It is **not** stored inside reactive state! Reactive state (`CelsFocusState`) is kept lightweight and double-buffered, tracking only active focus indices (`activeIndex`, `totalCount`, `wrapFocus`).

---

### 3. Child Introspection: Sibling Metrics & Layout Positioning

Child composables running inside any container or layout block can inspect their sibling metrics safely. All functions return `CelsResult` to protect against execution outside a container or session:

| Function | Output | Returns | Typical Use Case |
| :--- | :--- | :--- | :--- |
| `cel_child_info(&info)` | `CelsChildInfo` | `CelsResult` | Comprehensive query: `index`, `totalCount`, `parentId`, `childId`, `isFirst`, `isLast`. |
| `cel_child_index(&outIdx)` | `uint32_t` | `CelsResult` | 0-based sibling index among container children. |
| `cel_child_count(&outCount)` | `uint32_t` | `CelsResult` | Total direct child count inside parent container. |
| `cel_is_first_child(&outFirst)` | `bool` | `CelsResult` | `true` if `index == 0`. Useful for skipping top border dividers. |
| `cel_is_last_child(&outLast)` | `bool` | `CelsResult` | `true` if this child is the final item. Useful for closing borders. |

> [!NOTE]
> If a child composable is rendered standalone outside a container (or outside an active session), `cel_child_index` and `cel_child_info` return `CELS_ERROR_INVALID_STATE` rather than returning garbage values or crashing.

#### Example: Sibling-Aware Focusable Item
```c
CEL_Composable(FocusItem, const char*, label) {
    /* 1. Safely resolve sibling index */
    uint32_t myIndex = 0;
    if (cel_child_index(&myIndex) != CELS_OK) {
        // Standalone fallback: render as passive item
        RenderPassiveLabel(label);
        return;
    }

    /* 2. Safely resolve parent focus context */
    const CelsFocusState *focus = NULL;
    if (cel_get_context(CelsFocusState, &focus) == CELS_OK) {
        bool isFocused = (focus->focusedIndex == (int)myIndex);
        RenderSelectableLabel(label, isFocused);
    }

    /* 3. Check if first or last child for visual dividers */
    bool isLast = false;
    if (cel_is_last_child(&isLast) == CELS_OK && !isLast) {
        RenderDividerLine();
    }
}
```

---

### 4. The Container Synergy Pattern: State + Context
Containers (e.g. [`CEL_Layout`](file:///D:/cels-workspace/library/cels/examples/common/cels_layout.h#L147), `LazyColumn`, or `NavigationStack`) pair State and Context together:
1. **Container owns State**: Allocates reactive state in slot memory via `cel_remember_state`.
2. **Container publishes Context**: Shares the state pointer down its subtree via `cel_set_context`.
3. **Descendants consume & mutate**: Children query the state via `cel_get_context(&state)` and mutate it via `cel_mutate`.

#### Example: Authoring a `LazyColumn` Container

```c
/* 1. Define reactive container state */
CEL_State(LazyListState) {
    int firstVisibleIndex;
    int visibleItemCount;
    int totalItems;
};

/* 2. Container Composable: handles input and publishes context */
CEL_Composable(LazyColumn, LazyListState*, state, int, totalCount) {
    cel_watch(state);

    /* Process scroll navigation */
    if (cel_action_consume(ACTION_SCROLL_DOWN)) {
        cel_mutate(state) {
            if (this->firstVisibleIndex + this->visibleItemCount < this->totalItems) {
                this->firstVisibleIndex++;
            }
        }
    } else if (cel_action_consume(ACTION_SCROLL_UP)) {
        cel_mutate(state) {
            if (this->firstVisibleIndex > 0) {
                this->firstVisibleIndex--;
            }
        }
    }

    state->totalItems = totalCount;

    /* Provide state to children via ambient context */
    cel_set_context(LazyListState, state);
}
```

#### Consuming the Container:
```c
CEL_Composition(DirectoryView, const User*, users, int, count) {
    LazyListState *listState = cel_remember_state(LazyListState, {
        .firstVisibleIndex = 0,
        .visibleItemCount = 6,
        .totalItems = count
    });

    LazyColumn(listState, count);

    /* Render only visible window */
    int start = listState->firstVisibleIndex;
    int end = CELS_MIN(start + listState->visibleItemCount, count);
    for (int i = start; i < end; ++i) {
        cel_key(users[i].id) {
            UserCardRow(&users[i]);
        }
    }
}
```

---

### 3. Ambient Input Management & Focus Routing (`cel_set_context(CelsInputMap, ...)`)
Binds Unity-style declarative action maps down focused subtrees. Unfocused layouts simply don't receive an input map, preventing background keystroke bleed-through with zero global event handlers:

```c
/* Route active input mapping to whichever panel has focus */
CEL_Composable(WorkspaceView, WorkspaceState*, state) {
    if (state->activeFocus == LAYOUT_EDITOR) {
        cel_set_context(CelsInputMap, &g_editorMap);
        EditorPanel(state, true /* isFocused */);
        MixerPanel(state, false); // No map -> cel_action_consume() returns false!
    } else {
        EditorPanel(state, false);
        cel_set_context(CelsInputMap, &g_mixerMap);
        MixerPanel(state, true /* isFocused */);
    }
}
```
Inside the panel, query actions directly: `if (cel_action_consume(ACTION_UP)) { ... }`. See runnable code in [`examples/input/`](file:///D:/cels-workspace/library/cels/examples/input/).

---

### 4. Declarative Component Attachment (`cel_has`, `cel_has_tag`, `cel_get`, `cel_is`)
Attaches typed data components directly to composable entities (`cel_id()`, `cel_key()`). 
- **Automatic Omission Removal**: If a condition becomes false in a subsequent frame, CELS automatically diffs the node and stages a `CELS_OP_REMOVE` transaction for Flecs!
- **Automatic Unmount Cleanup**: When the entity leaves the tree, CELS stages `CELS_OP_DELETE`.

```c
CEL_Composable(CharacterNode, id, const CharacterState*, charState) {
    cel_has(Position, { charState->x, charState->y, charState->z });

    // Automatically added when true, and AUTOMATICALLY REMOVED when false!
    if (charState->isBurning) {
        cel_has(BurnDebuff, { .dps = 15.0f });
    }
}
```
For complete details and walkthroughs, see the `cels-modifications` skill and runnable code in [`examples/component/`](file:///D:/cels-workspace/library/cels/examples/component/) and [`examples/theming/`](file:///D:/cels-workspace/library/cels/examples/theming/).

---

## 10. Best Practices & Pitfalls

### Do This
- **Break UI into Focused Composables**: Keep each composable focused on a single responsibility (e.g. `Header`, `ItemList`, `ItemRow`).
- **Compose Children Naturally**: Call child composables sequentially within parent composable bodies. CELS automatically nests child groups in the slot table.
- **Pass Plain C Types as Props**: Primitive types (`int`, `float`, `const char*`) or pointers to state structs make composables easy to reason about and test.
- **Use Standard C Prototypes in Headers**: For cross-file composables, declare `void MyWidget(...)` in `.h` and implement with `CEL_ComposableDef` in `.c` to keep headers clean and portable.

### Don't Do That
- **Don't Invent Synthetic Container Macros**: Never try to wrap composables in trailing-block macros (e.g. `cel_container(...) { ... }`). Keep invocation universal: `Widget(...)` or `Widget(id, ...)`.
- **Don't Create Compositions for Small Widgets**: Never make buttons or text labels a `CEL_Composition`. Compositions are top-level session roots (like windows or full screens). Everything else is a `CEL_Composable`.
- **Don't Call Composables Outside an Active Session**: Calling a `CEL_Composable` from a random background thread or non-recomposition context will trigger an assertion:
  ```text
  Assertion failed: sess != NULL && "Widget called outside of an active CelsSession"
  ```
  Composables must always execute during a recomposition pass on the session thread.

---

## Next Steps

Now that you understand tree structure and conditional rendering:
- Move to [03. Reactive State Management](03-reactive-state.md) to master State Hoisting, double-buffering, and zero-string-key reactivity.
