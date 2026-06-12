# PBT / differential-fuzzing subsystem review — 2026-06-11

Scope: the property-based differential-fuzzing subsystem (`e2e/fuzz/`,
`testdata/cel_cpp_oracle.*`, the `Pbt*` pins in `e2e/known_bugs_test.cc`,
`scripts/fuzz.sh`, `.github/workflows/fuzz.yml`) and its docs
(`README.md`, `COVERAGE.md`, `m27`/`m30`). Conducted with three
read-only review agents (maintainability / comprehensiveness / docs)
plus direct verification. This report is the durable artifact; the
findings are woven into the m30 work queue (see §5).

## Verdict — **mixed, trending healthy**

The subsystem is in far better shape than the "unreviewable" complaint
implies: clean layering (`grammar` → `generator` → `oracle_harness`
→ `compare`), genuine honesty discipline (every gap, withheld
production, and over-permissive overload is pinned as a `Pbt*` test or
a triaged BLOCKED note with a cited blocker), a strong adversarial leaf
set, and an L1/L2/L3 validation ladder. The debt is concentrated and
mechanical, in three buckets:

1. **Docs tipped past their reviewability budget.** The dated
   notes-log is **61 %** of the README (lines 205–526, 17 same-day
   entries), burying ~200 lines of durable reference under a session
   journal. This *is* the "unreviewable" complaint.
2. **Mechanical duplication + drift in the code.** The mineable-target
   list is hand-synced across **four** files and has already drifted;
   the L3 test walker is a verbatim copy of the real generator; the
   comparison sextet is copy-pasted three times.
3. **Comprehensiveness blind spots that silently cost bugs.** Two
   missing leaf classes (numeric-shaped strings, exact `INT64_MIN`)
   mean four `*(string)` conversions and the int-overflow boundary are
   marked ✅ but only ever exercise the both-error branch; the
   `kBothErrored` comparison path compares *nothing*; aggregate
   targets are exercised only by the nightly sweep, not the
   `bazel test` gate.

**Top 3 to fix first:** (1) split the notes-log out of the README;
(2) add the numeric-string + `INT64_MIN` leaves (highest bug-yield per
byte changed); (3) make `kBothErrored` compare error kind.

---

## 1. Maintainability / code structure

| Sev | Area | Issue | Fix |
|---|---|---|---|
| P1 | `grammar_test.cc:393–466` (`namespace l3`) | The L3 walker duplicates `generator.cc:25–83`; the header even concedes the two can drift. | Delete the walker; have L3 call the real `GenerateExpr`. |
| P1 | `mine_divergences.cc`, `dump_samples.cc`, `fuzz.sh` | `ParseTarget` is copy-pasted and **already drifted** (dump is a strict subset of mine); `ALL_TARGETS` in `fuzz.sh` is a third hand-synced copy. | Extract a shared `targets.{h,cc}` (`AllTargets()` + `ParseTarget()`); generate `fuzz.sh`'s list from `--list-targets` or assert-equal in a test. |
| P1 | `grammar_scalars.cc` (557 lines) | Single TU spanning 6 feature families (arith/string/format/math/temporal/conversions); stretches "one logical unit per TU". | Split by family into `grammar_{arith,strings,math,temporal,conversions}.cc`, each exposing `RegisterX(GrammarBuilder&)`. |
| P1 | `RegisterBoolProducers`, `RegisterMathExt`, vocab loops | The comparison sextet (`eq/ne/lt/le/gt/ge`) is copy-pasted 3× (numeric/lex/temporal); math ops are hand-written. | `RegisterComparisons(b, type, ordered)` helper + a `static const MathOp[]` table fed to a `TableRegister`. ~120 lines → ~30 + tables. |
| P2 | `generator.cc:71–81` (`%i` subst) | Iterative one-slot-at-a-time `StrReplaceAll`; safe only because no format string emits `%`+digit — an unenforced invariant. | Single-pass: build the full `{"%0":a0,…}` vector, one `StrReplaceAll`. |
| P2 | `grammar.cc:70`, `grammar_aggregates.cc:36,52` | `new`-and-leak singletons violate the "no raw new/delete" rule. | Function-local `static const std::vector<…>` returned by const-ref. |
| P2 | `grammar_test.cc` (635 lines) | L2/L3 logic duplicated between scalar and aggregate sections. | `TEST_P` over `{scalar, full}` grammar builders. |
| P2 | `compare.cc:35,54,109` | Closed-enum `default:` arms return a fallback instead of `ABSL_CHECK(false)`; `ExpectedKind`'s silent `kUnknown` could mask a newly-emitted kind. | Document the legitimately-open ones; CHECK the closed ones. |
| P2 | `grammar.cc:114` | `i < 10` arity ceiling is a magic number; `Repeated(arity>10)` escapes the phantom-placeholder check silently. | Named `kMaxArity` shared with `GenerateExpr`; CHECK in `Repeated`. |
| P2 | `BUILD.bazel:56,69` | Stale "Slice B/C" milestone vocabulary in comments. | Reword to role-based when next touched. |

**Coverage gaps in the fuzz code's own tests:** the weight-0
non-leaf fallback (`generator.cc:59`), the `mine_divergences`
exit-code contract (the CI gate itself), and the `kSourceTooLarge`
path are untested.

## 2. Comprehensiveness — ~67–72 % of 241 overloads generated

No overload marked ✅ in COVERAGE.md was found un-generated (the
checkmarks are honest). The misses cluster:

**Blocked families (59 overloads, correctly triaged):** net_ext (20)
and optionals (14) need a `shared/type.h` opaque/optional kind — a real
compiler change; cross-type numeric comparison (24) is dyn-only, out of
the static subset by design; `type()` (1).

