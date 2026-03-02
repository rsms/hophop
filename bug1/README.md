# bug1: `out.format(...)` selector does not resolve imported `core/str.format`

Repro command:

```sh
_build/macos-aarch64-debug/slc checkpkg bug1/repro.sl
```

Expected (current buggy behavior):

```text
bug1/repro.sl:5:10: SL2002: unknown identifier 'format'
```
