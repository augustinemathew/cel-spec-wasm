# 2026-06-09 — Production-readiness & docs assessment

Status: review report — assessment executed 2026-06-09 on
`m28-configurable-linking` @ `4235653e` (contains all of
`origin/master`). Three parallel deep-dive passes (code/tech-debt,
docs audit, first-time-adopter walkthrough) + full test suite + a
docs/examples overhaul executed the same day (see §6).

**Verdict: mixed — engineering substance is strong; first-touch
surfaces were undermining it.** `bazel test //...` is green, the
architecture and test discipline are genuinely production-grade
(layering enforced at analysis time, stub/skip discipline, oracle
tests, pinned known-bugs registry). But until this session every C++
snippet an adopter would paste first was broken or referenced invented
API, and two P0 correctness items block an honest "production ready"
claim.

## 1. P0 — blocks a credible production claim

1. **Untrusted expression source can crash the host process**
   (cleanup-backlog #16). A literal ~10k-element list compiles fine
   and panics the wasmtime store at Eval (arena OOM → `arena_alloc`
   returns 0 → unchecked → trap that surfaces as a Rust panic, not a
   caught error). Until fixed, expression source must be treated as
   semi-trusted. Fix shape: graceful OOM out of `arena_alloc`
   consumers + a capacity pre-flight at Compile/Plan.
2. **Comprehension over an unknown range returns a silently wrong
   answer** (cleanup-backlog #14): `xs.exists(...)` with `xs` unknown
   returns `false` instead of unknown — a 3VL soundness gap, oracle-
   confirmed against cel-cpp. Pinned by skipped tests in
   `e2e/m2_partial_eval_test.cc` / `known_bugs_test.cc`.
3. **Codegen re-derives semantic decisions** (known-issues #1/#2):
   `MaybeRepickCrossNumericOverload` (expr_lower.cc:770-1092)
   re-decides overloads codegen-side against design.md §5.3, and
   CelValue ABI constants are hand-copied magic numbers
   (expr_lower.cc:1064, expr_lower_comprehension.cc:612). Silent-drift
   risk class, not a today-bug.

## 2. P1 — fix before promoting the repo

- **No CI gate**: `cloudbuild.yaml` runs `bazel build` only — no
  tests, no conformance, and **no `fetch_cel_cpp.sh` step** (a fresh
  cloud build should fail module resolution). No `.github/workflows/`.
- **No versioning**: no tags, no `version` in `module()`, no
  CHANGELOG. Adopters can't pin anything.
- **String-returning `@component` functions trap**
  ("cannot leave component instance", inside libc++ under the
  wasi-preview2 stubs; pinned by the GreetRoundTripsString skip).
  Scalar paths work — `examples/09_component_functions` demonstrates
  the working envelope. Empirically re-confirmed this session;
  custom IDL `Module` names work fine (tested `Module greeter`).
- **Host-fn error messages don't survive the wasm round-trip**
  (found this session via `examples/08`): a callback returning
  `Value::Error({code, message})` comes back as
  `"runtime error code N"` — the payload `message` is dropped at the
  boundary; only the code is wired. e2e tests only asserted the kind,
  so it went unnoticed.
- **`HostMapView` has no key-enumeration API** (only
  Size/Get/ContainsKey) — and the host-fn guide papered over it with a
  fictional `ForEach`. Either add enumeration or keep the docs honest
  (docs fixed this session; API gap remains).
- **No `Value::AsProto<T>()`** — the documented-but-invented typed
  proto accessor. The real path goes through `MessageBacking()` +
  an internal-visibility header. Add a public accessor.
- **`Engine::AddComponent` takes `FunctionLibrary`, whose target is
  `//:internal`** — a public API method whose parameter type external
  code can't legally depend on. (Surfaced wiring `examples/09`, which
  needed `//examples/...` added to the internal package_group.)
- **`cel run` doesn't exist** — the CLI can `compile` a `.wasm` it
  cannot itself execute; the core "compile once, run anywhere" pitch
  has no CLI proof. (Design already exists: cel-cli-design.md.)
- **No fuzzing** (parser, abi_decode, malformed Program bytes), no
  non-Bazel consumption story (no install/CMake/prebuilt binaries).

## 3. P2 — cleanup-when-touched

- Stale header comments that contradict shipped reality:
  `eval/engine.h` AddComponent "Status: not yet implemented" (it is
  implemented), `compiler/compiler.h` "storage only until Slice C.3".
- `wit_emitter.cc` hardcodes `world customfn` (works, but the
  module-name/world asymmetry is a latent confusion).
- lint backlog at ~123 warnings (function-size dominated); lint PCH
  noise (`google/protobuf/message.h not found`) on exec-config TUs
  still pollutes output.
- `Program::FromWasm` referenced in program.h's docblock; actual API
  is the `Program(std::vector<uint8_t>)` ctor.
- 128 KiB default arena is undocumented as a workload envelope
  (what fits? no guidance beyond "raise it").

## 4. Docs audit summary

Full inventory was taken (112 files). Key conclusions:

- **Not a true graveyard** — milestone docs under
  `implementation-plan/rewrite/` are mostly properly closed out with
  status headers; the convention is good. The "graveyard" feel came
  from: scattered adopter docs, a few stale headers (phase-c-*,
  design.md), and three actively-misleading entry docs.
- **Actively misleading (fixed this session, see §6)**:
  `doc/compiler-overview.md` (described the dissolved compiler_v2
  layout), `doc/README.md` ("cel2", dead link, stale conformance
  numbers), `MAINTAINERS.md` (upstream cel-spec council, not this
  repo's maintainers), README C++ snippet (did not compile),
  host-fn guide §1.5 (invented API), component guide §2.4 (broken
  snippet), `cel --help` (milestone jargon).
- **Structural recommendation (not executed — needs owner sign-off):**
  split `doc/` into adopter-docs vs contributor-docs trees, move the
  live checklists under a contributor entry point, and consolidate
  research/probe findings under `rewrite/research/`. Deferred because
  CLAUDE.md and scripts cite current paths.

## 5. What "super legit" still needs (priority order)

1. CI visible on GitHub (build + test + conformance, Linux + macOS) +
   badge; fix cloudbuild (fetch step, run tests).
2. Fix P0 #1 (arena OOM → graceful error) — it's also the security
   story's biggest asterisk.
3. Tag v0.x + CHANGELOG + `module()` version.
4. `cel run` — closes the CLI proof loop.
5. Error-message round-trip (P1) — error UX is policy-engine table
   stakes.
6. `BindFunction` ergonomic registration (in flight, this session).
7. Release tarball or documented non-Bazel path.

## 6. Executed this session (2026-06-09)

- **`examples/`** — nine runnable, smoke-tested examples (01 hello →
  09 sandboxed component fn), public-API-only deps,
  `:examples_smoke_test` asserts each one's documented output on every
  sweep. Examples doubled as verification: they caught the
  error-message round-trip loss (§2) and re-scoped the component
  string-trap envelope.
- **README.md** rewritten: compile-verified embed snippet, honest
  two-sided perf tables kept, "(per Claude)" removed, persona-routed
  docs section, status framed as beta with a Production-readiness
  section.
- **`doc/user-guide/getting-started.md`** — new, snippet-first.
- **`doc/user-guide/faq.md` + `security-model.md`** — new (agent,
  claims verified against code).
- **Stale-doc cleanup** (agent): doc/README.md router rewrite,
  compiler-overview.md rewrite, phase-c/design.md status headers,
  MAINTAINERS.md, tools/cel README `-o` fix, cel.cc usage-text jargon,
  user-guide truth pass (§8.4/8.5 contradiction, invented APIs).
- **`Engine::BindFunction` shipped**: register a host fn with the
  same `.celfn` decl string the compiler saw — no hand-spelled
  overload-id mangling, no `num_args+1` ABI leak, lambda signature
  validated against the declaration at registration (param index +
  both types named on mismatch). 31 engine_test cases + 17
  typed_function_test cases + one e2e proof; green in both link
  modes. `examples/04` and the host-fn guide now lead with it.
- **Stale public-header comments fixed**: compiler.h LinkMode
  ("kDynamic — default" → kStatic is the default; ~800 KB → ~1.1 MB
  measured), program.h (`Program::FromWasm` → the real byte-vector
  constructor).

## 7. Follow-ups for the backlog

- cleanup-backlog: add the error-message round-trip item (§2) and the
  AddComponent/FunctionLibrary visibility wart (§2).
- per-component-test-coverage.md: examples_smoke_test is a new
  always-on gate; note it.
- The doc-tree structural split (§4) — decide and execute as its own
  slice if wanted.
