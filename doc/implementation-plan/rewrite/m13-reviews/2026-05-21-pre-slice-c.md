# M13 pre-Slice-C review — 2026-05-21

## Verdict (one paragraph)

**Mixed, leaning clean.**  Slices A + B (overload-table refactor +
`.celfn` parser/library) shipped with sound code and an honest test
shell, and probes 1–5 cleanly retired the architectural risk before
Slice C touches production code.  But the design doc
`m13-custom-fns.md` was written ahead of the rename + ahead of the
probes and never re-flowed: it still names `compiler/celfn/`,
still types `FunctionLibrary` against `cel::FunctionDecl` (the
shipped class uses its own `CelfnDecl`), and still describes the
P5/host-callback shape with no entry in `wat-traces.md`.  Two
small but real build/code hygiene issues round out the list.  Top
three to look at first:

  1. **Doc-rename drift** — every `compiler/celfn/` reference
     in `m13-custom-fns.md` (§2 architecture overview, §3.7 parser
     surface, §11 testing obligations, §12 slice plan) must become
     `compiler/celfn/`.  Reader will trip on this on every
     re-read.  (P1)
  2. **`tools/celwasmc/BUILD.bazel` references a missing
     `celwasmc_v2.cc` source.**  The BUILD target is unbuildable
     today — anyone running `bazel build //tools/...`
     will fail.  (P0 if CI enforces; P1 if Slice C lands the file
     in the same commit.)
  3. **`m13-probes.md` "Probe 7" names `m13_p3_test.cc`** as its
     artifact — but `m13_p3_*` is already occupied by the
     multi-toolchain probes (C / WASI-C / Rust).  Slice-C author
     will misread this within the first minute.  (P1)

## Architectural drift

### A1. `FunctionLibrary` API differs from the design (`m13-custom-fns.md` §2.1 vs `function_library.h`)

The design (lines 246–262) sketches:

```cpp
static absl::StatusOr<FunctionLibrary> FromCelfnFile(
    absl::string_view path,
    const google::protobuf::DescriptorPool* pool = ...);
absl::Span<const cel::FunctionDecl> decls() const;
absl::Span<const FunctionBackend> backends() const;
```

The shipped class (`compiler/celfn/function_library.h:109-178`)
diverges in three load-bearing ways:

  - **No `FromCelfnFile(path, pool)`.**  File I/O is delegated to
    the CLI; the only loader is `ParseCelfnSource(string_view) →
    FunctionLibrary` (no descriptor pool — protos aren't resolved
    at parse time, just captured as FQN strings).  This is the
    right architectural call (keeps the library off the filesystem
    + off protobuf), but the design's §2.1 still describes the
    rejected shape.
  - **`decls()` returns `std::vector<CelfnDecl>`, not
    `std::vector<cel::FunctionDecl>`.**  The shipped `CelfnDecl`
    holds backend kind, fn_name, module_name, overload_id,
    num_args, params, return_type — a self-contained record.
    Conversion to cel-cpp's `FunctionDecl` happens later
    (Slice C); the library deliberately doesn't depend on cel-cpp.
    Sound, but the design doc reads as if `cel::FunctionDecl` is
    the live type today.
  - **No separate `FunctionBackend` struct.**  The backend kind +
    module_name live as fields on `CelfnDecl` itself — flatter
    and simpler.  Design's §3.7 still sketches a parallel
    `std::vector<FunctionBackend> backends;` array.

**Fix direction: doc, not code.**  The shipped shape is cleaner.
Rewrite §2.1 + §3.7 of `m13-custom-fns.md` to match
`function_library.h` verbatim.

### A2. P5/host-callback shape vs design §5.3 + §7

Design §5.3 + §7 say host-backed customs route through `OverloadTable`
with `ImportModule::kCelFn` and a `cel_fn.<helper>` wasm import.
Probe 5 (`m13_p5_host_test.cc`) validates the wasmtime-level wiring
of that exact shape (caller imports `cel_fn.length_string`; harness
binds via `wasmtime_linker_define_func`).  No drift — the probe
confirms the design.  Note for Slice C: the production `Engine` will
need to mediate this (probe wires the callback directly).

### A3. ABI `host_custom_imports[]` proto field — referenced but absent

