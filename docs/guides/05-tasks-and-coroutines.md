# Tasks & Cooperative Coroutines (`CEL_Task`)

Declarative composition is ideal for projecting reactive state into UI or world views. However, real-world applications frequently require **procedural, multi-step asynchronous processes**:
- Establishing network sockets with handshakes and timeouts.
- Sequenced cutscenes, scripted NPC behaviors, or boss phases.
- Timed delays without freezing the main application loop.
- Multi-step wizards or onboarding flows.

CELS provides `CEL_Task`, a **lightweight, stack-preserving cooperative coroutine DSL** implemented via fibers. Tasks execute across multiple engine frames without spawning OS threads or blocking the event loop.

---

## 1. The Mental Model of a Task

Unlike a regular composable that executes immediately from top to bottom every frame, a `CEL_Task`:
1. **Persists its Execution State**: Uses `CelsTaskState` pinned in slot memory.
2. **Suspends and Resumes Cooperatively**: Uses `cel_wait(ms)` (monotonic timer delays) or `cel_yield()` (pause until next frame) while keeping the full C stack intact.
3. **Guarantees Unmount Teardown**: Defines a `cancel { ... }` block that runs if cancelled explicitly **or** if the enclosing composable unmounts!

```
┌────────────────────────────────────────────────────────┐
│ CEL_Task(NetworkConnectTask, host, port)               │
│                                                        │
│  cancel {                                              │
│      CloseSocket(); // Runs on cancel OR unmount       │
│  }                                                     │
│                                                        │
│  run {                                                 │
│      ResolveDns(host);                                 │
│      cel_wait(200);   // Non-blocking 200ms delay      │
│      Connect(port);                                    │
│      cel_wait(150);   // Non-blocking 150ms delay      │
│      while(1) {                                        │
│          cel_wait(50); // Periodic polling             │
│          PumpPackets();                                │
│      }                                                 │
│  }                                                     │
└────────────────────────────────────────────────────────┘
```

---

## 2. Declaring a Task (`CEL_Task`)

Declare tasks at file scope. You can pass 0, 1, or 2 typed parameters:

```c
#include "cels.h"
#include <stdio.h>

CEL_Task(DownloadTask, const char*, url, int, timeoutMs) {
    cancel {
        printf("  [Task] CANCELLED: Aborting download for %s\n", url);
    }

    run {
        printf("  [Task] Initiating download from %s...\n", url);
        cel_wait(200); /* 200ms connection wait */

        printf("  [Task] Receiving data...\n");
        cel_wait(300); /* 300ms transfer wait */

        printf("  [Task] Download finished successfully!\n");
    }
}
```

---

## 3. Mounting and Executing Tasks

Inside any `CEL_Composition` or `CEL_Composable`, invoke the task with `cel_task(...)`:

```c
CEL_Composable(DownloadPanel, bool, isDownloading) {
    if (isDownloading) {
        /* While isDownloading is true, this task executes step-by-step */
        cel_task(DownloadTask, "https://api.example.com/data.bin", 5000);
    }
}
```

### Unmount Cancellation in Action
Notice what happens if `isDownloading` becomes `false` mid-execution:
1. `DownloadPanel` recomposes and skips the `if (isDownloading)` branch.
2. The CELS slot table reconciliation notices that `DownloadTask` was not called.
3. CELS **immediately runs the `cancel { ... }` block** and cleans up the fiber!

> [!IMPORTANT]
> You never have to manually track cancel tokens across screens. If the user navigates away or toggles off the view, CELS automatically cancels and tears down the task.

---

## 4. Real-World Walkthrough: The Network Task

