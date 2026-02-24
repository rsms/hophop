# slang (SL)

A small language with a C11 backend via `slc`.

Build with `./build.sh` (you'll need `clang` in `PATH`)

Documentation home: `docs/index.md`

## CLI

```sh
# tokenize
_build/macos-aarch64-debug/slc file.sl

# parse + print AST
_build/macos-aarch64-debug/slc ast file.sl

# typecheck single file
_build/macos-aarch64-debug/slc check file.sl

# typecheck package directory or a single file as a package
_build/macos-aarch64-debug/slc checkpkg <package-dir|file.sl>

# generate C header for package directory or single file
_build/macos-aarch64-debug/slc genpkg:c <package-dir|file.sl> [out.h]

# compile program to native executable via cc
_build/macos-aarch64-debug/slc [--cache-dir <dir>] compile <package-dir|file.sl> [-o <output>]

# compile + exec
_build/macos-aarch64-debug/slc [--cache-dir <dir>] run <package-dir|file.sl>
```

## Example

`hello.sl`:

```sl
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
_build/macos-aarch64-debug/slc checkpkg hello.sl
_build/macos-aarch64-debug/slc genpkg:c hello.sl hello.h
```

Compile and run:

```sh
_build/macos-aarch64-debug/slc compile hello.sl -o hello
./hello

_build/macos-aarch64-debug/slc run hello.sl
```

For more programs, see `examples/`.
