#include "compiler_v2/codegen/overload_table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace celwasm {

absl::string_view ImportModuleName(ImportModule m) {
  switch (m) {
    case ImportModule::kCelRuntime:
      return "cel";
    case ImportModule::kCelHost:
      return "cel_host";
  }
  ABSL_CHECK(false) << "unknown ImportModule=" << static_cast<int>(m);
  return {};
}

namespace {

// Built-in seeds. Maps every cel-cpp `StandardOverloadIds::k*`
// constant the runtime supports to the wasm export that
// implements it. Categories are grouped so a reader can see at a
// glance what's covered: arithmetic same-kind, cross-numeric and
// same-kind ordering, container size/in/concat, polymorphic
// equality, string ops, 3VL control flow, timestamp/duration
// arithmetic+ordering+accessors+conversions, numeric/string/bytes
// conversions.
//
// Every other cel-cpp id is in `kExplicitlyUnimplementedIds`
// (see below). The two sets together cover the full
// `StandardOverloadIds` surface — enforced by
// `overload_table_test::CoverageTripwire`.
//
// `_[_]` indexing, ternary `_?_:_`, and `not_strictly_false` are
// deliberately absent from the seeds: the kCall arm in
// `expr_lower.cc` special-cases them (indexing is origin-aware;
// `_?_:_` lowers as `BinaryenIf`). They appear in
// `kExplicitlyUnimplementedIds` so the tripwire still classifies
// every cel-cpp id.
//
// **Aggregate-op dispatch.** The `size_*` / `in_*` / `add_list` /
// aggregate-equality seeds name the **kDynamic dispatcher** (e.g.
// `cel_list_size`, NOT `cel_list_size_arena`). The dispatcher
// branches on the operand's runtime `kind` and
// `__attribute__((musttail))`-jumps to the arena fast-path or the
// kHost trampoline. Trade-off: one extra runtime branch on every
// aggregate-op call, but the OverloadTable stays a flat
// (id → helper-name) map and the codegen kCall arm in
// `expr_lower.cc` can do a single lookup-and-emit (mirrors
// arithmetic / compare). Three-path origin dispatch is documented
// in `rewrite/map-list-dispatch.md`.
constexpr std::array<Seed, 158> kBuiltinSeeds{
    // ── Arithmetic same-kind ──────────────────────────────────
    Seed{"add_int64", {ImportModule::kCelRuntime, "cel_int_add_at_vv"}},
    Seed{"add_uint64", {ImportModule::kCelRuntime, "cel_uint_add_at_vv"}},
    Seed{"add_double", {ImportModule::kCelRuntime, "cel_double_add_at_vv"}},
    Seed{"subtract_int64", {ImportModule::kCelRuntime, "cel_int_sub_at_vv"}},
    Seed{"subtract_uint64", {ImportModule::kCelRuntime, "cel_uint_sub_at_vv"}},
    Seed{"subtract_double",
         {ImportModule::kCelRuntime, "cel_double_sub_at_vv"}},
    Seed{"multiply_int64", {ImportModule::kCelRuntime, "cel_int_mul_at_vv"}},
    Seed{"multiply_uint64", {ImportModule::kCelRuntime, "cel_uint_mul_at_vv"}},
    Seed{"multiply_double",
         {ImportModule::kCelRuntime, "cel_double_mul_at_vv"}},
    Seed{"divide_int64", {ImportModule::kCelRuntime, "cel_int_div_at_vv"}},
    Seed{"divide_uint64", {ImportModule::kCelRuntime, "cel_uint_div_at_vv"}},
    Seed{"divide_double", {ImportModule::kCelRuntime, "cel_double_div_at_vv"}},
    Seed{"modulo_int64", {ImportModule::kCelRuntime, "cel_int_mod_at_vv"}},
    Seed{"modulo_uint64", {ImportModule::kCelRuntime, "cel_uint_mod_at_vv"}},
    Seed{"negate_int64", {ImportModule::kCelRuntime, "cel_int_neg_at_v"}},
    Seed{"negate_double", {ImportModule::kCelRuntime, "cel_double_neg_at_v"}},
    // ── Concat (`_+_` for strings / bytes / lists) ────────────
    Seed{"add_string", {ImportModule::kCelRuntime, "cel_string_concat_at_vv"}},
    Seed{"add_bytes", {ImportModule::kCelRuntime, "cel_bytes_concat_at_vv"}},
    // `add_list` names the kDynamic dispatcher; the dispatcher
    // tail-calls the kHost trampoline on host-backed operands.
    Seed{"add_list", {ImportModule::kCelRuntime, "cel_list_concat"}},
    // ── Same-kind ordering (`_<_`, `_<=_`, `_>_`, `_>=_`) ─────
    Seed{"less_int64", {ImportModule::kCelRuntime, "cel_int_lt_at_vv"}},
    Seed{"less_uint64", {ImportModule::kCelRuntime, "cel_uint_lt_at_vv"}},
    Seed{"less_double", {ImportModule::kCelRuntime, "cel_double_lt_at_vv"}},
    Seed{"less_string", {ImportModule::kCelRuntime, "cel_string_lt_at_vv"}},
    Seed{"less_bytes", {ImportModule::kCelRuntime, "cel_bytes_lt_at_vv"}},
    Seed{"less_equals_int64", {ImportModule::kCelRuntime, "cel_int_le_at_vv"}},
    Seed{"less_equals_uint64",
         {ImportModule::kCelRuntime, "cel_uint_le_at_vv"}},
    Seed{"less_equals_double",
         {ImportModule::kCelRuntime, "cel_double_le_at_vv"}},
    Seed{"greater_int64", {ImportModule::kCelRuntime, "cel_int_gt_at_vv"}},
    Seed{"greater_uint64", {ImportModule::kCelRuntime, "cel_uint_gt_at_vv"}},
    Seed{"greater_double", {ImportModule::kCelRuntime, "cel_double_gt_at_vv"}},
    Seed{"greater_equals_int64",
         {ImportModule::kCelRuntime, "cel_int_ge_at_vv"}},
    Seed{"greater_equals_uint64",
         {ImportModule::kCelRuntime, "cel_uint_ge_at_vv"}},
    Seed{"greater_equals_double",
         {ImportModule::kCelRuntime, "cel_double_ge_at_vv"}},
    // ── Cross-type numeric ladder ────────────────────────────
    // Every {int,uint,double} × {int,uint,double} cross-kind pair
    // for `<`, `<=`, `>`, `>=`.  All six routes through the
    // single `cel_numeric_<op>_at_vv` helper which dispatches on
    // the operand kinds at runtime — see
    // `cel_runtime.c::numeric_compare_kernel`.  Same-kind ladder
    // ids stay on the per-kind helpers above (one less branch
    // per call).
    //
    // Note: cel-cpp ships `greater_equals_uint_double` with `_uint`
    // (no `64`) — see `third_party/cel-cpp/common/standard_definitions.h`
    // line 212.  We mirror it verbatim; the coverage tripwire test
    // does a byte-equal lookup against `S::kGreaterEqualsUintDouble`
    // so any "fix" here would silently regress.
    Seed{"less_int64_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_int64_double",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_double_int64",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_double_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_uint64_int64",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_uint64_double",
         {ImportModule::kCelRuntime, "cel_numeric_lt_at_vv"}},
    Seed{"less_equals_int64_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"less_equals_int64_double",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"less_equals_double_int64",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"less_equals_double_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"less_equals_uint64_int64",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"less_equals_uint64_double",
         {ImportModule::kCelRuntime, "cel_numeric_le_at_vv"}},
    Seed{"greater_int64_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_int64_double",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_double_int64",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_double_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_uint64_int64",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_uint64_double",
         {ImportModule::kCelRuntime, "cel_numeric_gt_at_vv"}},
    Seed{"greater_equals_int64_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    Seed{"greater_equals_int64_double",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    Seed{"greater_equals_double_int64",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    Seed{"greater_equals_double_uint64",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    Seed{"greater_equals_uint64_int64",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    Seed{"greater_equals_uint_double",
         {ImportModule::kCelRuntime, "cel_numeric_ge_at_vv"}},
    // ── Bool ordering (`false < true` per langdef §"Booleans") ─
    Seed{"less_bool", {ImportModule::kCelRuntime, "cel_bool_lt_at_vv"}},
    Seed{"less_equals_bool", {ImportModule::kCelRuntime, "cel_bool_le_at_vv"}},
    Seed{"greater_bool", {ImportModule::kCelRuntime, "cel_bool_gt_at_vv"}},
    Seed{"greater_equals_bool",
         {ImportModule::kCelRuntime, "cel_bool_ge_at_vv"}},
    // ── String / bytes ordering tail (le / gt / ge) ───────────
    Seed{"less_equals_string",
         {ImportModule::kCelRuntime, "cel_string_le_at_vv"}},
    Seed{"less_equals_bytes",
         {ImportModule::kCelRuntime, "cel_bytes_le_at_vv"}},
    Seed{"greater_string", {ImportModule::kCelRuntime, "cel_string_gt_at_vv"}},
    Seed{"greater_bytes", {ImportModule::kCelRuntime, "cel_bytes_gt_at_vv"}},
    Seed{"greater_equals_string",
         {ImportModule::kCelRuntime, "cel_string_ge_at_vv"}},
    Seed{"greater_equals_bytes",
         {ImportModule::kCelRuntime, "cel_bytes_ge_at_vv"}},
    // ── Container size (function + member-call ids share helpers) ──
    // String / bytes have no origin (rodata or arena scalar
    // payload); seed names the leaf helper directly.
    Seed{"size_string", {ImportModule::kCelRuntime, "cel_string_size_at_v"}},
    Seed{"string_size", {ImportModule::kCelRuntime, "cel_string_size_at_v"}},
    Seed{"size_bytes", {ImportModule::kCelRuntime, "cel_bytes_size_at_v"}},
    Seed{"bytes_size", {ImportModule::kCelRuntime, "cel_bytes_size_at_v"}},
    // List / map size names the kDynamic dispatcher (Option B).
    Seed{"size_list", {ImportModule::kCelRuntime, "cel_list_size"}},
    Seed{"list_size", {ImportModule::kCelRuntime, "cel_list_size"}},
    Seed{"size_map", {ImportModule::kCelRuntime, "cel_map_size"}},
    Seed{"map_size", {ImportModule::kCelRuntime, "cel_map_size"}},
    // ── Container `in` (kDynamic dispatcher) ─────────────────
    Seed{"in_list", {ImportModule::kCelRuntime, "cel_list_in"}},
    Seed{"in_map", {ImportModule::kCelRuntime, "cel_map_in"}},
    // ── Polymorphic equals / not_equals ──────────────────────
    // Single overload id per cel-cpp; the runtime helper switches
    // on (a.kind, b.kind) and dispatches into same-kind / cross-
    // numeric / aggregate / message arms.  Mismatched scalar
    // kinds return `false`, NOT error (langdef §"Equality").
    Seed{"equals", {ImportModule::kCelRuntime, "cel_equals_at_vv"}},
    Seed{"not_equals", {ImportModule::kCelRuntime, "cel_not_equals_at_vv"}},
    // ── 3VL / control-flow operators ─────────────────────────
    // `_&&_` / `_||_` / `!_` route through the standard slot-out
    // ABI; non-strict semantics + the 3VL truth table live entirely
    // inside the runtime helper, so codegen treats them like any
    // other call.  `conditional` (`_?_:_`) is the odd one out — its
    // BinaryenIf-based lowering is special-cased in expr_lower and
    // stays in `kExplicitlyUnimplementedIds` accordingly.
    Seed{"logical_and", {ImportModule::kCelRuntime, "cel_and"}},
    Seed{"logical_or", {ImportModule::kCelRuntime, "cel_or"}},
    Seed{"logical_not", {ImportModule::kCelRuntime, "cel_not"}},
    // ── Timestamp / Duration arithmetic ──────────────────────
    // Pure-wasm kernels in cel_time.c.  Result kinds:
    //   (dur, dur) -> dur                          : add, sub
    //   (ts, dur) -> ts                            : add, sub
    //   (dur, ts) -> ts                            : add (commutative)
    //   (ts, ts) -> dur                            : sub  (delta)
    Seed{"add_duration_duration",
         {ImportModule::kCelRuntime, "cel_dur_add_at_vv"}},
    Seed{"add_duration_timestamp",
         {ImportModule::kCelRuntime, "cel_dur_ts_add_at_vv"}},
    Seed{"add_timestamp_duration",
         {ImportModule::kCelRuntime, "cel_ts_dur_add_at_vv"}},
    Seed{"subtract_duration_duration",
         {ImportModule::kCelRuntime, "cel_dur_sub_at_vv"}},
    Seed{"subtract_timestamp_duration",
         {ImportModule::kCelRuntime, "cel_ts_dur_sub_at_vv"}},
    Seed{"subtract_timestamp_timestamp",
         {ImportModule::kCelRuntime, "cel_ts_ts_sub_at_vv"}},
    // ── Timestamp / Duration ordering ────────────────────────
    // Lexicographic (seconds, nanos) compare on the sign-correlated
    // CelDurTs payload.  Equality / inequality route through the
    // standard `equals` / `not_equals` seeds above; the
    // cel_value_eq_polymorphic kernel in cel_runtime.c gained
    // CEL_DURATION / CEL_TIMESTAMP arms in the same slice.
    Seed{"less_duration", {ImportModule::kCelRuntime, "cel_dur_lt_at_vv"}},
    Seed{"less_timestamp", {ImportModule::kCelRuntime, "cel_ts_lt_at_vv"}},
    Seed{"less_equals_duration",
         {ImportModule::kCelRuntime, "cel_dur_le_at_vv"}},
    Seed{"less_equals_timestamp",
         {ImportModule::kCelRuntime, "cel_ts_le_at_vv"}},
    Seed{"greater_duration", {ImportModule::kCelRuntime, "cel_dur_gt_at_vv"}},
    Seed{"greater_timestamp", {ImportModule::kCelRuntime, "cel_ts_gt_at_vv"}},
    Seed{"greater_equals_duration",
         {ImportModule::kCelRuntime, "cel_dur_ge_at_vv"}},
    Seed{"greater_equals_timestamp",
         {ImportModule::kCelRuntime, "cel_ts_ge_at_vv"}},
    // ── Timestamp UTC accessors ──────────────────────────────
    // Pure-wasm kernels over the Hinnant civil-calendar helper in
    // cel_time.c.  No TZ argument — UTC by definition; with-TZ
    // variants route through a single dispatch trampoline (see
    // the with-TZ accessor shims below).
    Seed{"timestamp_to_year",
         {ImportModule::kCelRuntime, "cel_ts_year_utc_at_v"}},
    Seed{"timestamp_to_month",
         {ImportModule::kCelRuntime, "cel_ts_month_utc_at_v"}},
    Seed{"timestamp_to_day_of_month_1_based",
         {ImportModule::kCelRuntime, "cel_ts_day_of_month_1_utc_at_v"}},
    Seed{"timestamp_to_day_of_month",
         {ImportModule::kCelRuntime, "cel_ts_day_of_month_utc_at_v"}},
    Seed{"timestamp_to_day_of_year",
         {ImportModule::kCelRuntime, "cel_ts_day_of_year_utc_at_v"}},
    Seed{"timestamp_to_day_of_week",
         {ImportModule::kCelRuntime, "cel_ts_day_of_week_utc_at_v"}},
    Seed{"timestamp_to_hours",
         {ImportModule::kCelRuntime, "cel_ts_hours_utc_at_v"}},
    Seed{"timestamp_to_minutes",
         {ImportModule::kCelRuntime, "cel_ts_minutes_utc_at_v"}},
    Seed{"timestamp_to_seconds",
         {ImportModule::kCelRuntime, "cel_ts_seconds_utc_at_v"}},
    Seed{"timestamp_to_milliseconds",
         {ImportModule::kCelRuntime, "cel_ts_milliseconds_utc_at_v"}},
    // ── Duration accessors ───────────────────────────────────
    // Truncating integer divisions on the sign-correlated payload.
    Seed{"duration_to_hours",
         {ImportModule::kCelRuntime, "cel_dur_hours_at_v"}},
    Seed{"duration_to_minutes",
         {ImportModule::kCelRuntime, "cel_dur_minutes_at_v"}},
    Seed{"duration_to_seconds",
         {ImportModule::kCelRuntime, "cel_dur_seconds_at_v"}},
    Seed{"duration_to_milliseconds",
         {ImportModule::kCelRuntime, "cel_dur_milliseconds_at_v"}},
    // ── Timestamp / Duration <-> int conversions ─────────────
    // langdef: `int(timestamp)` returns epoch-seconds; `int(duration)`
    // returns whole seconds.  Reverse builds `(seconds, 0)`; the
    // timestamp form additionally range-checks the langdef [year 1,
    // year 9999] bound.
    Seed{"timestamp_to_int64",
         {ImportModule::kCelRuntime, "cel_ts_to_int_at_v"}},
    Seed{"duration_to_int64",
         {ImportModule::kCelRuntime, "cel_dur_to_int_at_v"}},
    Seed{"int64_to_timestamp",
         {ImportModule::kCelRuntime, "cel_int_to_ts_at_v"}},
    Seed{"int64_to_duration",
         {ImportModule::kCelRuntime, "cel_int_to_dur_at_v"}},
    // ── Identity conversions ─────────────────────────────────
    // `timestamp(timestamp)` / `duration(duration)` share the
    // `cel_copy_slot` shape used by the scalar identity
    // conversions below.
    Seed{"timestamp_to_timestamp",
         {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"duration_to_duration", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    // ── String <-> Timestamp / Duration ──────────────────────
    // RFC3339 parse / proto-Duration text-format parse + format.
    // Self-hosted inside cel_runtime.wasm via vendored absl
    // (`compiler_v2/runtime/cel_time_parse.cc`); see
    // `rewrite/phase-c-plan.md` §4.1-4.4 for the kernel contracts
    // (admit/reject envelope, error codes, RFC3339 format spec).
    Seed{"string_to_timestamp",
         {ImportModule::kCelRuntime, "cel_timestamp_parse_at_v"}},
    Seed{"string_to_duration",
         {ImportModule::kCelRuntime, "cel_duration_parse_at_v"}},
    Seed{"timestamp_to_string",
         {ImportModule::kCelRuntime, "cel_timestamp_format_at_v"}},
    Seed{"duration_to_string",
         {ImportModule::kCelRuntime, "cel_duration_format_at_v"}},
    // ── With-TZ accessor shims ───────────────────────────────
    // Each id seeds to a pure-wasm shim helper that calls the
    // single `cel_host.cel_timestamp_tz_accessor(out, ts, tz, kind)`
    // host trampoline with a fixed `accessor_kind` constant.  The
    // shim shape `_at_vv` (3-arg: out + ts + tz) matches the
    // standard helper ABI; the kind constant is supplied by the
    // shim, not the codegen.  See
    // `rewrite/m7b-duration-timestamp.md` §4.3-§4.4 for the
    // single-trampoline-per-TZ-accessor rationale.
    Seed{"timestamp_to_year_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_year_with_tz_at_vv"}},
    Seed{"timestamp_to_month_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_month_with_tz_at_vv"}},
    Seed{"timestamp_to_day_of_month_1_based_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_day_of_month_1_with_tz_at_vv"}},
    Seed{"timestamp_to_day_of_month_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_day_of_month_with_tz_at_vv"}},
    Seed{"timestamp_to_day_of_year_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_day_of_year_with_tz_at_vv"}},
    Seed{"timestamp_to_day_of_week_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_day_of_week_with_tz_at_vv"}},
    Seed{"timestamp_to_hours_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_hours_with_tz_at_vv"}},
    Seed{"timestamp_to_minutes_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_minutes_with_tz_at_vv"}},
    // cel-cpp ships this overload-id with `_tz` suffix (not
    // `_with_tz`); mirror verbatim — the coverage tripwire test
    // does a byte-equal lookup, so a "fix" here would silently
    // regress.  See `standard_definitions.h` in cel-cpp.
    Seed{"timestamp_to_seconds_tz",
         {ImportModule::kCelRuntime, "cel_ts_seconds_with_tz_at_vv"}},
    Seed{"timestamp_to_milliseconds_with_tz",
         {ImportModule::kCelRuntime, "cel_ts_milliseconds_with_tz_at_vv"}},
    // ── String ops (`contains` / `startsWith` / `endsWith`) ───
    Seed{"contains_string",
         {ImportModule::kCelRuntime, "cel_string_contains_at_vv"}},
    Seed{"starts_with_string",
         {ImportModule::kCelRuntime, "cel_string_starts_with_at_vv"}},
    Seed{"ends_with_string",
         {ImportModule::kCelRuntime, "cel_string_ends_with_at_vv"}},
    // Regex `matches(text, pat)` / `text.matches(pat)`.  RE2-backed
    // PartialMatch self-hosted in cel_runtime.wasm with a per-Instance
    // single-slot most-recent-pattern cache (the common
    // `list.exists(x, x.matches(pat))` shape hits the cache every
    // iteration past the first).  See `rewrite/phase-c-plan.md` §4.5.
    // cel-cpp's standard library registers both the global-form
    // `matches` overload id and the receiver-form `matches_string`
    // id; both route to the same kernel.
    Seed{"matches", {ImportModule::kCelRuntime, "cel_matches_at_vv"}},
    Seed{"matches_string", {ImportModule::kCelRuntime, "cel_matches_at_vv"}},
    // `type(x)` standard function.  Pure-runtime helper that
    // reads the operand kind, looks up the spec type-name in the
    // 12-row table in `cel_runtime.c`, and writes a CEL_TYPE
    // CelValue.  CEL_MESSAGE arm dispatches to the host
    // trampoline `cel_host_resolve_message_type_name`.  See
    // `rewrite/m9-type-subsystem.md`.
    Seed{"type", {ImportModule::kCelRuntime, "cel_type_of_at_v"}},
    // Identity conversions.  cel-cpp's standard library
    // registers `<kind>(<kind>)` overloads for every scalar kind;
    // each is a no-op at runtime.  Reuse `cel_copy_slot` (the
    // 24-byte CelValue memcpy used by the ternary lowering) —
    // its `(dst, src) -> void` ABI is bit-identical to the
    // conversion `(out_slot, in_slot) -> void` shape, and the
    // full-CelValue copy automatically propagates CEL_UNKNOWN /
    // CEL_ERROR absorbing-kind semantics verbatim.
    Seed{"bool_to_bool", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"int64_to_int64", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"uint64_to_uint64", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"double_to_double", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"string_to_string", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    Seed{"bytes_to_bytes", {ImportModule::kCelRuntime, "cel_copy_slot"}},
    // Numeric inter-conversions.  Each helper is a unary slot-out
    // kernel with the standard 3VL absorb prelude; overflow / NaN
    // / negative-source rejections poison with CEL_ERR_OVERFLOW
    // per langdef §"int" / §"uint" / §"double".  See
    // `rewrite/m10-conversions.md`.
    Seed{"uint64_to_int64",
         {ImportModule::kCelRuntime, "cel_uint_to_int_at_v"}},
    Seed{"double_to_int64",
         {ImportModule::kCelRuntime, "cel_double_to_int_at_v"}},
    Seed{"int64_to_uint64",
         {ImportModule::kCelRuntime, "cel_int_to_uint_at_v"}},
    Seed{"double_to_uint64",
         {ImportModule::kCelRuntime, "cel_double_to_uint_at_v"}},
    Seed{"int64_to_double",
         {ImportModule::kCelRuntime, "cel_int_to_double_at_v"}},
    Seed{"uint64_to_double",
         {ImportModule::kCelRuntime, "cel_uint_to_double_at_v"}},
    // String parsing.  Hand-rolled parsers in `cel_runtime.c`
    // mirror cel-cpp's `absl::SimpleAtoi` / `SimpleAtod`
    // admit-sets; malformed input poisons `CEL_ERR_OVERFLOW`.
    Seed{"string_to_int64",
         {ImportModule::kCelRuntime, "cel_string_to_int_at_v"}},
    Seed{"string_to_uint64",
         {ImportModule::kCelRuntime, "cel_string_to_uint_at_v"}},
    Seed{"string_to_double",
         {ImportModule::kCelRuntime, "cel_string_to_double_at_v"}},
    Seed{"string_to_bool",
         {ImportModule::kCelRuntime, "cel_string_to_bool_at_v"}},
    // Number/bool to string.  Arena-allocates the output string
    // bytes; lifetime model identical to the `cel_type_of_at_v`
    // per-Eval-arena pattern.  `double_to_string` is "round-trip
    // safe for typical magnitudes" — byte-exact match against
    // cel-cpp's `to_chars` general format is not guaranteed (see
    // `rewrite/m10-conversions.md` §4.4).
    Seed{"int64_to_string",
         {ImportModule::kCelRuntime, "cel_int_to_string_at_v"}},
    Seed{"uint64_to_string",
         {ImportModule::kCelRuntime, "cel_uint_to_string_at_v"}},
    Seed{"bool_to_string",
         {ImportModule::kCelRuntime, "cel_bool_to_string_at_v"}},
    Seed{"double_to_string",
         {ImportModule::kCelRuntime, "cel_double_to_string_at_v"}},
    // Bytes <-> string with UTF-8 validation.  Both share the
    // source's payload.s span (no arena copy); the kind tag in
    // CelValue disambiguates the byte semantics.
    Seed{"string_to_bytes",
         {ImportModule::kCelRuntime, "cel_string_to_bytes_at_v"}},
    Seed{"bytes_to_string",
         {ImportModule::kCelRuntime, "cel_bytes_to_string_at_v"}},
};

