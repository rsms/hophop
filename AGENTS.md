# Agent Instructions

As we are building this, take an incremental approach, where we always have _something_ that works and is doing something. As we make changes, test often and expand the test suite(s) as new code and functionality is added.

To get a deeper understanding of the project, read `*.md` files in the repo root.

After you have made changes:
- run `./build.sh test` to verify that everything works.
- update tests (end of `build.sh`) to ensure new and changed code is covered by tests.

## Build and Test

- `./build.sh test` builds in debug mode and runs tests
- `./build.sh` builds in debug mode into `_build/macos-aarch64-debug/`
- `./build.sh release` builds in release mode into `_build/macos-aarch64-release/`
- Run cli program with `_build/macos-aarch64-debug/slc`
