# Contributing to celwasmc

This document is the code-change workflow for the CEL → WASM compiler.
The rules in [CLAUDE.md](../CLAUDE.md) take precedence over anything
here; this file is the concrete "how to".

## Tool install (one-off)

| Tool            | Purpose                                   | Install                                              |
|-----------------|-------------------------------------------|------------------------------------------------------|
| `clang-format`  | Formatter (Google C++ style).             | `brew install llvm` → `PATH=/opt/homebrew/opt/llvm/bin:$PATH` |
| `clang-tidy`    | Linter (Google C++ checks + size gate).   | Same package as `clang-format` (brew `llvm`).        |
| `bazel`         | Build / test driver.                      | Pinned via `.bazelversion`; install with `bazelisk`. |
| `python3`       | Fallback compile-db generator.            | System Python 3 works.                               |

Putting brew's llvm on PATH in your shell rc is the simplest fix; the
repo's `scripts/lint.sh` auto-prepends `/opt/homebrew/opt/llvm/bin`
when it exists.

## Compile database (for clang-tidy)

clang-tidy needs a `compile_commands.json` to know each translation
unit's flags. Regenerate after adding or changing Bazel targets:

```bash
scripts/refresh_compile_db.sh
```

The simple path uses the aquery fallback in that script (no MODULE
changes needed). Hedron's `hedron_compile_commands` is preferred once
wired into `MODULE.bazel` — see the script's header for the snippet.
If no compile DB is present, `scripts/lint.sh` still runs clang-tidy
but warns that analysis will be partial.

## Code-change workflow

Follow these steps on every slice. **Do not skip any step.**

### 1. Write the code

- One logical unit per translation unit.
- Functions do **one thing**. Keep them under:
  - 60 lines
  - 40 statements
  - 15 branches
  - 6 parameters
  - 5 levels of nesting
  These are enforced by `readability-function-size` in `.clang-tidy`.
  If you exceed any of them, split the function.
- `absl::StatusOr<T>` for fallible returns; `ABSL_MUST_USE_RESULT` on
  status-returning free functions.
- Header guards: `CELWASM_<PATH>_H_`.
- No raw `new`/`delete`; prefer `std::unique_ptr` / `std::make_unique`
  or protobuf `Arena`.
- Mirror `third_party/cel-cpp/` conventions whenever in doubt — that
  repo is the style reference.

### 2. Format + lint changed files

```bash
scripts/lint.sh
```

This:

1. Runs `clang-format -i` in-place on each file that differs from
   `origin/master` (including the staged/unstaged diff).
2. Runs `clang-tidy` (with `--warnings-as-errors='*'`) against the
   same file set, using `compile_commands.json` if available.
3. Exits non-zero on any warning.

Common workflows:

```bash
scripts/lint.sh                 # changed files only (default)
scripts/lint.sh --all           # everything under compiler/
scripts/lint.sh compiler/codegen/expr_lower.cc  # just these
```

`third_party/` and `bazel-*/` are always skipped.

### 3. Build + test

```bash
bazel test //compiler/...
```

All tests must pass. For codegen work, also confirm:

- Binaryen validation (`module.Validate()` must return OK).
- At least one positive and one negative unit test per feature — see
  [`CLAUDE.md`](../CLAUDE.md) and
  [`doc/implementation-plan/testing-checklist.md`](implementation-plan/testing-checklist.md).

### 4. Update plan + checklist

Every merged feature:

- Ticks at least one box in
  [`doc/implementation-plan/testing-checklist.md`](implementation-plan/testing-checklist.md).
- Is reflected (added or ticked) in the active milestone doc under
  [`doc/implementation-plan/`](implementation-plan/).

If the user gave new guidance during the work, capture it as a tagged
bullet in the milestone doc.

### 5. Commit

Only after steps 1–4 are clean:

```bash
git add <paths>
git commit
```

Conventional messages: `feat:`, `fix:`, `refactor:`, `test:`, `docs:`.
Keep the subject under 72 chars; use the body for rationale.

## Function-size enforcement

`readability-function-size` is the main size gate. When clang-tidy
flags a function:

- **Refactor, don't suppress.** Split the body into named helpers;
  each helper should read top-to-bottom without scrolling.
- If a suppression is genuinely unavoidable (e.g. a generated table),
  justify it in a comment and use `// NOLINT(readability-function-size)`
  on the function signature line.

Known exceedances live in
[`doc/implementation-plan/lint-backlog.md`](implementation-plan/lint-backlog.md);
clear them before they rot.

## What NOT to do

- Don't run `clang-format` / `clang-tidy` over `third_party/`.
- Don't add `// NOLINT` drive-by; the linter is there to find real
  smells.
- Don't land code that hasn't gone through `scripts/lint.sh`.
- Don't skip `bazel test //compiler/...` — a formatter pass can still
  introduce semantics-affecting edits in the rare case the config is
  wrong; tests are the safety net.