// Overload ids the OverloadTable does NOT seed.  Every cel-cpp
// `StandardOverloadIds::k*` value is in either `kBuiltinSeeds`
// above or this set; the tripwire test enforces the partition.
//
// Two reasons land an id here:
//
//   1. Special-cased in `expr_lower.cc` and not routed through the
//      OverloadTable's general arm — `_[_]` indexing (origin-aware
//      kArena/kHost/kDynamic dispatch; see `rewrite/map-list-dispatch.md`),
//      `_?_:_` (lowered as `BinaryenIf`, not a slot-out helper),
//      `not_strictly_false` (comprehension internals; see
//      `rewrite/m5-comprehensions-design.md`).
//
//   2. Not yet implemented — `to_dyn` (dyn-passthrough — see
//      `rewrite/dyn-passthrough-plan.md`).
constexpr std::array<absl::string_view, 6> kExplicitlyUnimplementedIds{
    // Special-cased in expr_lower.cc — not slot-out helpers.
    "conditional",
    "not_strictly_false",
    "__not_strictly_false__",
    "index_list",
    "index_map",
    // Dyn passthrough (see `rewrite/dyn-passthrough-plan.md`).
    "to_dyn",
};

}  // namespace

bool OverloadTableIsExplicitlyUnimplemented(absl::string_view overload_id) {
  return std::any_of(kExplicitlyUnimplementedIds.begin(),
                     kExplicitlyUnimplementedIds.end(),
                     [overload_id](absl::string_view id) {
                       return id == overload_id;
                     });
}

