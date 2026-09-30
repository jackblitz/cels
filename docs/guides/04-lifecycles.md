# Resource Lifecycles (`CEL_Lifecycle`)

In declarative UI and game programming, managing **native resources** (such as GPU textures, shaders, audio voices, network sockets, and file descriptors) is often error-prone. Manual allocation and deallocation quickly lead to memory leaks or use-after-free bugs when views are toggled or unmounted.

CELS solves this with **pure topological lifecycle tracking**: when a composable enters the active tree, it **mounts**; when it is skipped or excluded, it **unmounts**. The `CEL_Lifecycle` macro guarantees that acquisition and release are perfectly paired.

---

## 1. The Mount & Unmount Mental Model

Every composable has a lifecycle governed by its presence in the composition hierarchy:

```
Frame 1: if (isActive) Widget();  ──► MOUNT
                                      ├── Resource acquired (GPU texture, socket)
                                      └── Unmount destructor registered

Frame 2..N: Widget() still called  ──► STEADY STATE
                                      └── Pointer updated; no re-allocation

Frame N+1: isActive is false      ──► UNMOUNT
                                      ├── Gap buffer detects omitted group
                                      └── Destructors execute in REVERSE order!
```

> [!NOTE]
> Lifecycles in CELS are **structural, not visual**. A component unmounts whenever the CELS slot table detects that its composable function was not invoked during the current recomposition pass.

---

## 2. Declaring a Lifecycle (`CEL_Lifecycle`)

Declare a lifecycle controller at file scope using `CEL_Lifecycle`. Inside, define two blocks:
- `mount`: Runs exactly once when the composable is first entered.
- `unmount`: Runs when the composable is excluded or the session is destroyed.

```c
#include "cels.h"
#include <stdio.h>

typedef struct AudioVoice {
    int voiceId;
    const char *soundFile;
} AudioVoice;

CEL_Lifecycle(AudioVoiceLifecycle, AudioVoice *voice) {
    mount {
        printf("  [Audio] Acquired audio voice #%d for %s\n",
               voice->voiceId, voice->soundFile);
    }
    unmount {
        printf("  [Audio] Released audio voice #%d\n", voice->voiceId);
    }
}
```

---

## 3. Attaching to a Composable (`cel_lifecycle`)

Attach the lifecycle inside any composable using `cel_lifecycle(LifecycleName, resourcePointer)`:

```c
CEL_Composable(SoundEmitter, const char*, soundFile) {
    /* Remember the resource handle across frames in the slot table */
    AudioVoice *voice = cel_remember(AudioVoice, ((AudioVoice){
        .voiceId = 101,
        .soundFile = soundFile
    }));

    /* Bind lifecycle controller to this composable node */
    cel_lifecycle(AudioVoiceLifecycle, voice);

    printf("  [SoundEmitter] Playing: %s\n", voice->soundFile);
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
   - The `mount` block runs immediately.
2. **While `soundEnabled` remains `true`**:
   - `SoundEmitter` runs each recomposition; `mount` does **not** re-run.
3. **When `soundEnabled` transitions to `false`**:
   - `SoundEmitter` is skipped.
   - CELS detects the omitted node and executes the `unmount` block immediately.

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
