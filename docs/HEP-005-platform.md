# HEP-5 platform (completed)

## Summary

HEP-5 defines the mechanism by which HopHop programs interact with the host environment: memory
allocation, I/O, panics, and process/thread entry points. The design follows a
syscall-style single-function ABI (`hop_platform_call`) that lets platforms be swapped at
link time without changing the generated HopHop code.

The first concrete platform is **`cli-libc`**, which targets POSIX-like systems with a C
standard library available. Future platforms might target embedded bare-metal, WASM runtimes,
or custom operating-system kernels.

---

## Motivation

Currently the HopHop runtime uses libc-like facilities directly (via `H2_TRAP`, the C assert
macros, and C-level `main`). This hard-codes a dependency on the host libc and makes
targeting non-POSIX environments impossible without modifying the generated headers.

HEP-5 introduces a thin abstraction layer:

- All host-service calls go through a single **platform call gate** (`hop_platform_call`).
- The platform **owns the process entry point** (`main`/`_start`); the HopHop program exports
  `hop_main()` instead.
- Different platform implementations can be linked at compile time without any change to
  the generated C headers or HopHop source files.

---

## Design overview

```
HopHop source
  → [hop codegen] → C header (pkg.h)
                     ↓
              [cli wrapper] → defines hop_main() { return pkg__main(); }
                     ↓
              [platform.c]  → defines main(), hop_platform_call()
                     ↓
              [cc link]     → executable
```

The generated C header contains:
- All type and function definitions for the package.
- An `extern` declaration of `hop_platform_call` (used by `H2_ASSERT_FAIL`, `hop_unwrap`, etc.).
- Platform opcode constants (`H2PlatformOp_*`).

The platform implementation is a separate C file that provides:
- `int main(void)` (calls `hop_main()`).
- `int64_t hop_platform_call(uint64_t op, ...)`.

---

## Language-level: `import "platform"`

HopHop source code that needs platform services uses `import "platform"`. This is an abstract
import resolved by the compiler/linker at build time — it does **not** correspond to a
directory on disk.

```hop
import "platform"

fn panic_example(msg str) {
    platform.panic(msg, 0)
}
```

The `platform` package exposes a thin HopHop API that delegates to `hop_platform_call`. Core
library packages (`hophop/builtin/gpa`, etc.) import `platform` rather than reaching for
libc directly. Application code rarely imports `platform` directly; instead it uses
higher-level libraries.

> **Note:** `import "platform"` is recognised as a special compiler directive (similar to
> `import "hophop/feature/..."`) and is skipped during package resolution on disk.

---

## C ABI

### Opcode enum

```c
enum {
    H2PlatformOp_NONE        = 0,
    H2PlatformOp_PANIC       = 1,  /* fn(msg_ptr u64, msg_len u64, flags u64) — no return */
    H2PlatformOp_CONSOLE_LOG = 2,  /* fn(msg_ptr u64, msg_len u64, flags u64) */
    H2PlatformOp_MEM_ALLOC   = 3,  /* fn(size u64, align u64, flags u64) → addr u64 */
    H2PlatformOp_MEM_RESIZE  = 4,  /* fn(addr u64, oldSize u64, newSize u64, align u64, flags u64) → addr u64 */
    H2PlatformOp_MEM_FREE    = 5,  /* fn(addr u64, size u64, flags u64) */
};
```

### Gate function

```c
int64_t hop_platform_call(uint64_t op,
                         uint64_t a, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f, uint64_t g);
```

Arguments beyond those required by a given opcode must be passed as `0`. Unused opcodes
return `-1`.

### Entry points

```c
/* Implemented by the compiled HopHop program; called by the platform's main/_start. */
int hop_main(void);

/* Optional: implemented by the HopHop program for spawned threads. */
void hop_thread(void* stack, uint32_t stackSize, uint64_t arg1, uint64_t arg2);
```

The platform provides `main` (or `_start` for freestanding environments) and calls
`hop_main()`. The HopHop compiler generates `hop_main()` as the entry wrapper for the package's
`main` function.

---

## Opcode semantics

### `H2PlatformOp_PANIC` (1)

| Arg | Meaning |
|-----|---------|
| `a` | `uint64_t` cast of a `const char*` pointing to the message (null-terminated C string) |
| `b` | Message byte length, or `0` to use `strlen` |
| `c` | Flags (reserved, pass `0`) |

The platform **must not return** from this call. If it does, the caller falls through to
`H2_TRAP()` as a safety net.

Used by: `hop_unwrap`, `H2_ASSERT_FAIL`, `H2_ASSERTF_FAIL`.

### `H2PlatformOp_CONSOLE_LOG` (2)

| Arg | Meaning |
|-----|---------|
| `a` | `uint64_t` cast of a `const char*` message |
| `b` | Message byte length, or `0` to use `strlen` |
| `c` | Flags: `0` = stdout, `1` = stderr |

Returns `0` on success, `-1` on failure.