// NOLINTNEXTLINE(modernize-use-equals-default) — body seeds builtins, can't be defaulted
OverloadTableBuilder::OverloadTableBuilder() {
  for (const Seed& s : kBuiltinSeeds) {
    // Built-ins use `constexpr` string_views — they point at stable
    // module-lifetime storage, so no copy is needed here.
    const uint32_t interned_id = static_cast<uint32_t>(impls_.size()) + 1u;
    impls_.push_back(s.impl);
    auto [it, inserted] = index_.emplace(s.overload_id, interned_id);
    ABSL_CHECK(inserted) << "kBuiltinSeeds duplicate: " << s.overload_id;
    builtin_ids_.insert(s.overload_id);
  }
}

absl::Status OverloadTableBuilder::RegisterCustom(
    absl::string_view overload_id, ImportModule module,
    absl::string_view helper_name) {
  if (builtin_ids_.contains(overload_id)) {
    return absl::AlreadyExistsError(absl::StrCat(
        "'", overload_id, "' is a standard built-in and cannot be overridden"));
  }
  if (index_.contains(overload_id)) {
    return absl::AlreadyExistsError(
        absl::StrCat("overload '", overload_id, "' already registered"));
  }

  // Commit: push stable storage, then the parallel arrays.
  const std::string& stored_id = custom_ids_.emplace_back(overload_id);
  const std::string& stored_name =
      custom_helper_names_.emplace_back(helper_name);
  const OverloadImpl impl{module, absl::string_view(stored_name)};
  const uint32_t interned_id = static_cast<uint32_t>(impls_.size()) + 1u;
  impls_.push_back(impl);
  index_.emplace(absl::string_view(stored_id), interned_id);
  return absl::OkStatus();
}

