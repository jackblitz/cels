# C99 Code Standards

Examples use a small **image loading library** as the running example — `Img` /
`IMG_` as the module prefix. Substitute your own prefix per library.

Each rule carries a short **why**, so the document can be argued with rather
than just obeyed.

This file covers **conventions** — rules with one right answer that a reviewer
can check without knowing the context. Performance decisions, which depend
entirely on whether the code is hot, live in the companion
`performance-guidelines.md`. Where the two touch, this file wins on style and
that one explains the cost.

---

## 1. Language and toolchain

- Target **C99** (`-std=c99`). No compiler extensions in headers; extensions in
  `.c` files must be guarded and documented.
- Build with `-Wall -Wextra -Wpedantic`. Warnings are errors in CI.
- Use fixed-width types from `<stdint.h>` (`uint32_t`, `int64_t`) whenever the
  width matters. Use `size_t` for sizes and counts, `ptrdiff_t` for pointer
  differences.
- Use `bool` from `<stdbool.h>`.

---

## 2. Naming

| Kind | Convention | Example |
|---|---|---|
| Public function | `PascalCase`, prefixed | `ImgImageCreate` |
| Public type | `PascalCase`, prefixed | `ImgImage`, `ImgLoadOptions` |
| Enum constant | `SCREAMING_SNAKE`, prefixed | `IMG_FORMAT_RGBA8` |
| Macro | `SCREAMING_SNAKE`, prefixed | `IMG_ARRAY_COUNT` |
| Local variable | `camelCase` | `pixelCount` |
| Parameter | `camelCase` | `filePath` |
| Out-parameter | `camelCase`, `out` prefix | `outImage` |
| Internal (static) function | `PascalCase`, no prefix | `FileReadAll` |
| Struct field | `camelCase` | `options.pixelFormat` |

Function names read **noun-first, verb-last**: `ImgImageResize`, not
`ImgResizeImage`. This groups the whole API for a type together alphabetically
and in autocomplete.

> **Why the mixed casing:** `PascalCase` for the API and `camelCase` for
> everything local means a call is visually distinct from a variable at a
> glance, without needing syntax highlighting.

**Out-parameters always carry the `out` prefix.** The type `ImgImage **` tells
you it's a pointer to a pointer; it doesn't tell you the function *writes*
there. The name does.

---

## 3. Types and structs

**Always `typedef`, with the tag name matching the typedef name.**

```c
typedef struct ImgSize {
    uint32_t width;
    uint32_t height;
} ImgSize;
```

Never write `struct ImgSize` in declarations once the typedef exists. Type
names are single tokens.

**Two categories of type, and the distinction is deliberate:**

*Plain data* — no invariants, no owning allocation. Full definition in the
header, passed and returned by value, caller-owned.

```c
typedef struct ImgLoadOptions {
    ImgFormat pixelFormat;
    uint32_t flags;
    const char *label;
} ImgLoadOptions;
```

*Opaque handles* — anything with invariants, internal state, or an owning
allocation. Forward-declared in the header, defined only in the `.c` file.

```c
// include/img/image.h
typedef struct ImgImage ImgImage;

// src/image.c
struct ImgImage {
    const ImgAllocator *allocator;
    ImgSize size;
    ImgFormat pixelFormat;
    uint8_t *pixels;
};
```

> **Why:** callers can't depend on layout they shouldn't know about, and the
> struct can change without an ABI break.

### Handle granularity

The invariant test above decides *whether* a type is a handle. This rule
decides *at what granularity*, and the two are easy to confuse.

Opaque handles are for objects you have tens or hundreds of — an image, a
device, a decoder. They are heap-allocated and reached through a pointer, so
every access is a potential cache miss.

Things you have thousands or millions of are **never** handles. They are plain
data in a packed array owned by a handle:

```c
struct ImgImage {
    const ImgAllocator *allocator;
    ImgSize size;
    ImgFormat pixelFormat;
    uint8_t *pixels;  // one allocation for all pixels, not one per pixel
};
```

A handle per pixel, per particle or per entity passes the invariant test and
fails this one. See `performance-guidelines.md` for what that costs.

### Field ordering and width

**Order fields largest to smallest.** Padding is inserted to satisfy each
field's alignment; descending size minimises it.

