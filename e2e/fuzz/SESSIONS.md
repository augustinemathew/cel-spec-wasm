# `e2e/fuzz/` session history

Chronological journal of differential-fuzzing sessions (newest first).
**Append new session entries here, not to `README.md`** — the README is
the topical reference; this file is the dated audit trail (the evidence
that each grammar surface mined clean and which bugs each session
surfaced). See [`README.md`](README.md) for how the rig works and
[`COVERAGE.md`](COVERAGE.md) for the per-overload checklist.

---

### 2026-06-12/13 — the simplification (SIMPLIFY.md executed and retired)

The folder was rebuilt around one rule — **everything is either a
catalog data row or goes through `RunOne()`** — in five
independently-landed steps, each proven pure by a byte-identical
fixed-seed `RESULT` diff (13 targets × 300 seeds, depth 5) against
the pre-refactor baseline:

1. **`verdict.{h,cc}`** — the one judge.  The classify/compare/
   render/pass-fail pipeline existed in 2.5 hand-rolled copies
   (miner switch, property-test switch, six duplicate comparators);
   now both drivers consume `RunOne` and cannot disagree about what
   a failure is.
2. **One comparator in the repo** — `compare.{h,cc}` (459 lines)
   deleted; `RunOne` compares via `conformance::CompareValue`, the
   same comparator the conformance gate scores 1973 rows with
   (semantic parity verified: identical NaN/±0.0 double algorithm,
   list/map recursion, kind-mismatch fails).
3. **One grammar, one test suite** — `generator.{h,cc}` merged into
   the engine (`grammar.{h,cc}`); `BuildScalarGrammar` deleted;
   `grammar_test.cc` collapsed to a single L1/L2/L3 ladder whose L3
   drives the REAL `GenerateExpr`.  Bonus: all 10 pre-existing
   clang-tidy warnings in `grammar.{h,cc}` cleared.
4. **Catalogs by family** — `grammar_scalars.cc` /
   `grammar_aggregates.cc` (session-accretion order) became
   `catalog_{leaves,ops,strings,temporal,aggregates}.cc` +
   `catalog.{h,cc}`; bodies moved verbatim by scripted extraction.
   `BuildGrammar` keeps the exact historical registration order
   (order is generation-affecting — append at the end, never
   re-sort).
5. **Closeout** — all 13 targets (aggregates included) now have
   CI-gated fuzztest properties (~13k iterations green);
   `mine_divergences --list-targets` is the canonical list and
   `fuzz.sh` derives its sweep from it (the last hand-synced copy
   deleted); `fuzz.sh kill` is now checkout-scoped (multi-agent
   safety).

Deferred (tracked in m30 §5): intra-family table consolidation
(comparison sextet ×3, math table) — the move stayed pure instead.

### 2026-06-11 — exact INT64_MIN leaf → found + FIXED a real modulo bug

- Added the exact `INT64_MIN` leaf (`int_min` =
  `(-9223372036854775807 - 1)` — can't be a plain literal, the
  magnitude overflows int64 at parse time). This is the
  two's-complement asymmetry boundary: `negate` / `abs` / `* -1` /
  `/ -1` / `% -1` / `- 1` all overflow here.
- **REAL BUG FOUND + FIXED:** `INT64_MIN % -1` returned `0` in our
  runtime (`cel_arith.c` `cel_int_mod_at_vv`), on a comment-asserted
  "cel-cpp returns 0" assumption. The oracle proved cel-cpp **errors**
  (integer overflow), confirmed in cel-cpp source (`CheckedMod`,
  `internal/overflow.cc` — the implied division `INT64_MIN / -1`
  overflows). Classic "codebase comment says A, oracle says B → oracle
  wins." Fixed to poison `CEL_ERR_OVERFLOW`, mirroring divide. Live
  regression `PbtModuloInt64MinByNegOneOverflows` (e2e, both link
  modes) + corrected kernel test `IntModIntMinByNegOnePoisons`.
- Every other INT64_MIN overflow case (negate/abs/mul/div/sub) already
  agreed (both-error). Post-fix, int/uint/double mine clean at d6.
  The overflow surfaces as a CEL error VALUE (3VL kError), not a
  failed status — the e2e assertion checks `v->IsError()`.

