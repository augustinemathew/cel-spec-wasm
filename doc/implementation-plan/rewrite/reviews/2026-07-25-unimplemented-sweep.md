# Unimplemented-code-path sweep — silent-failure audit

Date: 2026-07-25
Scope: whole first-party tree (`compiler/`, `eval/`, `runtime/`, `shared/`,
`abi/`, `tools/`, `conformance/`, `e2e/`, `bindings/`); `third_party/` excluded.
Baseline: `194b9d00` + working tree.
Standard audited against: CLAUDE.md § "Unimplemented features" —
*"the body MUST be `ABSL_CHECK(false) << "<symbol> is a stub until
<milestone>"`. No silent fallbacks, no empty bodies, no bare `TODO`
comment without the check."*

Report only. **No code, test, or doc other than this file was changed.**

---

## Verdict: MIXED, with a live P0 cluster

The convention is well-followed for *invariant violations* — 71 loud
`ABSL_CHECK(false)` / `ABSL_LOG(FATAL)` sites plus 9 `__builtin_trap()`
sites in the runtime, and the switch-`default:` discipline is close to
uniform. It is **not** followed for the one shape that actually bites:
**a not-yet-implemented arm on a host-origin (`CEL_LIST_HOST` /
`CEL_MAP_HOST`) aggregate**, which is consistently expressed as a
`poison(TYPE_MISMATCH)` or a `return false` rather than a loud check.
The concat/join pair that triggered this audit was not an isolated
mistake; it is an instance of a **five-member family**, and three of the
remaining four produce *silently wrong values* (not even an error) today.
`grep "stub until"` over the whole tree returns **zero** hits: the exact
form CLAUDE.md mandates is used nowhere, and the canonical example the
rule cites (`StaticMemoryBuilder::AllocateList`) no longer exists.

Empirically confirmed with the prebuilt `bazel-bin/tools/cel/cel`
(rebuilt 2026-07-25 13:52, i.e. *after* the concat/join fix — `xs + [9]`
and `ss.join("-")` both pass in this binary, so everything below is the
post-fix state):

| expression | origin | result | correct |
|---|---|---|---|
| `[[1,2]] == [[1,2]]` | arena | `true` | ✅ |
| `xss == [[1,2]]` | host | **`false`** | ❌ |
| `xss == xss` | host | **`false`** | ❌ **reflexivity violated** |
| `[1,2] in [[1,2]]` | arena | `true` | ✅ |
| `[1,2] in xss` | host | **`false`** | ❌ |
| `{"a":[1]} == {"a":[1]}` | arena | `true` | ✅ |
| `mm == {"a":[1]}` / `mm == mm` | host | **`false`** | ❌ |
| `math.least([1,2,3])` | arena | `1` | ✅ |
| `math.least(xs)` / `math.greatest(xs)` | host | **`error: type_mismatch`** | ❌ |
| `"%s".format([[1,2]])` | arena | `"[1, 2]"` | ✅ |
| `"%s".format([xs])` / `"%s".format([ms])` | host | **`error: invalid_argument`** | ❌ |
| `"%s".format(xs)` | host | **`error: type_mismatch`** | ❌ |

(`xss:list<list<int>>=[[1,2]]`, `mm:map<string,list<int>>={"a":[1]}`,
`xs:list<int>=[3,1,2]`, `ms:map<string,int>={"a":1}`.)

### Top 3 to look at first

1. **F1 — nested-aggregate equality across the host trampoline returns
   `false`, including for identical operands.** `xss == xss` → `false`.
   This is worse than the concat bug: concat returned an *error* (visible),
   this returns a *plausible boolean* (invisible). Root:
   `EncodeBackingScalar` encodes every aggregate to a `CEL_ERROR`
   placeholder (`eval/internal/cel_host.cc:2399`), and
   `HostScalarSameKindEq`'s `default:` (`:2337`) compares two such
   placeholders unequal. Purely origin-dependent: the arena kernel's
   `deep_values_equal` recurses correctly.
2. **F2 — `x in host_list` returns `false` for every aggregate and every
   message element.** `BackingValueEqualsQuery`'s `default:`
   (`eval/internal/cel_host.cc:2537`) returns `false` for
   `kMessage`/`kList`/`kMap`. The asymmetry is the tell: the *equality*
   walk (`ReadHostListEqElement`, `:2642`) grew a real `kMessage` arm via
   `CompareProtoMessages`, but the `in` scan never did. So
   `friend in person.friends` over a repeated-message field is a
   permanent `false`.
