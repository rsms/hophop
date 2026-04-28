# HEP-36 filename build tags

## Summary

HEP-36 adds filename build tags for directory packages. A tagged file is included only when all of
its tags match the current package build. Untagged `.hop` files are always included.

The syntax is:

```text
name[tag,!tag].hop
```

Examples:

```text
impl[wasm32].hop
impl[playbit,!aarch64].hop
helper[testing].hop
```

Comma means `AND`. A leading `!` negates one tag, so `impl[playbit,!aarch64].hop` is included only
when `playbit` is active and `aarch64` is inactive.

## Motivation

HopHop packages need a small conditional compilation mechanism for platform support, architecture
specialization, and test helpers. Filename tags keep this first version deterministic and visible in
the package layout without adding source-level preprocessor state.

## Goals

- Filter files in directory packages before parsing source.
- Support architecture tags, selected platform tags, and a test-only `testing` tag.
- Keep single-file CLI input explicit: a direct `foo[wasm32].hop` input is loaded even when `wasm32`
  is inactive.
- Reject malformed tag suffixes early.

## Non-goals

- No source-level conditional compilation syntax.
- No `system` tags such as `linux` or `macos` in v1.
- No OR expressions, nested expressions, or spaces.
- No new `--system` option.

## Active tags

A tag is active when it matches one of:

- the selected package platform target, from `--platform <target>`
- the selected package architecture target, from `--arch <name>`
- `testing`, when the internal `--testing` package option is enabled

The default platform is the existing package default. The default architecture is the compiler host
architecture, normalized to names such as `aarch64`, `x86_64`, or `wasm32`.

Unknown well-formed tags are inactive. In v1, a tag like `linux` only matches if `linux` is the
selected platform name.

## Filename grammar

Tags are recognized only in the final basename segment immediately before `.hop`.

Valid examples:

```text
foo[wasm32].hop
foo[playbit,!aarch64].hop
foo[testing].hop
```

Invalid examples:

```text
foo[].hop
foo[wasm32,].hop
foo[!!wasm32].hop
foo[wasm 32].hop
foo[wasm32.hop
```

Tag atoms use the same character policy as platform names: identifier characters plus `-` and `.`.

## Matching

For a tagged filename, every atom must match:

- `tag` requires that `tag` is active
- `!tag` requires that `tag` is inactive

If the expression does not match, the file is skipped. If a directory package has `.hop` files but
none match after filtering, loading fails with `no matching .hop files found in <dir>`.

## CLI

Package commands accept `--arch <name>`:

```text
hop check [--platform <target>] [--arch <name>] <pkgdir|srcfile>
hop build --output-format mir [--platform <target>] [--arch <name>] <pkgdir|srcfile>
hop build --output-format c [--platform <target>] [--arch <name>] <pkgdir|srcfile>
hop build [--platform <target>] [--arch <name>] <pkgdir|srcfile>
hop run [--platform <target>] [--arch <name>] <pkgdir|srcfile>
```

The internal `--testing` option is available for test infrastructure and is intentionally omitted
from public usage text.
