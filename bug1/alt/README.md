# bug1/alt: alternate repro (non-`core/str`) using imported package functions

Repro command:

```sh
_build/macos-aarch64-debug/slc checkpkg bug1/alt/program.sl
```

Observed output:

```text
bug1/alt/program.sl:7:6: SL2002: unknown identifier 'x.simple'
```

This demonstrates selector-call binding failure for an imported package function that is not from
`core/str`.
