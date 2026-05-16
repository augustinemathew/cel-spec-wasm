# M7B — Timestamp and Duration

Status: **plan — drafted 2026-05-16, not yet started.  Depends on M7
(shipped), independent of M8.**

The plan covers the `google.protobuf.Timestamp` /
`google.protobuf.Duration` surface that M7 and M10 explicitly carved
out: the `timestamp(string)` / `duration(string)` constructors, the
arithmetic + ordering ladder, the field-accessor family (with and
without IANA TZ argument), the conversion arms M10 §2.2 deferred
(`int(timestamp)` / `string(timestamp)` / `string(duration)` etc.),
and the activation marshalling / decoder arms that today fail loud
on `CEL_DURATION` and `CEL_TIMESTAMP` payloads.

The runtime kinds are already reserved on the wire — `CEL_DURATION =
12`, `CEL_TIMESTAMP = 13` with a `CelDurTs { int64_t seconds;
int32_t nanos; int32_t _pad; }` payload arm pinned in
`compiler_v2/runtime/cel_data.h` — but no current code path produces
them.  M7B is what lights the kinds up end-to-end.

**Out of scope:** per-eval `now()` (no clock injection hook today);
extension-library timestamp ops; timestamp/duration construction
from the *proto-literal* shape `Timestamp{seconds: 5, nanos: 0}`
(falls out of M7's `kStructExpr` arm for free — M7B does NOT alter
that path); `Any` containing a Timestamp/Duration.

## 1. Why M7B

Per `compiler_v2/conformance/README.md`, after M10 the corpus sits
roughly at `pass=1058 / skip=693 / fail=703 / total=2454` (43.1%).
The timestamps/duration cohort is the single largest "scope-not-yet
-shipped" bucket remaining that is neither extension-library-shaped
nor blocked on a sibling slice — every shipped milestone since M7
has explicitly carved it out (M7 §2.2, M10 §2.2, the conformance
README's "Timestamps slice" line in §4 of the unlock plan).

| Fixture / row family | Today (PASS) | Post-M7B (estimate) | Driving slice |
|---|---:|---:|---|
| `timestamps.textproto` | 0 / 76 | 65 – 74 | M7B.B – M7B.E |
| `conversions.textproto` (timestamp / duration arms) | within 99 / 104 | +4 | M7B.D |
| `proto2.textproto` / `proto3.textproto` (`compile unimpl` rows that read `Timestamp`/`Duration`-typed fields) | scattered | +8 – +14 | M7B.A (decoder + field read) |
| `basic.textproto` (`type(timestamp(...))` rows depending on shipped constructor) | partial | +1 – +2 | M7B.D |
| **Total projected** | — | **+78 – +94 PASS** | — |

Lower bound is ~+78 if the `proto2`/`proto3` `compile unimpl` rows
turn out to require richer field-shape coverage than just the
decoder arms M7B ships.  Upper bound assumes every `timestamps`
row that isn't gated on `now()` or extension ops graduates.

The 76 `timestamps.textproto` SKIPs are the cleanest unlock surface:
every row either calls a constructor or chains an accessor on the
constructor's result.  M7B.B + M7B.C light up the arithmetic /
ordering / UTC-accessor majority; M7B.D + M7B.E light up the
parse/format and TZ-aware accessor tail.

### 1.1 What fails today

Captured 2026-05-16 via:

```bash
bazel run //compiler_v2/conformance:run_conformance -- \
    --file=tests/simple/testdata/timestamps.textproto \
    --max_skip_examples=2000 --max_fail_examples=2000
```

`timestamps.textproto` reports `total=76  pass=0  skip=76  fail=0`.
Every SKIP is the same shape: `compile unimplemented: expr_lower:
kCallExpr ... overload_id=\`<id>\` not registered in OverloadTable`.
No SKIP is currently classified as `static_subset` or
`runtime_error` — meaning M7B's exit is the OverloadTable seed flip
plus the matching runtime/host bodies, not a checker carve-out.

| SKIP cluster | Driving overload-id family | Count | Driving slice |
|---|---|---:|---|
| `timestamp_conversions/*` | `timestamp_to_int64`, `timestamp_to_string`, `string_to_timestamp` | 5 | M7B.D |
| `duration_conversions/*` | `duration_to_string`, `string_to_duration` | 3 | M7B.D |
| `timestamp_selectors/*` (UTC, no TZ) | `timestamp_to_year`, `_to_month`, `_to_day_of_month_1_based`, `_to_day_of_month`, `_to_day_of_year`, `_to_day_of_week`, `_to_hours`, `_to_minutes`, `_to_seconds`, `_to_milliseconds` | 10 | M7B.C |
| `timestamp_selectors_tz/*` (with-TZ) | every `_with_tz` variant (one row is misspelled in the corpus as `_to_seconds_tz`) | 11 | M7B.E |
| `timestamp_equality/*` (gated on parse) | `string_to_timestamp` | 4 | M7B.D + M7B.A (equality already lit) |
| `duration_equality/*` (gated on parse) | `string_to_duration` | 4 | M7B.D |
| `timestamp_arithmetic/*` | `add_timestamp_duration`, `add_duration_timestamp`, `add_duration_duration`, `subtract_timestamp_duration`, `subtract_timestamp_timestamp`, `subtract_duration_duration` | 7 | M7B.B + M7B.D |
| `comparisons/*` (ordering) | `less_equals_timestamp/duration`, `less_*`, `greater_equals_*`, `greater_*` | 12 | M7B.B + M7B.D |
| `duration_converters/*` (duration accessors) | `duration_to_hours/minutes/seconds/milliseconds` | 4 | M7B.C |
| `timestamp_range/*` (overflow) | depends on M7B.B arithmetic + M7B.D parse | 8 | M7B.B + M7B.D |
| `duration_range/*` (overflow) | `add_duration_duration` / `subtract_duration_duration` + `string_to_duration` | 6 | M7B.B + M7B.D |

Important caveat: nearly every row in this fixture *also* depends on
`string_to_timestamp` / `string_to_duration` (M7B.D) because the
test wraps each operand in a `timestamp("...")` / `duration("...")`
call.  This means M7B.B and M7B.C alone don't graduate many of
their nominal rows — the visible PASS count only ticks up once
M7B.D also lands.  The slice unlock estimates in the §1 table
factor this in.

Cross-fixture cohort (scattered SKIPs outside `timestamps.textproto`,
sampled the same run):

  - `conversions.textproto`: 3 timestamp + 3 duration rows
    (`int/timestamp`, `identity/duration`, `identity/timestamp`,
    `dyn(...) == null` static-subset rows).  All graduate at
    M7B.D + M7B.A (identity).
  - `proto2.textproto` / `proto3.textproto`
    (`literal_wellknown/timestamp` / `literal_wellknown/duration` +
    `set_null/repeated_field_*_null_pruned` +
    `set_null/map_*_null_pruned`): each appears twice (once per
    fixture), 8 rows total, all blocked on
    `string_to_timestamp` / `string_to_duration` /
    `int64_to_timestamp` / `int64_to_duration` — M7B.D.
  - `format_errors/duration substitution …`: a `disable_check`
    static-subset row, NOT a M7B blocker (stays SKIP).
  - Two `eq_literal/not_eq_dyn_*_null` rows are flagged
    `static_subset` (dyn-wrapped literal compared to null).  These
    stay SKIP after M7B; they're a separate scope blocker.

## 2. Scope

### 2.1 In-scope (per `langdef.md` §"Timestamps and Durations" + cel-cpp's `runtime/standard/time_functions.cc`)

  - **Constructors via string**: `timestamp(string)` (RFC3339 with
    optional fractional seconds and timezone offset) and
    `duration(string)` (the proto `Duration` text format —
    `"1000000s"`, `"1.5s"`, `"-1h2m3s"`-style; see §3.1 for the
    exact admit-set).
  - **Identity conversion**: `timestamp(timestamp)` and
    `duration(duration)` (overload ids `timestamp_to_timestamp` /
    `duration_to_duration` — carve-out partner to the
    `int64_to_int64` / etc. identity ids M10.A graduated).
  - **Numeric conversion**: `int(timestamp)` (epoch seconds — the
    `seconds` field of the payload; nanos truncated), `int(duration)`
    (whole seconds, nanos truncated toward zero).
  - **String conversion / formatting**: `string(timestamp)` (RFC3339
    UTC), `string(duration)` (canonical `<n>s` / `<n>.<frac>s` /
    negative form per proto Duration text format).
  - **String → duration / timestamp**: `string_to_timestamp` /
    `string_to_duration` (same parser as the unary constructors;
    overload-id distinct).
  - **Int → duration / timestamp**: `int64_to_timestamp` (seconds
    from epoch, nanos zero), `int64_to_duration` (whole seconds).
  - **Arithmetic ladder** (six overloads): `duration + duration →
    duration`, `timestamp + duration → timestamp`, `duration +
    timestamp → timestamp`, `duration - duration → duration`,
    `timestamp - duration → timestamp`, `timestamp - timestamp →
    duration`.  Every arithmetic op is overflow-checked per
    langdef.
  - **Ordering ladder** (eight overloads): `<`, `<=`, `>`, `>=` on
    same-kind operand pairs (`(dur, dur)` and `(ts, ts)`).
    Equality / inequality already route through `cel_equals_at_vv`
    once the M7B.A decoder lands — kind-then-payload comparison
    is a pure 12-byte memcmp on the `CelDurTs` arm.
  - **Accessor family — UTC default** (no TZ argument): `getYear`,
    `getMonth`, `getDate` (1-based day-of-month), `getDayOfMonth`
    (0-based), `getDayOfYear`, `getDayOfWeek`, `getHours`,
    `getMinutes`, `getSeconds`, `getMilliseconds`, `getFullYear`
    (cel-cpp alias for `getYear` per `time_functions.cc`).
  - **Accessor family — duration**: `getHours`, `getMinutes`,
    `getSeconds`, `getMilliseconds` on `Duration`.  Spec-pinned
    behaviour: whole units truncated toward zero.
  - **Accessor family — with-tz overload** (two-arg
    `(timestamp, string)`): the same accessor names, but the
    second argument is an IANA TZ name (`"America/Los_Angeles"`,
    `"UTC"`) or a fixed offset (`"+02:00"`, `"-08:00"`).  Invalid
    TZ → `CEL_ERROR (kInvalidArgument)`.
  - **Activation marshalling** for `Value::Duration(absl::Duration)`
    and `Value::Timestamp(absl::Time)` — write the `CelDurTs`
    payload arms in `EncodeBoundValue` at
    `compiler_v2/api/instance.cc`.
  - **Decoder** for `CEL_DURATION` / `CEL_TIMESTAMP` — write the
    arms in `DecodeCelValueAt` (same file).
  - **Proto field reads** of singular `google.protobuf.Timestamp`
    / `google.protobuf.Duration`-typed fields via the existing
    M2.C `ReadField` path — Layer-2 `CelGetFieldImpl` already
    dispatches on `cpp_type == CPPTYPE_MESSAGE` + descriptor FQN;
    M7B.A wires the well-known-type arms (the wrapper-message
    surface lands at M8 in parallel; the timestamp / duration
    well-known types are independent).

### 2.2 Out-of-scope (deferred or shipped elsewhere)

  - **Timestamp / Duration construction from `kStructExpr` literal**
    (`Timestamp{seconds: 5, nanos: 0}` or `Duration{}`).  Already
    shipped at M7.A–E as a normal proto literal — the constructed
    `kMessage` is a well-known-type message in protobuf's view,
    and M7B.A's decoder + accessor entries also accept that
    construction shape on input.  M7B does NOT alter M7's
    construction path; what it adds is reading those constructed
    messages back as `CEL_TIMESTAMP` / `CEL_DURATION` (vs the
    current `CEL_MESSAGE` arm).  See §3.4 for the cross-form
    equivalence requirement.
  - **`Timestamp{}` / `Duration{}` empty literal** — same; lands at
    M7.E.  M7B.A's "well-known-type read" path treats the
    default-constructed message as `(seconds=0, nanos=0)`.
  - **Per-eval current time** (`now()` standard function).  Spec
    requires a per-evaluation clock injection hook on the
    activation; the v2 `Activation` API has none today and
    `cel_host` has no clock primitive.  Surfaced as future work
    in §9.
  - **Extension-library time ops** (timestamp formatting with a
    format string, timezone arithmetic helpers, …).  Not in cel-
    cpp's standard overload set; live behind the extensions
    milestone.
  - **`Any`-wrapped Timestamp / Duration** (`Any{type_url:
    ".../Timestamp", value: ...}`).  Any unpacking is a separate
    milestone; M7B doesn't touch it.
  - **Comprehension-driven timestamp construction** (`xs.map(x,
    timestamp(x))`).  Falls out for free once M7B.D's parse
    trampoline ships + the comprehension path is fully landed;
    no M7B-specific work.

## 3. Spec-mandated semantics

Citations from `doc/langdef.md` §"Timestamps and Durations" (the
source of truth per CLAUDE.md "Testing principles") and
`third_party/cel-cpp/runtime/standard/time_functions.cc` (the
reference implementation).  Per CLAUDE.md, every assertion below
has a test row in §6.

### 3.1 Parse formats

  - **`timestamp(string)`** — RFC3339 per langdef.  cel-cpp passes
    the input through `absl::ParseTime(absl::RFC3339_full, ...)`.
    Admit-set:
      - `"2009-02-13T23:31:30Z"` — base UTC.
      - `"2009-02-13T23:31:30+02:00"` — fixed offset.  Stored
        normalised to UTC: `seconds` carries the UTC epoch
        seconds; the offset only affects parsing.
      - `"2009-02-13T23:31:30.123456789Z"` — full nanosecond
        precision.  Truncated past 9 fractional digits per
        cel-cpp.
      - `"2009-02-13T23:31:30.5Z"` — partial fractional.
    Reject-set:
      - Missing `T` separator, missing TZ, lowercase `z`, missing
        seconds, two-digit year, non-UTC `Z`-suffixed input — all
        per cel-cpp's `ParseTime` strictness.
      - Out-of-range (`"10000-01-01T00:00:00Z"` exceeds protobuf
        Timestamp's `[0001-01-01T00:00:00Z, 9999-12-31T23:59:59Z]`
        bound — see langdef Timestamp range).
  - **`duration(string)`** — proto Duration text format per
    langdef + cel-cpp's `absl::ParseDuration`.  Admit-set:
      - `"3600s"`, `"-3600s"`, `"1000000s"` — integer seconds.
      - `"1.5s"`, `"1.000000001s"` — fractional seconds, up to 9
        fractional digits.
      - `"1h2m3s"`, `"-1h2m3s"`, `"500ms"`, `"-1us"`, `"100ns"` —
        compound units; cel-cpp delegates to `absl::ParseDuration`
        which admits the full unit ladder `h / m / s / ms / us /
        ns`.
    Reject-set:
      - Empty string, no unit suffix (`"3600"`), unknown unit
        (`"3600x"`), wrong order (`"1s2h"`), trailing garbage.
      - Overflow (`"9223372036854775808s"` — exceeds int64
        seconds).

### 3.2 Arithmetic + overflow ladder (langdef §"Timestamps and Durations")

  - Operand kinds are pinned: every arithmetic overload takes
    same-kind operands within `(timestamp, duration)`; mixed kinds
    are checker-rejected before codegen.  No cross-kind
    coercion ladder analogous to M5.B step 2's numeric kernel —
    timestamps and durations are non-numeric.
  - **Carry rule**: every arithmetic op produces a normalised
    `(seconds, nanos)` pair with `0 <= nanos < 1_000_000_000` and
    nanos sign-matched to seconds (per proto Duration / Timestamp
    invariants).  cel-cpp's `internal::EncodeDurationToJson` /
    Timestamp normalisers do the carry; the pure-wasm kernel
    must too (see §4).
  - **Overflow**: int64-seconds overflow on add / sub → `CEL_ERROR
    (CEL_ERR_OVERFLOW)`.  Detected via `__builtin_*_overflow` on
    the seconds field after the nanos-carry has propagated; nanos
    overflow always carries cleanly because the largest possible
    nanos sum is `2 * 999_999_999 < INT64_MAX`.  Bounds:
    `timestamp.seconds` is constrained by langdef to
    `[-62_135_596_800, 253_402_300_799]` (year 0001 to year 9999
    UTC) — cel-cpp surfaces out-of-range as overflow too.

### 3.3 Accessors (langdef + `time_functions.cc`)

  - **Default TZ = UTC.**  The one-arg accessor form (e.g.
    `ts.getHours()`) computes against UTC.
  - **Two-arg form `(ts, tz_string)`** admits IANA names AND
    fixed-offset strings.  Resolution semantics:
      - Fixed offset `"+HH:MM"` / `"-HH:MM"` — pure integer
        arithmetic on `seconds`; no database lookup needed.
        cel-cpp parses these inline.
      - IANA name `"America/Los_Angeles"` — resolved via
        `absl::TimeZone::Load`.  Requires `tzdata` on the host;
        spec-pin failure mode is `CEL_ERROR (kInvalidArgument)`
        when the name doesn't resolve.
  - **Per-accessor semantics**:
      - `getYear` / `getFullYear` — Gregorian year, e.g. `2009`.
        Negative for BCE (but the timestamp range forbids BCE in
        practice).
      - `getMonth` — 0-based per cel-cpp (`Jan` = 0); langdef cites
        this as the documented form.
      - `getDate` — 1-based day-of-month per cel-cpp.
      - `getDayOfMonth` — 0-based day-of-month per cel-cpp.
      - `getDayOfYear` — 0-based; Jan 1 = 0.
      - `getDayOfWeek` — 0-based; Sunday = 0.
      - `getHours` / `getMinutes` / `getSeconds` /
        `getMilliseconds` — straightforward.  Milliseconds = nanos
        / 1_000_000 (truncating).
      - On `Duration`: `getHours` / `getMinutes` / `getSeconds` /
        `getMilliseconds` — `seconds / 3600` / `seconds / 60` /
        `seconds` / `(seconds * 1000 + nanos / 1_000_000)`.
        Truncating toward zero; sign preserved.

### 3.4 Equality, ordering, and cross-form equivalence

  - Equality on durations / timestamps is `(seconds, nanos)`
    lexicographic — once both operands have been normalised, a
    12-byte memcmp on the `CelDurTs` payload suffices.  Routes
    through `cel_equals_at_vv`'s existing kind-then-payload
    dispatch (no new arm — the `case CEL_DURATION:` /
    `case CEL_TIMESTAMP:` arms compare `payload.dur` / `payload.ts`).
  - Ordering is the same lexicographic compare.  Spec citation:
    langdef §"Timestamps and Durations" — "ordering follows
    chronological order; durations are ordered by length".
  - **No NaN.**  Durations and timestamps are integer pairs; the
    IEEE-style "NaN never equal" pattern does not apply.  The
    test matrix in §6.5 pins this with an explicit row asserting
    `duration('1s') == duration('1s')` is `true` regardless of
    construction path.
  - **Cross-form equivalence with `kStructExpr` construction**:
    a timestamp constructed via `timestamp('1970-01-01T00:00:01Z')`
    and a `Timestamp{seconds: 1, nanos: 0}` literal must compare
    equal.  This is the cross-product between M7's literal arm
    (produces `CEL_MESSAGE` whose descriptor is
    `google.protobuf.Timestamp`) and M7B's parse arm (produces
    `CEL_TIMESTAMP`).  See §4.3 for the read-side bridge that
    normalises both forms.