### 2026-06-11 — numeric-shaped string leaves (conversion success path)

- Added oracle-agreeing numeric string leaves (`"42"`, `"3.14"`,
  `"-7"`) to `RegisterLexicalLeaves`. Previously every string leaf was
  non-numeric, so `int`/`uint`/`double`/`bool` of string only ever hit
  the **both-error** branch; now the parse **success** path is fuzzed
  (`int("42")`==42, `double("3.14")`==3.14, `int("-7")`==-7,
  `uint("42")`==42u — all confirmed against our pipeline + oracle).
- Deliberately withheld the whitespace (`"  3.14  "`) and leading-`+`
  (`"+5"`) forms: they trip our over-permissive parse vs cel-cpp,
  already pinned `DoubleFromStringRejectsWhitespace` /
  `IntFromStringLeadingPlus` / `UintFromStringLeadingPlus`. Emit only
  what the oracle can correctly judge.
- Mined int/uint/double/bool/string at d5 (400 seeds each): **0
  divergences**. grammar_test (L1/L2/L3) green.
- From the 2026-06-11 subsystem review's P0 comprehensiveness queue
  (`reviews/2026-06-11-pbt-subsystem.md`). Next: exact `INT64_MIN`
  leaf, then `kBothErrored` error-kind comparison.

### 2026-06-11 — timestamp `_with_tz` accessors + substring boundary bug

- `RegisterTimestampAccessors` (split out of `RegisterTemporal` for the
  function-size gate): every `getFullYear`/`getMonth`/`getDate`/… now
  also has a `_with_tz` production with a fixed valid timezone baked
  into the template — IANA names (`America/New_York`, `Australia/
  Sydney`, `Europe/London`, `Asia/Tokyo`, …) and offset forms
  (`-05:00`, `+09:30`). A baked tz exercises real cctz logic instead
  of the invalid-tz error an arbitrary generated string would yield.
  COVERAGE.md timestamp block → ✅ (no-tz + with-tz both done).
- **NEW BUG (oracle-confirmed, pinned):** two-arg
  `<string>.substring(start, end)` over-accepts the `end == size()`
  boundary. cel-cpp errors on every `end == size` slice where
  `start != end` (`'hello'.substring(0,5)`, `(1,5)` both ERROR; `(0,4)`
  and `(5,5)` are values) — a SubstringImpl off-by-one in
  `cel-cpp/common/values/string_value.cc` (~L705): the
  `size_code_points == end` early-return only fires at the top of the
  codepoint loop, which the full-length `end` never reaches, and the
  public bounds-check (L773) uses BYTE size with `>` so `end == size`
  passes through. We return the tail slice. Found mining `int` d5
  (substring nested under `lastIndexOf`); reduced to
  `"false".substring(1, 5)` → `"alse"`. Verified via a throwaway
  `SubstringProbe` in the oracle test (since removed). Pinned
  `PbtSubstringEndEqualsSizeOverPermissive`; two-arg substring
  withheld from the grammar (one-arg `substring(start)` stays).
- Post-fix sweep d6 (seed 777, 300 seeds each): bool/double/uint/
  string/int all **0 divergences**.

### 2026-06-11 — string.format (the last bug-rich grammar surface)

