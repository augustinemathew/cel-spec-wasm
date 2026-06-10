# `cel` command-line tool — target design

Status: design — drafted 2026-05-24, not yet implemented. Design-only: no
code lands from this doc directly. Captures the *target* surface for the
`cel` CLI (`tools/cel/`) and the gap between it and what ships
today (`eval` / `check` / `compile` only). Companion to the user guide §9
(`doc/user-guide.md`), which documents the as-shipped CLI and points here
for the target.

## 1. Guiding principle

The CLI is a **thin, complete mirror of the two-phase public API**
(`Compiler` → `Program` → `Engine` → `Instance`; see `doc/user-guide.md`
§1). Two rules follow:

  1. **Every public capability has a verb.** Nothing the C++ API can do
     should be reachable *only* from C++. Today the CLI funnels
     everything through "compile in-process," so the entire run-time half
     of the API (`Engine::Plan` over a precompiled `Program`, foreign
     `AddModule`) has no CLI surface — that is the core gap.

  2. **The compiled `Program` is a first-class, self-describing
     artifact.** A `.wasm` (+ its `cel.abi` section) carries everything a
     runner needs to know about its own requirements — variable schema,
     host imports, required foreign aliases. The CLI surfaces that
     (`inspect`) so the compile-once / ship / run-many loop works without
     the original source or `.celfn`.

The two API phases become two families of verbs: **compile-time**
(`check`, `compile`, `celfn`) and **run-time** (`run`), with `eval` as the
one-shot convenience spanning both.

## 2. Command surface

| Verb | Phase | Purpose | Status |
|---|---|---|---|
| `cel check <expr>` | compile | parse + type-check; diagnostics with source spans | ✅ ships today |
| `cel compile <expr>` | compile | emit `.wasm` (+ `cel.abi`) to `--output`/stdout | ✅ ships today |
| `cel eval <expr>` | both | compile + run in one shot; print result | ✅ ships today |
| `cel run <prog.wasm>` | run | evaluate a **precompiled** program (no recompile) | ⛔ planned — the missing half |
| `cel inspect <prog.wasm>` | — | dump what the artifact **requires** to run | ⛔ planned |
| `cel celfn check <lib.celfn>` | compile | validate a custom-fn library (language-independent) | ⛔ planned |
| `cel celfn gen --lang <go\|rust\|c> <lib.celfn>` | compile | generate foreign shims (celfnc); `--lang` **required** | ⛔ planned (`go` first) |

`repl` was considered and **dropped**: high surface (persistent env, line
editing, incremental re-declaration) for a tool whose center of gravity is
batch / scripted use, and `eval` already covers quick one-offs.

## 3. The three ideas that close the gaps

### 3.1 The Program is self-describing → `inspect` closes the compile/run loop

`cel.abi` already carries the variable schema; the target extends it (if
not already present) to enumerate **host imports** and **required foreign
aliases**. `inspect` decodes and prints it:

```
$ cel inspect expr.wasm
vars:     a:int, b:int
requires: foreign module 'rules'   (supply with --module rules=<file>.wasm)
host fns: none
```

This is what makes "compile here, run there" usable: the runner needs
neither the source nor the `.celfn` to learn what to feed in. It also
settles, at the *artifact* level, the question of whether evaluation needs
the IDL — `inspect`'s output never lists the `.celfn`, because the IDL is a
compile-time-only input (see §5 and user-guide §9.1).

### 3.2 `run` binds values, not types — types come from the artifact

Because the schema is in `cel.abi`, the run side does **not** re-declare
types; it binds values and the types are read from the artifact:

```
cel compile "a * b" --var a:int --var b:int -o p.wasm   # types declared at compile
cel run p.wasm --var a=6 --var b=7                       # values only; types from cel.abi
```

Plus a bulk path for real workloads — bind a whole request at once instead
of N `--var`s:

```
cel run policy.wasm --activation request.json      # or .textproto; keys → declared vars
```

`--activation` resolves message-typed vars against the same descriptor
source the program was compiled with (carried in / referenced by
`cel.abi`); a key with no matching declared var, or a value whose kind
disagrees with the declared type, is a usage error (exit 2).

### 3.3 Full-pipeline parity for custom functions

The IDL becomes a real CLI input, and the foreign story is end-to-end in
one tool:

```
cel compile "is_adult(u)" --celfn lib.celfn --var u:acme.User -o p.wasm   # @native baked in
cel celfn gen --lang go lib.celfn -o rules_celfn.go                       # foreign shims (celfnc)
cel run p.wasm --module rules=rules.wasm --activation req.json            # foreign bytes at run
```

- `--celfn <file>` — the CLI reads the file and feeds `ParseCelfnSource` →
  `AddLibrary`. **File reading is the CLI's job, not the library's** (the
  library API takes the whole IDL as a string — user-guide §5.1).
  Available to `check` / `compile` / `eval`.
- `--module <alias>=<path.wasm>` (repeatable) — supplies foreign module
  bytes at run time (`Engine::AddModule`). `run`/`inspect` validate the
  set of required aliases (from `cel.abi`) up front and error on a missing
  one before evaluating.