```c
typedef struct ImgTile {  // 16 bytes
    uint64_t hash;
    uint32_t offset;
    uint16_t width;
    uint8_t level;
    uint8_t flags;
} ImgTile;

typedef struct ImgTile {  // 32 bytes — identical fields, 16 wasted
    uint8_t level;
    uint64_t hash;
    uint16_t width;
    uint32_t offset;
    uint8_t flags;
} ImgTile;
```

**Enum constants, integer-width fields.** C99 gives no control over an enum
type's width — `enum : uint8_t` is C23 — so a field declared
`ImgFormat pixelFormat;` is int-sized. When packing matters, declare the field
as the smallest type that holds the range and name the enum it carries:

```c
uint8_t pixelFormat;  // ImgFormat
```

Use the enum type itself in signatures, parameters and locals, where the width
costs nothing and the type checking is worth having.

**Identifiers are integers, not strings.** Internal identity uses `uint32_t`
handles or hashes. A `const char *` identifier costs a pointer chase and a
`strcmp` per comparison, and the cost scales with the collection. Strings are
for text a user reads and for debug labels — `ImgLoadOptions.label` is the
latter, and nothing branches on it.

---

## 4. Error handling

Fallible functions **return an `ImgResult` code**. The produced value goes in
an out-parameter.

```c
typedef enum ImgResult {
    IMG_OK = 0,
    IMG_ERROR_INVALID_ARGUMENT,
    IMG_ERROR_OUT_OF_MEMORY,
    IMG_ERROR_NOT_FOUND,
    IMG_ERROR_UNSUPPORTED_FORMAT,
    IMG_ERROR_IO,
} ImgResult;

ImgResult ImgImageCreate(const ImgLoadOptions *options,
                         const ImgAllocator *allocator,
                         ImgImage **outImage);
```

Rules:

- `IMG_OK` is **always** zero. Never test success with `if (result)` — write
  `if (result != IMG_OK)`. Explicit beats clever.
- Every error check is written out as a visible `if`. **No `TRY`/`?` macro.**
  Control flow that can jump out of a function must be visible where it
  happens.
- A function that cannot fail returns `void` or its value directly. Don't
  return `ImgResult` "just in case" — it forces dead checks on every caller.
- On failure, **out-parameters are not written**. Callers may leave them
  uninitialised only if they check the result.
- Infallible accessors return the value directly:
  `ImgSize ImgImageSize(const ImgImage *image);`

**Never ignore a result silently.** If a failure is genuinely acceptable, say
so:

```c
(void)FileClose(file);  // best-effort: already unwinding
```

### Diagnostics

Every module provides a string mapper for its result codes:

```c
/**
 * Returns a short human-readable description of a result code.
 *
 * @param result Any ImgResult value, including unrecognised ones.
 * @return A static, never-NULL string owned by the library. Do not free it.
 */
const char *ImgResultToString(ImgResult result);
```

No allocation, no ownership question, no context object to thread through the
API. The trade-off is accepted deliberately: the code says *unsupported
format*, not *which* format — when a caller needs that detail, it belongs in a
dedicated query function, not in an error string.

---

## 5. Control flow and cleanup

**Hybrid model.** Plain early return while the function holds no resources;
single-exit `goto cleanup` from the first acquisition onward.

```c
ImgResult ImgImageLoadFromFile(const char *filePath,
                               const ImgAllocator *allocator,
                               ImgImage **outImage)
{
    IMG_ASSERT(filePath != NULL);
    IMG_ASSERT(allocator != NULL);
    IMG_ASSERT(outImage != NULL);

    // Everything released in cleanup is declared and NULL-initialised
    // before the first goto — see the carve-out in section 6.
    ImgFile *file = NULL;
    uint8_t *buffer = NULL;
    ImgImage *image = NULL;
    size_t size = 0;

    ImgResult result = FileOpen(filePath, &file);
    if (result != IMG_OK) goto cleanup;

    result = FileReadAll(file, allocator, &buffer, &size);
    if (result != IMG_OK) goto cleanup;

    result = ImgImageDecode(buffer, size, allocator, &image);
    if (result != IMG_OK) goto cleanup;

    *outImage = image;
    image = NULL;  // ownership transferred to caller

cleanup:
    ImgImageDestroy(image);
    ImgFree(allocator, buffer);
    FileClose(file);
    return result;
}
```

