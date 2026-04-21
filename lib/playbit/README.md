# Playbit Library

This directory contains small SL wrappers for the Playbit platform.

There are two layers:

- `platform/playbit`
  Low-level platform package. This is the ABI boundary used by the compiler-selected Playbit platform. It exposes imported syscalls, constants, structs, and startup helpers.
- `playbit/...`
  Higher-level library packages built in plain SL on top of `platform/playbit`.

Current packages:

- `playbit/console`
  Write log output through the current thread's console.
- `playbit/event`
  Poll the event queue into a caller-provided byte buffer.
- `playbit/handle`
  Find and close handles.
- `playbit/object`
  Observe and signal Playbit objects.
- `playbit/signal`
  Thin convenience wrappers over `playbit/object`.
- `playbit/thread`
  Thread/process exit helpers and thread-related constants.
- `playbit/time`
  Monotonic time helpers and duration constants.
- `playbit/window`
  Open, close, inspect, and configure windows.

Example:

```sl
import "playbit/console" { write }
import "playbit/time"    { now, since }

pub fn main() {
    var t0 = now()
    _ = write("hello from playbit")
    var dt = since(t0)
    _ = dt
}
```

Notes:

- These packages are intended for programs built for the `playbit` platform target.
- Keep wrappers simple and idiomatic SL. Put ABI-specific details in `platform/playbit` unless a higher-level package clearly needs them.
- The current startup path is `_start -> pb_syscall(ThreadEnterMain) -> sl_main`.