Design §4.6 declares `cel.abi.functions.host_custom_imports[]` will
gain a `CustomFunctionEntry` shape.  The proto under
`abi/cel_abi.proto` does NOT contain this message today
(grep confirms no `CustomFunctionEntry`, no `host_custom_imports`).
This is **expected for Slice A/B** (proto landing is Slice C work
per the slice plan), but worth noting so reviewer doesn't try to
verify §4.6's claims against the proto today.

**Fix direction: neither.**  Currently consistent with slice
plan; Slice C lands the proto.

## Tech-debt inventory

### T1. `tools/celwasmc/BUILD.bazel` references a missing source — P0/P1

`tools/celwasmc/BUILD.bazel:5-8` declares:

```python
cc_binary(
    name = "celwasmc_v2",
    srcs = ["celwasmc_v2.cc"],
    ...
)
```

…but `tools/celwasmc/celwasmc_v2.cc` does not exist
(directory contains only `BUILD.bazel`).  The target is marked
`tags = ["manual"]` so `bazel test //...` won't catch
it, but any `bazel build //tools/celwasmc:...` fails.
Either land the placeholder source in Slice C (likely intent) or
remove the target until then.  Effort: 1 commit either way.

### T2. `tools/cel/BUILD.bazel` deleted but referenced in 9+ design docs — P2

`git status` shows `D tools/cel/BUILD.bazel`; the new home is
`tools/celwasmc/`.  But docs still name the old path:
`design.md`, `m1-scalar-pipeline.md`, `m5-comprehensions-followon.md`,
`two-phase-runtime-isolation.md`, `feature-pipeline-checklist.md`,
`per-component-test-coverage.md`, `m2-ident-select-unknowns.md`,
`m13-custom-fns.md` (§6 line ~1335), and `compiler/README.md`.
Most are historic; flag-only-stale-bash-examples in the M-numbered
plans are low-priority, but the per-component-test-coverage row
"`//tools/cel:celwasmc_eval` (smoke)" is load-bearing.
Effort: one `sed` pass + spot-check, ~30 min.

### T3. `overload_table.h` NSDMI `= {}` everywhere with NOLINTs — P2

The header has 12 `= {}` brace initializers with paired
`NOLINTNEXTLINE(readability-redundant-member-init)` comments
(`overload_table.h:65,80,103,164,166,169,172,174,177,226,228,230,232,234`).
These work around a PCH-loading bug per CLAUDE.md's "PCH NOT
loading is silent" callout — the warnings ARE the right tradeoff
right now, but the volume signals the PCH may be regressing for
this file specifically (or the `= {}` pattern crept beyond
strictly necessary).  Effort: rerun `scripts/lint.sh` after
verifying PCH loads; if the warnings disappear, drop a NOLINT
band-aid.  Low priority; the file works.

### T4. Stale comment in `compile.cc:364` — P2

`compile.cc:364` reads:

```cpp
// Build the OverloadTable (built-ins only; embedder custom
// functions land via `RegisterFunction` — see
// `rewrite/m-custom-fns.md`) and install one wasm import per
```

`rewrite/m-custom-fns.md` is superseded by `m13-custom-fns.md`
(its own front-matter says so).  Update to point at the current
doc when next touching this file (Slice C is the natural moment).

### T5. `UsedImports()` drops module_name — P1 (must-fix-before-Slice-C-codegen)

`OverloadTable::UsedImports(used_ids)`
(`overload_table.cc:707-718`) returns `vector<pair<ImportModule,
string_view>>` — drops the `module_name` field that
`ImportModuleName(const OverloadImpl&)` needs to distinguish two
foreign aliases.  Today this is unused (every caller goes through
`LookupById` instead — see `InstallOverloadImports` at
`compile.cc:213-229`), but the API is misleading: a caller using
`UsedImports` to walk the kUserModule rows would lose the alias
distinction.  Either (a) extend the return type to carry the
`module_name`, or (b) delete `UsedImports` if it has no live
consumer.  Effort: 30 min; do before Slice C wires customs into
`InstallOverloadImports` for real.

### T6. `kCelFn` enum variant lives in `ImportModule` but no seed uses it — P2

