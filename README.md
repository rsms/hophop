# HopHop

<img src="etc/hophop.svg" width="116" height="100">

HopHop is a small programming language and reference compiler.

The language is in the C/Go family: plain functions, structs, unions, enums, packages, imports, explicit pointer/reference types, slices, optionals, and compile-time constants. The implementation is still changing, but there's a compiler, library, and test coverage to run real examples through an evaluator, C code generation, native compilation, and wasm.

The compiler is written in strict C11.

`hop` is a command-line program which loads files, resolves packages, formats source, checks programs, emits code, compiles native executables, and runs programs.

See [`docs/language.md`](docs/language.md) for the language specification

## Building hop

You need `clang`, `ninja` (and `python3` for tests) in `PATH`.

```sh
./build.sh
```

- Products are written to `_build/<sys>-<arch>-<mode>/`, i.e. `_build/macos-aarch64-debug/hop --help`
- Run the test suite with `./build.sh test`
- Build without the C backend `./build.sh c_backend=0`


## Example

```hop
fn twice(x i32) i32 {
    return x * 2
}

fn main() {
    x := twice(21)
    assert x == 42
    print("hello world")
}
```

Run with the evaluator:

```sh
$ _build/macos-aarch64-debug/hop run examples/hello.hop
hello world
```

Check (parse, resolve & typecheck) a single file:

```sh
$ _build/macos-aarch64-debug/hop check examples/basic.hop
```

Check a package:

```sh
$ _build/macos-aarch64-debug/hop check examples/packages/app
$ _build/macos-aarch64-debug/hop check examples/hello.hop
```

Transpile to C:

```sh
$ _build/macos-aarch64-debug/hop genpkg:c examples/hello.hop hello.h
```

Compile and run through the C11 backend: (will use a C compiler in PATH)

```sh
$ _build/macos-aarch64-debug/hop compile examples/hello.hop -o hello
./hello
```

See [`examples/`](examples) for more


## `hop` executable

See `hop --help` for usage.

Platform targets:

- `cli-libc`: native CLI program using the C backend and a small libc platform.
- `cli-eval`: evaluator runtime used by default for `hop run`.
- `wasm-min`: small Wasm host ABI used by tests and smoke runs.
- `playbit`: Wasm target for Playbit.

`genpkg` options:

- `genpkg:c` emits C11 text.
- `genpkg:wasm` emits a Wasm binary module.
- `genpkg` chooses a backend from the selected platform.

Examples:

```sh
_build/macos-aarch64-debug/hop mir examples/hello.hop
_build/macos-aarch64-debug/hop genpkg:wasm --platform wasm-min examples/hello.hop hello.wasm
_build/macos-aarch64-debug/hop run --platform wasm-min examples/hello.hop
```
