# Known-issues findings (overnight hunt)

Running log from the multi-agent bug hunt.  **Eval-reproducible bugs**
live as GTEST_SKIP regressions in `compiler_v2/e2e/known_bugs_test.cc`
(verified before landing).  This doc collects the **non-eval findings**:
design-invariant breakages, inaccurate claims, latent hazards — things
that aren't a single wrong-value expression but still matter.  Each is
the agent's claim; `[verified]` = I confirmed against source.

## Design-invariant breakages (wave 1)

1. **[HIGH] Codegen makes a semantic overload decision.**
   `expr_lower.cc:770-1092` `MaybeRepickCrossNumericOverload` inspects
   both operands' `Repr` and *overrides* the resolved `overload_id`
   (line 1091) — exactly the "no type-based helper-name derivation in
   codegen" rule design.md:196-200 forbids. Belongs in ResolvePass
   (which already has `Repr`). The `greater_equals_uint_double` vs
   `_uint64` asymmetry (line 852/864) hand-mirrors `overload_table.cc:188`
   and can silently drift. [verified — see eval probe in known_bugs]
2. **[HIGH] CelValue ABI constants hand-copied into codegen.** Bare
   magic numbers for kinds/offsets instead of `cel_data.h`:
   `expr_lower_comprehension.cc:612` (`Int32(2)`=CEL_INT),
   `expr_lower.cc:1064` (`1`=CEL_BOOL), offsets `0/4/8` at
   `expr_lower.cc:1060`. `_Static_assert`s still pass if the enum is
   renumbered → silent wrong-kind emits. No compile-time tie to the enum.
3. **[HIGH] `WrapperKindFromFqn` re-derives CelKind in codegen**
   (`expr_lower.cc:536-547`) as a bare-int ladder, a second copy of the
   wrapper-kind map already in `cel_host.cc` (`IsWrapperFqn`). Drift risk.
4. **[MEDIUM] Binaryen global state mutated in `Optimize`.**
   `module.cc:265-266` sets process-global `BinaryenSetOptimizeLevel` /
   `ShrinkLevel` before `BinaryenModuleOptimize`. Two threads compiling
   `Program`s at different levels race — contradicts the marketed
   process-wide-Engine / concurrent-compile model. Not in backlog.
5. **[MEDIUM] LayoutPass computes a dead `arena_base`**
   (`layout_pass.cc:422-423`) that design.md:312-314 itself says codegen
   no longer consults (arena is malloc-backed post-Phase-C). Stale
   `[0,8192)` offset that collides with wasi-libc static data if trusted.
6. **[LOW] `overload_id` string_view lifetime mixing.**
   `expr_lower.cc:1087-1092` re-points a copied annotation's
   `string_view` to a `constexpr` literal — safe only because today's
   substitutes are static-duration; a future temporary-sourced id dangles.
   Worth an assert/comment.

Top root cause: #1/#2/#3 are the same — codegen re-deciding type→helper/
kind facts the design says it must only read. Fix = move the decision to
ResolvePass + a generated constants header tied to `cel_data.h`.

## Eval-divergence candidates (wave 1) — being verified into known_bugs_test.cc

Confirmed-by-conformance or strong code trace; encoded as probe tests
(verify-then-skip). Listed here for the record:
- size(string) counts BYTES not code points — cel_string_ops.c:132 (size_at
  uses payload.s.len for strings). size('ÿ')→2 want 1; size('😀')→4 want 1.
- int(double) low-boundary: cel_convert.c:79 rejects only `< min` (non-strict);
  int(-9223372036854775808.0) returns INT64_MIN, cel-cpp errors.
- int('+5')/uint('+5'): cel_convert.c:203-223 mishandle leading '+'; error vs 5.
- has() on map field, dyn(double/uint) list index coercion, reserved-word &
  quoted-field map selectors, cel.bind selector, comprehension-var selector —
  all return error; conformance fixtures expect values (fields/parse/lists/
  namespace/bindings_ext .textproto).
- `[1] + []` rejected by static subset (empty-list elem = dyn); cel-cpp: list(int).

## Host / proto marshalling findings (wave 1) — need host/proto fixtures to test

- [HIGH] HostNumericCrossEq (cel_host.cc:1518-1541) casts int64/uint64→double
  for cross-type eq in host list `in`/eq + map eq — same lossy class as the
  map-key bug but in the host path; >2^53 ints collapse. Untested.
- [MED-HIGH] Host maps reject double query keys (cel_host.cc DecodeKey:600-616,
  no CEL_DOUBLE) — runtime ACCEPTS them (map_keys_equal), so host vs runtime
  diverge: `1.0 in hostMap{1:..}` → error (host) vs true (runtime).
- [MED] Any field resolution uses the Any descriptor's pool, not the containing
  msg's pool (cel_host.cc:375-376, UnpackOneAnyLayer:114-124) — dynamic-pool
  payloads → spurious kFieldNotFound.