`overload_table.cc` has no `kCelFn` seeds (it's reserved for
Slice C's customs).  Code path is exercised only via
`RegisterCustom` in tests today.  Acceptable for now (per CLAUDE.md
"Don't introduce stubs for later milestones without being asked",
the existence of the enum variant is fine because it's used by
real tests; just no production data flows through it yet).  Note
for Slice C: the first real `cel_fn.*` import will exercise this.

### T7. `m-custom-fns.md` checklist content still lists stale `compiler/functions/` paths — P2

Despite the superseded banner, the body still has bullet items
like `[ ] compiler/functions/function_set.proto` (not even
`compiler/`).  Most readers will stop at the banner.  Effort:
delete the body or shrink it to the breadcrumb-only header.

## Coverage gaps

### C1. `InferHelperArity` has no dedicated test — P1

The function (`overload_table.h:99` / `overload_table.cc:51-99`)
holds the source of truth for built-in helper arity and a 15-row
special-case dispatcher table.  No `overload_table_test.cc` case
directly tests it: every existing case asserts on the round-trip
through `RegisterCustom`/`LookupById`.  The longest-suffix-first
ordering (`_at_vvvv` before `_at_vvv` before `_at_vv` before
`_at_v`) is a known foot-gun explicitly called out in the
function's comment — that ordering needs at least one regression
test (e.g. `cel_string_replace_n_at_vvvv` resolves to arity 5,
NOT arity 2).  Effort: 4 new TEST_F cases, ~20 min.

### C2. No test for `OverloadImpl::module_name` round-trip through `Build()` — P1

`overload_table_test.cc` has `RegisterCustomLandsInCelHostNamespace`
+ test cases for collisions, but **no case** registers
`ImportModule::kUserModule` with a non-empty module_name and
asserts `ImportModuleName(impl) == "rules"` after `Build()`.  Slice
C's `cel_fn.*` work is downstream of this; Slice D/E's foreign
backends depend entirely on it.  Effort: 2 new TEST_Fs (one for
kUserModule lookup, one for two-aliases-same-helper), ~15 min.

### C3. `function_library_test.cc` missing edge cases vs design §3.3 — P2

The shipped tests cover the canonical positive cases + the headline
v1 cross-foreign-boundary rejection.  Missing per design §3.3:

  - `this` modifier with empty param list → no test (vacuously
    well-formed; verify the validator doesn't fire).
  - `Module foo;` directive but zero CEL-defined decls → no
    explicit test (currently accepted as no-op; verify intentional).
  - Two foreign decls with the **same** signature under the same
    alias → no test (`AddForeign` doesn't dedupe; rejected by the
    overload-id collision check downstream).
  - Reserved keyword used as identifier (e.g. `bool @host.list(int x)`)
    → no test (ANTLR grammar rejects; worth asserting the error
    message is useful).

These are P2 because the parser tree won't change between Slices B
and C — they can land any time.

### C4. No `wat_runner` integration for `m13_p5_caller.wat` — P2

The standard cadence for new WAT (per CLAUDE.md "WAT-first") is:
write WAT → assemble → run through `wat_runner` → land in
`wat-traces.md` → write the codegen arm.  `m13_p5_caller.wat`
exists but `wat_runner_test.cc` has no entry for it and
`wat-traces.md` has no §-section.  This is reasonable for a probe
that's already running through a dedicated host harness
(`m13_p5_host_test.cc`), but it breaks the WAT-traces invariant
the rest of the repo follows.  Decide: either add the trace
+ wat_runner case (consistency win), or document the
probe-WATs-skip-wat_runner exception in `wat-traces.md` (one-line
prologue).  Effort: 30 min either way.

### C5. Tests are all `tags = ["manual"]` — expected, not a gap, but flag for Slice C

Every `compiler/celfn/...` and `compiler/probes/m13_custom_fns/...`
target is `tags = ["manual"]`, so `bazel test //...`
will not catch regressions.  This is correct for probes (per the
BUILD.bazel comment) but UNTAGGING `function_library_test` and
`celfn_parser_probe_test` before Slice C ships is critical — those
are the load-bearing unit tests for a production library.  Per
CLAUDE.md "M2 silently shipped half-done with 29 skipped tests" —
manual-tagged production tests are how the same failure mode
recurs.  Tracking item for Slice C closeout.

## Doc drift

### D1. `m13-custom-fns.md` — directory rename not propagated

Stale path `compiler/celfn/` appears at:

  - line 170 — `compiler/celfn/` in architecture-overview bullet 1
  - line 604 — "`compiler/celfn/celfn_parser.{h,cc}`" in §3.7
    parser implementation
  - line 1679 — `compiler/celfn/celfn_parser_test.cc` in §11.1
  - line 1773 — `compiler/celfn/cel_body_compiler_test.cc`
    in §11.4
  - line 1853 — `compiler/celfn/` in Slice B description

Real path is `compiler/celfn/`.  Effort: `sed` + spot-check, ~5 min.

### D2. `m13-custom-fns.md` §2.1 + §3.7 describe an unshipped API

See A1 above.  §2.1's `FromCelfnFile(path, pool)`, §3.7's
`ParseCelfnFile(source, pool)`, and the parallel
`std::vector<FunctionBackend>` array all don't exist.  The
**shipped** shape — `Builder` API + `ParseCelfnSource(source)` +
self-contained `CelfnDecl` records — is what should be documented.
Effort: 30 min.

### D3. `m13-probes.md` Probe 7 cites a name already used — P1

`m13-probes.md:432` says:

> `compiler/probes/m13_custom_fns/m13_p3_test.cc` — invokes the
> Slice-A-extended celwasmc…

…but `m13_p3_*_test.cc` slots are already occupied by the Probe 3
multi-toolchain trio (`m13_p3_c_test`, `m13_p3_c_wasi_test`,
`m13_p3_rust_test`).  Probe 7 should be renamed `m13_p7_codegen_test.cc`
or similar.  Effort: 1-line edit.

### D4. `m13-probes.md` "Next steps" stale

The bottom section (lines 572-581) still says "Probe 2 lands as
soon as TinyGo is installed" and "Probe 3 is gated on Slice A" —
both shipped 2026-05-21 per the same doc.  Effort: replace with
a 3-line "next: Probe 6 (parser negative), Slice C kick-off, Probe
7/8 deferred" summary.

### D5. `wat-traces.md` M13 P1 entry — pre-constraint CEL example

`wat-traces.md:1684` models the CEL source as
`bool rules.allow(this proto(acme.User) u, string r);` — but per
the §4.5.1 constraint captured 2026-05-21, that signature is
specifically forbidden for foreign-backed customs.  The shipped
probe test actually uses `bool rules.allow(this string user_id,
string resource);` (per `m13-probes.md` §"v1 proto constraint").
The trace needs to be updated to match.  Effort: 1-line edit to
the CEL source line + a comment in the trace explaining that the
allocated CelMessage payload is "legacy from the pre-constraint
draft, retained because the slot layout is identical to a
CEL_STRING and the link-shape proof doesn't depend on payload
type."  ~10 min.

### D6. `m13-custom-fns.md` Slice plan references Slices A–F but the actual rename made Slice A's footprint smaller than written

§12 Slice A says it lands the `ImportModule` refactor + arity
move + WAT prototypes 42–46 "no codegen yet."  As shipped, Slice
A landed the refactor only; the WAT prototypes 42–46 the slice
plan names DO NOT EXIST (the new ones are `m13_p1_caller.wat`,
`m13_p1_rules_stub.wat`, `m13_p5_caller.wat`).  The numbered-WAT
naming scheme (`42_custom_host_receiver.wat` …) from §9 was
dropped in favor of the probe-numbered scheme.  §9 + §12 should
be updated to reflect the actual WAT names + the shipped probe
sequence.  Effort: 15 min.

## Summary tracking

P0 (ships-breaking, before Slice C):
  - none today (T1 is P0 only if anyone runs `bazel build
    //tools/celwasmc:...`; gate on Slice C delivering
    the source).

P1 (must-fix-before-Slice-C):
  - T1 (`celwasmc_v2.cc` missing — at minimum delete the target)
  - T5 (`UsedImports` drops module_name)
  - C1 (`InferHelperArity` test)
  - C2 (kUserModule round-trip test)
  - D3 (Probe 7 name collision in m13-probes.md)
  - D1 (`compiler/celfn/` rename in m13-custom-fns.md)

P2 (cleanup-when-touched):
  - T2 (cli/ path in old docs), T3 (PCH NOLINTs), T4 (compile.cc
    comment), T6 (kCelFn unused), T7 (m-custom-fns.md body)
  - C3 (extra parser cases), C4 (P5 WAT trace)
  - D2 (API §2.1+§3.7), D4 (probes "Next steps"), D5 (P1 CEL
    source), D6 (Slice plan WAT names)

None of the P1 items are blocking — Slice C can start in parallel
with the doc/test sweeps.  But all six should land in or before
the first Slice C commit; deferring them is how M13 starts looking
like M2 ("silently half-done with 29 skipped tests" — CLAUDE.md).

— review carried out by Claude Opus 4.7 (1M ctx) per the periodic
   code-review rule in CLAUDE.md.
