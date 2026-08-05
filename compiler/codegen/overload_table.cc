#include "compiler/codegen/overload_table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "abi/runtime_catalogue.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

namespace {

// One built-in seed: a cel-cpp overload id paired with the
// `cel_runtime.wasm` helper that implements it.  The import module
// ("cel") and arity come from the runtime catalogue at Build() time, so
// a seed only needs the (overload_id, helper_name) pair.
struct Seed {
  absl::string_view overload_id;
  absl::string_view helper_name;
};

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
constexpr std::array<Seed, 271> kBuiltinSeeds{
    // ── Arithmetic same-kind ──────────────────────────────────
    Seed{"add_int64", "cel_int_add_at_vv"},
    Seed{"add_uint64", "cel_uint_add_at_vv"},
    Seed{"add_double", "cel_double_add_at_vv"},
    Seed{"subtract_int64", "cel_int_sub_at_vv"},
    Seed{"subtract_uint64", "cel_uint_sub_at_vv"},
    Seed{"subtract_double", "cel_double_sub_at_vv"},
    Seed{"multiply_int64", "cel_int_mul_at_vv"},
    Seed{"multiply_uint64", "cel_uint_mul_at_vv"},
    Seed{"multiply_double", "cel_double_mul_at_vv"},
    Seed{"divide_int64", "cel_int_div_at_vv"},
    Seed{"divide_uint64", "cel_uint_div_at_vv"},
    Seed{"divide_double", "cel_double_div_at_vv"},
    Seed{"modulo_int64", "cel_int_mod_at_vv"},
    Seed{"modulo_uint64", "cel_uint_mod_at_vv"},
    Seed{"negate_int64", "cel_int_neg_at_v"},
    Seed{"negate_double", "cel_double_neg_at_v"},
    // ── Concat (`_+_` for strings / bytes / lists) ────────────
    Seed{"add_string", "cel_string_concat_at_vv"},
    Seed{"add_bytes", "cel_bytes_concat_at_vv"},
    // `add_list` names the kDynamic dispatcher; the dispatcher
    // tail-calls the kHost trampoline on host-backed operands.
    Seed{"add_list", "cel_list_concat"},
    // ── Same-kind ordering (`_<_`, `_<=_`, `_>_`, `_>=_`) ─────
    Seed{"less_int64", "cel_int_lt_at_vv"},
    Seed{"less_uint64", "cel_uint_lt_at_vv"},
    Seed{"less_double", "cel_double_lt_at_vv"},
    Seed{"less_string", "cel_string_lt_at_vv"},
    Seed{"less_bytes", "cel_bytes_lt_at_vv"},
    Seed{"less_equals_int64", "cel_int_le_at_vv"},
    Seed{"less_equals_uint64", "cel_uint_le_at_vv"},
    Seed{"less_equals_double", "cel_double_le_at_vv"},
    Seed{"greater_int64", "cel_int_gt_at_vv"},
    Seed{"greater_uint64", "cel_uint_gt_at_vv"},
    Seed{"greater_double", "cel_double_gt_at_vv"},
    Seed{"greater_equals_int64", "cel_int_ge_at_vv"},
    Seed{"greater_equals_uint64", "cel_uint_ge_at_vv"},
    Seed{"greater_equals_double", "cel_double_ge_at_vv"},
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
    Seed{"less_int64_uint64", "cel_numeric_lt_at_vv"},
    Seed{"less_int64_double", "cel_numeric_lt_at_vv"},
    Seed{"less_double_int64", "cel_numeric_lt_at_vv"},
    Seed{"less_double_uint64", "cel_numeric_lt_at_vv"},
    Seed{"less_uint64_int64", "cel_numeric_lt_at_vv"},
    Seed{"less_uint64_double", "cel_numeric_lt_at_vv"},
    Seed{"less_equals_int64_uint64", "cel_numeric_le_at_vv"},
    Seed{"less_equals_int64_double", "cel_numeric_le_at_vv"},
    Seed{"less_equals_double_int64", "cel_numeric_le_at_vv"},
    Seed{"less_equals_double_uint64", "cel_numeric_le_at_vv"},
    Seed{"less_equals_uint64_int64", "cel_numeric_le_at_vv"},
    Seed{"less_equals_uint64_double", "cel_numeric_le_at_vv"},
    Seed{"greater_int64_uint64", "cel_numeric_gt_at_vv"},
    Seed{"greater_int64_double", "cel_numeric_gt_at_vv"},
    Seed{"greater_double_int64", "cel_numeric_gt_at_vv"},
    Seed{"greater_double_uint64", "cel_numeric_gt_at_vv"},
    Seed{"greater_uint64_int64", "cel_numeric_gt_at_vv"},
    Seed{"greater_uint64_double", "cel_numeric_gt_at_vv"},
    Seed{"greater_equals_int64_uint64", "cel_numeric_ge_at_vv"},
    Seed{"greater_equals_int64_double", "cel_numeric_ge_at_vv"},
    Seed{"greater_equals_double_int64", "cel_numeric_ge_at_vv"},
    Seed{"greater_equals_double_uint64", "cel_numeric_ge_at_vv"},
    Seed{"greater_equals_uint64_int64", "cel_numeric_ge_at_vv"},
    Seed{"greater_equals_uint_double", "cel_numeric_ge_at_vv"},
    // ── Bool ordering (`false < true` per langdef §"Booleans") ─
    Seed{"less_bool", "cel_bool_lt_at_vv"},
    Seed{"less_equals_bool", "cel_bool_le_at_vv"},
    Seed{"greater_bool", "cel_bool_gt_at_vv"},
    Seed{"greater_equals_bool", "cel_bool_ge_at_vv"},
    // ── String / bytes ordering tail (le / gt / ge) ───────────
    Seed{"less_equals_string", "cel_string_le_at_vv"},
    Seed{"less_equals_bytes", "cel_bytes_le_at_vv"},
    Seed{"greater_string", "cel_string_gt_at_vv"},
    Seed{"greater_bytes", "cel_bytes_gt_at_vv"},
    Seed{"greater_equals_string", "cel_string_ge_at_vv"},
    Seed{"greater_equals_bytes", "cel_bytes_ge_at_vv"},
    // ── Container size (function + member-call ids share helpers) ──
    // String / bytes have no origin (rodata or arena scalar
    // payload); seed names the leaf helper directly.
    Seed{"size_string", "cel_string_size_at_v"},
    Seed{"string_size", "cel_string_size_at_v"},
    Seed{"size_bytes", "cel_bytes_size_at_v"},
    Seed{"bytes_size", "cel_bytes_size_at_v"},
    // List / map size names the kDynamic dispatcher (Option B).
    Seed{"size_list", "cel_list_size"},
    Seed{"list_size", "cel_list_size"},
    Seed{"size_map", "cel_map_size"},
    Seed{"map_size", "cel_map_size"},
    // ── Container `in` (kDynamic dispatcher) ─────────────────
    Seed{"in_list", "cel_list_in"},
    Seed{"in_map", "cel_map_in"},
    // ── Polymorphic equals / not_equals ──────────────────────
    // Single overload id per cel-cpp; the runtime helper switches
    // on (a.kind, b.kind) and dispatches into same-kind / cross-
    // numeric / aggregate / message arms.  Mismatched scalar
    // kinds return `false`, NOT error (langdef §"Equality").
    Seed{"equals", "cel_equals_at_vv"},
    Seed{"not_equals", "cel_not_equals_at_vv"},
    // ── 3VL / control-flow operators ─────────────────────────
    // `_&&_` / `_||_` / `!_` route through the standard slot-out
    // ABI; non-strict semantics + the 3VL truth table live entirely
    // inside the runtime helper, so codegen treats them like any
    // other call.  `conditional` (`_?_:_`) is the odd one out — its
    // BinaryenIf-based lowering is special-cased in expr_lower and
    // stays in `kExplicitlyUnimplementedIds` accordingly.
    Seed{"logical_and", "cel_and"},
    Seed{"logical_or", "cel_or"},
    Seed{"logical_not", "cel_not"},
    // ── Timestamp / Duration arithmetic ──────────────────────
    // Pure-wasm kernels in cel_time.c.  Result kinds:
    //   (dur, dur) -> dur                          : add, sub
    //   (ts, dur) -> ts                            : add, sub
    //   (dur, ts) -> ts                            : add (commutative)
    //   (ts, ts) -> dur                            : sub  (delta)
    Seed{"add_duration_duration", "cel_dur_add_at_vv"},
    Seed{"add_duration_timestamp", "cel_dur_ts_add_at_vv"},
    Seed{"add_timestamp_duration", "cel_ts_dur_add_at_vv"},
    Seed{"subtract_duration_duration", "cel_dur_sub_at_vv"},
    Seed{"subtract_timestamp_duration", "cel_ts_dur_sub_at_vv"},
    Seed{"subtract_timestamp_timestamp", "cel_ts_ts_sub_at_vv"},
    // ── Timestamp / Duration ordering ────────────────────────
    // Lexicographic (seconds, nanos) compare on the sign-correlated
    // CelDurTs payload.  Equality / inequality route through the
    // standard `equals` / `not_equals` seeds above; the
    // cel_value_eq_polymorphic kernel in cel_runtime.c gained
    // CEL_DURATION / CEL_TIMESTAMP arms in the same slice.
    Seed{"less_duration", "cel_dur_lt_at_vv"},
    Seed{"less_timestamp", "cel_ts_lt_at_vv"},
    Seed{"less_equals_duration", "cel_dur_le_at_vv"},
    Seed{"less_equals_timestamp", "cel_ts_le_at_vv"},
    Seed{"greater_duration", "cel_dur_gt_at_vv"},
    Seed{"greater_timestamp", "cel_ts_gt_at_vv"},
    Seed{"greater_equals_duration", "cel_dur_ge_at_vv"},
    Seed{"greater_equals_timestamp", "cel_ts_ge_at_vv"},
    // ── Timestamp UTC accessors ──────────────────────────────
    // Pure-wasm kernels over the Hinnant civil-calendar helper in
    // cel_time.c.  No TZ argument — UTC by definition; with-TZ
    // variants route through a single dispatch trampoline (see
    // the with-TZ accessor shims below).
    Seed{"timestamp_to_year", "cel_ts_year_utc_at_v"},
    Seed{"timestamp_to_month", "cel_ts_month_utc_at_v"},
    Seed{"timestamp_to_day_of_month_1_based", "cel_ts_day_of_month_1_utc_at_v"},
    Seed{"timestamp_to_day_of_month", "cel_ts_day_of_month_utc_at_v"},
    Seed{"timestamp_to_day_of_year", "cel_ts_day_of_year_utc_at_v"},
    Seed{"timestamp_to_day_of_week", "cel_ts_day_of_week_utc_at_v"},
    Seed{"timestamp_to_hours", "cel_ts_hours_utc_at_v"},
    Seed{"timestamp_to_minutes", "cel_ts_minutes_utc_at_v"},
    Seed{"timestamp_to_seconds", "cel_ts_seconds_utc_at_v"},
    Seed{"timestamp_to_milliseconds", "cel_ts_milliseconds_utc_at_v"},
    // ── Duration accessors ───────────────────────────────────
    // Truncating integer divisions on the sign-correlated payload.
    Seed{"duration_to_hours", "cel_dur_hours_at_v"},
    Seed{"duration_to_minutes", "cel_dur_minutes_at_v"},
    Seed{"duration_to_seconds", "cel_dur_seconds_at_v"},
    Seed{"duration_to_milliseconds", "cel_dur_milliseconds_at_v"},
    // ── Timestamp / Duration <-> int conversions ─────────────
    // langdef: `int(timestamp)` returns epoch-seconds; `int(duration)`
    // returns whole seconds.  Reverse builds `(seconds, 0)`; the
    // timestamp form additionally range-checks the langdef [year 1,
    // year 9999] bound.
    Seed{"timestamp_to_int64", "cel_ts_to_int_at_v"},
    Seed{"duration_to_int64", "cel_dur_to_int_at_v"},
    Seed{"int64_to_timestamp", "cel_int_to_ts_at_v"},
    Seed{"int64_to_duration", "cel_int_to_dur_at_v"},
    // ── Identity conversions ─────────────────────────────────
    // `timestamp(timestamp)` / `duration(duration)` share the
    // `cel_copy_slot` shape used by the scalar identity
    // conversions below.
    Seed{"timestamp_to_timestamp", "cel_copy_slot"},
    Seed{"duration_to_duration", "cel_copy_slot"},
    // ── String <-> Timestamp / Duration ──────────────────────
    // RFC3339 parse / proto-Duration text-format parse + format.
    // Self-hosted inside cel_runtime.wasm via vendored absl
    // (`runtime/cel_time_parse.cc`); see
    // `rewrite/phase-c-plan.md` §4.1-4.4 for the kernel contracts
    // (admit/reject envelope, error codes, RFC3339 format spec).
    Seed{"string_to_timestamp", "cel_timestamp_parse_at_v"},
    Seed{"string_to_duration", "cel_duration_parse_at_v"},
    Seed{"timestamp_to_string", "cel_timestamp_format_at_v"},
    Seed{"duration_to_string", "cel_duration_format_at_v"},
    // ── With-TZ accessor shims ───────────────────────────────
    // Each id seeds to a pure-wasm shim helper that calls the
    // single `cel_host.cel_timestamp_tz_accessor(out, ts, tz, kind)`
    // host trampoline with a fixed `accessor_kind` constant.  The
    // shim shape `_at_vv` (3-arg: out + ts + tz) matches the
    // standard helper ABI; the kind constant is supplied by the
    // shim, not the codegen.  See
    // `rewrite/m7b-duration-timestamp.md` §4.3-§4.4 for the
    // single-trampoline-per-TZ-accessor rationale.
    Seed{"timestamp_to_year_with_tz", "cel_ts_year_with_tz_at_vv"},
    Seed{"timestamp_to_month_with_tz", "cel_ts_month_with_tz_at_vv"},
    Seed{"timestamp_to_day_of_month_1_based_with_tz",
         "cel_ts_day_of_month_1_with_tz_at_vv"},
    Seed{"timestamp_to_day_of_month_with_tz",
         "cel_ts_day_of_month_with_tz_at_vv"},
    Seed{"timestamp_to_day_of_year_with_tz",
         "cel_ts_day_of_year_with_tz_at_vv"},
    Seed{"timestamp_to_day_of_week_with_tz",
         "cel_ts_day_of_week_with_tz_at_vv"},
    Seed{"timestamp_to_hours_with_tz", "cel_ts_hours_with_tz_at_vv"},
    Seed{"timestamp_to_minutes_with_tz", "cel_ts_minutes_with_tz_at_vv"},
    // cel-cpp ships this overload-id with `_tz` suffix (not
    // `_with_tz`); mirror verbatim — the coverage tripwire test
    // does a byte-equal lookup, so a "fix" here would silently
    // regress.  See `standard_definitions.h` in cel-cpp.
    Seed{"timestamp_to_seconds_tz", "cel_ts_seconds_with_tz_at_vv"},
    Seed{"timestamp_to_milliseconds_with_tz",
         "cel_ts_milliseconds_with_tz_at_vv"},
    // ── String ops (`contains` / `startsWith` / `endsWith`) ───
    Seed{"contains_string", "cel_string_contains_at_vv"},
    Seed{"starts_with_string", "cel_string_starts_with_at_vv"},
    Seed{"ends_with_string", "cel_string_ends_with_at_vv"},
    // Regex `matches(text, pat)` / `text.matches(pat)`.  RE2-backed
    // PartialMatch self-hosted in cel_runtime.wasm with a per-Instance
    // single-slot most-recent-pattern cache (the common
    // `list.exists(x, x.matches(pat))` shape hits the cache every
    // iteration past the first).  See `rewrite/phase-c-plan.md` §4.5.
    // cel-cpp's standard library registers both the global-form
    // `matches` overload id and the receiver-form `matches_string`
    // id; both route to the same kernel.
    Seed{"matches", "cel_matches_at_vv"},
    Seed{"matches_string", "cel_matches_at_vv"},
    // `type(x)` standard function.  Pure-runtime helper that
    // reads the operand kind, looks up the spec type-name in the
    // 12-row table in `cel_runtime.c`, and writes a CEL_TYPE
    // CelValue.  CEL_MESSAGE arm dispatches to the host
    // trampoline `cel_host_resolve_message_type_name`.  See
    // `rewrite/m9-type-subsystem.md`.
    Seed{"type", "cel_type_of_at_v"},
    // Identity conversions.  cel-cpp's standard library
    // registers `<kind>(<kind>)` overloads for every scalar kind;
    // each is a no-op at runtime.  Reuse `cel_copy_slot` (the
    // 24-byte CelValue memcpy used by the ternary lowering) —
    // its `(dst, src) -> void` ABI is bit-identical to the
    // conversion `(out_slot, in_slot) -> void` shape, and the
    // full-CelValue copy automatically propagates CEL_UNKNOWN /
    // CEL_ERROR absorbing-kind semantics verbatim.
    Seed{"bool_to_bool", "cel_copy_slot"},
    Seed{"int64_to_int64", "cel_copy_slot"},
    Seed{"uint64_to_uint64", "cel_copy_slot"},
    Seed{"double_to_double", "cel_copy_slot"},
    Seed{"string_to_string", "cel_copy_slot"},
    Seed{"bytes_to_bytes", "cel_copy_slot"},
    // Numeric inter-conversions.  Each helper is a unary slot-out
    // kernel with the standard 3VL absorb prelude; overflow / NaN
    // / negative-source rejections poison with CEL_ERR_OVERFLOW
    // per langdef §"int" / §"uint" / §"double".  See
    // `rewrite/m10-conversions.md`.
    Seed{"uint64_to_int64", "cel_uint_to_int_at_v"},
    Seed{"double_to_int64", "cel_double_to_int_at_v"},
    Seed{"int64_to_uint64", "cel_int_to_uint_at_v"},
    Seed{"double_to_uint64", "cel_double_to_uint_at_v"},
    Seed{"int64_to_double", "cel_int_to_double_at_v"},
    Seed{"uint64_to_double", "cel_uint_to_double_at_v"},
    // String parsing.  Hand-rolled parsers in `cel_runtime.c`
    // mirror cel-cpp's `absl::SimpleAtoi` / `SimpleAtod`
    // admit-sets; malformed input poisons `CEL_ERR_OVERFLOW`.
    Seed{"string_to_int64", "cel_string_to_int_at_v"},
    Seed{"string_to_uint64", "cel_string_to_uint_at_v"},
    Seed{"string_to_double", "cel_string_to_double_at_v"},
    Seed{"string_to_bool", "cel_string_to_bool_at_v"},
    // Number/bool to string.  Arena-allocates the output string
    // bytes; lifetime model identical to the `cel_type_of_at_v`
    // per-Eval-arena pattern.  `double_to_string` is "round-trip
    // safe for typical magnitudes" — byte-exact match against
    // cel-cpp's `to_chars` general format is not guaranteed (see
    // `rewrite/m10-conversions.md` §4.4).
    Seed{"int64_to_string", "cel_int_to_string_at_v"},
    Seed{"uint64_to_string", "cel_uint_to_string_at_v"},
    Seed{"bool_to_string", "cel_bool_to_string_at_v"},
    Seed{"double_to_string", "cel_double_to_string_at_v"},
    // Bytes <-> string with UTF-8 validation.  Both share the
    // source's payload.s span (no arena copy); the kind tag in
    // CelValue disambiguates the byte semantics.
    Seed{"string_to_bytes", "cel_string_to_bytes_at_v"},
    Seed{"bytes_to_string", "cel_bytes_to_string_at_v"},
    // ── `string_ext` extension (cel-cpp `extensions/strings.cc`) ──
    // 19 overload IDs covering charAt, lowerAscii, upperAscii, trim,
    // reverse, indexOf (×2), lastIndexOf (×2), substring (×2), replace
    // (×2), split (×2), join (×2), quote, format.  All kernels are
    // self-hosted in `cel_runtime.wasm`; see
    // `rewrite/m12-string-ext.md` §4.2 + the
    // `cel_string_{ext,format}.{h,cc}` family.
    //
    // These IDs are NOT in cel-cpp's `StandardOverloadIds` (they're
    // extension-only), so the coverage tripwire test in
    // `overload_table_test.cc::CoverageTripwireClassifiesEveryStandardId`
    // does NOT enumerate them — they live in `kBuiltinSeeds` without
    // a tripwire arm.
    Seed{"string_char_at_int", "cel_string_char_at_at_vv"},
    Seed{"string_index_of_string", "cel_string_index_of_at_vv"},
    Seed{"string_index_of_string_int", "cel_string_index_of_at_vvv"},
    Seed{"string_last_index_of_string", "cel_string_last_index_of_at_vv"},
    Seed{"string_last_index_of_string_int", "cel_string_last_index_of_at_vvv"},
    Seed{"string_lower_ascii", "cel_string_lower_ascii_at_v"},
    Seed{"string_upper_ascii", "cel_string_upper_ascii_at_v"},
    Seed{"string_replace_string_string", "cel_string_replace_at_vvv"},
    Seed{"string_replace_string_string_int", "cel_string_replace_n_at_vvvv"},
    Seed{"string_split_string", "cel_string_split_at_vv"},
    Seed{"string_split_string_int", "cel_string_split_n_at_vvv"},
    Seed{"string_substring_int", "cel_string_substring_at_vv"},
    Seed{"string_substring_int_int", "cel_string_substring_range_at_vvv"},
    Seed{"string_trim", "cel_string_trim_at_v"},
    Seed{"list_join", "cel_string_join_at_v"},
    Seed{"list_join_string", "cel_string_join_sep_at_vv"},
    Seed{"strings_quote", "cel_string_quote_at_v"},
    Seed{"string_format", "cel_string_format_at_vv"},
    Seed{"string_reverse", "cel_string_reverse_at_v"},

    // ── math_ext extension (cel-cpp `extensions/math_ext.cc`) ──
    // Resolved overload IDs enumerated from `math_ext_decls.cc` +
    // an AST probe (`m16-ast-probe-findings.md`).  Many IDs
    // map to one kind-dispatching kernel (abs/sign/sqrt/min/max).
    // `math.greatest` / `math.least` never reach here — the parser
    // macros expand them to `math.@min` / `math.@max` calls; their
    // unary (single-operand) overloads are identity, so they bind
    // to the existing `cel_copy_slot`.
    //
    // Scalar.
    Seed{"math_ceil_double", "cel_math_ceil_at_v"},
    Seed{"math_floor_double", "cel_math_floor_at_v"},
    Seed{"math_round_double", "cel_math_round_at_v"},
    Seed{"math_trunc_double", "cel_math_trunc_at_v"},
    Seed{"math_isInf_double", "cel_math_is_inf_at_v"},
    Seed{"math_isNaN_double", "cel_math_is_nan_at_v"},
    Seed{"math_isFinite_double", "cel_math_is_finite_at_v"},
    Seed{"math_abs_int", "cel_math_abs_at_v"},
    Seed{"math_abs_uint", "cel_math_abs_at_v"},
    Seed{"math_abs_double", "cel_math_abs_at_v"},
    Seed{"math_sign_int", "cel_math_sign_at_v"},
    Seed{"math_sign_uint", "cel_math_sign_at_v"},
    Seed{"math_sign_double", "cel_math_sign_at_v"},
    Seed{"math_sqrt_int", "cel_math_sqrt_at_v"},
    Seed{"math_sqrt_uint", "cel_math_sqrt_at_v"},
    Seed{"math_sqrt_double", "cel_math_sqrt_at_v"},
    // Bitwise.
    Seed{"math_bitAnd_int_int", "cel_math_bit_and_at_vv"},
    Seed{"math_bitAnd_uint_uint", "cel_math_bit_and_at_vv"},
    Seed{"math_bitOr_int_int", "cel_math_bit_or_at_vv"},
    Seed{"math_bitOr_uint_uint", "cel_math_bit_or_at_vv"},
    Seed{"math_bitXor_int_int", "cel_math_bit_xor_at_vv"},
    Seed{"math_bitXor_uint_uint", "cel_math_bit_xor_at_vv"},
    Seed{"math_bitNot_int_int", "cel_math_bit_not_at_v"},
    Seed{"math_bitNot_uint_uint", "cel_math_bit_not_at_v"},
    Seed{"math_bitShiftLeft_int_int", "cel_math_bit_shift_left_at_vv"},
    Seed{"math_bitShiftLeft_uint_int", "cel_math_bit_shift_left_at_vv"},
    Seed{"math_bitShiftRight_int_int", "cel_math_bit_shift_right_at_vv"},
    Seed{"math_bitShiftRight_uint_int", "cel_math_bit_shift_right_at_vv"},
    // Variadic min — unary (identity → cel_copy_slot), pairwise, list.
    Seed{"math_@min_int", "cel_copy_slot"},
    Seed{"math_@min_uint", "cel_copy_slot"},
    Seed{"math_@min_double", "cel_copy_slot"},
    Seed{"math_@min_int_int", "cel_math_min_at_vv"},
    Seed{"math_@min_int_uint", "cel_math_min_at_vv"},
    Seed{"math_@min_int_double", "cel_math_min_at_vv"},
    Seed{"math_@min_uint_int", "cel_math_min_at_vv"},
    Seed{"math_@min_uint_uint", "cel_math_min_at_vv"},
    Seed{"math_@min_uint_double", "cel_math_min_at_vv"},
    Seed{"math_@min_double_int", "cel_math_min_at_vv"},
    Seed{"math_@min_double_uint", "cel_math_min_at_vv"},
    Seed{"math_@min_double_double", "cel_math_min_at_vv"},
    Seed{"math_@min_list_int", "cel_math_min_list_at_v"},
    Seed{"math_@min_list_uint", "cel_math_min_list_at_v"},
    Seed{"math_@min_list_double", "cel_math_min_list_at_v"},
    // Variadic max.
    Seed{"math_@max_int", "cel_copy_slot"},
    Seed{"math_@max_uint", "cel_copy_slot"},
    Seed{"math_@max_double", "cel_copy_slot"},
    Seed{"math_@max_int_int", "cel_math_max_at_vv"},
    Seed{"math_@max_int_uint", "cel_math_max_at_vv"},
    Seed{"math_@max_int_double", "cel_math_max_at_vv"},
    Seed{"math_@max_uint_int", "cel_math_max_at_vv"},
    Seed{"math_@max_uint_uint", "cel_math_max_at_vv"},
    Seed{"math_@max_uint_double", "cel_math_max_at_vv"},
    Seed{"math_@max_double_int", "cel_math_max_at_vv"},
    Seed{"math_@max_double_uint", "cel_math_max_at_vv"},
    Seed{"math_@max_double_double", "cel_math_max_at_vv"},
    Seed{"math_@max_list_int", "cel_math_max_list_at_v"},
    Seed{"math_@max_list_uint", "cel_math_max_list_at_v"},
    Seed{"math_@max_list_double", "cel_math_max_list_at_v"},
    // ── `encoders` extension (cel-cpp `extensions/encoders.cc`) ──
    // base64.encode(bytes)->string + base64.decode(string)->bytes.
    // Overload IDs are cel-cpp's `MakeOverloadDecl` strings (confirmed
    // against `encoders.cc`); kernels self-hosted in `cel_runtime.wasm`
    // (`cel_base64_{encode,decode}_at_v`, see `runtime/BUILD.bazel`'s
    // `-Wl,--export=cel_base64_*` lines).  Extension-only IDs (NOT in
    // cel-cpp's `StandardOverloadIds`) → no coverage-tripwire arm.
    Seed{"base64_encode_bytes", "cel_base64_encode_at_v"},
    Seed{"base64_decode_string", "cel_base64_decode_at_v"},
    // ── CEL `optional<T>` overloads ──────────────────────────
    // Overload IDs from `third_party/cel-cpp/checker/optional.cc`.
    // Kernels self-hosted in `cel_runtime.wasm` (see
    // `runtime/cel_optional.{h,c}` + the
    // `-Wl,--export=cel_optional_*` lines in
    // `runtime/BUILD.bazel`).
    //
    // The `.?field` and `[?key]` Call paths route through
    // `cel_select_optional_field_at_vv` alongside the
    // Select-on-optional kSelectExpr (one kernel handles both
    // surfaces).  Chained-index variants
    // (`optional_map_optindex_optional_value`, etc.) also map
    // here — the kernel polymorphic-dispatches on `src.kind`
    // internally.
    Seed{"optional_of", "cel_optional_of_at_v"},
    Seed{"optional_ofNonZeroValue", "cel_optional_of_non_zero_at_v"},
    Seed{"optional_none", "cel_optional_none_at"},
    Seed{"optional_hasValue", "cel_optional_has_value_at_v"},
    Seed{"optional_value", "cel_optional_value_at_v"},
    Seed{"optional_or_optional", "cel_optional_or_at_vv"},
    Seed{"optional_orValue_value", "cel_optional_or_value_at_vv"},
    Seed{"select_optional_field", "cel_select_optional_field_at_vv"},
    Seed{"map_optindex_optional_value", "cel_select_optional_field_at_vv"},
    Seed{"optional_map_optindex_optional_value",
         "cel_select_optional_field_at_vv"},
    Seed{"list_optindex_optional_int", "cel_select_optional_field_at_vv"},
    Seed{"optional_list_optindex_optional_int",
         "cel_select_optional_field_at_vv"},
    Seed{"optional_list_index_int", "cel_select_optional_field_at_vv"},
    Seed{"optional_map_index_value", "cel_select_optional_field_at_vv"},
    // ── network_ext (net.IP / net.CIDR) overloads ────────────
    // Overload IDs are the self-declared decls in
    // `frontend/parse_and_check.cc` (no cel-cpp library exists for
    // network_ext); they MUST match the decl ids there.  Kernels
    // self-hosted in `cel_runtime.wasm` (`runtime/cel_net_ext.{h,c}`).
    // Several ids fold onto one kernel: both string-arg overloads of
    // containsIP/containsCIDR runtime-parse, and the two `isIP`
    // overloads + the string-arg `string()` round-trips share their
    // value-kernel.  Extension-only IDs → no coverage-tripwire arm.
    Seed{"net_ip_string", "cel_ip_parse_at_v"},
    Seed{"net_cidr_string", "cel_cidr_parse_at_v"},
    Seed{"net_isIP_string", "cel_isip_at_v"},
    Seed{"net_isIP_string_int", "cel_isip_at_v"},
    Seed{"net_ip_isCanonical_string", "cel_ip_is_canonical_at_v"},
    Seed{"net_string_ip", "cel_ip_to_string_at_v"},
    Seed{"net_string_cidr", "cel_cidr_to_string_at_v"},
    Seed{"net_ip_family", "cel_ip_family_at_v"},
    Seed{"net_ip_isLoopback", "cel_ip_is_loopback_at_v"},
    Seed{"net_ip_isUnspecified", "cel_ip_is_unspecified_at_v"},
    Seed{"net_ip_isGlobalUnicast", "cel_ip_is_global_unicast_at_v"},
    Seed{"net_ip_isLinkLocalUnicast", "cel_ip_is_link_local_unicast_at_v"},
    Seed{"net_ip_isLinkLocalMulticast", "cel_ip_is_link_local_multicast_at_v"},
    Seed{"net_cidr_containsIP_ip", "cel_cidr_contains_ip_at_vv"},
    Seed{"net_cidr_containsIP_string", "cel_cidr_contains_ip_at_vv"},
    Seed{"net_cidr_containsCIDR_cidr", "cel_cidr_contains_cidr_at_vv"},
    Seed{"net_cidr_containsCIDR_string", "cel_cidr_contains_cidr_at_vv"},
    Seed{"net_cidr_ip", "cel_cidr_ip_at_v"},
    Seed{"net_cidr_masked", "cel_cidr_masked_at_v"},
    Seed{"net_cidr_prefixLength", "cel_cidr_prefix_length_at_v"},
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

absl::string_view ImportModuleName(const OverloadDef& def) {
  switch (def.wasm_import_module_type) {
    case ImportModuleSource::kCel:
      return "cel";
    case ImportModuleSource::kCelHost:
      return "cel_host";
    case ImportModuleSource::kCelFn:
      return "cel_fn";
  }
  ABSL_CHECK(false) << "unknown ImportModuleSource="
                    << static_cast<int>(def.wasm_import_module_type);
}

namespace {

// Append the built-in seed set: source kCel ("cel" module), arity from
// the runtime catalogue (the single source of truth for built-in
// functions).  A seed whose helper is missing from the catalogue is a
// build-time invariant violation.
void SeedBuiltins(std::vector<OverloadDef>& impls,
                  absl::flat_hash_map<std::string, size_t>& index) {
  for (const Seed& s : kBuiltinSeeds) {
    const abi::CelRuntimeFunction* helper =
        abi::FindBuiltinHelper(abi::AbiModule::kCelRuntime, s.helper_name);
    ABSL_CHECK(helper != nullptr)
        << "OverloadTable: built-in seed `" << s.overload_id << "` → `cel."
        << s.helper_name
        << "` is not in the runtime catalogue.  Mark the helper's "
           "declaration in `runtime/cel_*.{h,c}` with `// cel:codegen-export`, "
           "or drop the seed if the helper is gone.";
    const size_t idx = impls.size();
    impls.push_back(OverloadDef{
        std::string(s.overload_id), std::string(s.helper_name),
        ImportModuleSource::kCel, static_cast<uint8_t>(helper->num_args())});
    const bool inserted = index.emplace(std::string(s.overload_id), idx).second;
    ABSL_CHECK(inserted) << "kBuiltinSeeds duplicate: " << s.overload_id;
  }
}

// Append the embedder customs after the seeds.  A collision (with a
// built-in or an earlier custom) is a user error returned as
// AlreadyExists; the other malformed-input cases are compiler invariants
// (CHECK).
absl::Status AppendCustoms(absl::Span<const OverloadDef> customs,
                           size_t builtin_count,
                           std::vector<OverloadDef>& impls,
                           absl::flat_hash_map<std::string, size_t>& index) {
  for (const OverloadDef& c : customs) {
    ABSL_CHECK(c.wasm_import_module_type != ImportModuleSource::kCel)
        << "OverloadTable: custom `" << c.overload_id
        << "` uses module type kCel, which is reserved for built-in seeds";
    ABSL_CHECK_GE(c.num_args, 1u)
        << "OverloadTable: custom `" << c.overload_id
        << "` num_args must be >= 1 (out_slot is always present)";
    if (auto it = index.find(c.overload_id); it != index.end()) {
      const bool shadows_builtin = it->second < builtin_count;
      return absl::AlreadyExistsError(absl::StrCat(
          "custom overload '", c.overload_id, "' collides with ",
          shadows_builtin ? "a standard built-in (shadowing is forbidden)"
                          : "an earlier custom registration"));
    }
    const size_t idx = impls.size();
    impls.push_back(c);
    index.emplace(c.overload_id, idx);
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<OverloadTable> OverloadTable::Build(
    absl::Span<const OverloadDef> customs) {
  OverloadTable t;
  t.impls_.reserve(kBuiltinSeeds.size() + customs.size());
  SeedBuiltins(t.impls_, t.index_);
  if (absl::Status s = AppendCustoms(customs, /*builtin_count=*/t.impls_.size(),
                                     t.impls_, t.index_);
      !s.ok()) {
    return s;
  }
  return t;
}

const OverloadDef* OverloadTable::Lookup(absl::string_view overload_id) const {
  auto it = index_.find(overload_id);
  if (it == index_.end()) return nullptr;
  return &impls_[it->second];
}

}  // namespace celwasm