Supporting rules:

- **Destructors accept `NULL` and do nothing.** This is what makes the single
  cleanup block work without a cascade of `if (x != NULL)`.
- Release in **reverse order of acquisition**.
- The label is always named `cleanup`. If a function needs two cleanup tiers,
  it's doing too much — split it.
- `goto` is permitted **only** for forward jumps to a cleanup label. Never
  backwards, never into a block.
- Set a transferred pointer to `NULL` immediately after handing off ownership,
  so the cleanup block is unconditionally correct.
- Braces on every `if`/`for`/`while` body, except single-line `goto cleanup`
  guards as shown above.

### Assertions vs error codes

The two exist for different failures and must not be conflated:

```c
IMG_ASSERT(data != NULL);            // contract violation: a caller bug.
                                     // Compiled out in release builds.

if (size < IMG_HEADER_SIZE) {        // bad input data: a legitimate runtime
    return IMG_ERROR_UNSUPPORTED_FORMAT;   // outcome, always checked.
}
```

- **`IMG_ASSERT` for programmer errors** — NULL where the contract says
  non-NULL, an index out of range, a state machine in an impossible state.
  These are bugs. They should be loud in development and cost nothing in
  release.
- **Error codes for anything the caller can't control** — missing files,
  malformed data, allocation failure, unsupported formats.

The test: *could a correct caller ever trigger this?* If yes, it's an error
code. If no, it's an assert.

---

## 6. `const` and immutability

**Locals are `const` unless mutated.** Treat `const` as Kotlin's `val` — the
default, with mutability as the exception you opt into.

```c
const ImgSize size = ImgImageSize(image);
const size_t pixelCount = (size_t)size.width * size.height;

uint8_t *cursor = pixels;  // mutated below, so not const
```

**Declare variables at first use**, initialised in the same statement. This is
what makes the `const` rule workable: a `const` local cannot be declared in one
place and assigned in another.

⚠️ **One carve-out.** Variables released in a `cleanup` block are declared and
`NULL`-initialised *before the first `goto`*, and are not `const`. C99 permits
`goto` to jump over a declaration, but the variable is then indeterminate — so
`cleanup` would free garbage. See `ImgImageLoadFromFile` above.

**Pointed-to data is `const` whenever the function doesn't write it.** This one
is part of the API contract, not just documentation:

```c
ImgResult ImgImageDecode(const uint8_t *data, size_t size,
                         const ImgAllocator *allocator,
                         ImgImage **outImage);
```

Notes and limits:

- Pass read-only structs as `const T *`, not by value, once they exceed roughly
  two machine words. Small value types (`ImgSize`) pass by value.
- `const` on a scalar local is documentation only — C won't stop mutation
  through an aliasing pointer. Rely on it for readability, not for guarantees.
- When a value depends on a branch, prefer a ternary or a small helper function
  over dropping `const`:
  ```c
  const uint32_t flags = premultiply ? IMG_LOAD_PREMULTIPLY : 0u;
  ```

---

## 7. Memory and ownership

**Every allocating API takes an explicit allocator.**

```c
typedef struct ImgAllocator {
    void *(*alloc)(void *userData, size_t size, size_t alignment);
    void (*free)(void *userData, void *pointer);
    void *userData;
} ImgAllocator;

ImgResult ImgImageCreate(const ImgLoadOptions *options,
                         const ImgAllocator *allocator,
                         ImgImage **outImage);
void ImgImageDestroy(ImgImage *image);
```

Rules:

- `Create` / `Destroy` are the paired names for heap-allocated handles.
  `Init` / `Shutdown` are reserved for caller-provided storage, if that's ever
  added.
- **A handle stores the allocator it was created with.** `Destroy` takes only
  the handle — callers must never have to remember which allocator built which
  object.
- **Nothing in the library calls `malloc` or `free` directly.** All allocation
  goes through the passed allocator. This is the whole point of the convention;
  one direct `malloc` invalidates arena and tracking allocators.
- Only opaque handles are ever heap-allocated by the library. Plain data types
  are caller-owned, by value, always.
- A function that allocates into an out-parameter documents who frees it and
  with which call.
