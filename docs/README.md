# CELS Documentation Roadmap

Welcome to the official documentation for **CELS** (Composition, Evaluation, Lifecycle, and State) — a high-performance reactive composition engine for ANSI C99.

This documentation is organized into three distinct tiers based on your needs:

```
                              ┌───────────────────────────┐
                              │     ROOT README.md        │
                              │    (The Landing Page)     │
                              └─────────────┬─────────────┘
                                            │
               ┌────────────────────────────┼────────────────────────────┐
               ▼                            ▼                            ▼
     ┌───────────────────┐        ┌───────────────────┐        ┌───────────────────┐
     │   docs/guides/    │        │ docs/architecture/│        │  docs/reference/  │
     │  "How do I build  │        │ "How does this    │        │  "What is the     │
     │   applications?"  │        │  actually work?"  │        │   exact API?"     │
     └───────────────────┘        └───────────────────┘        └───────────────────┘
```

---

## 1. Developer Guides (`docs/guides/`)

Practical, snippet-heavy, task-oriented guides designed to take you from a blank file to building complex reactive applications.

| Guide | Description | Key Concepts |
| :--- | :--- | :--- |
| **[`01-getting-started.md`](guides/01-getting-started.md)** | Build your first CELS application in under 5 minutes. | CMake `cels_add_application`, minimal host loop, `CEL_App`. |
| **[`02-compositions-and-tree.md`](guides/02-compositions-and-tree.md)** | Structure hierarchical component trees and handle dynamic branches. | Compositions vs. Composables, passing props, `if/else` control flow, multi-file translation units. |
| **[`03-reactive-state.md`](guides/03-reactive-state.md)** | Master reactive state management and State Hoisting. | `cel_state`, `cel_watch(ptr)`, `cel_mutate(ptr)`, zero string IDs, double-buffering mental model, `cel_remember`. |
| **[`04-lifecycles.md`](guides/04-lifecycles.md)** | Automate native hardware and OS resource management. | `CEL_Lifecycle`, `mount` and `unmount` blocks, GPU buffers, audio voices, reverse-order teardown. |
| **[`05-tasks-and-coroutines.md`](guides/05-tasks-and-coroutines.md)** | Write non-blocking multi-step procedural workflows. | `CEL_Task`, `cel_wait(ms)`, `cel_yield()`, guaranteed `cancel { ... }` unmount teardown. |
| **[`06-transitions-and-motion.md`](guides/06-transitions-and-motion.md)** | Implement smooth visual motion and continuous interpolation. | `cel_transition()`, in-flight retargeting, 0% CPU idle, built-in easing curves (`CEL_EASE_OUT_QUAD`). |
| **[`07-hot-reloading.md`](guides/07-hot-reloading.md)** | Set up live DLL code reloading without losing runtime state. | Sub-50ms hot swaps, host runtime checks, shadow-copying, state retention. |
| **[`08-events-signals-broadcasts.md`](guides/08-events-signals-broadcasts.md)** | Master discrete events, session signals, and engine-wide broadcasts. | `cel_event` / `cel_listen` tree bubbling, `cel_signal` / `cel_connect` cross-session delivery, `cel_broadcast` / `cel_bind` thread-safe bus, non-blocking task awaiting (`cel_wait_for`, `cel_wait_for_timeout`). |

---

## 2. Systems Architecture (`docs/architecture/`)

Deep-dive technical specifications explaining the low-level engine mechanics, memory geometries, cache alignment, and algorithmic complexity.