OverloadTable OverloadTableBuilder::Build() && {
  return {std::move(custom_ids_), std::move(custom_helper_names_),
          std::move(impls_), std::move(index_)};
}

OverloadTable::OverloadTable(
    std::deque<std::string> custom_ids,
    std::deque<std::string> custom_helper_names,
    std::vector<OverloadImpl> impls,
    absl::flat_hash_map<absl::string_view, uint32_t> index)
    : custom_ids_(std::move(custom_ids)),
      custom_helper_names_(std::move(custom_helper_names)),
      impls_(std::move(impls)),
      index_(std::move(index)) {}

const OverloadImpl* OverloadTable::Lookup(absl::string_view overload_id) const {
  auto it = index_.find(overload_id);
  if (it == index_.end()) return nullptr;
  return &impls_[it->second - 1];
}

uint32_t OverloadTable::InternOverloadId(absl::string_view overload_id) const {
  auto it = index_.find(overload_id);
  if (it == index_.end()) return 0;
  return it->second;
}

const OverloadImpl& OverloadTable::LookupById(uint32_t interned_id) const {
  ABSL_CHECK_GE(interned_id, 1u);
  ABSL_CHECK_LE(static_cast<size_t>(interned_id), impls_.size());
  return impls_[interned_id - 1];
}

std::vector<std::pair<ImportModule, absl::string_view>>
OverloadTable::UsedImports(
    const absl::flat_hash_set<uint32_t>& used_ids) const {
  std::vector<std::pair<ImportModule, absl::string_view>> out;
  out.reserve(used_ids.size());
  for (const uint32_t id : used_ids) {
    if (id == 0 || id > impls_.size()) continue;
    const OverloadImpl& impl = impls_[id - 1];
    out.emplace_back(impl.module, impl.name);
  }
  return out;
}

}  // namespace celwasm