- `Destroy(NULL)` is a no-op.
- Never free memory the caller supplied.
- **Allocate in bulk.** `ImgAllocator` is a struct of function pointers, so
  every allocation is an indirect call the compiler cannot inline. That is
  irrelevant once per image and unacceptable once per element: allocate one
  array of N, never N objects.

> **What this convention buys you:** because nothing calls `malloc` directly,
> a caller can pass an arena, a pool or a tracking allocator without a single
> API change. `performance-guidelines.md` shows an arena implemented against
> this interface.

---

## 8. Initialization

**Use designated initializers.** They're the C99 feature closest to Kotlin's
named arguments, and unmentioned fields are zero-initialised.

```c
const ImgLoadOptions options = {
    .pixelFormat = IMG_FORMAT_RGBA8,
    .flags = IMG_LOAD_PREMULTIPLY,
    .label = "player-sprite",
};
```

- **Never use positional struct initialization** for anything with more than
  two fields. `{ 3, 2, "player-sprite" }` cannot be reviewed.
- **Compound literals are banned.** Always name the value:
  ```c
  const ImgSize target = { 256, 256 };
  ImgImageResize(source, target, allocator, &thumbnail);   // yes

  ImgImageResize(source, (ImgSize){ 256, 256 },            // no
                 allocator, &thumbnail);
  ```
  The name is a free unit of documentation at the call site, and a compound
  literal's lifetime ends with the enclosing block — a subtle dangling-pointer
  source when one is passed by address.
- Adding a field to a public struct must not break existing initializers — a
  corollary of the above, and a reason to keep the zero value of every field
  meaningful.
- Prefer designing structs so that **all-zero is a valid default state**. Then
  `= {0}` is a legitimate starting point.
- Options and descriptor structs (`ImgLoadOptions`) are the preferred way to
  pass more than three or four arguments to a `Create` function.

---

## 9. Files, headers and layout

**The directory carries the namespace; filenames do not repeat it.**

```
include/img/image.h        #include "img/image.h"
include/img/decode.h       #include "img/decode.h"
src/image.c
src/decode.c
src/file.h                 // internal — private by location, not by suffix
src/file.c
```

- **One module per header**, not one type per header. `image.h` holds
  `ImgImage`, `ImgSize`, `ImgLoadOptions` and everything that operates on them.
- **Internal headers are private by location.** Anything under `src/` is
  internal; no `_internal` or `_priv` suffix. If it's in `include/`, it's
  public API.
- **`#pragma once`** at the top of every header. No include guards.
- Headers include only what they need to compile; forward-declare handles
  rather than including their headers.
- Everything not declared in a public header is `static`.
- Header structure: `#pragma once` → file doc block → includes → forward
  declarations → types → constants → functions.
- Include order, each group alphabetised, blank line between groups:
    1. The matching header (in a `.c` file, first — proves the header is
       self-contained)
    2. C standard library
    3. Third-party
    4. Project headers

> **Trade-off accepted:** with no prefix in the filename, your editor may show
> several tabs named `image.c` across libraries. That's an editor-configuration
> problem; the include lines stay clean.

---

## 10. Comments and documentation

- `//` for comments in code. `/** */` Doxygen blocks for documentation.
- Comment **why**, not what. The code says what it does.
- **Every public and every static function is documented** — no exceptions for
  internal helpers. The doc block covers how the function works, what goes in,
  and what comes out.
- Nullability is documented in prose for every pointer parameter; C99 has no
  annotation for it.

### Every header opens with a usage example

A developer landing on the header should see how the API is actually used
before reading a single signature.

```c
#pragma once

/**
 * @file image.h
 * @brief Loading, decoding and inspecting images.
 *
 * Typical usage:
 * @code
 *     ImgImage *image = NULL;
 *     ImgResult result = ImgImageLoadFromFile("sprite.png", allocator,
 *                                             &image);
 *     if (result != IMG_OK) {
 *         fprintf(stderr, "%s\n", ImgResultToString(result));
 *         return result;
 *     }
 *
 *     const ImgSize size = ImgImageSize(image);
 *     ImgImageDestroy(image);
 * @endcode
 *
 * Every Create/Load function takes an allocator and returns an owning
 * handle; release it with ImgImageDestroy. Plain data types (ImgSize,
 * ImgLoadOptions) are caller-owned and never allocated by this library.
 *
 * Thread safety: ImgImage is not internally synchronised. Concurrent reads
 * of one image are safe; any mutation requires external synchronisation.
 * The allocator must itself be thread-safe if images are created from
 * multiple threads.
 */
```

