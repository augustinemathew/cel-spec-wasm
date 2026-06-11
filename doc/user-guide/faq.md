# FAQ

Short answers, with pointers to the code or doc that backs each one.

### Why would I use this over cel-cpp / cel-go?

Three reasons, one trade:

- **Speed at repetition.** The expression is compiled to wasm ahead of
  time and JIT'd to native code at `Plan` time — no AST walk, no
  interpreter dispatch per Eval. Corpus-wide it's parity with cel-cpp
  (geomean 0.95×); on anything with length or control flow it's 9–25×
  faster (see "How fast is it really?" below).
- **One artifact everywhere.** A `Program` is pure bytes (wasm +
  `cel.abi`). Compile once, ship it, evaluate it in a process — or, in
  the future, a language — that never links the compiler. Semantic
  drift between host implementations is structurally impossible.
- **Sandboxed custom functions.** `@component` functions run in their
  own wasm linear memory with no syscalls and no access to your
  process. Stock CEL only gives you trusted in-process callbacks.
  See the [security model](security-model.md).

The trade: no `dyn` (next question), and a smaller extension surface
than cel-cpp. Of the upstream conformance corpus, **every attempted
row passes (0 fails)**; 481 rows are intentionally skipped — 227 need
`dyn`, 144 are check-disabled, and 110 sit on not-yet-shipped scope
(55 of those on unimplemented extension rows). Live per-fixture
breakdown: [`conformance/README.md`](../../conformance/README.md).

### Does it support `dyn`?

No, by design. The compiler accepts CEL's **static subset**: every
variable and intermediate is statically typed, declared up front. The
gate is the `RejectDyn` pass in
`compiler/frontend/parse_and_check.cc` — a `dyn`-typed expression
fails at `Compile` with `InvalidArgument`, never silently. This is the
load-bearing 227-row conformance skip. If your workload genuinely
needs dynamic typing, use cel-cpp or cel-go; this is not a drop-in
replacement.

### How fast is it really?

Measured over a 232-cell corpus against cel-cpp's tree-walking
evaluator (`-c opt`, Apple Silicon, static-linked mode): **corpus-wide
geomean 0.95× — parity — with a sharply two-sided distribution.**

- **Wins:** 9× on a 100-element `.all()`, 25× on a 20-element
  `.map()`, 2.2× on a 100-term string-concat chain. (The 1000-term
  arithmetic-chain numbers predate the slot-reuse codegen rework; we
  don't quote a speedup there until it's re-measured.) The crossover
  is roughly 10 operations — anything with repetition amortizes the
  compiled code.
- **Losses:** 44× on a 100-entry **map literal** (constant aggregates
  are rebuilt every Eval today; cel-cpp folds them at plan time), 8×
  on 1000-char string equality (no wasm SIMD memcmp yet), and 1.2–1.9×
  on single-op expressions — the per-Eval floor is 62 ns (one wasm
  boundary crossing + arena reset), which tiny expressions can't
  amortize.

Full methodology and every loss row's cause:
[`m28-bench-results.md`](../implementation-plan/rewrite/m28-bench-results.md).
Reproduce with `benchmark/eval/run.sh` (three-way: dynamic / static /
cel-cpp).

### How big is a compiled Program?

Depends on `CompilerOptions::link_mode` (`compiler/compiler.h`):

- **`kStatic` (the default):** ~1.1 MB measured (1,162,962 bytes for
  `1 + 2`) — the runtime kernel is merged into the Program, so it's
  fully self-contained and fastest to evaluate.
- **`kDynamic`:** ~10 KB — just the compiled expression; the runtime
  helpers are imported and supplied by the Engine at `Plan` time. Use
  it when you cache many distinct Programs and memory footprint
  matters.

`Engine::Plan` accepts both shapes transparently; callers never branch
on link mode.

### How long do Compile / Plan / Eval take?

- **Compile** — the slow phase: a few hundred µs for small expressions
  at `optimize_level = 0`, rising ~2–3× at level 2 (the recommended
  production setting — see the per-level table in
  `compiler/compiler.h`). Pay it once per expression.
- **Plan** — ~240–300 µs: Cranelift JITs the Program's wasm to native
  code. Pay it once per Program per process; amortized across evals.
- **Eval** — the floor is ~62 ns (boundary crossing + arena reset +
  result decode); real expressions add their actual work on top.

### Is it thread-safe?

The contract, verbatim from `eval/engine.h` / `eval/instance.h`:

| Object | Concurrency |
|---|---|
| `Program` | pure bytes, immutable — share and serialize freely |
| `Engine::Plan` | **safe to call concurrently** from many threads |
| `Engine::AddFunction` / `BindFunction` / `AddModule` / `AddComponent` | **not** thread-safe — configure once at startup, then `Plan` from many threads |
| `Instance` | thread-owned, single-threaded — bind one per worker; it outlives the Engine handle (shared_ptr) |

### Can my custom function return an error or unknown?

Yes. A host function returning `absl::StatusOr<Value>` has three
distinct non-value outcomes:

1. **`Value::Error(...)`** — a CEL *error value*. The expression keeps
   evaluating under CEL's absorption rules (`true || error` is still
   `true`). Use for domain errors the policy should see.
2. **`Value::Unknown(AttributeId{kFunctionUnknownSentinel})`** — a CEL
   *unknown*: "this function couldn't answer yet." Composes with
   partial evaluation and 3-valued logic.
3. **A non-OK `absl::Status`** — infrastructure failure. The whole
   `Eval()` fails with that status. Use for "my backend is down,"
   never for policy-visible conditions.

All three are runnable in
[`examples/08_function_errors_and_unknowns.cc`](../../examples/08_function_errors_and_unknowns.cc).
One caveat (verified 2026-06-09): the `ErrorPayload`'s **error code**
survives the wasm round-trip, but the free-text `message` currently
does not — the decoded error carries a synthesized
`"runtime error code N"` string.

### What proto schemas work?

Any message in the process-wide generated descriptor pool — i.e. any
`cc_proto_library` your binary links is reachable automatically via
`CelType::Message("your.fqn.Type")`. The CLI additionally accepts
dynamic schemas: `--proto <file>` (a `.proto` source file) or
`--descriptor_set <file>` (a serialized `FileDescriptorSet`); see
`tools/cel/cel.cc`.

### What CEL extensions ship?

- `string_ext`, including `strings.format` (172/216 conformance rows,
  0 fails)
- `math_ext` (194/199, 0 fails)
- `encoders` (`base64.encode` / `base64.decode`)
- `network_ext` (69/69)
- `optionals` — partial (26/70; the rest need `dyn`)

### Is it production-ready?

Not yet — and the gaps are listed, not hidden. The specific items:
constant list/map literals rebuilt every Eval (the 44× loss row),
oversized literal aggregates that can trap the runtime instead of
erroring gracefully, no fuzzing yet, and no bindings beyond C++. See
"Production readiness" in the [README](../../README.md), the
[security model](security-model.md) for the exact threat-relevant
items, and `e2e/known_bugs_test.cc` +
`doc/implementation-plan/cleanup-backlog.md` where every known gap is
pinned by a test or a tracked entry.

### How do I use it outside Bazel?

You don't, today. The build is Bazel-only — there is no CMake build,
no `make install`, no prebuilt release artifacts. What *is* portable
is the output: a compiled `Program` is plain bytes you can store and
ship anywhere, and the evaluation side's requirements (wasmtime + the
`cel.abi` section) are designed to support non-Bazel and non-C++
embedders eventually — designed, not built.