- [LOW-MED] Value::StructurallyEquals (value.cc:244-256) compares msg/map/list
  by shared_ptr identity, not contents.

## Codegen findings (wave 1)

- [HIGH] Ternary with a bare-ident condition reads wrong address: expr_lower.cc:1050
  treats cond_ann.storage.payload as an addr, but kIdent storage payload is the
  wasm LOCAL INDEX. `b ? 1 : 2` (b:bool=true) → wrong. Needs a bound var to test.
- [MED-HIGH] cel.bind with a non-const (ident) value CRASHES the compiler:
  expr_lower_comprehension.cc:352-356 ABSL_CHECK(false) for kLocal accu_init.
  `cel.bind(x, y, x+1)` (y:int). Verify in isolation (aborts process).
- (cross-numeric == gap — encoded as CrossNumericEqualityViaDyn probe.)

## Checker / frontend findings (wave 1)

- [HIGH, wrongly-rejected] TypeParser has no `optional<T>` keyword
  (parse_and_check.cc:298-308) though OptionalCheckerLibrary IS registered —
  `v:optional<int>` var spec rejected. Library/spec asymmetry.
- [MED, wrongly-accepted/latent] UnacceptableLabel (parse_and_check.cc:373-377)
  admits any abstract type whose params are scalar w/o checking name(); only
  optional_type maps to a repr (typed_ast.cc:63) → others get kUnknown into codegen.
- [MED] IsSelectThroughAny admits has() through Any + arbitrary field names
  (parse_and_check.cc:575-622) → kUnknown repr, field_number 0. `has(a.foo)` a:any.
- [MED] ReprOf(cel::Type)→kEnum is dead; enum field reads stamp kInt
  (typed_ast.cc:54-71 has no enum arm) — enum/int indistinguishable downstream.

## Inaccurate claims (wave 1)

- [MED] README §7 (README.md:465-468) describes the DELETED pre-Phase-C arena
  ("bottom ~16 bytes reserved for arena cursor ... rest is the bump arena") —
  arena is now a 64 KiB malloc buffer, cursor in BSS (cel_arena.c). Lone stale
  authoritative doc (design.md is correct). NOT the known wat-traces.md drift.
- [MED] design.md:17 + cel_layout.h:22-28 say "min ~4 pages" / "3-4"; the
  enforced floor (cel_layout.h:29, engine.cc:321 ABSL_CHECK_GE) is 2 — guard
  weaker than the doc claims.
