# slang (SL)

A small language that transpiles to C via `slc`.

Build with `./build.sh` (you'll need `clang` in `PATH`)

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
```

## Example

`hello.sl`:

```sl
fn twice(x i32) i32 {
    return x * 2
}

fn main() i32 {
    var s str = "hello"
    assert len(s) > 0
    return twice(21)
}
```

Typecheck and generate C header:

```sh
_build/macos-aarch64-debug/slc checkpkg hello.sl
_build/macos-aarch64-debug/slc genpkg:c hello.sl hello.h
```

For more programs, see `examples/`.