In [`examples/task/app/network_task.h`](file:///D:/cels-workspace/library/cels/examples/task/app/network_task.h), a complete 5-step network connection task demonstrates procedural state progression:

```c
CEL_Task(NetworkConnectTask, NetworkState*, net, const char*, server, int, port) {
    cancel {
        printf("  [NetworkTask] Teardown triggered! Closing socket to %s:%d...\n",
               server, port);
        cel_mutate(net) {
            this->status = NET_CANCELLED;
            this->isConnecting = false;
        }
    }

    run {
        /* Step 1: DNS Resolution */
        cel_mutate(net) {
            this->status = NET_RESOLVING;
        }
        cel_wait(200);

        /* Step 2: TCP Socket */
        cel_mutate(net) {
            this->status = NET_CONNECTING;
        }
        cel_wait(150);

        /* Step 3: Server Handshake */
        cel_mutate(net) {
            this->status = NET_HANDSHAKE;
        }
        cel_wait(250);

        /* Step 4: Steady-State Polling */
        while (1) {
            cel_wait(100);
            cel_mutate(net) {
                this->packetsReceived++;
            }
        }
    }
}
```

---

## 5. Task Control Operators & Queries

CELS provides a rich suite of control operators and query macros:

### Inside the Task Body
- **`cel_wait(ms)`**: Suspends the fiber until monotonic time advances by `ms` milliseconds. Other composables and frame updates continue running smoothly at 60+ FPS.
- **`cel_yield()`**: Suspends the fiber until the very next engine frame tick.
- **`cel_cancel()`**: Aborts execution from within the task and jumps directly into the `cancel { ... }` block.
- **`cel_wait_for(Type, outPtr)`**: Suspends fiber execution until a child UI event of type `Type` bubbles up to this session. Automatically extracts payload into `*outPtr`.
- **`cel_wait_signal(Type, outPtr)`**: Suspends fiber execution until an external or inter-session signal of type `Type` arrives on this session.
- **`cel_wait_broadcast(Type, outPtr)`**: Suspends fiber execution until a global engine broadcast of type `Type` is published.
- **`cel_wait_for_timeout(Type, outPtr, timeoutMs)`**: Suspends fiber execution until an event of `Type` arrives or `timeoutMs` expires. Returns `true` if event received, `false` on timeout.

> [!TIP]
> For a full walkthrough and architecture deep-dive of the three messaging channels and fiber waiting, see [08. Events, Signals & Broadcasts](08-events-signals-broadcasts.md).


### Outside the Task (UI & Callbacks)
| Query / Command | Description | Example Use Case |
| :--- | :--- | :--- |
| `cel_is_task_running(TaskName)` | Returns `true` if active | Display loading spinners, disable submit button |
| `cel_is_task_done(TaskName)` | Returns `true` if `run` completed | Show success checkmark, advance to next screen |
| `cel_is_task_cancelled(TaskName)` | Returns `true` if aborted | Display retry banner |
| `cel_cancel_task(TaskName)` | Externally cancels the task | "Cancel Download" button callback |
| `cel_restart_task(TaskName)` | Resets step counter to 0 | "Retry Connection" button callback |

```c
CEL_Composable(SubmitButton) {
    if (cel_is_task_running(DownloadTask)) {
        printf("  [Button] Loading... (Disabled)\n");
    } else {
        printf("  [Button] Click to Download\n");
    }
}
```

---

## 6. Best Practices & Pitfalls

### Do This
- **Always Include a `cancel { ... }` Block**: Any resource opened or allocated during `run` (file handles, network sockets, temporary buffers) must have its teardown in `cancel`.
- **Use `cel_wait(ms)` for Latency**: Simulate or wait for timeouts using `cel_wait(ms)`. This yields CPU time to other composables and the host loop.
- **Mutate State to Reflect Progress**: Keep tasks focused on logic. Use `cel_mutate` to update reactive state and let declarative composables render the HUD.

### Don't Do That
- ❌ **Never Call Blocking OS Sleeps**:
  ```c
  /* CATASTROPHIC: Freezes the entire engine, host loop, and all rendering! */
  run {
      sleep(1); 
  }
  ```
  *Always use `cel_wait(1000)` instead.*
- ❌ **Don't Use Tasks for Continuous Visual Animations**: Tasks are for chronological, multi-step procedures. For smooth interpolation of health bars or positions, use `cel_transition`.

---

## Next Steps

Now that you can run asynchronous procedural flows:
- Proceed to [06. Transitions & Declarative Motion](06-transitions-and-motion.md) for continuous visual smoothing, retargeting mid-flight, and easing curves.