- [LOW] cel_string_ext.h:2 "13 ... functions" miscount (12 kernel families /
  11 cel-cpp non-format); also calls `trim` a strings-ext fn (it's not).

## Wave 2 additions (encoded in known_bugs_test.cc)

- Max-range timestamp construction rejects valid nanos at the MAX second:
  cel_time_parse.cc:178 / cel_time.c:46 (`seconds==MAX && nanos>0`).
  `string(timestamp('9999-12-31T23:59:59.999999999Z'))` -> overflow.
- double->string is NOT shortest-round-trip: cel_convert.c:579-724.
  string(123.456)->"123.45600000000000306"; string(1e10)->"10000000000"
  want "1e+10"; -0.0->"0" want "-0"; Inf->"+Inf" want "inf".

## ROOT CAUSE: the map-select cluster is ONE bug (verified)

HasOnMap{Present,Absent}Key, ReservedWordMapSelector, CelBindSelectorOnBoundVar,
ComprehensionVarSelector, MapFieldSelectSugar (+ backtick/nested/var map
selects) ALL share one cause: EmitKSelect (expr_lower.cc:199-261) has no
map-operand branch, so `m.k` (== m['k']) lowers to the proto field-read
trampoline which rejects non-message operands (cel_host.cc:1394 ->
CEL_ERR_TYPE_MISMATCH). Codegen gap, not checker/IR. One fix flips the cluster.

## Proto field-setter findings (wave 3) — conformance-verified; need proto fixtures to encode as e2e

REAL BUGS (surgical fixes; all route through CelSetFieldImpl, cel_host.cc:2882):
- [HIGH] No range-check on int32/enum/wrapper field assignment (~6 conf rows):
  - `proto2.TestAllTypes{single_int32_wrapper: 12345678900}` → silent
    static_cast truncation; want "range error". cel_host.cc:2135 (wrapper
    INT32) + :2241 (scalar INT32).
  - `proto2.TestAllTypes{standalone_enum: 5000000000}` / `{...: -7000000000}`
    → no bounds check. cel_host.cc:2312 (singular) / :2501 (repeated).
  - Oracle: cel-cpp struct_value_builder.cc:1090-1095 (enum) / :1125-1129
    (Int32Value → OutOfRange). Fixtures: dynamic.textproto:88, enums.textproto:88,329.
- [MED] No null-pruning of repeated/map MESSAGE-typed elements (~10 rows):
  `TestAllTypes{repeated_timestamp:[timestamp(1), null]}` should drop null →
  [timestamp(1)]; actual trap at cel_host.cc:2509 (repeated) / :2688 (map insert).
  NOTE asymmetry: repeated_any/repeated_value RETAIN null (null is a valid
  Any/Value) — prune only for real WKT/wrapper element types. proto3.textproto:698-757.

UNIMPLEMENTED (not bugs — no codepath): ~33 rows needing a JSON-boxing kernel
(CEL scalar/list/map → google.protobuf.Value/Struct/ListValue, raw value → Any).
SetScalarField CPPTYPE_MESSAGE only handles the 9 wrapper FQNs + message→Any-pack
(cel_host.cc:215-225, 2080, 2331); no Value/Struct/ListValue branch.

TODO to encode: add //proto/cel/expr/conformance/{proto2,proto3} deps + type
registration to a new proto-aware known_bugs test (the range-check + null-prune
repros are clean once TestAllTypes is registered).

## Audited CLEAN (no divergence) — don't re-investigate

- **math_ext** (cel_math_ext.c) — full audit vs cel-cpp + math_ext.textproto:
  no bugs. greatest/least cross-type lossy-double is CORRECT here (cel-cpp's
  internal::Number min/max is equally lossy + same first-operand tie-break);
  abs(INT64_MIN)→overflow matches the fixture; bit-shift ≥64/negative, NaN
  min/max, sign/round/ceil/sqrt all match. The map-key bug does NOT recur in
  math because min/max copies the winning operand (no key-lookup semantics).
- **Integer arithmetic** (cel_arith.c) — INT64_MIN/-1, INT64_MIN%-1, -INT64_MIN,
  div/mod by zero, mul overflow: all correct (wave 1).
- **base64, split/replace, list/map structural eq, NaN-in-list** — correct (wave 1).
- **string_ext** (cel_string_ext_*.cc / cel_string_format_render.cc) — audited
  vs cel-cpp: charAt/substring (multibyte, end-sentinel, byte-bound), replace
  (empty needle, overlap, n-limit), split (empty-sep, final-piece), join,
  lowerAscii/upperAscii, quote, and most of format (%s/%d/%o/%b/%x, precision,
  max_precision, arg-count) all CORRECT. Only divergences: indexOf/lastIndexOf
  pos bound + %f/%e type acceptance (both encoded in known_bugs_test.cc, wave 4).
- **cel_convert.c type conversions** — audited vs absl SimpleAtoi/SimpleAtod +
  conversions.textproto. Most CORRECT (int/uint hex-reject, whitespace-reject,
  leading-zeros, bool(string) truth table, all range gates, int(double) trunc,
  type(x), identity overloads, bytes<->string utf8). Divergences:
  - double(string) whitespace -> ENCODED (DoubleFromStringRejectsWhitespace).
  - double(string) hex-float (`double('0x1p4')`) -> NOT encoded: self-documented
    gap (m10-conversions.md:649). Real divergence, but known/tracked.
  - double(string) not correctly-rounded (apply_decimal_scale iterative *=10,
    cel_convert.c:308-319) -> may differ 1 ULP on long mantissas; input-
    dependent, conformance cases pass. FLAGGED, not encoded.
  - Error TEXT: all reject paths poison CEL_ERR_OVERFLOW even for invalid-arg /
    bad-utf8 cases (cel-cpp says "range"/"invalid"). Only matters if the
    harness matched error text — it doesn't (CompareEvalError is kind-only).
    Not a value divergence. FLAGGED.

## Optionals findings (wave 5)

- ENCODED: `.?field` optional select wrongly rejected by static subset
  (OptionalSelectOnMapRejected). parse_and_check.cc:631-641 recurses into the
  synthetic field-name child that cel-cpp's HandleOptSelect erases.
- [HIGH, host-var trap — needs binding setup to encode] `[?key]`/`[?idx]` on a
  HOST-backed (bound var) map/list TRAPS (`unreachable`): cel_optional.c:304-319
  dispatch_lookup `__builtin_trap` for CEL_MAP_HOST/LIST_HOST/MESSAGE ("Slice B"
  stub). `m[?"a"].value()` with m bound. cel-cpp returns 1.
- [HIGH, host-var trap] `optional.ofNonZeroValue` on a host-backed list/map/msg
  TRAPS: is_zero_value `__builtin_trap` cel_optional.c:116-124 (Slice B stub).
- [subset strictness, not a clean bug] bare `optional.none()` and empty-collection
  literal into ofNonZeroValue rejected as dyn (optional_type(dyn)/list(dyn)) —
  same class as the known `[]`-dyn cut; cel-cpp accepts.
- CORRECT: of/none/value/hasValue, ofNonZeroValue scalar zero-detection,
  orValue/or short-circuit, optMap/optFlatMap, [?]/.? on literals, [?…]/{?…}
  pruning, equality.
