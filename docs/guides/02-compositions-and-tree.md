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

## 6. Conditional Rendering (`if/else`)

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

## 7. Loops and Dynamic Lists

To render lists of items, loop over them with standard `for` loops. When rendering dynamic collections where items can be inserted or reordered, ensure sub-nodes have stable keys:

```c
typedef struct TodoItem {
    int id;
    const char *title;
} TodoItem;

CEL_Composable(TodoList, const TodoItem*, items, int, count) {
    for (int i = 0; i < count; ++i) {
        printf("  - [%d] %s\n", items[i].id, items[i].title);
    }
}
```

---

## 8. Best Practices & Pitfalls

### Do This
- **Break UI into Focused Composables**: Keep each composable focused on a single responsibility (e.g. `Header`, `ItemList`, `ItemRow`).
- **Pass Plain C Types as Props**: Primitive types (`int`, `float`, `const char*`) or pointers to state structs make composables easy to reason about and test.
- **Use Standard C Prototypes in Headers**: For cross-file composables, declare `void MyWidget(...)` in `.h` and implement with `CEL_ComposableDef` in `.c` to keep headers clean and portable.

### Don't Do That
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