| Architecture Deep-Dive | Description | Engineering Highlights |
| :--- | :--- | :--- |
| **[`slot-table-architecture.md`](architecture/slot-table-architecture.md)** | Contiguous memory slab partitioning and dual gap buffer mechanics. | 64-byte L1 cache alignment, zero-malloc guarantee, `CelsSlotGroup` 32-byte packing, address-stable slots. |
| **[`recomposition-reconciler.md`](architecture/recomposition-reconciler.md)** | Invalidation queues and $\mathcal{O}(1)$ subtree bypass. | Dirty bit propagation, logical cursor jumping, in-place sibling reordering via `memmove`, dead branch pruning. |
| **[`double-buffering-model.md`](architecture/double-buffering-model.md)** | Lock-free reads and double-buffered transaction pipelines. | Front vs back buffer staging, atomic frame boundary publish, `CelsStateHeader` pointer metadata extraction. |
| **[`fiber-coroutine-mechanics.md`](architecture/fiber-coroutine-mechanics.md)** | Stack-preserving cooperative fiber coroutines. | Platform fiber ABIs (Win32 Fibers & POSIX `ucontext_t`), non-blocking monotonic deadlines, unmount cancellation. |
| **[`dynamic-hot-reload-engine.md`](architecture/dynamic-hot-reload-engine.md)** | Live code swapping without process restarts. | Windows DLL lock evasion via shadow-copying, PE image validation, TLS session sync, slot schema evolution on struct resize. |

---

## 3. Reference & Standards (`docs/reference/`)

Exhaustive lookups, API cheat sheets, mathematical specifications, and coding standards.

| Reference Document | Description | Highlights |
| :--- | :--- | :--- |
| **[`api-cheat-sheet.md`](reference/api-cheat-sheet.md)** | Single-page lookup of all CELS macros, functions, and scopes. | Categorized by tier: Tier 1 (`CEL_`), Tier 2 (`cel_`), Tier 3 (`cels_` / `Cels`). Includes allowed calling scopes and signatures. |
| **[`coding-standards-c99.md`](reference/coding-standards-c99.md)** | Strict ANSI C99 coding standards and portability conventions. | Macro comma safety, compound literal rules `((Type){...})`, struct alignment, naming conventions. |
| **[`motion-spec.md`](reference/motion-spec.md)** | Mathematical foundations of temporal motion and physics. | Algebraic formulas for all 12 easing curves, boundary velocity derivatives, analytical solutions for damped harmonic springs ($m\ddot{x} + c\dot{x} + kx = 0$), cubic Bézier root finding. |

---

## Suggested Reading Paths

### 🚀 "I want to build an application or game UI"
1. Read **[`guides/01-getting-started.md`](guides/01-getting-started.md)** to configure CMake and build the host.
2. Read **[`guides/02-compositions-and-tree.md`](guides/02-compositions-and-tree.md)** to understand how to structure your UI components.
3. Read **[`guides/03-reactive-state.md`](guides/03-reactive-state.md)** to hoist state and make your UI reactive.
4. Keep the **[`reference/api-cheat-sheet.md`](reference/api-cheat-sheet.md)** open as a quick lookup.

### ⚙️ "I want to understand the engine internals and performance guarantees"
1. Read **[`architecture/slot-table-architecture.md`](architecture/slot-table-architecture.md)** for memory slab layout and gap buffers.
2. Read **[`architecture/recomposition-reconciler.md`](architecture/recomposition-reconciler.md)** for the $\mathcal{O}(1)$ subtree skipping reconciler.
3. Read **[`architecture/double-buffering-model.md`](architecture/double-buffering-model.md)** for thread safety and atomic frame publication.
4. Read **[`architecture/dynamic-hot-reload-engine.md`](architecture/dynamic-hot-reload-engine.md)** for hot reloading and schema evolution.

### ⏱️ "I need smooth animations, timers, or network flows"
1. Read **[`guides/06-transitions-and-motion.md`](guides/06-transitions-and-motion.md)** for `cel_transition` and easing.
2. Read **[`guides/05-tasks-and-coroutines.md`](guides/05-tasks-and-coroutines.md)** for `CEL_Task` and async coroutines.
3. Consult **[`reference/motion-spec.md`](reference/motion-spec.md)** for the underlying mathematical models.
