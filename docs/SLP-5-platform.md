# SLP-5 platform

## Summary

SLP-5 defines the mechanism by which SL programs interact with the host environment: memory
allocation, I/O, panics, and process/thread entry points. The design follows a
syscall-style single-function ABI (`sl_platform_call`) that lets platforms be swapped at
link time without changing the generated SL code.

The first concrete platform is **`cli-libc`**, which targets POSIX-like systems with a C
standard library available. Future platforms might target embedded bare-metal, WASM runtimes,
or custom operating-system kernels.

---

## Motivation

Currently the SL runtime uses libc-like facilities directly (via `SL_TRAP`, the C assert
macros, and C-level `main`). This hard-codes a dependency on the host libc and makes
targeting non-POSIX environments impossible without modifying the generated headers.

SLP-5 introduces a thin abstraction layer:

- All host-service calls go through a single **platform call gate** (`sl_platform_call`).
- The platform **owns the process entry point** (`main`/`_start`); the SL program exports
  `sl_main()` instead.
- Different platform implementations can be linked at compile time without any change to
  the generated C headers or SL source files.

---

## Design overview

```
SL source
  → [slc codegen] → C header (pkg.h)
                     ↓
              [cli wrapper] → defines sl_main() { return pkg__main(); }
                     ↓
              [platform.c]  → defines main(), sl_platform_call()
                     ↓
              [cc link]     → executable
```

The generated C header contains:
- All type and function definitions for the package.
- An `extern` declaration of `sl_platform_call` (used by `SL_ASSERT_FAIL`, `sl_unwrap`, etc.).
- Platform opcode constants (`SLPlatformOp_*`).

The platform implementation is a separate C file that provides:
- `int main(void)` (calls `sl_main()`).
- `int64_t sl_platform_call(uint64_t op, ...)`.

---

## Language-level: `import "platform"`

SL source code that needs platform services uses `import "platform"`. This is an abstract
import resolved by the compiler/linker at build time — it does **not** correspond to a
directory on disk.

```sl
import "platform"

fn panic_example(msg str) {
    platform.panic(msg, 0)
}
```

The `platform` package exposes a thin SL API that delegates to `sl_platform_call`. Core
library packages (`slang/core/gpa`, etc.) import `platform` rather than reaching for
libc directly. Application code rarely imports `platform` directly; instead it uses
higher-level libraries.

> **Note:** `import "platform"` is recognised as a special compiler directive (similar to
> `import "slang/feature/..."`) and is skipped during package resolution on disk.

---

## C ABI

### Opcode enum

```c
enum {
    SLPlatformOp_NONE        = 0,
    SLPlatformOp_PANIC       = 1,  /* fn(msg_ptr u64, msg_len u64, flags u64) — no return */
    SLPlatformOp_CONSOLE_LOG = 2,  /* fn(msg_ptr u64, msg_len u64, flags u64) */
    SLPlatformOp_MEM_ALLOC   = 3,  /* fn(size u64, align u64, flags u64) → addr u64 */
    SLPlatformOp_MEM_RESIZE  = 4,  /* fn(addr u64, oldSize u64, newSize u64, align u64, flags u64) → addr u64 */
    SLPlatformOp_MEM_FREE    = 5,  /* fn(addr u64, size u64, flags u64) */
};
```

### Gate function

```c
int64_t sl_platform_call(uint64_t op,
                         uint64_t a, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f, uint64_t g);
```

Arguments beyond those required by a given opcode must be passed as `0`. Unused opcodes
return `-1`.

### Entry points

```c
/* Implemented by the compiled SL program; called by the platform's main/_start. */
int sl_main(void);

/* Optional: implemented by the SL program for spawned threads. */
void sl_thread(void* stack, uint32_t stackSize, uint64_t arg1, uint64_t arg2);
```

The platform provides `main` (or `_start` for freestanding environments) and calls
`sl_main()`. The SL compiler generates `sl_main()` as the entry wrapper for the package's
`main` function.

---

## Opcode semantics

### `SLPlatformOp_PANIC` (1)

| Arg | Meaning |
|-----|---------|
| `a` | `uint64_t` cast of a `const char*` pointing to the message (null-terminated C string) |
| `b` | Message byte length, or `0` to use `strlen` |
| `c` | Flags (reserved, pass `0`) |

The platform **must not return** from this call. If it does, the caller falls through to
`SL_TRAP()` as a safety net.

Used by: `sl_unwrap`, `SL_ASSERT_FAIL`, `SL_ASSERTF_FAIL`.

### `SLPlatformOp_CONSOLE_LOG` (2)

| Arg | Meaning |
|-----|---------|
| `a` | `uint64_t` cast of a `const char*` message |
| `b` | Message byte length, or `0` to use `strlen` |
| `c` | Flags: `0` = stdout, `1` = stderr |

Returns `0` on success, `-1` on failure.

### `SLPlatformOp_MEM_ALLOC` (3)

| Arg | Meaning |
|-----|---------|
| `a` | Size in bytes |
| `b` | Alignment in bytes (must be power-of-two ≥ 1) |
| `c` | Flags (reserved, pass `0`) |

Returns the allocated address cast to `uint64_t`, or `0` on failure.

### `SLPlatformOp_MEM_RESIZE` (4)

| Arg | Meaning |
|-----|---------|
| `a` | Existing allocation address |
| `b` | Old size in bytes |
| `c` | New size in bytes |
| `d` | Alignment in bytes |
| `e` | Flags (reserved, pass `0`) |

Returns the new address (may differ from `a`), or `0` on failure.