- `RegisterStringFormat`: `"<directives>".format([args])` with
  directive→type-matched productions — `%d`/`%x`/`%o` int, `%s`
  string, `%b` bool, `%f`/`%e` double, `%%` escape, `[%d]` literal
  text, two-arg `%d %s` / `%s=%d`. format's param is an explicit
  `list(dyn)`, which the static subset admits, so the heterogeneous
  two-arg lists type-check. Format strings never contain `%0`-`%9`
  (would collide with the grammar's `%i` placeholders).
- Mining string at d4 (50 + 200 seeds) = **0 divergences**. Notably
  `%f`/`%e` of double are clean — format's printf-style double
  formatting does NOT have the `string(double)` to_chars
  scientific-vs-fixed bug.
- The type-MISMATCHED combos (`%f` of int / `%f` of string "NaN")
  are deliberately NOT generated — cel-cpp errors on them and we
  don't, already pinned as `FormatFixedRejectsInt` /
  `FormatFixedAcceptsNanToken`. Same discipline as `int(duration)` /
  `string(double)`: emit only what the oracle can correctly judge.

### 2026-06-11 — nightly CI fuzz job (M30.F complete)

- `.github/workflows/fuzz.yml`: a scheduled (nightly) +
  workflow_dispatch job that runs `scripts/fuzz.sh validate` then
  `scripts/fuzz.sh sweep` over all 13 targets and **fails on any
  divergence**. Its own workflow (not per-PR ci.yml) so the
  minutes-long sweep doesn't gate every PR. The fuzzer is now
  protective, not just on-demand: a regression (miscompile or
  over-permissive overload) landing on master fails the nightly.
- Validated: YAML well-formed; `fuzz.sh sweep 8 3` exits 0 across
  all 13 targets; the per-target loop+exit-code logic confirmed.

### 2026-06-11 — string/bytes ordering + net_ext/optionals triage

- Added string/bytes `<` `<=` `>` `>=` (the grammar had deferred
  them on an outdated "locale handling" concern — verified `'a' <
  'b'` type-checks in our static subset). Mining bool at d4 = 0
  divergences; the multi-byte / embedded-NUL string leaves exercise
  the bytewise comparison and agree with cel-cpp. Comparison block
  now just-missing the cross-type ⊘ rows.
- **net_ext (20) and optionals (~14) triaged as BLOCKED**, not
  to-do: both need types the fuzzer's `CelType` can't represent —
  `net.IP`/`net.CIDR` opaque types and `optional<T>`. The bare-name
  net functions work in our subset (`isIP`/`ip`/`cidr`), but
  `ip(...)` yields an opaque type with no `CelType` kind. Wiring
  either requires extending `shared/type.h`'s type vocabulary (a
  compiler change), out of grammar-only scope. Marked in COVERAGE.

### 2026-06-11 — encoders (base64) + cross-type comparison triage

- Added `base64.encode(bytes)` / `base64.decode(string)`
  (`grammar_scalars.cc`). Oracle gained the encoders extension
  (`EncodersCompilerLibrary` + `RegisterEncodersFunctions`, same
  pattern as strings/math). Mining string + bytes at d4 = 0
  divergences (base64.decode on non-base64 leaves both-errors).
  COVERAGE encoders block ✅.
- **Cross-type comparisons triaged OUT** of the fuzz scope: verified
  `1 < 2u` / `1.0 < 2` / `2u > 1` all fail type-check in our
  compiler (RejectDyn) — the heterogeneous-numeric ordering
  overloads (`greater_double_int64` etc.) are `dyn`-only, outside
  the static subset by design. Marked ⊘ in COVERAGE, not a target.

### 2026-06-11 — string-ext remainder + a double-format finding

- Added `charAt` / `lowerAscii` / `upperAscii` / `trim` / `reverse` /
  `strings.quote` (`grammar_scalars.cc`) and `join` / `join(sep)`
  (`grammar_aggregates.cc`). Strings ext — no oracle change. The new
  functions themselves mine clean.
- **Found**: mining surfaced `string(double)` divergences — e.g.
  `string(4294967295.0)` → ours scientific "4.294967295e+09",
  conformant "4294967295". TWO layers: (a) the oracle's cel-cpp
  build lacks <charconv> double-to-chars and falls back to %.17g
  ("3.14" → "3.1400000000000001"), so it **cannot validate
  double→string**; (b) our wasm libc++ `to_chars(general)` emits
  scientific where conformant `to_chars` gives the shorter fixed
  form — a real toolchain bug. Pinned directly as
  `KnownBugs.PbtStringDoubleScientificForm` (asserted against the
  known-correct value, not the unreliable oracle). Removed
  `string(double)` from the grammar — the oracle can't answer it and
  double formatting is already pinned by the DoubleToString* tests.

### 2026-06-11 — conversion family

