# hophop (HopHop)

A small language with a C11 backend via `hop`.

Build with `./build.sh` (you'll need `clang` in `PATH`).
For an evaluator-only binary without the C backend, use `./build.sh c_backend=0`.

Documentation home: `docs/index.md`

## `hop` at a glance

After `./build.sh`, use `_build/macos-aarch64-debug/hop`.
Run `_build/macos-aarch64-debug/hop --help` (or `_build/macos-aarch64-debug/hop`) for the full command reference.

- Lexing, AST dump, and single-file checking.
- Package-level checking (`checkpkg`) with import loading.
- Package code generation (`genpkg[:backend]`), including C header generation.
- Formatting (`fmt`) with optional check mode.
- Native compilation (`compile`) through the C11 backend.
- Compile-and-run (`run`) for quick iteration.
- `--platform` and `--cache-dir` options for target/runtime selection and build cache control.

In `c_backend=0` builds, `hop` still supports parse/check/fmt plus `run --platform cli-eval`,
but rejects C-backend commands such as `compile` and `genpkg`.

## Example

`hello.hop`:

```hop
import "platform"

fn twice(x i32) i32 {
    return x * 2
}

fn main() {
    var s = "hello"
    assert len(s) > 0
    assert twice(21) == 42
    platform.exit(0)
}
```

Typecheck and generate C header:

```sh
_build/macos-aarch64-debug/hop checkpkg hello.hop
_build/macos-aarch64-debug/hop genpkg:c hello.hop hello.h
```

Compile and run:

```sh
_build/macos-aarch64-debug/hop compile hello.hop -o hello
./hello

_build/macos-aarch64-debug/hop run hello.hop
```

For more programs, see `examples/`.
