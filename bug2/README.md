bug repro of using imported package functions

Repro command:

```sh
cd this-directory
PATH_TO/_build/macos-aarch64-debug/slc checkpkg program.sl
```

Observed output:

```text
error: failed to resolve package path /Users/rsms/src/slang.SLP-26/package
program.sl:1:1: SL0000: failed to resolve import package
```

This demonstrates selector-call binding failure for an imported package function.