### `SLPlatformOp_MEM_FREE` (5)

| Arg | Meaning |
|-----|---------|
| `a` | Allocation address |
| `b` | Size in bytes |
| `c` | Flags (reserved, pass `0`) |

Returns `0`.

---

## Generated C header integration

The preamble emitted by `genpkg:c` / `compile` includes the opcode enum and a weak
`extern` declaration of `sl_platform_call`. This means any translation unit that includes
the generated header can call platform operations; the linker resolves the definition from
whichever platform `.c` is compiled alongside.

```c
/* emitted in every generated header */
#ifndef SLPlatformOp_NONE
  enum {
    SLPlatformOp_NONE        = 0,
    SLPlatformOp_PANIC       = 1,
    SLPlatformOp_CONSOLE_LOG = 2,
    SLPlatformOp_MEM_ALLOC   = 3,
    SLPlatformOp_MEM_RESIZE  = 4,
    SLPlatformOp_MEM_FREE    = 5
  };
#endif
extern __sl_i64 sl_platform_call(__sl_u64 op, __sl_u64 a, __sl_u64 b, __sl_u64 c,
                                  __sl_u64 d, __sl_u64 e, __sl_u64 f, __sl_u64 g);

#define SL_ASSERT_FAIL(file,line,msg) \
    do { sl_platform_call(SLPlatformOp_PANIC,(uint64_t)(uintptr_t)(msg),0,0,0,0,0,0); \
         SL_TRAP(); } while(0)

#define sl_unwrap(p) \
    ((p) != (void*)0 ? (p) : \
     ((void)sl_platform_call(SLPlatformOp_PANIC, \
          (uint64_t)(uintptr_t)"unwrap: null value",18,0,0,0,0,0), SL_TRAP(), (p)))
```

---

## `cli-libc` platform

The first platform ships alongside `slc` as an embedded C source fragment (written to a
temp file at compile time). It implements `sl_platform_call` using standard libc functions
and provides `main`.

```c
/* platforms/cli-libc/platform.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int sl_main(void);

int main(void) { return sl_main(); }

int64_t sl_platform_call(uint64_t op,
    uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    (void)f; (void)g;
    switch (op) {
    case 1: { /* PANIC */
        size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
        fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
        fflush(stderr);
        abort();
    }
    case 2: { /* CONSOLE_LOG */
        size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
        FILE* out = (c & 1) ? stderr : stdout;
        fprintf(out, "%.*s\n", (int)n, (const char*)(uintptr_t)a);
        fflush(out);
        return 0;
    }
    case 3: /* MEM_ALLOC */
        return (int64_t)(uintptr_t)(a ? malloc((size_t)a) : (void*)0);
    case 4: /* MEM_RESIZE */
        (void)b;
        return (int64_t)(uintptr_t)(c ? realloc((void*)(uintptr_t)a, (size_t)c) : (void*)0);
    case 5: /* MEM_FREE */
        (void)b; (void)c;
        free((void*)(uintptr_t)a);
        return 0;
    default:
        return -1;
    }
}
```

---

## `slang/core/gpa`

The general-purpose allocator package (`slang/core/gpa`) wraps `SLPlatformOp_MEM_ALLOC`,
`SLPlatformOp_MEM_RESIZE`, and `SLPlatformOp_MEM_FREE` to provide a `MemAllocator`-compatible
interface. Application code that needs dynamic allocation imports `slang/core/gpa`:

```sl
import "slang/core/gpa"

fn example(n uint) {
    var ma mut&MemAllocator = gpa.allocator()
    var buf *[u8 n] = new(ma, u8, n)
    // ...
}
```

The `gpa` package is implemented as a thin SL wrapper that calls `platform.alloc` /
`platform.resize` / `platform.free`.

> **Deferred:** `slang/core/gpa` implementation is a follow-up to this SLP. For now the
> `MemAllocator` type and `new` builtin remain as they are.

---

## `slc` CLI changes

`slc compile` and `slc run` now:

1. Generate the SL package as a C header (unchanged).
2. Write a C wrapper that defines `sl_main()` calling the package entry point (was `main()`).
3. Write the embedded `cli-libc` platform source to a second temp file.
4. Invoke `cc` to compile both files together into the executable.

No `--platform` flag is required for now; `cli-libc` is the only built-in platform and is
used automatically. Future work will add `--platform=<name>` to select among registered
platforms.

---

## Entry point convention

The package `main` function is found by looking for a top-level function named `main` in
the entry package (current behaviour). Codegen wraps it:

```c
/* generated wrapper (compiled alongside the package header) */
int sl_main(void) { return (int)pkgname__main(); }
```

The platform's `main` calls `sl_main()`:

```c
/* inside cli-libc/platform.c */
extern int sl_main(void);
int main(void) { return sl_main(); }
```

---

## Platform selection propagation

At link time all translation units must agree on the platform. The platform is currently
selected by which platform file is compiled alongside the generated code — no linker symbol
table magic or per-translation-unit flags are used.

Future SLPs may introduce a `platform.toml` manifest or a `--platform=<name>` CLI flag
backed by a platform registry directory.

---

## Open questions

1. Should `sl_thread` be mandatory or optional (weak symbol)?
2. Should `SLPlatformOp_CONSOLE_LOG` pass an `sl_strhdr*` instead of a raw C string?
3. Should `slang/core/gpa` be auto-imported for packages that use `new` without an explicit
   `MemAllocator`?
4. Interaction with WASM: `sl_platform_call` maps naturally to a host import table; what
   is the right WASM calling convention for the opcode args?