**"Almost-done" gaps inside ✅/🟡 families (the actionable yield):**

| Gap | Severity | Bug class left undriven |
|---|---|---|
| Numeric-shaped string leaves (`"42"`,`"3.14"`,`"  3.14  "`,`"+5"`) | **P0** | success path of `int/double/uint/bool(string)` — ✅ today but only the both-error branch fires; `DoubleFromStringRejectsWhitespace`, leading-`+`. |
| Exact `INT64_MIN` leaf (`-9223372036854775808`) | **P0** | `negate_int64`/`int(double)`/`math.abs(int)` two's-complement overflow at the boundary the catalog dodges by one. |
| `kBothErrored` compares nothing (`oracle_harness.cc:224`) | **P0/P1** | wrong-error-kind agreement: we raise div-by-zero where cel-cpp raises overflow, etc. The single biggest comparator blind spot. |
| CI property test covers only the 6 scalar targets (`cel_oracle_property_test.cc:183`) | **P1** | list/map/nested aggregate codegen has no `bazel test` gate — only the nightly sweep. |
| Two-arg pos/limit string forms (`indexOf(sub,pos)`, `split(sep,n)`, `replace(old,new,n)`) | **P1** | `indexOf(sub,pos)` resurfaces a *known live* codepoint-vs-byte bug. |
| `divide_double`, `add_list`, temporal `+`/`-` | **P1** | three distinct codegen arms; temporal arith exercises the overflow-error path the oracle is configured for. |
| Regex-metachar + one large (>10 KiB) string leaf | **P1** | `matches()` totality assumption; length-prefix paths. |
| Multi-entry map literals (constructors are 1-entry only) | **P2** | map iteration-order / duplicate-key / collision codegen at width > 1. |

**Leaf set is the strongest part** — INT64/UINT64 boundaries, 2^53±1,
±0.0, denormal, 1e308, embedded-NUL, multi-byte/invalid UTF-8 are all
present. The two P0 gaps above are the notable holes.

## 3. Docs / design coherence

| Sev | Doc | Issue | Fix |
|---|---|---|---|
| P1 | `README.md` §Notes log (L205–526) | 61 % of the file; buries the reference half. | Move to `SESSIONS.md`; leave a pointer. |
| P1 | `testing-checklist.md` L2903–2925 | No "Rewrite M30" section; M27 section still names retired `grammar_slice_b.{h,cc}`; m30 §4 promised the tick, never did it. | Add M30 section; fix filenames. |
| P1 | `README.md:102–103` | `Targets:` lists 11 but the miner ships 13 (`list_list_int`, `map_string_list_int` missing); contradicts the file's own M30.F prose ("all 13"). | Append the two nested targets. |
| P2 | `README.md:16–21`, `COVERAGE.md:33,51` | Stale "queue" claims — math_ext/timestamp/encoders listed as ⬜ but shipped; COVERAGE summary lags its own detail (string/bytes ordering). | Trim to genuinely-open rows. |
| P2 | sibling docs | `design.md`, `per-component-test-coverage.md`, `feature-pipeline-checklist.md` never reference the PBT subsystem. | Add a PBT cross-ref to `per-component-test-coverage.md` (note `cel_oracle_property_test` is `manual`-tagged). |
| P2 | `m30` §5 | "Future work" empty though most slices shipped. | Populate with the open slices. |
| P2 | `known_bugs_test.cc` | 8 `Pbt*` tests are exemplary individually but there's no single inventory; README "Trophy case" uses informal names that don't match the test names. | Add a "PBT-found bugs" table to COVERAGE.md mapping `Pbt*` → status → fix-layer. |

**Proposed doc structure:** README becomes pure reference (~205 lines);
`SESSIONS.md` holds the chronological journal (appended to, not the
README); COVERAGE.md gains the PBT-found-bugs table beside the coverage
grid.

---

## 4. What is genuinely good (keep)

- The `MakeEntry` activation schema↔value desync `ABSL_CHECK`
  (`oracle_harness.cc:158`) — exemplary single-source-of-truth.
- Error-ness as a 5-way compared dimension (`GenAndEvalStatus`).
- The `GrammarBuilder` shorthand vocabulary — the right abstraction;
  placeholder/arg-count consistency becomes a type-system property.
- `Compare`'s type-driven recursion with a never-agree default for
  unsupported types (a silent gap can't masquerade as agreement).
- The honesty discipline: every withheld production is pinned.

## 5. Work queue (woven into m30 "Pre-close cleanup")

**Done in the follow-up commits to this review:**
- [ ] Docs reorg: split `SESSIONS.md`, fix README target list + stale
  queue claims, COVERAGE summary/detail reconciliation + PBT-bug table,
  testing-checklist M30 section + filename fix, m30 Future-work.
- [ ] Comprehensiveness P0 leaves: numeric-shaped strings + `INT64_MIN`
  (mine; pin any surfaced divergence).
- [ ] Code structure: shared `targets.{h,cc}` registry; `%i`
  single-pass; `new`/leak → function-local static; L3 walker → real
  generator.

**P2 / tracked for later (cleanup-backlog):**
- `kBothErrored` error-kind comparison (P0/P1 — larger; comparator
  change with its own test matrix).
- list/map/nested `FUZZ_TEST` registrations (P1 — close the CI gate
  gap).
- `grammar_scalars.cc` family split; comparison/math table helpers.
- Two-arg pos/limit string forms; `divide_double`/`add_list`/temporal
  arith; regex-metachar + large-string leaves; multi-entry maps.
- net_ext + optionals: schedule the `shared/type.h` opaque/optional
  type-vocabulary extension (largest single coverage hole, 34
  overloads, genuinely blocked).
