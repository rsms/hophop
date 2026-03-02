# SL language project

This file defines the default working protocol for coding agents in this repository.
Scope: entire repository.

## 1) Project overview

SL is a systems-programming language; a custom minimal programming language inspired by C and Go. We are developing the reference compiler (slc) and the specification (`docs/language.md`). The reference compiler has support for multiple target backends, currently a **C11 backend** and an **evaluator**. There is a concept of "platforms" on which SL programs can run (`lib/platform/`).

- The reference compiler is written in C11 and composed of two major components:
    1. `libsl.h`, a library which does **not** use libc (compiled with `-ffreestanding`), which contains the majority of the compiler (parser, typechecker, code generation etc.) `lib_sources` in `build.sh`
    2. `slc`, a CLI program which uses libc to load files, drives the library and writes output. `cli_sources` in `build.sh`.
- The syntax is **regular and LLM-friendly**
- The language spec and EBNF grammar is in `docs/language.md`
- Feature proposals are at `docs/SLP-*.md`
- Source of truth for implementation details: the code itself (`src/`, `lib/`, `examples/`, `tests/`).
- If a high-level summary conflicts with code, trust `docs/language.md` first, and current source files second, before anything else.

### 1.1 Non-goals

- Full C interop with arbitrary headers (we can add `extern` later).
- Heavy optimization.
- Sophisticated module systems beyond Go-like packages.

### 1.2 Feature proposals

Language evolution happens via explicit proposal documents called "SLP" (SL Proposal). Each logical change is documented in one SLP document stored at `docs/SLP-*.md`

## 2) Engineering Principles (Normative)

These principles are mandatory. They are implementation constraints, not suggestions.

### 3.1 KISS (Keep It Simple, Stupid)

Required:
- Prefer straightforward control flow over meta-programming.
- Prefer explicit comptime branches and typed structs over hidden dynamic behavior.
- Keep error paths obvious and localized.

### 3.2 YAGNI (You Aren't Gonna Need It)

Required:
- Do not add config features, command line arguments or other features without a concrete caller/user.
- Do not introduce speculative abstractions.
- Keep unsupported paths explicit (panic or return clear error) rather than silent no-ops.

### 3.3 DRY (Don't Repeat Yourself) + Rule of Three ("Three strikes and you refactor")

Required:
- Duplicate small local logic when it preserves clarity.
- Extract shared helpers only after repeated, stable patterns (rule-of-three).
- When extracting, preserve module boundaries and avoid hidden coupling.

### 3.4 Fail fast + Explicit errors

Required:
- Prefer explicit errors for unsupported or unsafe states.
- Never silently broaden permissions or capabilities.

### 3.5 Secure by Default + Least Privilege

Required:
- Deny-by-default for access and exposure boundaries.
- Never log secrets, raw tokens, or sensitive payloads.
- Keep network/filesystem/shell scope as narrow as possible.

### 3.6 Determinism + No Flaky Tests

Required:
- Tests must not use real network connections, read from system randomness sources, or depend on system state.
- Use `builtin.is_test` to bypass side effects (spawning, real hardware I/O).
- Tests must be reproducible.

## 3) Workflow (Required)

- Take an incremental approach: keep the compiler working at each step.
- After changes, run `./build.sh test` (or `python3 tools/test.py run ...` only if you are confident a change only affects some tests) and add/update entries in `tests/tests.jsonl` for new or changed behavior.

## 4) Agent Workflow (Required)

1. **Read before write** — inspect existing implementation, related examples and adjacent tests before editing.
2. **Define scope boundary** — one concern per change; avoid mixed feature+refactor+infra patches.
3. **Implement minimal patch** — apply KISS/YAGNI/DRY rule-of-three explicitly.
4. **Test everything** — Write tests for any new feature or change (add/update `tests/tests.jsonl`). Everything must have tests.
5. **Validate** — `./build.sh test` must show 0 failures. Add/update entries in `tests/tests.jsonl` for new or changed behavior.
6. **Document impact** — update comments and `docs/` for behavior changes, risk, and side effects.
7. **Version control**:
    - Serialize git index writes: never run `git add`, `git commit`, `git rm`, `git mv`, or similar index-mutating commands in parallel.
    - Merge worktree changes into `main` only when the operator explicitly asks (e.g. "merge to main" / "merge changes to the main repo"), by running `wt merge --no-remove` from that worktree. If merge conflicts happen during `wt merge --no-remove`, stop and resolve them interactively with the operator; do not auto-resolve conflicts. This is required. Never use `--no-verify` with `wt merge` (unless operator explicitly asks you to do so.)

### 3.1 Agent Coordination

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
- Optional: JSON object when structured fields help, e.g. `agent-worklog '{"message":"running tests","SLP":"14"}'`
- If plain text is used, `agent-worklog` wraps it into JSON and adds `timestamp` plus `from` (current branch name)

Reacting to important updates:
- If another agent announces a change that may affect your current work, use git to inspect or integrate it now.
- Integration options include: `git stash` + merge/rebase + `git stash pop`, committing your WIP and then merging/rebasing, or another safe git workflow.
- If another agent reports they merged, update your branch from `main` promptly (`git merge main` or `git rebase main`) to pick up those changes.

Guidelines for announcement types:
- start of planned work: include an abbreviated version of the plan as a "plan" JSON property, e.g. `agent-worklog '{"message":"starting work on migrating prelude to core package","plan":"PLAN GOES HERE"}'`
- adding, removing or modifying files: include a list of filenames, e.g. `agent-worklog '{"message":"MESSAGE HERE","files":["file1", "file2"]}'`

## 5) Building and testing

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

## 6) Reference compiler CLI

See `_build/<target>/slc` for a comlpete list of commands.
Here are a few examples:

- `slc tokens file.sl` — tokenize
- `slc ast file.sl` — parse + print AST
- `slc check file.sl` — typecheck single file
- `slc genpkg:c <dir|file.sl> [out.h]` — generate C output via C11 backend
- `slc compile <dir|file.sl> -o <exe>` — compile via C11 backend + system compiler
- `slc run <dir|file.sl>` — run with evaluator (not C11 backend)