### `H2PlatformOp_MEM_ALLOC` (3)

| Arg | Meaning |
|-----|---------|
| `a` | Size in bytes |
| `b` | Alignment in bytes (must be power-of-two ≥ 1) |
| `c` | Flags (reserved, pass `0`) |

Returns the allocated address cast to `uint64_t`, or `0` on failure.

### `H2PlatformOp_MEM_RESIZE` (4)

| Arg | Meaning |
|-----|---------|
| `a` | Existing allocation address |
| `b` | Old size in bytes |
| `c` | New size in bytes |
| `d` | Alignment in bytes |
| `e` | Flags (reserved, pass `0`) |

Returns the new address (may differ from `a`), or `0` on failure.

### `H2PlatformOp_MEM_FREE` (5)

| Arg | Meaning |
|-----|---------|
| `a` | Allocation address |
| `b` | Size in bytes |
| `c` | Flags (reserved, pass `0`) |

Returns `0`.

---

## Generated C header integration

The preamble emitted by `genpkg:c` / `compile` includes the opcode enum and a weak
`extern` declaration of `hop_platform_call`. This means any translation unit that includes
the generated header can call platform operations; the linker resolves the definition from
whichever platform `.c` is compiled alongside.

```c
/* emitted in every generated header */
#ifndef H2PlatformOp_NONE
  enum {
    H2PlatformOp_NONE        = 0,
    H2PlatformOp_PANIC       = 1,
    H2PlatformOp_CONSOLE_LOG = 2,
    H2PlatformOp_MEM_ALLOC   = 3,
    H2PlatformOp_MEM_RESIZE  = 4,
    H2PlatformOp_MEM_FREE    = 5
  };
#endif
extern __hop_i64 hop_platform_call(__hop_u64 op, __hop_u64 a, __hop_u64 b, __hop_u64 c,
                                  __hop_u64 d, __hop_u64 e, __hop_u64 f, __hop_u64 g);

#define H2_ASSERT_FAIL(file,line,msg) \
    do { hop_platform_call(H2PlatformOp_PANIC,(uint64_t)(uintptr_t)(msg),0,0,0,0,0,0); \
         H2_TRAP(); } while(0)

#define hop_unwrap(p) \
    ((p) != (void*)0 ? (p) : \
     ((void)hop_platform_call(H2PlatformOp_PANIC, \
          (uint64_t)(uintptr_t)"unwrap: null value",18,0,0,0,0,0), H2_TRAP(), (p)))
```

---

## `cli-libc` platform

The first platform ships alongside `hop` as an embedded C source fragment (written to a
temp file at compile time). It implements `hop_platform_call` using standard libc functions
and provides `main`.

```c
/* lib/platform_libc.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int hop_main(void);

int main(void) { return hop_main(); }

int64_t hop_platform_call(uint64_t op,
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

## `hophop/builtin/gpa`

The general-purpose allocator package (`hophop/builtin/gpa`) wraps `H2PlatformOp_MEM_ALLOC`,
`H2PlatformOp_MEM_RESIZE`, and `H2PlatformOp_MEM_FREE` to provide a `MemAllocator`-compatible
interface. Application code that needs dynamic allocation imports `hophop/builtin/gpa`:

```hop
import "hophop/builtin/gpa"

fn example(n uint) {
    var ma mut&MemAllocator = gpa.allocator()
    var buf *[u8 n] = new(ma, u8, n)
    // ...
}
```

The `gpa` package is implemented as a thin HopHop wrapper that calls `platform.alloc` /
`platform.resize` / `platform.free`.

> **Deferred:** `hophop/builtin/gpa` implementation is a follow-up to this HEP. For now the
> `MemAllocator` type and `new` builtin remain as they are.

---

## `hop` CLI changes

`hop compile` and `hop run` now:

1. Generate the HopHop package as a C header (unchanged).
2. Write a C wrapper that defines `hop_main()` calling the package entry point (was `main()`).
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
int hop_main(void) { return (int)pkgname__main(); }
```

The platform's `main` calls `hop_main()`:

```c
/* inside lib/platform_libc.c */
extern int hop_main(void);
int main(void) { return hop_main(); }
```

---

## Platform selection propagation

At link time all translation units must agree on the platform. The platform is currently
selected by which platform file is compiled alongside the generated code — no linker symbol
table magic or per-translation-unit flags are used.

Future H2Ps may introduce a `platform.toml` manifest or a `--platform=<name>` CLI flag
backed by a platform registry directory.

---

## Open questions

1. Should `hop_thread` be mandatory or optional (weak symbol)?
2. Should `H2PlatformOp_CONSOLE_LOG` pass an `hop_strhdr*` instead of a raw C string?
3. Should `hophop/builtin/gpa` be auto-imported for packages that use `new` without an explicit
   `MemAllocator`?
4. Interaction with WASM: `hop_platform_call` maps naturally to a host import table; what
   is the right WASM calling convention for the opcode args?