### 3.4 `celfn gen` requires an explicit target language

`gen` emits language-specific guest shims, so the target is a **required
argument**, never a defaulted guess:

```
cel celfn gen --lang go   lib.celfn -o rules_celfn.go      # probe-validated (foreign-go-bindgen-findings.md)
cel celfn gen --lang rust lib.celfn -o src/rules_celfn.rs  # designed, not yet validated
cel celfn gen --lang c    lib.celfn -o rules_celfn.c       # designed, not yet validated
```

- Omitting `--lang` is a usage error (exit 2) that lists the supported
  targets.
- `celfn check` takes **no** `--lang` — it validates the IDL itself, which
  is language-independent.
- **`go` is the only probe-validated target** (`foreign-go-bindgen-findings.md`,
  `celfn-go-bindgen-design.md`); `rust` / `c` are designed-not-validated,
  so they emit behind a "not yet validated" banner (or are gated) rather
  than implying parity.
- The proto caveat travels per-language: `--lang go` + a `proto(...)`
  param implies **stock Go, not TinyGo** (TinyGo's `reflect.NewAt` trap in
  `proto.Unmarshal` — findings §gotcha 1). `gen` surfaces this in the
  generated file's header comment.

## 4. The host-function boundary (what the CLI deliberately can't do)

A generic CLI cannot supply `@host` C++ implementations — the IDL declares
the *signature*, but the *behavior* is user C++ registered via
`Engine::AddFunction`. The tool handles this **explicitly**, not
cryptically:

  - `inspect` lists host imports so you know up front.
  - `run` on a program with unsatisfied host imports errors:
    `"program imports host fn 'rate_string'; the CLI cannot supply host
    impls — use the C++ API, or redefine it as @native / foreign."`

Crucial distinction to keep in the docs: **built-in CEL extension
libraries (strings / math / encoders / …) are compiled-in runtime, not
host fns** — those evaluate in the CLI with no extra wiring. Only *user*
`@host` C++ is unreachable from the stock tool.

## 5. Why evaluation never needs the `.celfn` IDL

The IDL is a **compile-time-only** input — consumed by `Compiler` to
type-check call sites and, for `@native`, to lower bodies into the wasm.
Run-time requirements by backend (mirrors user-guide §9.1):

| Backend | Needed at run time | `.celfn` at run time? |
|---|---|---|
| `@native` | nothing — body compiled *into* the wasm | No — self-contained |
| Foreign | the module **bytes** under the alias (`--module`) | No — call is lowered to a trampoline keyed by alias + overload id |
| `@host` | a C++ impl (`Engine::AddFunction`) | No, and the CLI can't supply the impl regardless |

The variable schema needed to bind `--var` / `--activation` rides in
`cel.abi`, so the run side is self-describing for variables too.

## 6. Suggested build order (cheapest-first, each independently useful)

  1. **`run` + `inspect`** — pure run-time; no compiler changes, just a
     `cel.abi` → schema decoder shared by both. Unlocks compile-once /
     run-many and makes the artifact legible. Highest value, lowest cost.
  2. **`--celfn`** — wire `ParseCelfnSource` → `AddLibrary` into
     `check` / `compile` / `eval`. Makes custom-fn-using expressions
     checkable/compilable from the CLI.
  3. **`--module alias=path.wasm` + `--activation <file>`** — run-time
     inputs for foreign modules + bulk binding. (Foreign depends on the
     §8 / `modules-and-ffi.md` §5 backend landing.)
  4. **`celfn gen` (celfnc)** — the foreign shim generator, `go` first
     (`celfn-go-bindgen-design.md`).

## 7. Open questions

  - **Does `cel.abi` already carry host-import + foreign-alias lists, or
    is that an ABI addition?** `inspect` (§3.1) and `run`'s up-front
    validation (§3.3) depend on it. If it's not there, scope a small
    `cel.abi` extension before step 1.
  - **`--activation` descriptor source.** A precompiled program ran in
    another process may reference message types not in the runner's
    `generated_pool()`. Decide whether `cel.abi` embeds a
    `FileDescriptorSet` (self-contained, larger artifact) or `run`
    requires `--descriptor_set` for message-typed vars (smaller artifact,
    extra flag). Lean self-contained for the ship-elsewhere story.
  - **Expression input source.** Positional `<expr>` only today; consider
    `--file expr.cel` / stdin for large expressions and piping. Low cost,
    orthogonal to the above.

## 8. Reference docs

  - `doc/user-guide.md` §9 / §9.1 — as-shipped CLI + the compile/run split.
  - `doc/implementation-plan/rewrite/m13-custom-fns.md` — custom-fn backends,
    `.celfn` grammar (§3.0).
  - `doc/implementation-plan/rewrite/modules-and-ffi.md` §5 — foreign backend.
  - `doc/implementation-plan/rewrite/celfn-go-bindgen-design.md`,
    `foreign-go-bindgen-findings.md` — celfnc Go target (probe-validated).
