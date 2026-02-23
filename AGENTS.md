# SL language project

## Agent Coordination

There are multiple concurrent agents running. You are one of them.
Each agent should run in a dedicated git worktree managed by Worktrunk (`wt`).

- Create/switch worktrees with `wt switch --create <branch>` and `wt switch <branch>`
- Inspect active worktrees with `wt list`
- Find your current branch with `git branch --show-current`

When making plans, editing files or running commands that change things (but not during discussion-only phases), coordinate through worklogs:

- Write announcements with `agent-worklog <announcement> ...`
- Every `agent-worklog ...` announcement also read updates from other agents, so no separate immediate poll is needed after posting
- If you have no new announcement, poll with `agent-worklog` every 20-60 seconds
- The `agent-worklog` program writes to a per git repo (not per worktree) JSONL file (see `agent-worklog --print-db-path`)
- Keep announcements short, clear, concise, and to the point
- Announce before starting a concrete change and after each major step
- When starting up, run `agent-worklog` to catch up on what's happening.

Announcement format:

- Preferred: plain short message text, e.g. `agent-worklog "editing typecheck: fix mutable slice assign"`
- Optional: JSON object when structured fields help, e.g. `agent-worklog '{"message":"running tests","proposal":"14A"}'`
- If plain text is used, `agent-worklog` wraps it into JSON and adds `timestamp` plus `from` (current branch name)

Reacting to important updates:

- If another agent announces a change that may affect your current work, use git to inspect or integrate it now.
- Integration options include: `git stash` + merge/rebase + `git stash pop`, committing your WIP and then merging/rebasing, or another safe git workflow.
- If another agent reports they merged, update your branch from `main` promptly (`git merge main` or `git rebase main`) to pick up those changes.

Guidelines for announcement types:

- start of planned work: include an abbreviated version of the plan as a "plan" JSON property, e.g. `agent-worklog '{"message":"starting work on migrating prelude to core package","plan":"PLAN GOES HERE"}'`


## Build and Test

- `./build.sh test` — build (debug) and run full test suite
- `./build.sh` — build debug into `_build/macos-aarch64-debug/`
- `./build.sh release` — build release into `_build/macos-aarch64-release/`
- `./build.sh verbose=1` — show compiler commands
- Run the CLI: `_build/macos-aarch64-debug/slc`
- List tests: `python3 tools/test.py list`
- Run tests directly: `python3 tools/test.py run --build-dir _build/macos-aarch64-debug --cc clang`
- Run one suite: `python3 tools/test.py run --suite <suite> --build-dir _build/macos-aarch64-debug --cc clang`
- Lint manifest: `python3 tools/test.py lint`

The build script invokes `clang-format` automatically, so source files you edit will be reformatted on build.

Tests are defined in `tests/tests.jsonl` and run by `tools/test.py` (which `./build.sh test` delegates to). See `tests/README.md` for manifest format, test kinds, sidecar files (including `.expected.c`), and command usage.

## CLI Commands

```sh
slc tokens file.sl          # tokenize
slc ast file.sl             # parse + print AST
slc check file.sl           # typecheck single file
slc checkpkg <dir|file.sl>  # typecheck package
slc genpkg:c <dir|file.sl> [out.h]  # generate C header
slc compile <dir|file.sl> -o <exe>  # compile via C11 backend + system compiler
slc run <dir|file.sl>       # compile + execute
```

## Architecture

SL is a language currently compiled via a C11 backend in a multi-stage pipeline:

```
Source → [Lexer] → Tokens → [Parser] → AST → [Typechecker] → [Codegen] → C header
```

### Source Layout (`src/`)

| File | Role |
|---|---|
| `libsl.h` | Public API: type definitions, token/AST node kinds, function declarations |
| `libsl.c` | Freestanding core: arena allocator, lexer (with semicolon insertion), diagnostics |
| `parse.c` | Recursive-descent parser; builds a flat array of AST nodes (indexed, not pointer-linked) |
| `typecheck.c` | Symbol tables, name resolution, type inference, mutability/reference checking |
| `slc.c` | Hosted CLI: package loading, import graph, command dispatch |
| `codegen_c.c` | C backend: emits single-header library, mangles symbols, lowers defer, VSS accessors |
| `codegen.h/.c` | Backend interface (currently only C backend exists) |
| `tools/amalgamate.py` | Merges `libsl.*` sources into a single distributable header |

### Key Design Points

**Freestanding core**: `libsl.h/c` and `parse.c`/`typecheck.c` have no libc dependencies. All allocation goes through an explicit `SLArena` arena passed by the caller. The hosted CLI (`slc.c`) provides the growable backing store.

**Flat AST**: Nodes are stored in a contiguous array; cross-references use `int32_t` indices, not pointers. This simplifies serialization and avoids pointer chasing.

**Single-header output**: `genpkg:c` emits a `.h` file (C11, compilable as freestanding) containing all type definitions, function declarations, and implementations for a package.

**Symbol mangling**: Package symbols are emitted as `pkg__Name` (double underscore separates package from identifier).

**Selector inference**: The typechecker resolves `.` vs `->` so the codegen always knows whether to emit `.` or `->` for field access.

**Defer lowering**: `defer` statements are reordered to LIFO at block exits during codegen, not in the AST.

**Variable-size structs (SLP-1)**: Structs can have a trailing `[.lenField]T` dependent array. Codegen emits accessor functions and runtime `sizeof` helpers. See `docs/SLP-1-variable-size-structs.md`.

**References and slices (SLP-2)**: `&T` = borrowed reference, `*T` = owned pointer, `[T]` = read-only slice, `mut[T]` = mutable slice. `new(ma, T)` allocates via an explicit `MemAllocator`. See `docs/SLP-2-types.md`.

## Workflow Notes

- Take an incremental approach: keep the compiler working at each step.
- After changes, run `./build.sh test` (or `python3 tools/test.py run ...`) and add/update entries in `tests/tests.jsonl` for new behavior.
- The language spec and EBNF grammar is in `docs/language.md`; project overview is in `docs/project-overview.md`; feature proposals are in `docs/SLP-*.md`.
- Serialize git index writes: never run `git add`, `git commit`, `git rm`, `git mv`, or similar index-mutating commands in parallel.
- Merge worktree changes into `main` only when the operator explicitly asks (e.g. "merge to main" / "merge changes to the main repo"), by running `wt merge --no-remove` from that worktree.
- If merge conflicts happen during `wt merge --no-remove`, stop and resolve them interactively with the operator; do not auto-resolve conflicts.