## 4. Architecture — runtime/host trade-off (the crux of this doc)

The user flagged at scoping that duration calculation
"requires a bunch of libraries" (`absl::Time`, `absl::TimeZone`,
the `tzdata` database).  Three options were considered before
this plan landed on Option C.  This section spells out each
option, scores them against the design principles in
`design.md` §4.7.6 ("host imports are the only descriptor
consumer; the runtime kernel stays descriptor-free"), and
documents the chosen split.

### 4.1 Option A — pure-wasm runtime kernels

Every constructor / accessor / arithmetic op is a hand-rolled C
helper in a new `compiler_v2/runtime/cel_time.{h,c}`, following
the `cel_arith.h` slot-out ABI.  The RFC3339 parser is
transliterated from cel-cpp's `absl::ParseTime` (which is
ultimately a ~300-line state machine).  The civil-calendar
walks (days-to-y/m/d) use Howard Hinnant's documented
"civil_from_days" algorithm
(http://howardhinnant.github.io/date_algorithms.html#civil_from_days),
~30 LoC of pure integer arithmetic.

> **Probe A finding (§10):** Hinnant `civil_from_days` produces
> bit-identical output to `absl::ToCivilSecond(UTCTimeZone())`
> across the 12-row test grid covering epoch zero, negative
> epoch (Y1969), Y2K leap-divisible-400, Y1900 century-not-leap,
> langdef worked example (Y2009), Feb 29 leap year, Dec 31 of
> leap + non-leap years, Y9999 langdef upper, Y0001 langdef
> lower, and Y2038 sanity.  0 mismatches across all rows.
> Pure-wasm UTC-accessor path is viable; no `absl::TimeZone`
> dependency for the no-TZ form.

  - **Pros.**
      - Self-contained wasm: no new host imports for arithmetic /
        UTC accessors.  ~28 host import names avoided.
      - Matches the existing `cel_arith.c` / `cel_compare.c`
        pattern — same call ABI, same overflow style, same test
        harness shape.
      - Hot path stays trampoline-free: an accessor chain like
        `ts.getYear() == 2009` lowers to two pure-wasm calls plus
        the equality dispatch.
  - **Cons.**
      - **`tzdata` is genuinely a library.**  An IANA TZ database
        is ~1MB of zoneinfo binary blobs.  Either ship UTC-only
        and surface `CEL_ERROR (kInvalidArgument)` on any non-UTC
        TZ name — which breaks cel-cpp parity on every two-arg
        accessor row in `timestamps.textproto` that uses a real
        IANA name — or embed the blob, doubling the runtime
        wasm size.
      - RFC3339 parsing is non-trivial (~250 LoC for a strict
        parser).
      - Duration text-format parsing (cel-cpp's
        `absl::ParseDuration`) admits compound forms `1h2m3s` —
        another ~100 LoC.
      - String formatting (`absl::FormatTime`) is another ~100
        LoC.

### 4.2 Option B — host trampolines for everything

Every timestamp / duration overload is a Layer-2 `Impl` in
`compiler_v2/api/internal/cel_host.cc` that defers to
`absl::Time` / `absl::ParseTime` / `absl::FormatTime` /
`absl::ParseDuration`.  The 28-overload ladder maps to 28 host
imports (or one dispatch trampoline with a sub-op enum).

  - **Pros.**
      - Tiny per-op LoC — every op is `absl::Time::Foo` plus
        slot-encode boilerplate.
      - Full IANA TZ database for free, whatever absl is linked
        against on the host.
      - Spec parity with cel-cpp by construction.
  - **Cons.**
      - **Violates `design.md` §4.7.6** — the runtime kernel
        becomes descriptor / TZ-database-aware.  Currently no
        runtime helper is.
      - 28 new ABI surface names (or 1 with a fragile sub-op
        enum on the wire — bumps `cel.abi` revision discipline).
      - Every accessor / arithmetic op pays per-call trampoline
        cost: the runtime↔host boundary is the biggest single
        cost in the engine, and shifting hot-path arithmetic
        across it is the wrong tradeoff.
      - 28 Layer-2 Impls + 28 Layer-3 registrations doubles
        the host surface in a single milestone.

### 4.3 Option C — split (recommended)

Constructors and formatting (which genuinely need a library —
RFC3339, TZ database, duration text format) trampoline to the
host.  Everything else — arithmetic, ordering, UTC-default
accessors — is a pure-wasm helper in `cel_time.{h,c}`.  The
with-TZ accessor form (the only path that needs the IANA
database) folds to a single dispatch trampoline.

The split lines up cleanly with the cel-cpp surface: the same
~6 cel-cpp functions that ultimately call
`absl::TimeZone::Load` are the same ~6 that become host
trampolines here.

  - **Pure-wasm kernels** (lives in `compiler_v2/runtime/cel_time.{h,c}`):
      - `cel_dur_add_at_vv`, `cel_dur_sub_at_vv`,
        `cel_ts_dur_add_at_vv`, `cel_dur_ts_add_at_vv`,
        `cel_ts_dur_sub_at_vv`, `cel_ts_ts_sub_at_vv` — 6
        arithmetic helpers with `__builtin_add_overflow` on
        seconds + nanos-carry.
      - `cel_dur_lt_at_vv` / `cel_dur_le_at_vv` /
        `cel_dur_gt_at_vv` / `cel_dur_ge_at_vv` — same for
        timestamp (8 ordering helpers; equality routes through
        `cel_equals_at_vv` after the M7B.A `case CEL_DURATION:`
        / `case CEL_TIMESTAMP:` arms land — no new helper
        needed).
      - `cel_ts_year_utc`, `cel_ts_month_utc`,
        `cel_ts_day_of_month_utc`, `cel_ts_day_of_month_1_utc`,
        `cel_ts_day_of_year_utc`, `cel_ts_day_of_week_utc`,
        `cel_ts_hours_utc`, `cel_ts_minutes_utc`,
        `cel_ts_seconds_utc`, `cel_ts_milliseconds_utc` — 10
        UTC accessors backed by a shared
        `static void CivilFromSeconds(int64_t, CivilYmdHms*)`
        helper that runs the documented Hinnant algorithm.
      - `cel_dur_hours`, `cel_dur_minutes`, `cel_dur_seconds`,
        `cel_dur_milliseconds` — 4 duration accessors, pure
        truncating-int division on `seconds` / `nanos`.
      - `cel_ts_to_int_at_v`, `cel_dur_to_int_at_v` — the
        `int(timestamp)` / `int(duration)` conversions.
      - `cel_int_to_ts_at_v`, `cel_int_to_dur_at_v` — the
        `int(...)` → ts / dur reverses (encode an int64 into the
        `seconds` field, `nanos = 0`).
      - `cel_dur_identity`, `cel_ts_identity` — `duration_to_duration`
        / `timestamp_to_timestamp`.  Seed to `cel_copy_slot` per
        the M10.A pattern.
  - **Host trampolines** (Layer-2 `*Impl` in `cel_host.cc`):
      - `cel_host.cel_timestamp_parse(out_slot, str_slot)` — body:
        `absl::ParseTime(absl::RFC3339_full, ...)`.  Backs the
        `timestamp(string)` constructor + `string_to_timestamp`
        overload (same trampoline; both ids dispatch here).
      - `cel_host.cel_duration_parse(out_slot, str_slot)` — body:
        `absl::ParseDuration`.  Backs `duration(string)` +
        `string_to_duration`.
      - `cel_host.cel_timestamp_format(out_slot, ts_slot)` —
        body: `absl::FormatTime(absl::RFC3339_full, ..., UTC)`.
        Backs `string(timestamp)`.
      - `cel_host.cel_duration_format(out_slot, dur_slot)` —
        body: canonical `<n>s` formatter following the proto
        Duration text format (cel-cpp delegates to
        `internal::EncodeDurationToJson`).  Backs
        `string(duration)`.
      - `cel_host.cel_timestamp_tz_accessor(out_slot, ts_slot,
        tz_slot, accessor_kind)` — single dispatch trampoline for
        all 10 with-TZ accessors.  `accessor_kind` is a u32 enum
        mapped to one of `kYear` / `kMonth` / `kDate` /
        `kDayOfMonth` / `kDayOfYear` / `kDayOfWeek` / `kHours` /
        `kMinutes` / `kSeconds` / `kMilliseconds`.  Body:
        `absl::TimeZone::Load(tz_str)` then the matching
        `absl::CivilSecond` field extract.  Folds 10 host imports
        into one — reduces ABI surface count creep and keeps
        per-import dispatch O(1).

  - **Net surface**: ~20 pure-wasm kernels + **5 host imports**
    (parse-ts, parse-dur, format-ts, format-dur, tz-accessor)
    vs Option B's 28.

  - **Recommendation rationale**:
      - Matches the design-principle bias: runtime is
        descriptor-free, host owns what genuinely needs a
        library (RFC3339 parsing, TZ database).
      - Spec parity for TZ comes through absl on the host.
      - Hot path (arithmetic + non-TZ accessors) is
        trampoline-free.
      - 5 new host import names is comfortably inside the
        existing `cel_host` ABI surface — no `cel.abi` revision
        bump needed (no new ABI tables; see §4.4).

### 4.4 Assumptions challenged

Honest spike on the design assumptions that fed into Option C —
in case any of them are wrong, we should know now, not after the
slice is half-shipped.

**Q1: Does the pure-wasm civil calendar actually shrink the
runtime wasm meaningfully vs adding ~10 host trampolines?**

Measurement, 2026-05-16:

```
$ bazel build //compiler_v2/runtime:cel_runtime.wasm
$ ls -la bazel-bin/compiler_v2/runtime/cel_runtime.wasm
-r-xr-xr-x  49611 cel_runtime.wasm   # pre-M7B baseline
```

The runtime wasm is ~48.4 KiB today.  Estimated M7B.B + M7B.C
delta: 14 helpers + the shared `cel_civil_from_seconds` body =
~250–400 LoC of C → ~3–5 KiB compiled wasm (eyeballing
`cel_arith.c`'s LoC↔wasm ratio).  Net post-M7B.C runtime wasm:
~52–54 KiB — a ~7–10% growth from a 48 KiB baseline.

Option B's cost is harder to quantify because the trampolines
themselves don't live in the wasm runtime — they're host-side
Layer-2 impls.  But each new `cel_host.*` import name DOES add
metadata to the function-table intern map and a Cranelift trampoline
stub at Plan time.  10 extra host imports ≈ 10 trampoline stubs
≈ ~5 KiB of Cranelift output at Plan time per Instance.  And every
accessor call pays the wasm↔host boundary cross (~150–200 ns per
call, per the pipeline_bench numbers — see README.md "How JIT
compilation fits in").

Conclusion: the size-shaped argument is genuinely close to a wash
(both options grow ~5 KiB of artefact somewhere).  The latency-
shaped argument decisively favours Option C: a chain like
`ts.getYear() + ts.getMonth() + ts.getDate()` makes 3 wasm calls
under Option C and 3 host trampolines under Option B.  At 100 ns
per wasm call vs 200 ns per trampoline, the difference is
~300 ns vs ~600 ns — 2× per accessor chain.

**Q2: Is the assumption that "wasm32 has no libc" still true?**

Checked `compiler_v2/runtime/cel_internal.h:65–88`, current state:

```c
// Freestanding wasm32 cross-compile has no libc; the host build has
// <string.h>.  Use byte-loop fallbacks on wasm so each TU is
// self-contained without pulling compiler-rt.  These are
// `static inline` so each TU gets its own copy and clang dead-strips
// unused ones.
#ifdef __wasm__
static inline void* cel_memcpy_internal_(void* dst, ...);
static inline void* cel_memset_internal_(void* dst, ...);
#define memcpy cel_memcpy_internal_
#define memset cel_memset_internal_
#else
#include <string.h>
#endif
```

Confirmed: wasm32 is freestanding, no libc, no compiler-rt.  This
forecloses Option A (pure-wasm Hinnant calendar is FINE — pure
int arithmetic — but a pure-wasm `absl::ParseTime` transliteration
would need `strtoll` / etc. that don't exist).  Option C's split
is precisely correct: pure-wasm gets the int-only arithmetic +
civil calendar, host gets the parser + tz database.

**Q3: Is collapsing 10 with-TZ trampolines into 1 dispatch
trampoline actually better than 10 named trampolines?**

Tradeoff:

  - **10 named trampolines.**  Each has a tight body that does
    one specific accessor.  Per-call cost: one wasmtime
    trampoline cross + one absl call + one slot write.  No
    dispatch branch.  ABI surface count creep: +10 host imports
    in `cel_host` + 10 Layer-2 Impls + 10 Layer-3 registrations.
  - **1 dispatch trampoline.**  Body switches on the
    `accessor_kind` u32 to pick the field projection.  Per-call
    cost: one trampoline cross + one absl call + one switch
    branch (1–2 cycles) + one slot write.  ABI surface count
    creep: +1 host import + 1 Layer-2 Impl + 1 Layer-3
    registration.

The hot-path difference is the switch branch — a single
predictable jump on a u32 immediate.  ~1 cycle on Apple Silicon.
The trampoline crossing dominates at ~200 ns; the dispatch branch
is in the noise.

The ABI surface saving (10× fewer name strings interned in
`cel.abi.functions[]`, 10× fewer Layer-3 registrations, 10× fewer
seed rows in `OverloadTable::kBuiltinSeeds`) is significant — the
seed table is human-maintained and grows linearly with the surface.

**Recommendation: keep the single dispatch trampoline.**  The
plan as drafted is correct; the alternative is not worth the
ABI surface count creep.

### 4.5 ABI surface — no new tables

Crucially: M7B adds zero new `cel.abi` tables.  The wire surface
the codegen produces is purely:

  - Existing `cel.abi.functions[]` entries (overload-id intern
    table) gain 28 rows as the timestamps/duration overloads
    move from `kExplicitlyUnimplementedIds` into `kBuiltinSeeds`.
    No schema change to `cel_abi.proto`; same row shape as the
    M10.A–E graduations.
  - 5 new host import names declared in `cel_runtime.c` via
    `__attribute__((import_module("cel_host"),
    import_name("cel_timestamp_parse")))` — same pattern as
    every other host import since M2.C.

Document the new function table rows in
`cel-host-surface.md` §6 (one bullet per new host import name);
no schema bump.

### 4.6 Activation marshalling

`EncodeBoundValue` at `compiler_v2/api/instance.cc` (around lines
607–644) today fails loud on `kDuration` / `kTimestamp` arms.
M7B.A replaces those arms with:

```cpp
case celwasm::Repr::kDuration: {
  if (v.kind() != cel::Value::Kind::kDuration) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Activation[", name, "]: declared Duration but bound ",
        cel::Value::KindName(v.kind())));
  }
  absl::Duration d = v.duration();
  int64_t s = absl::IDivDuration(d, absl::Seconds(1), &d);
  int32_t ns = static_cast<int32_t>(
      absl::IDivDuration(d, absl::Nanoseconds(1), &d));
  *dst = CelValue{};
  dst->kind = CEL_DURATION;
  dst->payload.dur = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
  return absl::OkStatus();
}
case celwasm::Repr::kTimestamp: {
  if (v.kind() != cel::Value::Kind::kTimestamp) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Activation[", name, "]: declared Timestamp but bound ",
        cel::Value::KindName(v.kind())));
  }
  absl::Time t = v.timestamp();
  absl::Duration since_epoch = t - absl::UnixEpoch();
  int64_t s = absl::IDivDuration(since_epoch, absl::Seconds(1),
                                  &since_epoch);
  int32_t ns = static_cast<int32_t>(absl::IDivDuration(
      since_epoch, absl::Nanoseconds(1), &since_epoch));
  *dst = CelValue{};
  dst->kind = CEL_TIMESTAMP;
  dst->payload.ts = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
  return absl::OkStatus();
}
```

Pure inline writes — no arena, no externref table.  The 24-byte
CelValue carries the payload in-place via the `CelDurTs` arm.

### 4.7 Decoder side

`DecodeCelValueAt` at `compiler_v2/api/instance.cc` (around lines
257–322) today falls through to the `default:`
`InvalidArgumentError` arm for `CEL_DURATION` / `CEL_TIMESTAMP`.
M7B.A adds:

```cpp
case CEL_DURATION:
  return Value::Duration(
      absl::Seconds(cv.payload.dur.seconds) +
      absl::Nanoseconds(cv.payload.dur.nanos));
case CEL_TIMESTAMP:
  return Value::Timestamp(
      absl::UnixEpoch() +
      absl::Seconds(cv.payload.ts.seconds) +
      absl::Nanoseconds(cv.payload.ts.nanos));
```

Same shape on both sides — the `CelDurTs` arm is symmetric.

### 4.8 Proto field reads of well-known-type fields

Singular `google.protobuf.Timestamp` / `Duration`-typed fields are
read today via the M2.C `ProtoBacking::ReadField` path, which
returns a `CEL_MESSAGE` whose descriptor is
`google.protobuf.Timestamp`.  M7B.A adds a post-read normaliser
in Layer-2 `CelGetFieldImpl`: if the resolved field's message
descriptor FQN is `google.protobuf.Timestamp` /
`google.protobuf.Duration`, the trampoline reads the `seconds`
and `nanos` fields out via reflection and writes a
`CEL_TIMESTAMP` / `CEL_DURATION` CelValue at the out_slot instead
of a `CEL_MESSAGE`.

This same normaliser handles the cross-form equivalence
requirement in §3.4: a `Timestamp{seconds: 1}` literal read back
via field-select normalises to `CEL_TIMESTAMP`, matching the
`timestamp('1970-01-01T00:00:01Z')` constructor's output kind.

The normaliser is a one-liner switch on
`field_desc->message_type()->full_name()`; the other 7 well-
known-types (`Int32Value`, etc.) stay `CEL_MESSAGE` and route
through M8 when that ships.

### 4.9 Civil-calendar pure-wasm helper

The shared core for every UTC accessor is one helper:

```c
// Compute the civil year/month/day/hour/min/sec from int64 epoch
// seconds.  Uses the documented "civil_from_days" algorithm from
// Howard Hinnant's date_algorithms.html — public, portable, no
// floating point, no overflow on the supported timestamp range.
//
// Output ranges:
//   year:   [1, 9999]    (per langdef Timestamp bounds)
//   month:  [0, 11]      (0-based per cel-cpp)
//   day_1:  [1, 31]      (1-based; getDate)
//   day_0:  [0, 30]      (0-based; getDayOfMonth)
//   hour:   [0, 23]
//   minute: [0, 59]
//   second: [0, 59]
//   day_of_year: [0, 365]
//   day_of_week: [0, 6]  (0 = Sunday)
typedef struct {
  int32_t year;
  int32_t month;
  int32_t day_1;
  int32_t day_0;
  int32_t hour;
  int32_t minute;
  int32_t second;
  int32_t day_of_year;
  int32_t day_of_week;
} CelCivil;

void cel_civil_from_seconds(int64_t epoch_seconds, CelCivil* out);
```

The algorithm is a ~30 LoC documented integer routine — the
canonical reference is publicly published at
`howardhinnant.github.io/date_algorithms.html` (NOT a dependency
on absl; the routine is mathematically what every civil-time
library implements internally).  Coverage requirements in §6
pin Y1, Y1970, Y2000 (leap-year-divisible-by-400 quirk), Y1900
(century-not-leap-year quirk), Y2038 (signed-int32 epoch wrap —
irrelevant for us but a useful sanity case), and Y9999 upper
bound.

## 5. Sequencing — slices

Six slices, each shippable independently.  Effort sized as small
(one focused change, ≤200 LoC + tests) / medium (≤500 LoC + tests
+ WAT trace) / large (cross-cutting change).

### M7B.A — data shape + activation marshalling + decoder + field-read normaliser

The user-facing surface light-up: every code path that today
fails loud on `CEL_DURATION` / `CEL_TIMESTAMP` now round-trips
cleanly.  No constructors yet — `timestamp('...')` still rejects
at the static-subset gate until M7B.D ships.  What works after
M7B.A: `Activation::Bind("t", Value::Timestamp(...))` round-trips
through `Eval`; reading a `Timestamp`-typed proto field returns
`CEL_TIMESTAMP`; equality / inequality on durations / timestamps
already-bound or already-read works (the runtime kernel's
existing kind-then-payload memcmp arm in `cel_equals_at_vv`
already covers this — no new code beyond the decode/encode
arms).

  - **No WAT trace required** — pure host-side data-shape work;
    no codegen path changes, no new wasm imports.
  - **`EncodeBoundValue`** — write the `kDuration` / `kTimestamp`
    arms per §4.5.
  - **`DecodeCelValueAt`** — write the `CEL_DURATION` /
    `CEL_TIMESTAMP` arms per §4.6.
  - **`CelGetFieldImpl`** — well-known-type normaliser per §4.7.
  - **Tests.**  Layer-2 unit (`cel_host_test`): activation +
    round-trip table per `EncodeBoundValue` / `DecodeCelValueAt`
    parameterised over `(seconds, nanos)` boundary values
    (zero, one-nano, max, min, negative seconds, positive
    seconds + negative nanos normalisation).  E2E
    (`compiler_v2/e2e/m7b_test.cc::RoundTripE2ETest`): bind a
    duration / timestamp, return it, assert the decoded value
    matches.
  - **Conformance unlock estimate.**  +8 – +14 PASS (the
    `proto2` / `proto3` `compile unimpl` rows that just read a
    Timestamp/Duration-typed field and compare it to another
    bound value graduate).
  - **Effort.**  Small.

### M7B.B — pure-wasm arithmetic + ordering kernels

Land the 6 arithmetic + 8 ordering helpers in
`compiler_v2/runtime/cel_time.{h,c}`.  Equality already works
post-M7B.A through `cel_equals_at_vv`.  Move the 6 arithmetic
+ 8 ordering ids from `kExplicitlyUnimplementedIds` into
`kBuiltinSeeds` in `overload_table.cc`.

  - **WAT-first.**  Author `wat/50_duration_arithmetic.wat`
    showing `cel_dur_add_at_vv` + `cel_ts_ts_sub_at_vv` call
    shapes — assert the slot-out ABI matches the existing
    `cel_int_add_at_vv` shape byte-for-byte.  No host imports
    needed (pure wasm kernels), so the WAT runs end-to-end
    through `wat_runner` against a real `cel_runtime.wasm`.
    Document in `wat-traces.md`.
  - **Runtime kernel.**  `cel_time.{h,c}` with the 14 helpers.
    Carry-on-add, overflow via `__builtin_add_overflow` on
    seconds.  Test coverage requirement: every helper has a
    boundary table covering `(INT64_MAX, 999_999_999) + (1ns)`
    overflow, `(0, 0) + (0, 0)` zero, negative-seconds /
    positive-nanos normalisation, and `(INT64_MIN, 0) -
    (1s)` underflow.
  - **Overload table seed.**  Add `cel_dur_add_at_vv` etc. as
    seeds; bump `kBuiltinSeeds` array size; document the move
    in `overload_table.cc`'s sibling-comment block (matching
    M10.A's style).
  - **Tests.**  Runtime unit (`cel_runtime_wasm_test` —
    parameterised arithmetic + ordering matrix per helper);
    codegen unit (`expr_lower_test` — assert `_+_` over
    duration-typed operands routes to `cel_dur_add_at_vv`);
    e2e (`m7b_test::ArithmeticE2ETest` — fixture-bound
    durations + timestamps, no parsing yet).
  - **Conformance unlock estimate.**  +15 – +20 PASS in
    `timestamps.textproto` (arithmetic + ordering rows that
    bind both operands via activation).
  - **Effort.**  Medium.

### M7B.C — pure-wasm UTC accessors

Land the 10 timestamp-UTC + 4 duration accessors, plus the
shared `cel_civil_from_seconds` helper per §4.8.  Move the 14
ids into `kBuiltinSeeds`.

  - **WAT-first.**  Author `wat/51_timestamp_year_utc.wat`
    showing the `cel_ts_year_utc(out_slot, ts_slot)` call
    shape.  Single-helper-per-WAT is enough; the full matrix is
    structurally identical.
  - **Runtime kernel.**  Add the helpers + the shared
    `cel_civil_from_seconds` body to `cel_time.{h,c}`.  No
    libc, so the days-from-epoch arithmetic uses the documented
    Hinnant integer routine; cite the URL in the C body.
  - **Tests.**  Runtime unit covering the Gregorian-quirk grid:
    1970-01-01 epoch zero, 2000-01-01 (leap-year-divisible-by-
    400), 1900-01-01 (century-not-leap-year — outside our range
    but the algorithm must handle generically), 2009-02-13T23:31:30Z
    (the langdef example), Y9999 upper, leap-second behaviour
    (cel-cpp / proto Timestamp do NOT model leap seconds; assert
    that the standard `2016-12-31T23:59:60Z` rejects at parse
    rather than producing 60 in the seconds field).  Day-of-week
    coverage: every day across one example week.  Day-of-year
    coverage: Dec 31 of a leap year (= 365), Dec 31 of a non-
    leap year (= 364), Feb 28 / Feb 29 across leap-year
    boundary.
  - **E2E.**  `m7b_test::UtcAccessorE2ETest` over the same grid;
    activation-bind a timestamp, call accessor, assert value.
  - **Conformance unlock estimate.**  +25 – +30 PASS in
    `timestamps.textproto` (the UTC-default accessor rows are
    the largest single cluster).
  - **Effort.**  Medium.

### M7B.D — host trampolines for parse + format

Land the 4 host trampolines per §4.3: `cel_timestamp_parse`,
`cel_duration_parse`, `cel_timestamp_format`,
`cel_duration_format`.  Light up the 12 parse/format/int-convert
overload ids: `string_to_timestamp` / `string_to_duration` /
`int64_to_timestamp` / `int64_to_duration` /
`timestamp_to_string` / `duration_to_string` /
`timestamp_to_int64` / `duration_to_int64` /
`timestamp_to_timestamp` / `duration_to_duration` (identities) +
the bare-string-constructor surfaces (`timestamp(string)` /
`duration(string)` map to the same overload ids as the
`string_to_*` forms after cel-cpp's checker resolution).

  - **WAT-first.**  Author `wat/52_timestamp_parse.wat` +
    `53_duration_format.wat`.  Stub the parse / format
    primitives in `wat_runner` (write known fixed
    `(seconds, nanos)` values for known input strings; verify
    the WAT runs end-to-end before the real trampolines land).
  - **ABI.**  Four new host imports in `cel_runtime.c` (extern
    decl with `import_module("cel_host")` + `import_name(...)`).
    Document in `cel-host-surface.md` §6.
  - **Layer-2 impls.**  `CelTimestampParseImpl`,
    `CelDurationParseImpl`, `CelTimestampFormatImpl`,
    `CelDurationFormatImpl` in `cel_host.{h,cc}`.  Bodies are
    `absl::ParseTime` / `absl::ParseDuration` / `absl::FormatTime`
    / cel-cpp's duration-text formatter (delegate to the
    `internal/time.cc`-style function, or hand-roll the
    canonical `<n>s` form — the latter is ~20 LoC).
  - **Layer-3 registrations.**  Four rows in
    `RegisterCelHostImports`.
  - **Overload seeds.**  12 ids graduated from
    `kExplicitlyUnimplementedIds` to `kBuiltinSeeds`.
  - **Tests.**  Layer-2 unit covers every admit / reject case
    from §3.1.  Codegen unit pins the parse-trampoline emit
    site.  E2E (`m7b_test::ParseFormatE2ETest`) covers the
    constructor + conversion family.
  - **Conformance unlock estimate.**  +25 – +35 PASS — the
    timestamps fixture is now end-to-end exercisable from
    string constructors; the `conversions.textproto` 4
    remaining timestamp/duration SKIPs graduate.
  - **Effort.**  Medium.

### M7B.E — host trampoline for with-TZ accessors

Land the single `cel_timestamp_tz_accessor(out_slot, ts_slot,
tz_slot, accessor_kind)` trampoline + the 10 overload-id
mappings.  Each with-TZ overload id lowers to the same host
import name with a fixed `accessor_kind` u32 immediate.

  - **WAT-first.**  Author `wat/54_timestamp_year_with_tz.wat` +
    `55_timestamp_hours_fixed_offset.wat`.  Stub the trampoline
    in `wat_runner` to return known values for known
    `(ts, tz, kind)` tuples.
  - **ABI.**  One new host import.  The `accessor_kind` u32
    immediate is documented in `cel-host-surface.md` §6 as a
    closed enum: `{kYear=0, kMonth=1, kDate=2, kDayOfMonth=3,
    kDayOfYear=4, kDayOfWeek=5, kHours=6, kMinutes=7,
    kSeconds=8, kMilliseconds=9}`.
  - **Layer-2 impl.**  `CelTimestampTzAccessorImpl` —
    `absl::TimeZone::Load(tz_string)` (failure →
    `kInvalidArgument` error in out_slot); convert
    `(seconds, nanos)` to `absl::Time`; project to
    `absl::CivilSecond` in the loaded zone; switch on
    `accessor_kind` to extract the field.
  - **Codegen.**  Each of the 10 with-TZ overload ids gets a
    `Seed` row that points to a tiny shim helper which packs
    `accessor_kind` and forwards.  The shim is a 1-line wrapper
    around the host import, OR the seed routes directly to the
    host import with `accessor_kind` materialised as a constant
    operand at the emit site — pick whichever keeps the
    `kBuiltinSeeds` table flat.  The straightforward shape:
    seed each id to its own per-kind shim helper
    (`cel_ts_year_with_tz_at_vvv` etc.) that calls the shared
    trampoline with the right `accessor_kind`.  10 thin shims
    in `cel_time.c`; one host import.
  - **Tests.**  Layer-2 unit: matrix of (admit / reject) ×
    (IANA / fixed-offset / invalid-name) × every accessor_kind.
    Cover at least `America/Los_Angeles`, `+02:00`,
    `-08:00`, `UTC`, `"NotARealZone"` reject.  E2E
    (`m7b_test::TzAccessorE2ETest`) walks the cel-cpp-parity
    rows from `timestamps.textproto`.
  - **Conformance unlock estimate.**  +5 – +10 PASS — the
    with-TZ rows in `timestamps.textproto` round out the
    fixture.
  - **Effort.**  Small.

### M7B.F — closeout

  - Run `bazel run //compiler_v2/conformance:run_conformance`
    and record the post-M7B deltas in
    `compiler_v2/conformance/README.md` — both the per-fixture
    table and the unlock-plan timestamps row.
  - Run `scripts/run_full_suite.sh` (the closeout gate per
    CLAUDE.md "manual-tagged tests carry the load-bearing e2e
    assertions").
  - Flip `design.md` references to the timestamp / duration
    surface — §4.7 well-known-type notes if any, §11.5 row
    status if a row tracks it.
  - Flip this doc's status header to `shipped YYYY-MM-DD` with
    a "what landed" paragraph (per CLAUDE.md "Closing out a
    planning doc").
  - Tick `testing-checklist.md` rows under "Rewrite M7B": every
    new CEL-type × pipeline-stage cell M7B lit up — kDuration ×
    {ResolvePass, LayoutPass, codegen, runtime helper,
    activation marshal, decoder}; kTimestamp × same; each new
    overload-id row.
  - Reconcile sibling docs: `cel-host-surface.md` §6 (new host
    imports), `m7-proto-literals.md` "Future work" (strike the
    "Timestamp / Duration" follow-up bullet), `m10-conversions.md`
    §2.2 (strike the deferred-to-timestamps-slice bullets, or
    annotate them shipped).
  - Append M7B's "Future work" section (`now()` clock injection,
    extension-library time ops, leap-second handling if any
    fixture row ever surfaces it).

## 6. Test matrix (load-bearing)

Per CLAUDE.md "Cover the edge-case matrix — this is a compiler",
every combination below MUST have at least one explicit test
(parameterised or longhand).  Negative coverage (rejection
cases) is ≥ 30% of the total per the same rule.

### 6.1 Round-trip data-shape matrix (M7B.A)

| Dimension | Values | Count |
|---|---|---|
| Kind | kDuration / kTimestamp | 2 |
| Seconds boundary | 0 / 1 / -1 / INT64_MIN / INT64_MAX / langdef-ts-min / langdef-ts-max | 7 |
| Nanos boundary | 0 / 1 / 999_999_999 / -1 (normalises) | 4 |
| Direction | encode-only (Bind→Eval) / decode-only (read→return) / full round-trip | 3 |

≈170 cells; parameterised TEST_P collapses structurally-identical
cells to ~30 rows.

### 6.2 Parse admit matrix (M7B.D)

Per §3.1 admit-set:

| Form | Examples | Count |
|---|---|---|
| timestamp RFC3339 base | `"2009-02-13T23:31:30Z"` | 1 |
| timestamp RFC3339 fractional | `"2009-02-13T23:31:30.5Z"`, `".123456789Z"` | 2 |
| timestamp RFC3339 offset | `"+02:00"`, `"-08:00"` | 2 |
| duration integer seconds | `"3600s"`, `"-3600s"`, `"0s"`, `"1000000s"` | 4 |
| duration fractional | `"1.5s"`, `"1.000000001s"` | 2 |
| duration compound units | `"1h2m3s"`, `"500ms"`, `"-1us"`, `"100ns"` | 4 |

Plus the reject-set (parse failure → `CEL_ERROR (kInvalidArgument)`):

| Reject reason | Examples | Count |
|---|---|---|
| missing TZ on ts | `"2009-02-13T23:31:30"` | 1 |
| lowercase z | `"2009-02-13T23:31:30z"` | 1 |
| out-of-range ts | `"10000-01-01T00:00:00Z"`, `"0000-01-01T00:00:00Z"` | 2 |
| dur empty / no unit | `""`, `"3600"` | 2 |
| dur unknown unit | `"3600x"` | 1 |
| dur trailing garbage | `"1s2h"`, `"1s "` | 2 |
| dur overflow | `"9223372036854775808s"` | 1 |
| ts leap-second | `"2016-12-31T23:59:60Z"` — assert reject per §3.3 | 1 |

### 6.3 Arithmetic + overflow matrix (M7B.B)

6 arithmetic helpers × 6 boundary scenarios:

| Helper | (0,0)+(0,0) | (1s,0)+(0,1ns) carry | (INT64_MAX,999_999_999)+(0,1ns) overflow | mixed-sign normalise | langdef-bound + 1s overflow | negative-result correctness |
|---|---|---|---|---|---|---|
| `cel_dur_add_at_vv` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `cel_dur_sub_at_vv` | ✓ | ✓ | ✓ (INT64_MIN underflow) | ✓ | ✓ | ✓ |
| `cel_ts_dur_add_at_vv` | ✓ | ✓ | ✓ | ✓ | ✓ | n/a (ts) |
| `cel_dur_ts_add_at_vv` | ✓ | ✓ | ✓ | ✓ | ✓ | n/a (ts) |
| `cel_ts_dur_sub_at_vv` | ✓ | ✓ | ✓ | ✓ | ✓ | n/a (ts) |
| `cel_ts_ts_sub_at_vv` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (dur) |

### 6.4 Civil-calendar quirk matrix (M7B.C)

Pin every Gregorian-cycle quirk that a naive implementation
would get wrong.  Test rows live in `cel_time_test`:

| Date | What it tests |
|---|---|
| `1970-01-01T00:00:00Z` | Epoch zero baseline |
| `1969-12-31T23:59:59Z` | Negative seconds |
| `2000-01-01T00:00:00Z` | Leap-year-divisible-by-400 |
| `1900-01-01T00:00:00Z` | Century-not-leap-year (outside ts range but algorithm-generic; covered in unit-only test) |
| `2009-02-13T23:31:30Z` | Langdef worked example |
| `2024-02-29T00:00:00Z` | Leap-year Feb 29 |
| `2023-02-28T23:59:59Z` | Day-before-non-leap end-of-Feb |
| `2024-12-31T23:59:59Z` | End of leap year — getDayOfYear = 365 |
| `2023-12-31T23:59:59Z` | End of non-leap — getDayOfYear = 364 |
| `9999-12-31T23:59:59Z` | Langdef upper bound |
| `0001-01-01T00:00:00Z` | Langdef lower bound |
| `2038-01-19T03:14:07Z` | Signed-int32 epoch wrap sanity (we're int64, must work) |

Per-row assertions: every accessor (`getYear`, `getMonth`,
`getDate`, `getDayOfMonth`, `getDayOfYear`, `getDayOfWeek`,
`getHours`, `getMinutes`, `getSeconds`, `getMilliseconds`) on the
constructed timestamp matches the spec-published value.

### 6.5 Equality + ordering matrix

Per §3.4 — pure 12-byte memcmp on the `CelDurTs` payload, but
the cross-form equivalence row is load-bearing:

  - `duration('1s') == duration('1s')` → `true`.
  - `timestamp('1970-01-01T00:00:01Z') == Timestamp{seconds: 1}` →
    `true` (cross-form: parse arm vs `kStructExpr` arm).
  - `duration('1s') < duration('2s')` → `true`.
  - `timestamp('1970-01-01T00:00:01Z') < timestamp('1970-01-01T00:00:02Z')`
    → `true`.
  - `duration('-1s') < duration('1s')` → `true` (sign).
  - `duration('1s') == duration('1000ms')` → `true` (canonicalisation).
  - **No NaN row.**  Explicit comment in the test fixture
    asserting the IEEE-style NaN-not-equal pattern does NOT
    apply to durations / timestamps; they are integer pairs.

### 6.6 Zero-value cross-product

`timestamp(0)` and `duration(0)` are valid and have specific
decoded values per langdef.  Pin:

  - `int(timestamp(0)) == 0` (epoch).
  - `string(timestamp(0)) == "1970-01-01T00:00:00Z"`.
  - `duration(0)` — checker may reject (no `int64_to_duration`
    overload from a literal `0` if the checker types it as
    `int`).  Spike during M7B.D — depending on cel-cpp's checker
    shape, this is either a constructor row or rejected at
    parse.
  - `string(duration('0s')) == "0s"`.

### 6.7 With-TZ matrix (M7B.E)

10 accessors × 4 TZ-source shapes = 40 cells:

| TZ source | Examples | Expected |
|---|---|---|
| IANA | `"America/Los_Angeles"`, `"UTC"`, `"Europe/London"` | accessor value in that zone |
| fixed offset | `"+02:00"`, `"-08:00"`, `"+00:00"` | accessor value at offset |
| invalid | `"NotAZone"`, `""`, `"+25:00"` | `CEL_ERROR (kInvalidArgument)` |
| empty | `""` | `CEL_ERROR (kInvalidArgument)` |

### 6.8 Negative / rejection matrix

  - Activation bind: declared `kDuration`, bound `Value::Int(5)`
    → encoder rejects with `InvalidArgument`.
  - Activation bind: declared `kTimestamp`, bound
    `Value::String("2009-02-13T23:31:30Z")` → encoder rejects;
    the user must call `timestamp(string)` inside the expression
    instead.
  - `timestamp(int)` overflow — `int(INT64_MAX) → timestamp` is
    out of the langdef Timestamp range; `CEL_ERR_OVERFLOW`.
  - `duration('Xs')` parse failure → `CEL_ERROR
    (kInvalidArgument)`.
  - `ts_a + ts_b` (timestamp + timestamp) — checker-rejected
    (no overload); regression-test that it's the checker, not
    codegen, that catches.
  - With-TZ accessor with invalid name → `CEL_ERROR`.
  - With-TZ accessor with the wrong arity (`ts.getYear(tz,
    extra)`) — checker rejects.

### 6.9 Test placement

  - `compiler_v2/api/internal/cel_host_test.cc` — Layer-2
    `CelTimestampParseImpl` / `CelDurationParseImpl` /
    `CelTimestampFormatImpl` / `CelDurationFormatImpl` /
    `CelTimestampTzAccessorImpl` parameterised tables.
  - `compiler_v2/runtime/cel_time_test.cc` (new) — pure-wasm
    helper tables (arithmetic + ordering + civil-calendar +
    UTC accessors).
  - `compiler_v2/codegen/expr_lower_test.cc` — `_+_` /
    accessor / parse lowering shapes; assert emitted wasm
    matches the WAT traces byte-for-byte.
  - `compiler_v2/api/instance_test.cc` — activation marshalling
    + decoder round-trip table (M7B.A).
  - `compiler_v2/e2e/m7b_test.cc` (new) — every conformance-row-
    shape, parameterised against the matrix above, plus the
    §6.5 cross-form equivalence rows and the §6.7 with-TZ rows.
  - `doc/implementation-plan/rewrite/wat/50_duration_arithmetic.wat`
    (M7B.B), `51_timestamp_year_utc.wat` (M7B.C),
    `52_timestamp_parse.wat` (M7B.D), `53_duration_format.wat`
    (M7B.D), `54_timestamp_year_with_tz.wat` (M7B.E),
    `55_timestamp_hours_fixed_offset.wat` (M7B.E).

## 7. Risks + open questions

Ranked highest → lowest.

  - **R1 — Civil-calendar correctness on the pure-wasm side.**
    Gregorian-cycle boundary cases are easy to get wrong: leap
    year rules (every 4, except every 100, except every 400),
    pre-1970 negative seconds (the `seconds / 86400` rounds
    toward zero in C, not toward negative infinity — must
    handle the floor-division explicitly), day-of-week phase at
    epoch (Thursday = 4 in absl's `Weekday::thursday`), end-of-
    month for leap-year February.  Mitigation: pin to the
    publicly-documented Howard Hinnant `civil_from_days`
    algorithm (cited in source); parameterise §6.4's quirk
    matrix so every test row exercises one specific quirk; run
    the absl-backed Layer-2 trampoline as the *oracle* in a
    cross-check unit test (a host-side `cel_civil_oracle_test`
    that calls absl directly and asserts the pure-wasm helper
    matches, parameterised over the §6.4 grid plus a wider
    sweep of ~10_000 random epoch seconds).
  - **R2 — RFC3339 parser admit-set drift vs cel-cpp.**  cel-cpp
    delegates to `absl::ParseTime`, which has its own admit
    set (strict vs lenient mode).  If we trampoline to
    `absl::ParseTime` from the Layer-2 impl, we inherit absl
    parity for free — that's the design choice, but the
    admit/reject test grid must still pin cel-cpp's exact
    behaviour, not just absl's.  Mitigation: golden table from
    cel-cpp's `runtime/standard/time_functions_test.cc`; if a
    fixture row disagrees, surface during M7B.D.
  - **R3 — IANA `tzdata` availability on host.**  Different host
    OSes ship different tz database versions; some embedded
    targets ship none.  `absl::TimeZone::Load("Africa/Cairo")`
    can return `false` even on common Linux distros.
    Mitigation: failure surfaces as `CEL_ERROR (kInvalidArgument)`
    per cel-cpp parity; document in
    `cel-host-surface.md` that the host's tzdata is the source
    of truth for TZ resolution; recommend distro-provided
    tzdata as a deploy requirement.
  - **R4 — Cross-form equivalence cost.**  The §4.7 field-read
    normaliser adds a per-field-read `full_name()` string
    compare for any singular message-typed field.  Hot-path
    cost: one strcmp + branch on every field read, regardless
    of whether the field is a well-known type.  Mitigation:
    cache the well-known-type descriptor pointers on `Instance`
    at Plan time (analogous to M8.B's wrapper-descriptor cache);
    swap the string compare for two pointer compares.
  - **R5 — Overflow semantics on arithmetic boundary.**
    `(seconds=INT64_MAX, nanos=999_999_999) + duration(1ns)` —
    nanos carry into seconds first, then seconds overflows.
    The naive implementation produces a wrap.  Mitigation:
    `__builtin_add_overflow` on the post-carry seconds, per
    §4.3 helper-body shape; pinned by §6.3's overflow row in
    every arithmetic helper.
  - **R6 — Constructor checker emit shape.**  cel-cpp's checker
    may emit `timestamp('...')` either as a direct call to the
    `string_to_timestamp` overload, or as a `Constant` if the
    argument is a string literal (constant-fold path).  Probe-
    spike at start of M7B.D — author a one-line cel-cpp
    roundtrip on `timestamp('2009-02-13T23:31:30Z')` and inspect
    the resulting `CheckedExpr`.  If checker constant-folds,
    M7B.D's parse trampoline is still needed for the
    activation-bound / cross-call case; just one more codegen
    arm.
  - **R7 — ABI surface count creep.**  M7B adds 5 host imports.
    Mitigation: the `cel_timestamp_tz_accessor` single-
    dispatch trampoline absorbs 10 would-be imports into 1;
    keep the `accessor_kind` enum stable on the wire (closed
    set, append-only).
  - **R8 — `ParseDuration` ↔ cel-cpp delegation.**  cel-cpp's
    duration parser internally calls `absl::ParseDuration` but
    also strips / canonicalises in `internal/time.cc`.  M7B.D's
    Layer-2 impl must include that canonicalisation step or
    the admit-set drifts.  Mitigation: copy cel-cpp's wrapper
    shape verbatim into `CelDurationParseImpl`.

## 8. Out-of-scope (re-stated)

  - **Per-eval current time** (`now()`).
  - **Extension-library time ops.**
  - **`Timestamp{...}` / `Duration{...}` proto-literal
    construction** — shipped at M7.E.  M7B's read normaliser
    (§4.7) ensures both forms decode to the same `CEL_TIMESTAMP`
    / `CEL_DURATION` kind.
  - **`Any`-wrapped Timestamp / Duration.**  Separate milestone.
  - **Leap-second modelling.**  cel-cpp / proto Timestamp don't;
    M7B doesn't.
  - **Wrapper coercion / auto-wrap into Timestamp / Duration
    fields.**  Wrapper types are M8; well-known-type Timestamp
    / Duration are M7B but follow a different normalisation
    path (`CEL_TIMESTAMP` vs the wrapper peel).
  - **Comprehension-driven timestamp construction** — falls out
    once M7B.D + comprehensions both ship; no M7B-specific
    work.

## 9. Future work

Surfaced during M7B planning but out of scope of this slice.

  - **`now()` standard function**.  Requires per-evaluation
    clock injection on `Activation`; today neither
    `Activation` nor `cel_host` has a clock primitive.  Design
    sketch: a `RuntimeBindings`-style hook on `Activation`
    carrying a `std::function<absl::Time()>`; new
    `cel_host.cel_now(out_slot)` import; codegen lowers `now`
    to the import.  Spec-pinned to be stable within a single
    evaluation (multiple `now()` calls in one expression
    return the same value).  Sized small; defer to when a
    fixture row gates on it.
  - **Extension-library time ops** — timestamp formatting with
    user-supplied format strings, `timezone(...)` constructors
    that return TZ objects.  Live behind the extensions
    milestone whenever that is scheduled.
  - **Well-known-type descriptor cache on `Instance`** — see
    §7 R4.  M7B.A wires the normaliser as a string compare;
    a follow-up swaps to a pointer-compare cache once the M8
    wrapper-descriptor cache pattern lands and is generalisable.
  - **Leap-second handling.**  If a future conformance update
    adds rows that mandate `2016-12-31T23:59:60Z` admit at
    parse, M7B.D's parse trampoline gains an arm; the
    arithmetic kernels stay correct because the seconds field
    is monotone.
  - **`Timestamp` / `Duration` over `Any`.**  Bundle with the
    Any milestone.
  - **Wrapper-style coercion for Timestamp / Duration arithmetic
    in the M8 sense** — `Timestamp{seconds: 1} + duration('1s')`.
    Shipped for free via the §4.7 read normaliser + the M7B.B
    arithmetic kernels (the read normaliser unwraps the proto
    literal into `CEL_TIMESTAMP`, then arithmetic dispatches
    normally).  No separate work needed; pin via the §6.5
    cross-form equivalence test.

## 10. Probes

Spike scripts that verified the architectural assertions in §4
before the plan landed.  All four probes were executed via the
bazel + abseil host toolchain on 2026-05-16; scratch sources
lived under `/tmp/celwasm_probes/` and were removed after the
findings were captured here.  None of the probes ship as
tracked source.

### 10.1 Probe A — civil-calendar correctness

**Question.**  Does the Hinnant `civil_from_days` algorithm
(`http://howardhinnant.github.io/date_algorithms.html#civil_from_days`)
produce output bit-identical to
`absl::ToCivilSecond(UTCTimeZone())` across the §6.4 quirk grid?

**Method.**  C++ scratch program (~50 LoC) implementing
`civil_from_days` per the documented Hinnant routine plus a
floor-division-by-86400 days-from-epoch wrapper; runs the result
side-by-side with `absl::FromUnixSeconds(s) + absl::ToCivilSecond`
across 12 epoch-second rows.

**Result.**  0 mismatches across all 12 rows:

| Date label | epoch_s | Hinnant (Y-M-D) | absl (Y-M-D) | match |
|---|---:|---|---|---|
| epoch | 0 | 1970-01-01 | 1970-01-01 | yes |
| 1969-12-31T23:59:59Z (negative epoch) | -1 | 1969-12-31 | 1969-12-31 | yes |
| Y2K leap-divisible-400 | 946684800 | 2000-01-01 | 2000-01-01 | yes |
| 1900-01-01 (cent-not-leap) | -2208988800 | 1900-01-01 | 1900-01-01 | yes |
| langdef sample 2009-02-13T23:31:30Z | 1234567890 | 2009-02-13 | 2009-02-13 | yes |
| 2024-02-29 leap-Feb | 1709164800 | 2024-02-29 | 2024-02-29 | yes |
| 2023-02-28 nonleap end-Feb | 1677628800 | 2023-03-01 | 2023-03-01 | yes |
| 2024-12-31 last leap day-of-year | 1735689599 | 2024-12-31 | 2024-12-31 | yes |
| 2023-12-31 last non-leap | 1704067199 | 2023-12-31 | 2023-12-31 | yes |
| 9999-12-31 langdef-upper | 253402300799 | 9999-12-31 | 9999-12-31 | yes |
| 0001-01-01 langdef-lower | -62135596800 | 0001-01-01 | 0001-01-01 | yes |
| 2038-01-19 Y2038 sanity | 2147483647 | 2038-01-19 | 2038-01-19 | yes |

**Implication.**  The pure-wasm civil-calendar path (§4.9
`cel_civil_from_seconds` + its 10 per-field projection helpers
under M7B.C) is correctness-equivalent to absl.  Option C's
no-TZ-accessor split is viable; no host trampoline needed for
the 10 UTC accessors.

### 10.2 Probe B — `absl::ParseTime` vs CEL admit-set

**Question.**  Does the Layer-2 `CelTimestampParseImpl` need to
post-validate after `absl::ParseTime(absl::RFC3339_full, ...)`,
or does absl admit exactly the CEL spec's set?

**Method.**  Scratch program runs `absl::ParseTime` against 15
inputs split admit / reject per the §6.2 plan; flags where absl
deviates from CEL.

**Result.**  4 drift rows; absl is laxer than CEL on 4 reject
cases:

| input | CEL expected | absl-parses | match |
|---|---|---|---|
| `2009-02-13T23:31:30Z` | admit | admit | yes |
| `2009-02-13T23:31:30+02:00` | admit | admit | yes |
| `2009-02-13T23:31:30-08:00` | admit | admit | yes |
| `2009-02-13T23:31:30.123456789Z` | admit | admit | yes |
| `2009-02-13T23:31:30.5Z` | admit | admit | yes |
| `1970-01-01T00:00:00Z` | admit | admit | yes |
| `0001-01-01T00:00:00Z` | admit | admit | yes |
| `9999-12-31T23:59:59Z` | admit | admit | yes |
| `2009-02-13T23:31:30` | reject (missing TZ) | reject | yes |
| `2009-02-13T23:31:30z` | reject (lowercase z) | **admit** | **DRIFT** |
| `2009-02-13 23:31:30Z` | reject (space sep) | reject | yes |
| `2009-02-13T23:31:30Z extra` | reject (trailing) | reject | yes |
| `10000-01-01T00:00:00Z` | reject (year > 9999) | **admit** | **DRIFT** |
| `2016-12-31T23:59:60Z` | reject (leap-second) | **admit** | **DRIFT** |
| `09-02-13T23:31:30Z` | reject (two-digit year) | **admit** | **DRIFT** |

**Implication.**  M7B.D's `CelTimestampParseImpl` MUST
post-validate the absl-admitted seconds against the langdef
range `[-62_135_596_800, 253_402_300_799]` (year 0001–9999) AND
reject leap-second + lowercase-z + two-digit-year inputs.
Concretely: re-scan the input string for these patterns before
trusting `absl::ParseTime`'s success.  Cited from this probe in
R2 of §7.

### 10.3 Probe C — `absl::ParseDuration` vs CEL admit-set

**Question.**  Same question as Probe B, for
`absl::ParseDuration`.

**Method.**  Scratch program over 17 inputs split admit / reject.

**Result.**  1 drift row:

| input | CEL expected | absl-parses | match |
|---|---|---|---|
| `3600s` | admit | admit | yes |
| `-3600s` | admit | admit | yes |
| `0s` | admit | admit | yes |
| `1000000s` | admit | admit | yes |
| `1.5s` | admit | admit | yes |
| `1.000000001s` | admit | admit | yes |
| `1h2m3s` | admit | admit | yes |
| `-1h2m3s` | admit | admit | yes |
| `500ms` | admit | admit | yes |
| `-1us` | admit | admit | yes |
| `100ns` | admit | admit | yes |
| `` | reject | reject | yes |
| `3600` | reject (no unit) | reject | yes |
| `3600x` | reject (unknown unit) | reject | yes |
| `1s2h` | reject (wrong order) | **admit (= 2h1s)** | **DRIFT** |
| `1s ` | reject (trailing space) | reject | yes |
| `9223372036854775808s` | reject (overflow) | reject | yes |

**Implication.**  M7B.D's `CelDurationParseImpl` must reject
unordered compound forms (`1s2h`, `3s1m`, …) that absl admits.
Concretely: post-validate by walking the input once and
verifying unit order is strictly decreasing (`h` > `m` > `s` >
`ms` > `us` > `ns`).  Cited in R8 of §7.

### 10.4 Probe D — `CelDurTs` wire layout vs `absl::Duration`

**Question.**  Does the `CelDurTs { int64 seconds; int32 nanos;
int32 _pad }` layout in `compiler_v2/runtime/cel_data.h`
accommodate the full `absl::Duration` range, and what sign
convention does the arithmetic kernel pick?

**Method.**  Scratch program decomposes 6 representative
durations via two absl decompositions: the `IDivDuration` ladder
that `EncodeBoundValue` in §4.6 uses, and `absl::ToTimespec`.

**Result.**

| label | IDiv(s) | IDiv(ns) | Timespec(s) | Timespec(ns) | nanos convention |
|---|---:|---:|---:|---:|---|
| +1.5s | 1 | 500000000 | 1 | 500000000 | matches |
| -1.5s | -1 | -500000000 | -2 | 500000000 | **diverges** |
| -1ns | 0 | -1 | -1 | 999999999 | **diverges** |
| +1ns | 0 | 1 | 0 | 1 | matches |
| int64 max-ish (s=9223372036, ns=854775807) | 9223372036 | 854775807 | 9223372036 | 854775807 | matches |
| int64 min-ish (s=-9223372036, ns=0) | -9223372036 | 0 | -9223372036 | 0 | matches |

**Implication.**  Two valid `(seconds, nanos)` decompositions
for any negative duration:

  - **IDivDuration convention** (sign-correlated): `seconds` and
    `nanos` share sign.  `nanos ∈ (-1_000_000_000, 1_000_000_000)`.
  - **Timespec convention** (Unix-floor): `seconds` is floor of
    the real duration, `nanos ∈ [0, 1_000_000_000)`.  Negative
    durations carry seconds one tick more negative.

The proto Duration text format **mandates sign-correlated**
(per `google.protobuf.Duration` docstring), matching the
`IDivDuration` form.  M7B's `CelDurTs` arithmetic kernels MUST
use this convention.  This is a one-line invariant in
`cel_time.c`'s post-carry normaliser; pin it via a §6.3 row
that explicitly asserts `-1.5s` decomposes to
`(seconds=-1, nanos=-500000000)`, NOT `(-2, 500000000)`.  Add
the matching assertion to the §6.1 round-trip matrix's
`DurationIntMinS` cell — that row binds a negative duration
and round-trips through Eval, surfacing any
EncodeBoundValue → DecodeCelValueAt convention drift.

## 11. Implementation scaffolding (in tree as of 2026-05-16)

### 11.1 E2e tests — `compiler_v2/e2e/m7b_test.cc`

~770-line scaffold mirroring `m7_test.cc`'s shape.  Every test
class SKIPs with a category-specific message (`M7B.A not yet
shipped`, `M7B.B not yet shipped`, …); turns on row-by-row as
the slices ship.  Build target:
`bazel build //compiler_v2/e2e:m7b_test` (green 2026-05-16).

Tests grouped per the §5 slice carve-out, with the §6 matrix
materialised as `INSTANTIATE_TEST_SUITE_P` tables.  Key cross-
references:

  - §6.1 round-trip data-shape matrix → `RoundTripE2ETest`
    parameterised by `RoundTripCase` (13 boundary rows covering
    `DurationZero`, `DurationOneSec`, `DurationOneNs`,
    `DurationMaxNanos`, `DurationNegOneNs`, `DurationIntMaxS`,
    `DurationIntMinS`, `TimestampZero`, `TimestampEpochOneSec`,
    `TimestampLangdef`, `TimestampLangdefMin`, `TimestampLangdefMax`,
    `TimestampMaxNanos`) + standalone bind-mismatch rows.
  - §6.2 parse admit/reject matrix →
    `ParseFormatE2ETest::AdmitOrReject` parameterised over
    `TimestampParseAdmit` (`TsAdmit_*` / `TsReject_*` — 10 rows
    covering base UTC, fixed offset, fractional, year bounds,
    leap-second reject, lowercase-z reject) and
    `DurationParseAdmit` (`DurAdmit_*` / `DurReject_*` — 17 rows
    covering integer/fractional/compound + every Probe-C drift
    pattern).
  - §6.3 arithmetic + overflow matrix →
    `ArithmeticE2ETest::BoundaryMatrix` parameterised over
    `BoundaryMatrix` (22 rows, six helpers × the §6.3 grid)
    plus standalone checker-reject regressions for `ts+ts` and
    `dur-ts`.
  - §6.4 civil-calendar quirk matrix →
    `UtcAccessorE2ETest::ProjectField` parameterised over
    `QuirkGrid` (31 rows covering the §6.4 calendar quirks ×
    11 accessor projections) plus `DurationAccessorE2ETest`
    (15 truncating-division rows).
  - §6.5 equality + ordering matrix → ordering split into
    `OrderingE2ETest::LexicographicCompare` parameterised over
    `LexCompareGrid` (19 rows); cross-form equality lives in
    `CrossFormEquivalenceE2ETest` (7 cases including the §6.5
    `NoNaNRegression` pin and the `CanonicalisationEqualsAcrossUnits`
    `1s == 1000ms` row).
  - §6.6 zero-value cross-product → folded into the
    `RoundTripE2ETest` zero rows + `FormatConvertE2ETest::Int64ToTimestamp`
    + `FormatConvertE2ETest::TimestampToString` cases.
  - §6.7 with-TZ matrix →
    `TzAccessorE2ETest::IanaOrFixedOffset` parameterised over
    `TzGrid` (10 rows: IANA LA / UTC / Sydney + fixed
    `+02:00` / `-08:00` / `+00:00` + invalid name / empty /
    invalid offset reject).
  - §6.8 negative / rejection matrix → `RejectE2ETest` with 8
    test methods covering parse error, checker rejection,
    arity reject, invalid TZ runtime error, int→ts overflow,
    descriptor mismatch.
  - §6.6 type regression → `TypeRegressionE2ETest`.

### 11.2 WAT traces

Two files added under `doc/implementation-plan/rewrite/wat/`,
both `wasm-as` cleanly:

  - `42_timestamp_parse.wat` — `cel_host.cel_timestamp_parse(out_slot,
    str_slot)` for `timestamp("2009-02-13T23:31:30Z")`.  Host
    trampoline; codegen emits the call shape after M7B.D ships.
  - `43_timestamp_accessor.wat` — pure-wasm
    `cel_ts_day_of_month_1_utc(out_slot, ts_slot)` for
    `ts.getDate()`.  No host trampoline (UTC-only accessor path
    per Option C + Probe A's civil-calendar-correctness
    confirmation).

Walkthroughs added in `wat-traces.md` §42 / §43 (immediately
before "Future entries").  Both files mirror the existing
`16_arith_int_add.wat` / `08_map_index_host.wat` shape exactly —
slot-out ABI, rodata layout, codegen call-site comment.

### 11.3 Benchmarks — `compiler_v2/bench/m7b_time_bench.cc`

New ~270-line file alongside `kernel_bench.cc` /
`pipeline_bench.cc`.  Distinct binary so the M7B-specific
benches don't increase `kernel_bench`'s linker churn for every
slice that touches an unrelated kernel.  Today most BMs are
guarded behind `CELWASM_M7B_SHIPPED`; the file compiles green
and the BUILD target `//compiler_v2/bench:m7b_time_bench`
validates.

Bench cohort (per the README "M7B time benchmarks" section):

  - **Sanity baseline.**  `BM_IntAddBaseline` — duplicates
    `kernel_bench::BM_IntAdd` so the M7B numbers can be read
    against a tight integer baseline without cross-referencing
    a second bench file.  Active today (no guard).
  - **Arithmetic** (M7B.B).  `BM_DurationAdd`, `BM_DurationSub`,
    `BM_TimestampSubTimestamp`, `BM_TimestampAddDuration`.
    Each exercises one of the §4.3 pure-wasm kernels.  Expected
    cost: within ~2× of `BM_IntAddBaseline`.
  - **UTC accessor** (M7B.C).  `BM_TimestampYearUtc` (langdef
    example, typical civil walk), `BM_TimestampYearUtcLangdefMax`
    (Y9999 worst-case `era` arm), `BM_TimestampDayOfWeekUtc`
    (different field projection out of same `CelCivil`).
    Expected cost: 5–10× a numeric kernel call (integer-divide
    cascade in `cel_civil_from_seconds`).
  - **Duration accessor** (M7B.C).  `BM_DurationHours` —
    truncating int division, no civil walk.  Expected: same
    band as `BM_IntAddBaseline`.

The host-trampoline parse bench
(`cel_host.cel_timestamp_parse` for M7B.D) is sketched in the
file under `#ifdef CELWASM_M7B_PIPELINE_BENCH_SHIPPED` but
belongs in `pipeline_bench.cc` rather than this kernel-bench
because the trampoline is only reachable through wasmtime.  The
1–2-orders-of-magnitude gap vs the kernel benches is the
documented expectation; the bench captures the boundary-cross
cost in a load-bearing way.

Build target: `bazel build //compiler_v2/bench:m7b_time_bench`
(green 2026-05-16).

### 11.4 Production-code touchpoints (pending implementation)

For M7B.A–E the touchpoints are:

  - `compiler_v2/runtime/cel_time.{h,c}` — NEW.  Pure-wasm
    arithmetic + UTC accessor kernels.  Slot-out ABI per
    `design.md §4.2`.
  - `compiler_v2/api/internal/cel_host.cc` — add
    `CelTimestampParseImpl`, `CelDurationParseImpl`,
    `CelTimestampFormatImpl`, `CelDurationFormatImpl`, plus
    with-TZ accessor trampoline per the §4.3 single-dispatch
    decision (recommended in §4.4 Q3).
  - `compiler_v2/api/internal/cel_host_wasmtime.cc` —
    `RegisterCelHostImports` rows for the new trampolines.
  - `compiler_v2/api/instance.cc` — fill in `EncodeBoundValue`
    arms for `Repr::kDuration` / `Repr::kTimestamp` (today they
    return `UnimplementedError`); fill in `DecodeCelValueAt`
    arms for `CEL_DURATION` / `CEL_TIMESTAMP` (today they fall
    into the default `InvalidArgument`).
  - `compiler_v2/codegen/overload_table.cc` — move ~28
    timestamp / duration overload ids from
    `kExplicitlyUnimplementedIds` to `kBuiltinSeeds`, paired
    with the new runtime helpers.

None touched in the 2026-05-16 LLD pass; they are the M7B.A–E
implementation scope.