- `RegisterConversions`: cross-numeric (`int(uint)` / `uint(int)` /
  `int(double)` / `uint(double)`), the `string(x)` family, the
  fallible string→numeric / string→bool parses, and `bytes(string)`.
  Standard library — no oracle change.
- Mining all six scalar targets at d4 = **0 divergences** (int 113,
  uint 55, double 46, string 41, bytes 48, bool 38 agreed; the
  both_errored buckets are range/parse failures both engines agree
  on). No over-permissiveness this time (contrast `int(duration)`).
- Note: the current string leaves are non-numeric, so the
  string→numeric *parse* path mostly both-errors. Numeric-shaped
  string leaves (`"42"`, `"  3.14  "`, `"+5"`) — which would
  exercise the parse path and resurface
  `DoubleFromStringRejectsWhitespace` — are deferred to a focused
  follow-up so that known bug gets pinned deliberately.

### 2026-06-11 — timestamp/duration + a conformance finding

- `RegisterTemporal`: timestamp/duration leaves, the no-tz accessor
  family (getFullYear/getMonth/…/getMilliseconds → Int), duration
  accessors, eq+ordering comparisons, `int(timestamp)` /
  `string(timestamp|duration)`. Standard library — no oracle
  extension needed.
- **Conformance bug found** (the fuzzer's job): mining `int` flagged
  8 `ERROR-DIVERGE` on `int(duration(...))` — our compiler accepts
  it and returns the whole-second count, but cel-cpp REJECTS it at
  type-check (CEL's `int()` takes int/uint/double/string/timestamp,
  NOT duration; cel-cpp
  `type_conversion_functions.cc::RegisterIntConversionFunctions`).
  We over-accept because `overload_table.cc` carries a
  `duration_to_int64` seed. Pinned as
  `KnownBugs.PbtIntOfDurationOverPermissive` (GTEST_SKIP — the fix is
  dropping the overload from the checker, a compiler change); removed
  `int(duration)` from the grammar to keep it emitting conformant
  CEL. After removal, int/bool/string mine clean at d4 (0 div).

### 2026-06-11 — math_ext (28 overloads, COVERAGE.md block ✅)

- Added the whole `math.*` family (`RegisterMathExt`): abs / sign /
  ceil / floor / round / trunc / sqrt / isFinite / isInf / isNaN /
  bitAnd / bitOr / bitXor / bitNot / bitShiftLeft / bitShiftRight,
  over int / uint / double. The M30.A boundary leaves make these
  bug-rich (abs(INT64_MIN) overflows, bit shifts past 63 error,
  sqrt of a negative is NaN, round/trunc on 2^53).
- **Oracle extended** (like strings): `MathCompilerLibrary` +
  `RegisterMathExtensionFunctions` in `testdata/cel_cpp_oracle.cc`,
  else it would reject `math.abs(...)` we accept. Runtime-extension
  registration factored into a `RegisterRuntimeExtensions` helper.
  Oracle's own test still green.
- L1/L2/L3 green (grammar_test 22 tests); mining at d4 = **0
  divergences** on all four numeric/bool targets (int 179/0/21
  both_errored, uint 64/0/16, double 78/0/2, bool 61/0/19) —
  math_ext matches cel-cpp, including every overflow / range-error
  path (both_errored = agreement). First big ⬜ block checked off.

### 2026-06-11 — function inventory + fallible string forms

- **Coverage checklist created** ([`COVERAGE.md`](COVERAGE.md)): all
  241 overloads from `compiler/codegen/overload_table.cc`, grouped by
  family, each marked ✅/🟡/⬜. Made the gap concrete — math_ext (28),
  net_ext (20), timestamp accessors (23), duration (7), encoders (2),
  optionals (~14), most conversions, and half the string functions
  are NOT yet fuzzed. This is now the master queue.
- Added the deferred string forms (`replace`, `substring(start)`,
  `substring(start, end)`, `lastIndexOf`). Mining string/bool at d4 =
  0 divergences, `both_errored` from substring out-of-range (both
  engines agree on the range error). Audited the sibling kernels
  (`replace`, `indexOf`, `substring`) for the M30.D split-aliasing
  class — all safe (they finish reading the source before writing
  `out`; only `split`'s AllocList-then-reread had the bug).

### 2026-06-11 — M30.D string functions + FIRST REAL BUG FOUND

The loop's purpose realized: a new surface found a real miscompile.

- Added scalar string functions to the grammar (`contains`,
  `startsWith`, `endsWith`, `indexOf(sub)`, `matches` →
  `grammar_scalars.cc`; `split` → `list<string>` in
  `grammar_aggregates.cc`).
- **Oracle gap found and closed first.** Mining showed a burst of
  `ORACLE-REJECT` on every `split`/`indexOf` source: the oracle's
  checker/runtime registered only the standard + optional libraries,
  not the **strings extension** our compiler has — so cel-cpp
  rejected `s.split(...)` we accept. Fixed by registering
  `StringsCompilerLibrary` + `RegisterStringsFunctions` in
  `testdata/cel_cpp_oracle.cc` (the "extend the oracle, don't guess"
  rule). With the oracle aligned, the rejects became real
  comparisons — and surfaced a divergence.
- **THE BUG** (`list_string` seed=104): `('hello'+'hello').split('é')`
  returned `["\x01\x00\x00\x00…"]` (garbage) instead of
  `["hellohello"]`. Root cause: the compiler gives `split`'s output
  the same workspace slot as its **computed** string input (slot
  reuse), and `DoSplit` re-read `s->payload.s.ptr` *after*
  `AllocList(out,…)` stamped a list header over that shared slot —
  so every element pointed at the header bytes. The slot-aliasing
  bug class this suite exists to catch. The CLI literal case always
  worked (literals don't alias the output slot) — **only the
  differential fuzzer over computed receivers caught it.**
- **Fixed** in `runtime/cel_string_ext_list.cc` (capture
  `source_ptr` before `AllocList`); pinned at two layers —
  `runtime/cel_string_ext_list_test.cc::SplitOutputAliasesSourceSlot*`
  (kernel, direct slot alias) and
  `e2e/known_bugs_test.cc::PbtSplitComputedReceiverSlotAlias` (e2e,
  the exact PBT shape). Both live guards.

### 2026-06-11 — M30.C nested aggregates

- `RegisterNestedAggregates` adds one-level-nested targets:
  `list<list<{int,string,bool}>>`, `list<map<string,int>>`,
  `map<string,list<int>>` — leaf + constructors + `size()` + two
  container-iter_var comprehensions (`list<list<int>>.exists/all`).
  Every inner type is already a registered target, so L1's
  arg-reachability check passes; the recursive `compare.cc` already
  handles the verdict, so no harness change was needed.
- L1/L2/L3 green (grammar_test: 22 tests); mining `list_list_int`
  (38 agreed/0 div/2 both_errored) and `map_string_list_int` (40
  agreed/0 div) at d4 — nested codegen agrees with cel-cpp,
  including error propagation through the nesting.
- **Process-hygiene fix**: stray `mine_divergences` processes from
  earlier loop iterations were competing for CPU and inflating the
  per-seed times in the M30.B notes (~0.85 s was partly contention,
  not pure cost). `pkill -f mine_divergences` between sessions; the
  nightly job (m30.F) should bound concurrency.

### 2026-06-11 — M30.B error-producing arithmetic

- Admitted int/uint `/` and `%` (`RegisterFallibleArithmetic`).
  These error on a zero divisor — reachable from the `0`/`0u`
  leaves or a computed zero — so an error value now flows through
  comprehensions and 3VL logic, the bug class the guarded-total
  grammar structurally could not reach. Double `/` stays with the
  total ops (`x/0.0` is ±inf/NaN, a value in CEL).
- L1/L2/L3 green; mining (200 seeds × {int, uint, bool} at d4)
  shows **0 value divergences / 0 our-rejects**, with `both_errored`
  a steady bucket (int 5, uint 31, bool 18 — division/modulo by
  zero, both engines agree) — the error-classification path
  (e9ab2fc) doing its job.
- **Throughput note**: with M30.A's boundary leaves + aggregate
  literals, a depth-4 expression is already hundreds of nodes, so
  one full Compile→JIT→Eval + cel-cpp round-trip is ~0.85 s. A
  10k-seed sweep is ~2.5 h — fine for a nightly job (m30.F), too
  slow for the inner loop. Mine 200–600 seeds interactively; the
  property test's shrinker is the better deep-search tool.

### 2026-06-11 — M30.A adversarial leaves + readability refactor

- 19 boundary/unicode leaf productions landed (8236374); every one
  round-trips the real cel-cpp checker (L2) and composes (L3).
  Mining sweep across 8 targets × depths 6/8 in flight; results
  appended here when it completes.
- Folder refactored for reviewability: `grammar_slice_b/c` →
  `grammar_scalars/aggregates`, all `SliceB`/`SliceC` identifiers
  and prose renamed to role-based names, and the activation became
  schema-derived (`ActivationSchema()` + `MakeEntry()` with a CHECK
  tripwire) instead of two hand-synced lists.

### 2026-06-11 — bound aggregates + error classification + harness dedup

- **Bound aggregates live**: `xs: list<int>` / `ms: map<string,int>`
  joined the activation (gap #0a) — PBT now reaches host-origin
  aggregate codegen.  Validation: bool d6 × 2000 seeds → 1997
  agreed / 0 diverged / 3 capacity-rejects.
- **Found + root-caused**: cel-cpp's `comprehension_max_iterations`
  (10k/eval DoS guard) fired on a generated 4-level `exists_one`
  nest needing ~11k iterations (bool seed=683 d6) — the oracle now
  runs with a 10M budget (`testdata/cel_cpp_oracle.cc`) and the
  seed agrees on value.  Production note for m29: cel-wasm has NO
  eval iteration cap at all.
- **Error-ness is now a compared dimension**: `GenAndEvalStatus`
  split `kOracleErrorValue` → `kBothErrored` (agreement) vs
  `kOracleErrorOnly` (the error-swallowing detector; miner prints
  `ERROR-DIVERGE`).
- **Dedup**: the property test carried a full pre-harness copy of
  the plumbing (activation drift was the failure mode).  Now routed
  through `oracle_harness`; property depth domain raised 0..8 with
  ResourceExhausted capacity-rejects skipped.
- **New `compare.{h,cc}` (+14-case test)**: recursive type-driven
  ours-vs-oracle comparator replacing per-kind if-chains.
- Dev-tooling unblocked in passing: `refresh_compile_db.sh` no
  longer dies on the wasm32-only manual target;
  `build_lint_pch.sh` tries candidate TU flag sets until the PCH
  compiles.

### 2026-06-11 — first deep-depth mining session (gap #1 opened)

Miner runs (fastbuild, Apple Silicon), all on the full grammar:

| target | depth | seeds evaluated | agreed | DIVERGE | OUR-REJECT |
| --- | --- | --- | --- | --- | --- |
| `bool` | 7 | 889 | 881 | 0 | 8 (~0.9%) |
| `bool` | 8 | 37 (stopped at 3 rejects) | 34 | 0 | 3 (~8%) |
| `list_int` | 8 | 113 | 110 | 0 | 3 (~2.7%) |
| `int` | 8 | 230 | 227 | 0 | 3 (~1.3%) |

- **Zero value divergences at depths 7–8** (1252 agreeing samples).
  The #34 arena cliff did not reappear post-growable-arena.
- **The new ceiling is compile capacity, not eval correctness** —
  every reject is the 8192-byte static window
  (`ValidateExprStaticRegion`); rate grows steeply with depth.
  Production impact: a large policy fails to compile with no
  recourse but hand-splitting.  Feeds the m29 readiness plan.

### 2026-06-11 — baseline analysis (loop session start)

- Audited the folder against m27 + the repo; miner, harness,
  property tests, and L1/L2/L3 all exist as documented.
- The #34 blocker on deep-depth mining is gone (growable arena,
  merge 3079b37) but the depth-6 cap and mining cadence never
  caught up — opened as the first target.
- Pinned-bug bookkeeping check: all prior PBT pins are un-skipped
  (fixed) — no orphaned skips.