3. **F3/F4 — `math.least/greatest(host_list)` and
   `"…".format(host_list)` poison instead of working.** Same shape as
   join: `runtime/cel_math_ext.c:376` and `runtime/cel_string_format.cc:351`
   accept `CEL_LIST_ARENA` only, and
   `runtime/cel_string_format_render.cc:138,155` refuse a host list/map
   nested inside an arena args list. All three are one-line origin gates
   with a `// deferred` comment next to them.

**None of these five is pinned in `e2e/known_bugs_test.cc`.** That file
has 21 `GTEST_SKIP`s, every one of them a *scalar / parser / formatting*
bug; the host-origin aggregate family is entirely absent from it.

---

## Findings table

Severity: **P0** = silently wrong answers for ordinary input;
**P1** = silent but only on unusual input, or loud but undocumented;
**P2** = cosmetic / correctly-loud-but-untracked.

| # | file:line | symbol | what's unimplemented | how it fails today | reachable from public API? | fuzzer catches it? | sev | recommended action |
|---|---|---|---|---|---|---|---|---|
| F1 | `eval/internal/cel_host.cc:2399` (`EncodeBackingScalar`) + `:2315`/`:2337` (`HostScalarSameKindEq` `default:`) + `:2680` (`ListEqElementEquals`) + `:2896`/`:2921`/`:2926` (`SnapshotMapEntries`) | list/map equality over host-origin operands | nested-aggregate element/value equality. Aggregates encode to a `CEL_ERROR{TYPE_MISMATCH}` placeholder; two placeholders then compare **unequal** | **silent wrong value** (`false`) — not an error | **Yes.** `Activation::Bind` any `list<list<T>>` / `map<K,list<T>>` / `list<map<…>>`, then `==`. Verified: `xss == xss` → `false` | **No** — `ActivationSchema()` binds `UniverseAggregates(max_depth=1)` only (`e2e/fuzz/grammar_scalars.cc:93`); depth-2 values exist solely as arena literals (`grammar_aggregates.cc:279`) | **P0** | Recurse: route aggregate elements back through the origin-agnostic walk (the arena kernel's `deep_values_equal` is the reference). Until then the arm must be a loud CHECK, not `false` |
| F2 | `eval/internal/cel_host.cc:2487`, `default:` at `:2537` (`BackingValueEqualsQuery`), used by `CelListInImpl` `:2543` | `in` over a host-backed list | element match for `kMessage` / `kList` / `kMap` backing values | **silent wrong value** (`false`) | **Yes.** `msg in x.repeated_msgs`, `[1,2] in xss`. Verified `[1,2] in xss` → `false` | **No** — no proto types in the fuzz type universe; no depth-2 host bindings | **P0** | Give `in` the same `kMessage` arm equality already has (`CompareProtoMessages`) and an aggregate arm; the shared normalizer is the right factoring. The `default:` on a closed `celwasm::Value::Kind` should be `ABSL_CHECK(false)` once the real arms exist |
| F3 | `runtime/cel_math_ext.c:376` (in `math_minmax_list`, `:371`) | `math.least(l)` / `math.greatest(l)` on `CEL_LIST_HOST` | the whole host arm — `if (list->kind != CEL_LIST_ARENA) { poison(out, CEL_ERR_TYPE_MISMATCH); }` | **silent poison** (indistinguishable from a real type error) | **Yes.** `math.least(xs)` type-checks (`math_@min_list_int` is seeded at `overload_table.cc:438`) and returns `error: type_mismatch` | **No** — the only `math_@min_list_*` production is the 3-arg macro form `math.least(%0,%1,%2)` (`grammar_scalars.cc:424-426`), which the macro always rewrites to an arena literal | **P0** | Either snapshot via `cel_list_arena_view` (the existing origin-normalizing helper, `cel_runtime.c:405`) or add a host trampoline. Same fix shape as `join` |
| F4a | `runtime/cel_string_format.cc:351` (`cel_string_format_at_vv`, `:344`) | `"…".format(args)` where `args` is a host list | the host-args arm — `args->kind != CEL_LIST_ARENA` → poison. Comment literally says *"CEL_LIST_HOST (proto-repeated) deferred — same policy as split/join"* | **silent poison** `TYPE_MISMATCH` | **Yes.** `"%s".format(xs)` → `error: type_mismatch` | **No** — every `format` production hard-codes `[%0]` with scalar holes (`grammar_scalars.cc:338-351`) | **P0** | `cel_list_arena_view` the args slot before the pre-scan loop |
| F4b | `runtime/cel_string_format_render.cc:138` (`AppendListCanonical`) and `:155` (`AppendMapCanonical`) | `%s` rendering of a host list / host map | `if (v->kind != CEL_LIST_ARENA) return false;  // host-list deferred` | **silent poison** `INVALID_ARGUMENT` (the `false` becomes a parse-ish error) | **Yes.** `"%s".format([xs])` → `error: invalid_argument`; arena twin `"%s".format([[1,2]])` → `"[1, 2]"` | **No** — no `format` production has an aggregate hole | **P0** | Same normalization. Note this is reachable even when the outer args list is a literal, so fixing F4a alone is not enough |
| F5 | `eval/internal/cel_host.cc:3171-3178` (`CelMakeMessageImpl`, `:3155`) | proto-literal construction for **dynamic** descriptors | `generated_factory()->GetPrototype()` returns null for a descriptor loaded via `SchemaProtoSource`; comment says *"Dynamic-descriptor support is a follow-up tied to the conformance harness's descriptor mode"* → `WriteWireError(CEL_ERR_TYPE_MISMATCH)` | **silent poison** — a "follow-up" wearing a spec-error costume | **Yes**, but only with `--schema` / `--descriptor_set` message types + a message literal | **No** — no message types in the fuzz universe | **P1** | Either implement via `DynamicMessageFactory` or make the branch loud. As written a reader cannot tell it from the *legitimate* `entry.descriptor == nullptr` poison 6 lines above (`:3158`) |
| F6 | `eval/internal/cel_host.cc:130` (`ResolveAndParseAnyPayload`) | `Any` unpack for dynamic descriptors | same `generated_factory` limitation | **named** error (`kFieldNotFound`, *"has no generated_factory prototype"*) | Yes, same conditions as F5 | No | **P2** | Loud-enough (the message names the cause) but shares F5's root; fix together |
| F7 | `runtime/cel_optional.c:33-37` (`cel_host_cel_set_field` weak stub) | native-build weak stub for the `cel_set_field` host import | **empty body** — `(void)msg_slot; (void)field_ref_id; (void)value_slot;` and nothing else | **silent no-op** | **No** — wasm builds import the real trampoline; only a native unit test that forgets to link a strong override hits it | No | **P2** | Sibling weak stubs in the same file (`cel_message_is_zero`, `:56`) at least poison. Make this one poison or trap for consistency |
| F8 | `compiler/celfn/celfnc_emit/cpp_codec_emitter.cc:371` (`kMapLiftTpl`) | `lower()` for map-typed foreign-fn parameters | emits `// TODO(m26): lower($1*, const $0&) not yet emitted; declare manually if needed.` **into generated user code** | bare `TODO` with no check; fails at the *user's* compile if they need it | Yes (`Engine::AddComponent` / `celfnc` users with a map param) | n/a (not a CEL overload) | **P2** | A generated artifact should emit a `static_assert(false, …)` or a stub that fails loudly, not a comment. Also violates the no-milestone-in-comments rule |
| F9 | `runtime/cel_optional.h:88` and `:143` | doc, not code | header says `ofNonZeroValue` and `select_optional_field` "trap until Slice B / E adds the host trampolines"; `is_zero_value`/`host_value_is_zero` (`cel_optional.c:127-149`) now **implements** the host arms | doc drift only | n/a | n/a | **P2** | Update the two header comments; the `cel_optional.c:403` trap in `dispatch_lookup` is still accurate and is a model positive case |
| F10 | `CLAUDE.md` § "Unimplemented features" | doc, not code | cites `StaticMemoryBuilder::AllocateList` in `compiler/codegen/static_memory_builder.cc` as "the canonical form"; that symbol no longer exists anywhere in `compiler/` | the rule has no live exemplar; `grep "stub until"` = 0 hits repo-wide | n/a | n/a | **P2** | Repoint the rule at a live example — `dispatch_lookup`'s host arm (`runtime/cel_optional.c:390-403`) is the best one in the tree |

### Explicitly checked and judged **legitimate** (not stubs)

Recorded so a future sweep does not re-litigate them:

- `runtime/cel_runtime.c:105,162,474,508,539` (`cel_map_insert*`,
  `cel_list_append_at*`) — `kind != CEL_*_ARENA → return` is the
  documented *"error sticks"* no-op for a poisoned accumulator. The
  accumulator is always codegen-built arena; a non-arena kind means
  already-poisoned, and a silent no-op is the intended 3VL behaviour.
- `runtime/cel_runtime.c:785,881,918,988,1015,1029,1085` — these are the
  `*_arena` inner kernels, reached only after the dispatchers at
  `:1206-1360` have already routed `CEL_*_HOST` to a host trampoline. The
  poison is genuine defence-in-depth.
- `runtime/cel_map_index.c:157` — a host map has no arena index to build;
  the no-op is correct.
- `eval/internal/cel_host.cc:502, 1051, 3367, 3630, 3655, 4126, 4165,
  4280, 4305, 4343, 4530, 4601, 4627, 4675` — `default: return
  std::nullopt` in a chain-of-responsibility dispatch; each chain
  terminates in a real `ABSL_CHECK(false)` (`:3733`, `:4241`, `:4378`,
  `:4715`). Correct idiom.
- `eval/host/cel_log.cc::FormatDirective` — the sanctioned open-switch
  exception; used to calibrate. `runtime/cel_string_format_render.cc:240,261`
  are the same open-parse shape.
- All `absl::UnimplementedError` returns
  (`compiler/codegen/expr_lower.cc:1037,1044,1337`;
  `expr_lower_comprehension.cc:341,364,739`; `eval/instance.cc:398,979`;
  `eval/internal/cel_component.cc:846`; `cel_host.cc:4858`) — loud,
  named, Status-carrying. This is the *right* answer for a compile-time
  rejection and the convention is applied uniformly there.

### Needs owner confirmation

- **F5 vs. a genuine error path.** If `SchemaProtoSource`-loaded message
  *literals* are considered out of scope by design rather than "a
  follow-up", F5 is a documentation problem, not a code problem. The
  comment's wording ("is a follow-up") is what makes me classify it as a
  stub; someone who owns the descriptor-mode work should settle it.
- **F1/F2 scope boundary.** `doc/implementation-plan/rewrite/cel-host-surface.md`
  is cited in the code as defining a scalar-only contract for the host
  arms. If that document deliberately excludes nested aggregates, the
  finding is still P0 — because the *observable CEL semantics* diverge by
  operand origin, and `xss == xss` → `false` cannot be a defensible
  contract — but the fix is then a doc revision plus code, not code alone.

---

## Rule compliance

| category | count | notes |
|---|---|---|
| Loud `ABSL_CHECK(false)` / `ABSL_LOG(FATAL)` (first-party, non-test) | 71 | Spread across 20 files; overwhelmingly closed-switch invariant guards |
| Loud `__builtin_trap()` (runtime, non-test) | 9 | Includes the two model stub-traps, `cel_optional.c:147` and `:403` |
| Loud `ABSL_CHECK(false)` in the fuzz harness | 5 | `oracle_harness.cc`, `type_universe.cc` — schema-drift tripwires |
| Loud `absl::UnimplementedError` returns | ~12 | Compile-time rejections; correct and consistent |
| **Uses of the mandated `"<symbol> is a stub until <milestone>"` string** | **0** | The literal form CLAUDE.md requires appears nowhere |
| **Silent stubs found by this sweep** | **7** (F1–F5, F7, F8) | 5 of them P0/P1 and reachable from ordinary CEL |
| Bare `TODO` / `FIXME` in first-party code | 2 | `cpp_codec_emitter.cc:371` (F8) and `conformance/runner.cc:539` (upstream cel-spec TODO, not ours) |

**Is the convention trending better or worse?** Mixed, and the split is
along a clean axis:

- **Better** in the *compiler* and in *closed-enum dispatch everywhere*.
  Every `switch` over `CelKind` / `ExprKindCase` / `CelfnType::Kind` /
  `cpp_type` I sampled terminates loudly. `eval/activation.cc:30,38`
  (`BindLazy`, `OverrideFunction`) are textbook signature-final stubs
  with named CHECKs. `runtime/cel_optional.c:390-403` is the single best
  example in the tree — it traps, names the missing trampoline, *and*
  explains why the path is currently unreachable and what would light it
  up. That is exactly what the rule asks for.
- **Worse** in the *runtime + host-trampoline boundary*, and worse in a
  systematic way. Six independent sites
  (`cel_math_ext.c:376`, `cel_string_format.cc:351`,
  `cel_string_format_render.cc:138`, `:155`, plus the two already-fixed
  concat/join sites, plus `cel_host.cc:2537`/`2399`) all made the same
  choice: express "no host arm yet" as a poison or a `false`. Four of
  them carry a comment that says *"deferred"* or *"not supported in
  M12"* right next to the silent return — the authors knew, and the
  convention did not stop them. The failure mode is a *pattern*, not
  seven individual lapses, which is why the recommendation below is
  structural rather than a list of one-line fixes.

**Structural recommendation.** The rule as written ("stub → CHECK")
collides with a real constraint in `runtime/`: `cel_runtime.wasm` cannot
`ABSL_CHECK`, and trapping the whole module on a merely-unimplemented arm
is worse for an embedder than returning an error. That tension is
probably *why* the poison shape keeps winning. The fix is to make the
tension explicit rather than let each author resolve it silently — e.g. a
dedicated `CEL_ERR_UNIMPLEMENTED` code (distinct from `TYPE_MISMATCH`,
which is the whole reason the concat bug hid for a year), or a
`poison_unimplemented(out, "<symbol>")` macro that poisons in wasm and
traps in the native build where tests run. Either makes "not implemented"
greppable and un-mistakable for "your input was ill-typed."

---

## Recommended grammar extensions

Every P0 above is fuzz-invisible, and the reason is the same in all five
cases: **the coverage model is keyed by overload id, not by
(overload id × operand origin × element depth).** `e2e/fuzz/coverage_test.cc`
would report `equals_list`, `in_list`, `math_@min_list_int`, and
`string_format` all as *covered* — and they are, for arena literals. The
uncovered-list tripwire cannot see this class at all. That is worth
recording in `COVERAGE.md` independently of the productions below.

Concretely, for the next m36 slice:

1. **Depth-2 host bindings — unlocks F1 and F2 (the two worst).**
   `ActivationSchema()` (`e2e/fuzz/grammar_scalars.cc:93`) iterates
   `UniverseAggregates(/*max_depth=*/1)`; the literal catalog
   (`grammar_aggregates.cc:279`) and target set (`targets.cc:50`) already
   use `max_depth=2`. Raising the schema to 2 is a one-token change but
   adds 200 bindings (40 → 240, per `type_universe_test.cc:21-22`) and
   200 `MakeEntry` values. A **curated subset** is the better first
   step — `list<list<int>>`, `list<map<string,int>>`,
   `map<string,list<int>>`, `map<int,map<string,int>>` — which is enough
   to make `v == v` and `v == <literal>` generate for a nested host
   operand. Expected result: immediate VALUE-DIVERGE on the first
   `equals_list` / `equals_map` sample that picks a nested ident leaf.
   Add the matching `in` production (`(%1 in %0)` with a
   `list<list<int>>` hole) to cover F2.
2. **`math.least` / `math.greatest` single-list-argument form — unlocks F3.**
   `RegisterMathMinMax` (`grammar_scalars.cc:415-427`) only emits the
   unary-scalar, pairwise, and 3-arg forms; the 3-arg form is rewritten
   by cel-cpp's macro into a *literal* list, so the host path is
   unreachable. Add
   `b.Unary(t, name + "_list_arg", "math." + fn + "(%0)", CelType::List(t))`
   for `t ∈ {int, uint, double}`. The `list<int>` hole resolves to either
   the `v_list_int` ident leaf (host) or a list literal (arena), so one
   production covers both origins. Note the macro's `IsValidArgType`
   (cel-cpp `macros.cc:48`) admits idents but **not** list literals, so
   this production may only ever pick the ident — which is exactly the
   uncovered case.
3. **`format` with an aggregate hole — unlocks F4a and F4b.**
   Every production in `RegisterStringFormat` (`grammar_scalars.cc:338-351`)
   hard-codes `[%0]` around a scalar. Add both shapes:
   - `b.Unary(s, "fmt_s_list", R"("%s".format([%0]))", CelType::List(CelType::Int()))`
     and the `map<string,int>` twin → drives `AppendListCanonical` /
     `AppendMapCanonical` with a host operand nested inside an arena args
     list (F4b).
   - `b.Unary(s, "fmt_s_direct_list", R"("%s%s".format(%0))", CelType::List(CelType::String()))`
     → drives the args-slot gate itself (F4a). Confirmed type-checkable:
     `"%s".format(xs)` with `xs:list<int>` compiles today and fails at
     runtime, so the checker does accept a concrete `list<T>` for the
     `list(dyn)` parameter.
4. **Message types in the universe — would unlock F2's repeated-message
   half and F5.** Much larger lift (the universe is scalar+container
   today, and the oracle harness would need proto bindings), so this is a
   "name it, don't schedule it" item. Until it lands, the
   repeated-message `in` gap should get a `GTEST_SKIP`-pinned case in
   `e2e/known_bugs_test.cc` instead.

**Also worth adding regardless of grammar work:** an *origin-parity*
property test — for every generable expression whose holes are aggregate
typed, evaluate it twice, once with the aggregate as an arena literal and
once bound through the activation, and assert the two results are
identical. That single property would have caught concat, join, F1, F2,
F3, F4a and F4b, and it does not require the type universe to grow at
all.

---

## Cross-reference: `GTEST_SKIP` inventory

68 skips repo-wide, 16 files. Audited for "this skip is a signpost to a
silent stub":

- `e2e/known_bugs_test.cc` — 21 skips, all with a verified reason and a
  root-cause `file:line`. **Exemplary discipline**, and every one is a
  scalar / parser / formatting / limits bug. **Zero** cover the
  host-origin aggregate family, which is the audit's central point: the
  pin file did not fail to *track* F1–F4, it failed to *notice* them.
- `e2e/foreign_fn_type_matrix_test.cc` (10) +
  `demo_component_e2e_test.cc` (1) — blocked on
  `Engine::AddComponent` (`eval/engine.cc:1576`), which is a
  *documented, loud, Status-returning* not-yet-wired surface. These are
  correct per-case skips against a named blocker. Positive case.
- `e2e/m5b_test.cc` (7), `e2e/wkt_field_set_test.cc` (6),
  `e2e/host_fn_type_matrix_test.cc` (7), `e2e/slot_aliasing_test.cc` (4),
  `e2e/m9_test.cc` (2), `e2e/m4_test.cc` (1),
  `e2e/activation_boundary_test.cc` (1) — spot-checked; each names a
  concrete blocker, none is a fixture-level `SetUp` skip. No new stubs
  surfaced through them.
- `eval/internal/cel_component_test.cc` (2),
  `tools/wat_runner/wat_runner_test.cc` (2),
  `runtime/cel_runtime_wasm_test.cc` (1), `runtime/cel_list_test.cc` (1),
  `benchmark/eval/corpus_loader_test.cc` (1),
  `e2e/fuzz/grammar_scalars.cc` (1) — infrastructure / environment skips.

No skip in the tree points at F1–F5. The stubs this audit found were
found by reading origin gates, not by following skips — which suggests
the skip-discipline and the stub-discipline are not yet connected: a
silent stub produces no failing test, therefore no skip, therefore no
pin.

---

## Suggested tracking

Per CLAUDE.md § "Tracking what the review surfaces":

- **P0 → active milestone "Pre-close cleanup":** F1, F2, F3, F4a, F4b.
  Each should land with a `e2e/known_bugs_test.cc` case first (asserting
  the spec-correct value, `GTEST_SKIP`'d), so the gap is pinned even if
  the fix slips.
- **P1 → same section, lower:** F5.
- **P2 → `doc/implementation-plan/cleanup-backlog.md`, tagged
  `2026-07-25-unimplemented-sweep`:** F6, F7, F8, F9, F10.
- **Grammar items 1–3 above → the m36 fuzz milestone doc**, with the
  origin-parity property test called out as the highest-leverage single
  addition.
