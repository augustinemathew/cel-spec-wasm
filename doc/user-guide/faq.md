# FAQ

Short answers, with pointers to the code or doc that backs each one.

### Why would I use this over cel-cpp / cel-go?

Three reasons, one trade:

- **Speed at repetition.** The expression is compiled to wasm ahead of
  time and JIT'd to native code at `Plan` time — no AST walk, no
  interpreter dispatch per Eval. The crossover vs cel-cpp is roughly
  3–14 operations depending on operator family; above it, wins run
  from 3× (string chains) to 200×+ (constant aggregates materialized
  at compile time). See "How fast is it really?" below.
- **One artifact everywhere.** A `Program` is pure bytes (wasm +
  `cel.abi`). Compile once, ship it, evaluate it in a process — or, in
  the future, a language — that never links the compiler. Semantic
  drift between host implementations is structurally impossible.
- **Sandboxed custom functions.** `@plugin` functions run in their
  own wasm linear memory with no syscalls and no access to your
  process. Stock CEL only gives you trusted in-process callbacks.
  See the [security model](security-model.md).

The trade: no `dyn` (next question), and a smaller extension surface
than cel-cpp. Of the upstream conformance corpus, **every attempted
row passes (0 fails)**; 481 rows are intentionally skipped — 227 need
`dyn`, 144 are check-disabled, and 110 sit on not-yet-shipped scope
(55 of those on unimplemented extension rows). Live per-fixture
breakdown: [`conformance/README.md`](https://github.com/augustinemathew/cel-wasm/blob/master/conformance/README.md).

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

Measured over the eval corpus against cel-cpp's evaluator (`-c opt`,
Apple Silicon, static-linked mode). The distribution is two-sided and
workload-dependent; the crossover is roughly 3–14 operations.

- **Wins:** constant aggregates materialize into the Program at
  compile time — `size([…1000])` 224×, constant-map lookup with the
  baked hash index 118–198× at 256 entries; 1000-term arithmetic
  chains 18×; complex regex 59× (pattern cached per Instance).
- **Losses:** single proto accessors ~1.7× slower (one host trampoline
  per read, amortized by any larger expression); large
  activation-bound aggregates pay a per-Eval marshal (an early-exit
  `in` over a 1000-string bound list is ~14× slower); `contains()` on
  a 10 KB haystack is ~2.7× slower even after the SIMD128 scan
  (the fixed eval floor dominates what remains).
- **The floor:** a trivial expression evaluates in ~50 ns (boundary
  crossing + arena reset), which single-op expressions can't amortize.

Current published tables: [`benchmark/README.md`](https://github.com/augustinemathew/cel-wasm/blob/master/benchmark/README.md)
and [`benchmark/eval/results/`](https://github.com/augustinemathew/cel-wasm/tree/master/benchmark/eval/results).
Reproduce with `benchmark/eval/run.sh` (three-way: dynamic / static /
cel-cpp).

### How big is a compiled Program?

Depends on `CompilerOptions::link_mode` (`compiler/compiler.h`):

- **`kStatic` (the default):** ~2.4 MB measured — the runtime kernel
  is merged and optimized into the Program, so it's fully
  self-contained and fastest to evaluate.
- **`kDynamic`:** ~6.5 KB — just the compiled expression; the runtime
  helpers are imported and supplied by the Engine at `Plan` time. Use
  it when you cache many distinct Programs and memory footprint
  matters.

`Engine::Plan` accepts both shapes transparently; callers never branch
on link mode.

### How long do Compile / Plan / Eval take?

Depends on link mode (measured with the public API, Apple Silicon,
`-c opt`; reproduce with `bazel run -c opt
//benchmark/compiler:stage_bench`):

- **Compile** — ~60 ms static (the runtime kernel is merged and
  optimized into each Program — real parallel work), ~0.5 ms dynamic.
  Pay it once per expression.
- **Plan** — ~65 ms static / ~0.5 ms dynamic: Cranelift JITs the
  Program to native code. Once per Program per process.
- **Eval** — the floor is ~50 ns static (~290 ns dynamic); real
  expressions add their actual work on top. `Engine` construction is
  a further ~70 ms, once per process.

### Is it thread-safe?

The contract, verbatim from `eval/engine.h` / `eval/instance.h`:

| Object | Concurrency |
|---|---|
| `Program` | pure bytes, immutable — share and serialize freely |
| `Engine::Plan` | **safe to call concurrently** from many threads |
| `Engine::AddFunction` / `BindFunction` / `AddModule` / `AddPlugin` | **not** thread-safe — configure once at startup, then `Plan` from many threads |
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
[`examples/08_function_errors_and_unknowns.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/08_function_errors_and_unknowns.cc).
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

Beta. The pipeline, sandbox, and conformance results are real and
reproducible: differential fuzzing against the cel-cpp oracle runs
nightly in CI, constant list/map literals materialize into the Program
at compile time, and oversized literals are rejected at compile with a
graceful `ResourceExhausted`. What remains is listed, not hidden: no
bindings beyond C++, allocator caps and CPU-time limits for plugin
functions still to come, and no release-versioning policy yet. See
"Limitations" in the [README](https://github.com/augustinemathew/cel-wasm/blob/master/README.md), the
[security model](security-model.md) for the threat-relevant items, and
`e2e/known_bugs_test.cc` +
`doc/implementation-plan/cleanup-backlog.md` where every known gap is
pinned by a test or a tracked entry.

### How do I use it outside Bazel?

You don't, today. The build is Bazel-only — there is no CMake build,
no `make install`, no prebuilt release artifacts. What *is* portable
is the output: a compiled `Program` is plain bytes you can store and
ship anywhere, and the evaluation side's requirements (wasmtime + the
`cel.abi` section) are designed to support non-Bazel and non-C++
embedders eventually — designed, not built.