### Public function

```c
/**
 * Loads and decodes an image from a file.
 *
 * Reads the file in full, then decodes it into a newly allocated image. The
 * file handle is closed and the read buffer freed before returning, on both
 * the success and the failure path.
 *
 * @param filePath  Path to the encoded image. Non-NULL. Not retained.
 * @param allocator Allocator for the read buffer and the image. Non-NULL,
 *                  and must outlive the returned image.
 * @param outImage  On success, receives an owning handle; release it with
 *                  ImgImageDestroy. Untouched on failure.
 * @return IMG_OK, IMG_ERROR_INVALID_ARGUMENT, IMG_ERROR_NOT_FOUND,
 *         IMG_ERROR_IO, IMG_ERROR_UNSUPPORTED_FORMAT or
 *         IMG_ERROR_OUT_OF_MEMORY.
 */
ImgResult ImgImageLoadFromFile(const char *filePath,
                               const ImgAllocator *allocator,
                               ImgImage **outImage);
```

### Static function — same shape, above the definition in the `.c`

```c
/**
 * Reads an entire open file into a newly allocated buffer.
 *
 * Seeks to the end to size the file, then reads in a single pass. The file
 * position is left at end-of-file.
 *
 * @param file      Open file handle. Non-NULL.
 * @param allocator Allocator for the buffer. Non-NULL.
 * @param outBuffer On success, receives the buffer; release it with ImgFree
 *                  and the same allocator. Untouched on failure.
 * @param outSize   On success, receives the byte count.
 * @return IMG_OK, IMG_ERROR_IO or IMG_ERROR_OUT_OF_MEMORY.
 */
static ImgResult FileReadAll(ImgFile *file, const ImgAllocator *allocator,
                             uint8_t **outBuffer, size_t *outSize);
```

---

## 11. Thread safety

**The caller synchronises.** No type in the library takes a lock on its own
behalf.

- Every module documents its guarantees in the header's file doc block, in the
  form shown in section 10.
- The default contract: concurrent *reads* of one handle are safe; any mutation
  requires external synchronisation.
- If a handle is created or destroyed from multiple threads, **the allocator
  must be thread-safe** — the library does not add its own locking around it.
- A function that is not reentrant says so explicitly in its doc block.

> **Why:** internal locking costs every single-threaded path and still can't
> prevent deadlock across two handles. Making the contract explicit puts the
> synchronisation where the caller already knows the access pattern.

---

## 12. Formatting

- **K&R braces**: opening brace on its own line for functions, trailing for
  everything else.
- **80 column limit**, **4 spaces**, never tabs.
- Enforced by `.clang-format` and **checked in CI** — formatting is never a
  review comment.

```yaml
# .clang-format
Language:        Cpp
BasedOnStyle:    LLVM
ColumnLimit:     80
IndentWidth:     4
TabWidth:        4
UseTab:          Never
BreakBeforeBraces: Linux
AlignAfterOpenBracket: Align
PointerAlignment: Right
IndentCaseLabels: false
SpaceAfterCStyleCast: false
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: WithoutElse
SortIncludes:    false
```

Two notes on that config:

- `AllowShortIfStatementsOnASingleLine: WithoutElse` is deliberate — it's what
  preserves the `if (result != IMG_OK) goto cleanup;` guard from section 5.
  Setting it to `Never` would split every guard across two lines and fight the
  cleanup pattern.
- `SortIncludes: false` because clang-format sorts alphabetically within a
  block but can't express the four-group ordering in section 9; the groups are
  maintained by hand, separated by blank lines.

At 80 columns with prefixed `PascalCase` names, most three-parameter signatures
wrap. That's expected — align continuation parameters under the first one, as
every example here does.

---

## Not yet covered

Genuinely undecided, for a later pass:

- Testing conventions — framework, file layout, naming.
- `enum` vs `#define` for integer constants, and when a `static const` is
  preferable to both.
- Versioning and ABI stability rules for public structs.
- Variable shadowing, and whether `-Wshadow` is on.
- Integer conversion and casting policy (`-Wconversion`, explicit cast style).
