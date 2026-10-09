# Resource Lifecycles & Execution Model

In declarative UI and game programming, managing **native resources** (such as GPU textures, shaders, audio voices, network sockets, and file descriptors) is often error-prone. Manual allocation and deallocation quickly lead to memory leaks or use-after-free bugs when views are toggled or unmounted.

CELS solves this with **pure topological lifecycle tracking**: when a composable enters the active tree, it **attaches** (mounts); when it is skipped or excluded, it **detaches** (unmounts). Acquisition and release are perfectly paired.

---

## 1. Architectural Mental Model: Code Execution Order vs. Presence in the Tree

Rather than visualizing lifecycles around abstract frame ticks or timer loops, CELS lifecycles follow **Code-to-Code Execution**:

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
   - **Scope Entry (Composable Body)**: Executes *before* any children run. Publishes ambient context, attaches declarative ECS components (`cel_has`), and allocates/reads reactive state.
   - **Children Execution**: Direct child composables execute sequentially within the group scope.
   - **Scope Exit (`on_exit(childCount) { ... }`)**: Executes when all children have completed at the closing `}` of the container scope (or when the composable exits). Receives the settled, exact `childCount` of direct children (ideal for layout metrics, directional focus clamping, and grid wrapping).
2. **Presence in the Tree (Topology Lifetime)**:
   - **`on_attach { ... }`**: Executes once on the very first pass when the composable is freshly mounted into the session hierarchy.
   - **`on_detach { ... }`**: Executes when the composable leaves the hierarchy (parent condition evaluates to `false`, or root teardown).

---

## 2. Primary Approach: Inline Hooks inside `CEL_Composable`

You can declare lifecycle hooks directly inside any `CEL_Composable`:

```c
#include "cels.h"
#include <stdio.h>

CEL_Composable(SoundEmitter, const char*, soundFile) {
    /* Remember resource handle across passes in the slot table */
    AudioVoice *voice = cel_remember(AudioVoice, ((AudioVoice){
        .voiceId = 101,
        .soundFile = soundFile
    }));

    on_attach {
        /* Runs once when this node is first mounted */
        voice->handle = AudioEngine_Play(voice->soundFile);
        printf("  [SoundEmitter] Attached & playing: %s\n", voice->soundFile);
    }

    on_detach {
        /* Runs once when this node is omitted or pruned from the tree */
        AudioEngine_Stop(voice->handle);
        printf("  [SoundEmitter] Detached & stopped: %s\n", voice->soundFile);
    }
}
```

For container composables that manage children (e.g. `Column`, `Row`, `Toolbar`):

```c
CEL_Composable(CelsToolbar, CelsLayoutDir, dir, int, cols) {
    // 1. Scope Entry: Runs before children execute
    Toolbar config = { .dir = dir, .cols = cols };
    cel_has(Toolbar, config);
    cel_set_context(Toolbar, &config);

    // 2. Scope Exit: Runs after all children execute (at closing '}')
    on_exit(childCount) {
        printf("Toolbar rendered with %u settled children!\n", childCount);
    }
}

#define Toolbar(...) cel_scope(CelsToolbar, ##__VA_ARGS__)
```

---

## 3. Decoupled Approach: Standalone `CEL_Lifecycle`

When you want to reuse a lifecycle controller across multiple composables or decouple lifecycle logic across separate compilation units, use standalone `CEL_Lifecycle`:

```c
typedef struct DialogState {
    int renderedItems;
    bool isVisible;
} DialogState;

CEL_Lifecycle(DialogLifecycle, DialogState *dialog) {
    mount {
        printf("  [Dialog] Mounted!\n");
    }
    before_children {
        /* Runs every pass before children */
        cel_set_context(UITheme, &DarkTheme);
    }
    after_children(childCount) {
        /* Runs every pass after children with settled child count */
        dialog->renderedItems = childCount;
    }
    unmount {
        printf("  [Dialog] Unmounted!\n");
    }
}

CEL_Composable(DialogView, DialogState*, dialog) {
    cel_lifecycle(DialogLifecycle, dialog);
    // ...
}
```

Now let's see how conditional rendering manages this lifecycle automatically:

```c
CEL_Composable(WorldAudioView, bool, soundEnabled) {
    if (soundEnabled) {
        SoundEmitter("ambient_wind.wav");
    }
}
```

1. **When `soundEnabled` transitions from `false` to `true`**:
   - `SoundEmitter` executes for the first time.
   - Slot memory is allocated for `AudioVoice`.
   - The `on_attach` block runs immediately.
2. **While `soundEnabled` remains `true`**:
   - `SoundEmitter` runs each recomposition; `on_attach` does **not** re-run.
3. **When `soundEnabled` transitions to `false`**:
   - `SoundEmitter` is skipped.
   - CELS detects the omitted node and executes the `on_detach` block immediately.

---

## 4. Real-World Example: Status Badge from `examples/window/`

In [`examples/window/app/composition/status_badge.h`](file:///D:/cels-workspace/library/cels/examples/window/app/composition/status_badge.h), a status badge demonstrates this exact decoupled lifecycle:

```c
typedef struct BadgeData {
    const char *label;
} BadgeData;

CEL_Lifecycle(StatusBadgeLifecycle, BadgeData *badge) {
    mount {
        printf("    [BadgeLifecycle] MOUNT: Status badge mounted ('%s')\n",
               badge->label);
    }
    unmount {
        printf("    [BadgeLifecycle] UNMOUNT: Status badge unmounted\n");
    }
}

CEL_Composable(StatusBadge) {
    BadgeData *badge = cel_remember(BadgeData, { .label = "Connected / Active" });
    cel_lifecycle(StatusBadgeLifecycle, badge);
    printf("    [Badge] Status: %s\n", badge->label);
}
```

When you toggle the badge in the host (by pressing `B`), the badge cleanly mounts and unmounts, while the parent window and root composition stay alive!

---

## 5. Reverse Order Destruction Guarantee

When a parent composable containing multiple children is unmounted, CELS tears down resources in **strict reverse order of mounting** (LIFO: Last In, First Out).

Consider a GPU rendering component:
```c
CEL_Composable(RenderCanvas) {
    /* 1. Allocate Texture */
    cel_lifecycle(TextureLifecycle, texHandle);

    /* 2. Allocate Shader Pipeline depending on Texture */
    cel_lifecycle(PipelineLifecycle, pipelineHandle);
}
```

On unmount:
1. `PipelineLifecycle` unmounts **first**.
2. `TextureLifecycle` unmounts **second**.

This guarantees that dependent resources are never torn down before the things that depend on them!

---

## 6. Hot-Reload Safety (`CelsSessionUpdateLifecycle`)

In languages like C, hot-reloading code DLLs is notoriously hazardous for lifecycles: if a DLL is unloaded and reloaded at a new base address, existing function pointers to destructors will crash with access violations.

CELS completely solves this:
- During DLL reload, CELS invokes `CelsSessionUpdateLifecycle`.
- It rewrites existing lifecycle destructor pointers to the newly reloaded function addresses in the newly loaded library.
- When an unmount later triggers, CELS safely jumps into the updated code!

---

## 7. Best Practices & Pitfalls

### Do This
- **Pair Acquisition and Release in One Place**: Always put acquisition in `mount` and cleanup in `unmount`. This guarantees leak-free behavior.
- **Use `cel_remember` for Resource Handles**: Store the native handle (ID, pointer, descriptor) in a slot using `cel_remember` so it survives across recomposition ticks until unmount.
- **Use Parameterized Lifecycles**: Pass the resource pointer as an argument to `cel_lifecycle(MyLifecycle, handle)` rather than reading from global variables.

### Don't Do That
- ❌ **Don't Allocate in the Composable Body without a Lifecycle**:
  ```c
  /* LEAK HAZARD: Allocates every frame or leaks on unmount! */
  CEL_Composable(BadWidget) {
      FILE *f = fopen("log.txt", "w"); // Who closes this when unmounted?
  }
  ```
- ❌ **Don't Call `unmount` Code Manually**: Let CELS drive the unmount phase through topological reconciliation.

---

## Next Steps

Now that you can safely manage native resources:
- Explore [05. Asynchronous Tasks & Coroutines](05-tasks-and-coroutines.md) for multi-frame procedural logic and non-blocking timers.
