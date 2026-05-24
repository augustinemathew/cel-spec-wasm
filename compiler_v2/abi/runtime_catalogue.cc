// Authoritative catalogue of every wasm import an emitted expr
// module declares.  See `runtime_catalogue.h` for the design
// rationale and the three pre-existing surfaces this consolidates.
//
// Maintenance discipline:
//   - Adding a runtime helper or host trampoline: ONE line here,
//     plus the impl + linker `--export=` plumbing.  Codegen
//     consumes arity from this catalogue (no more name-suffix
//     sniff).  The engine's runtime-export-binding pass derives
//     its allowlist from `CelRuntimeHelpers()`; the host trampoline
//     registry checks against `CelHostFunctions()`.
//   - Changing a helper's arity or return shape is a breaking ABI
//     change — bump `kRuntimeAbiVersion` in the header.  Engine's
//     instantiate-time check then rejects programs compiled
//     against the old version with a clear diagnostic.

#include "compiler_v2/abi/runtime_catalogue.h"

#include <array>
#include <cstdint>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/cel_abi.pb.h"

namespace celwasm::abi {

absl::string_view AbiModuleName(AbiModule m) {
  switch (m) {
    case AbiModule::kCelRuntime: return "cel";
    case AbiModule::kCelHost:    return "cel_host";
    case AbiModule::kCelEnv:     return "cel_env";
    case AbiModule::kCelFn:      return "cel_fn";
  }
  ABSL_CHECK(false) << "AbiModuleName: unhandled AbiModule="
                    << static_cast<int>(m);
}

namespace {

// ────────────────────────────────────────────────────────────────
// Compact-form helpers — collapse the four common shapes that
// account for ~85% of entries.  Each macro takes the helper name
// and expands to a full `AbiHelper` struct literal.  Non-suffix
// helpers and non-void returns are written out long-form below.
//
// Convention: "_at_v" → 2 args (out + 1 value); "_at_vv" → 3
// (out + 2); "_at_vvv" → 4; "_at_vvvv" → 5.  Every "_at_*" kernel
// is void-returning by construction (results land in out_slot via
// linear memory, not in the wasm return value).
// ────────────────────────────────────────────────────────────────

#define K_AT_V(n)    AbiHelper{n, AbiModule::kCelRuntime, 2, false}
#define K_AT_VV(n)   AbiHelper{n, AbiModule::kCelRuntime, 3, false}
#define K_AT_VVV(n)  AbiHelper{n, AbiModule::kCelRuntime, 4, false}
#define K_AT_VVVV(n) AbiHelper{n, AbiModule::kCelRuntime, 5, false}

// Non-suffix dispatchers / control-flow / aggregate ops — explicit
// arity per entry.  `_RT_VOID` / `_RT_I32` discriminate result.
#define RT_VOID(n, a) AbiHelper{n, AbiModule::kCelRuntime, (a), false}
#define RT_I32(n, a)  AbiHelper{n, AbiModule::kCelRuntime, (a), true}

#define HOST_VOID(n, a) AbiHelper{n, AbiModule::kCelHost, (a), false}
#define ENV_VOID(n, a)  AbiHelper{n, AbiModule::kCelEnv, (a), false}

// ════════════════════════════════════════════════════════════════
// `cel` namespace — pure-wasm helpers exported by cel_runtime.wasm
// that the expr module imports.  Grouped by category.  Total: see
// `kCelRuntimeHelpersArr` static_assert below.
// ════════════════════════════════════════════════════════════════
constexpr AbiHelper kCelRuntimeHelpersArr[] = {
    // ── Arena primitives (host trampolines also call these) ────
    RT_VOID("arena_reset", 0),
    RT_I32("arena_alloc", 1),

    // ── Same-kind arithmetic ──────────────────────────────────
    K_AT_VV("cel_int_add_at_vv"), K_AT_VV("cel_int_sub_at_vv"),
    K_AT_VV("cel_int_mul_at_vv"), K_AT_VV("cel_int_div_at_vv"),
    K_AT_VV("cel_int_mod_at_vv"), K_AT_V("cel_int_neg_at_v"),
    K_AT_VV("cel_uint_add_at_vv"), K_AT_VV("cel_uint_sub_at_vv"),
    K_AT_VV("cel_uint_mul_at_vv"), K_AT_VV("cel_uint_div_at_vv"),
    K_AT_VV("cel_uint_mod_at_vv"), K_AT_VV("cel_double_add_at_vv"),
    K_AT_VV("cel_double_sub_at_vv"), K_AT_VV("cel_double_mul_at_vv"),
    K_AT_VV("cel_double_div_at_vv"), K_AT_V("cel_double_neg_at_v"),

    // ── Same-kind comparison ─────────────────────────────────
    K_AT_VV("cel_int_lt_at_vv"), K_AT_VV("cel_int_le_at_vv"),
    K_AT_VV("cel_int_gt_at_vv"), K_AT_VV("cel_int_ge_at_vv"),
    K_AT_VV("cel_uint_lt_at_vv"), K_AT_VV("cel_uint_le_at_vv"),
    K_AT_VV("cel_uint_gt_at_vv"), K_AT_VV("cel_uint_ge_at_vv"),
    K_AT_VV("cel_double_lt_at_vv"), K_AT_VV("cel_double_le_at_vv"),
    K_AT_VV("cel_double_gt_at_vv"), K_AT_VV("cel_double_ge_at_vv"),
    K_AT_VV("cel_bool_lt_at_vv"), K_AT_VV("cel_bool_le_at_vv"),
    K_AT_VV("cel_bool_gt_at_vv"), K_AT_VV("cel_bool_ge_at_vv"),

    // ── Cross-type numeric ladder ─────────────────────────────
    K_AT_VV("cel_numeric_lt_at_vv"), K_AT_VV("cel_numeric_le_at_vv"),
    K_AT_VV("cel_numeric_gt_at_vv"), K_AT_VV("cel_numeric_ge_at_vv"),

    // ── String + bytes ops ────────────────────────────────────
    K_AT_VV("cel_string_concat_at_vv"), K_AT_V("cel_string_size_at_v"),
    K_AT_VV("cel_string_lt_at_vv"), K_AT_VV("cel_string_le_at_vv"),
    K_AT_VV("cel_string_gt_at_vv"), K_AT_VV("cel_string_ge_at_vv"),
    K_AT_VV("cel_string_contains_at_vv"),
    K_AT_VV("cel_string_starts_with_at_vv"),
    K_AT_VV("cel_string_ends_with_at_vv"),
    K_AT_VV("cel_bytes_concat_at_vv"), K_AT_V("cel_bytes_size_at_v"),
    K_AT_VV("cel_bytes_lt_at_vv"), K_AT_VV("cel_bytes_le_at_vv"),
    K_AT_VV("cel_bytes_gt_at_vv"), K_AT_VV("cel_bytes_ge_at_vv"),

    // ── Aggregate construction / mutation ─────────────────────
    RT_VOID("cel_list_create", 2),
    RT_VOID("cel_list_append_at", 2),
    RT_VOID("cel_list_append_at_if_bool", 3),
    RT_VOID("cel_list_at_arena", 3),
    RT_VOID("cel_list_at", 3),
    RT_VOID("cel_map_create", 2),
    RT_VOID("cel_map_insert", 3),
    RT_VOID("cel_map_insert_at", 3),
    RT_VOID("cel_map_insert_at_if_bool", 4),
    RT_VOID("cel_map_lookup_arena", 3),
    RT_VOID("cel_map_lookup", 3),

    // ── Aggregate kDynamic dispatchers ────────────────────────
    RT_VOID("cel_list_size", 2),
    RT_VOID("cel_list_in", 3),
    RT_VOID("cel_list_eq", 3),
    RT_VOID("cel_list_concat", 3),
    RT_VOID("cel_map_size", 2),
    RT_VOID("cel_map_in", 3),
    RT_VOID("cel_map_eq", 3),

    // ── Comprehension iter ABI ────────────────────────────────
    RT_I32("cel_map_iter_init", 1),
    RT_I32("cel_map_iter_next", 1),
    RT_VOID("cel_map_iter_key_at", 2),
    RT_VOID("cel_map_iter_value_at", 2),
    // Kind-dispatching count helper for comprehension pre-sizing
    // over host-backed maps (m5b §CCF-8).
    RT_I32("cel_map_count", 1),
    // Kind-dispatching list-iter snapshot for host lists (m5b §CCF-8
    // Slice 2).  Arena passthrough; host calls back through the
    // matching cel_host.cel_list_iter_open trampoline.
    RT_I32("cel_list_arena_view", 1),

    // ── Polymorphic equality ──────────────────────────────────
    K_AT_VV("cel_equals_at_vv"),
    K_AT_VV("cel_not_equals_at_vv"),

    // ── 3VL / control flow ────────────────────────────────────
    RT_VOID("cel_and", 3),
    RT_VOID("cel_or", 3),
    RT_VOID("cel_not", 2),
    RT_VOID("cel_unknown_merge", 3),
    RT_VOID("cel_copy_slot", 2),

    // ── Type system ───────────────────────────────────────────
    K_AT_V("cel_type_of_at_v"),

    // ── Numeric inter-conversion ──────────────────────────────
    K_AT_V("cel_uint_to_int_at_v"), K_AT_V("cel_double_to_int_at_v"),
    K_AT_V("cel_int_to_uint_at_v"), K_AT_V("cel_double_to_uint_at_v"),
    K_AT_V("cel_int_to_double_at_v"), K_AT_V("cel_uint_to_double_at_v"),

    // ── String-parse ──────────────────────────────────────────
    K_AT_V("cel_string_to_int_at_v"), K_AT_V("cel_string_to_uint_at_v"),
    K_AT_V("cel_string_to_double_at_v"), K_AT_V("cel_string_to_bool_at_v"),

    // ── Number/bool/bytes-to-string formatters ───────────────
    K_AT_V("cel_int_to_string_at_v"), K_AT_V("cel_uint_to_string_at_v"),
    K_AT_V("cel_bool_to_string_at_v"), K_AT_V("cel_double_to_string_at_v"),
    K_AT_V("cel_string_to_bytes_at_v"), K_AT_V("cel_bytes_to_string_at_v"),

    // ── Timestamp / duration arithmetic + ordering ────────────
    K_AT_VV("cel_dur_add_at_vv"), K_AT_VV("cel_dur_sub_at_vv"),
    K_AT_VV("cel_ts_dur_add_at_vv"), K_AT_VV("cel_dur_ts_add_at_vv"),
    K_AT_VV("cel_ts_dur_sub_at_vv"), K_AT_VV("cel_ts_ts_sub_at_vv"),
    K_AT_VV("cel_dur_lt_at_vv"), K_AT_VV("cel_dur_le_at_vv"),
    K_AT_VV("cel_dur_gt_at_vv"), K_AT_VV("cel_dur_ge_at_vv"),
    K_AT_VV("cel_ts_lt_at_vv"), K_AT_VV("cel_ts_le_at_vv"),
    K_AT_VV("cel_ts_gt_at_vv"), K_AT_VV("cel_ts_ge_at_vv"),

    // ── Timestamp UTC + duration accessors ────────────────────
    K_AT_V("cel_ts_year_utc_at_v"), K_AT_V("cel_ts_month_utc_at_v"),
    K_AT_V("cel_ts_day_of_month_1_utc_at_v"),
    K_AT_V("cel_ts_day_of_month_utc_at_v"),
    K_AT_V("cel_ts_day_of_year_utc_at_v"),
    K_AT_V("cel_ts_day_of_week_utc_at_v"),
    K_AT_V("cel_ts_hours_utc_at_v"), K_AT_V("cel_ts_minutes_utc_at_v"),
    K_AT_V("cel_ts_seconds_utc_at_v"),
    K_AT_V("cel_ts_milliseconds_utc_at_v"),
    K_AT_V("cel_dur_hours_at_v"), K_AT_V("cel_dur_minutes_at_v"),
    K_AT_V("cel_dur_seconds_at_v"), K_AT_V("cel_dur_milliseconds_at_v"),

    // ── Int <-> ts/dur ────────────────────────────────────────
    K_AT_V("cel_ts_to_int_at_v"), K_AT_V("cel_dur_to_int_at_v"),
    K_AT_V("cel_int_to_ts_at_v"), K_AT_V("cel_int_to_dur_at_v"),

    // ── Timestamp with-TZ accessors ───────────────────────────
    K_AT_VV("cel_ts_year_with_tz_at_vv"),
    K_AT_VV("cel_ts_month_with_tz_at_vv"),
    K_AT_VV("cel_ts_day_of_month_1_with_tz_at_vv"),
    K_AT_VV("cel_ts_day_of_month_with_tz_at_vv"),
    K_AT_VV("cel_ts_day_of_year_with_tz_at_vv"),
    K_AT_VV("cel_ts_day_of_week_with_tz_at_vv"),
    K_AT_VV("cel_ts_hours_with_tz_at_vv"),
    K_AT_VV("cel_ts_minutes_with_tz_at_vv"),
    K_AT_VV("cel_ts_seconds_with_tz_at_vv"),
    K_AT_VV("cel_ts_milliseconds_with_tz_at_vv"),

    // ── Runtime-hosted parse / format ─────────────────────────
    K_AT_V("cel_timestamp_parse_at_v"), K_AT_V("cel_duration_parse_at_v"),
    K_AT_V("cel_timestamp_format_at_v"), K_AT_V("cel_duration_format_at_v"),

    // ── Regex matches ─────────────────────────────────────────
    K_AT_VV("cel_matches_at_vv"),

    // ── M12 string_ext extension kernels ──────────────────────
    K_AT_VV("cel_string_char_at_at_vv"),
    K_AT_V("cel_string_lower_ascii_at_v"),
    K_AT_V("cel_string_upper_ascii_at_v"),
    K_AT_V("cel_string_trim_at_v"),
    K_AT_V("cel_string_reverse_at_v"),
    K_AT_VV("cel_string_index_of_at_vv"),
    K_AT_VVV("cel_string_index_of_at_vvv"),
    K_AT_VV("cel_string_last_index_of_at_vv"),
    K_AT_VVV("cel_string_last_index_of_at_vvv"),
    K_AT_VV("cel_string_substring_at_vv"),
    K_AT_VVV("cel_string_substring_range_at_vvv"),
    K_AT_VVV("cel_string_replace_at_vvv"),
    K_AT_VVVV("cel_string_replace_n_at_vvvv"),
    K_AT_VV("cel_string_split_at_vv"),
    K_AT_VVV("cel_string_split_n_at_vvv"),
    K_AT_V("cel_string_join_at_v"),
    K_AT_VV("cel_string_join_sep_at_vv"),
    K_AT_V("cel_string_quote_at_v"),
    K_AT_VV("cel_string_format_at_vv"),

    // ── CEL optional<T> kernels ───────────────────────────────
    // Eight value-level helpers + select-field + three
    // predicate-gated `_if_present` mutators for map / list / proto
    // entries.  The `_if_present` trio mutates its first arg in place
    // (no out_slot), matching the `_if_bool` siblings above.
    RT_VOID("cel_optional_none_at", 1),
    K_AT_V("cel_optional_of_at_v"),
    K_AT_V("cel_optional_of_non_zero_at_v"),
    K_AT_V("cel_optional_has_value_at_v"),
    K_AT_V("cel_optional_value_at_v"),
    K_AT_VV("cel_optional_or_at_vv"),
    K_AT_VV("cel_optional_or_value_at_vv"),
    K_AT_VV("cel_select_optional_field_at_vv"),
    RT_VOID("cel_map_insert_at_if_present", 3),
    RT_VOID("cel_list_append_at_if_present", 2),
    RT_VOID("cel_set_field_at_if_present", 3),
};

// ════════════════════════════════════════════════════════════════
// `cel_host` namespace — wasmtime host trampolines registered by
// `cel_host_wasmtime.cc::RegisterCelHostImports`.  Names + arities
// MUST agree with `kEntries[]` in that file; the static_assert
// loop below trips at compile time if they drift.
// ════════════════════════════════════════════════════════════════
constexpr AbiHelper kCelHostFunctionsArr[] = {
    HOST_VOID("cel_get_field", 4),
    HOST_VOID("cel_has_field", 4),
    HOST_VOID("cel_map_lookup", 3),
    HOST_VOID("cel_map_iter_open", 2),
    HOST_VOID("cel_list_iter_open", 2),
    HOST_VOID("cel_list_at", 3),
    HOST_VOID("cel_list_size", 2),
    HOST_VOID("cel_list_in", 3),
    HOST_VOID("cel_list_eq", 3),
    HOST_VOID("cel_list_concat", 3),
    HOST_VOID("cel_map_size", 2),
    HOST_VOID("cel_map_in", 3),
    HOST_VOID("cel_map_eq", 3),
    HOST_VOID("cel_message_eq", 3),
    HOST_VOID("cel_make_message", 2),
    HOST_VOID("cel_set_field", 3),
    HOST_VOID("resolve_message_type_name", 2),
    HOST_VOID("cel_timestamp_tz_accessor", 4),
    HOST_VOID("cel_wkt_unwrap_time", 2),
    HOST_VOID("cel_wkt_unwrap_wrapper", 3),
};

// ════════════════════════════════════════════════════════════════
// `cel_env` namespace — host environment trampolines.  Currently
// just `cel_log`; reserved for future cross-cutting concerns.
// ════════════════════════════════════════════════════════════════
constexpr AbiHelper kCelEnvFunctionsArr[] = {
    ENV_VOID("cel_log", 4),
};

#undef K_AT_V
#undef K_AT_VV
#undef K_AT_VVV
#undef K_AT_VVVV
#undef RT_VOID
#undef RT_I32
#undef HOST_VOID
#undef ENV_VOID

// Name→helper lookups, eagerly built per namespace on first use.
// Distinct maps because `cel.cel_list_at` (the dispatcher) and
// `cel_host.cel_list_at` (the trampoline it tail-calls) share a
// name across namespaces by design.
template <std::size_t N>
absl::flat_hash_map<absl::string_view, const AbiHelper*> BuildIndex(
    const AbiHelper (&arr)[N]) {
  absl::flat_hash_map<absl::string_view, const AbiHelper*> m;
  m.reserve(N);
  for (const auto& h : arr) m[h.name] = &h;
  return m;
}

const absl::flat_hash_map<absl::string_view, const AbiHelper*>&
RuntimeIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const AbiHelper*>>
      kIndex(BuildIndex(kCelRuntimeHelpersArr));
  return *kIndex;
}

const absl::flat_hash_map<absl::string_view, const AbiHelper*>& HostIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const AbiHelper*>>
      kIndex(BuildIndex(kCelHostFunctionsArr));
  return *kIndex;
}

const absl::flat_hash_map<absl::string_view, const AbiHelper*>& EnvIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const AbiHelper*>>
      kIndex(BuildIndex(kCelEnvFunctionsArr));
  return *kIndex;
}

}  // namespace

absl::Span<const AbiHelper> CelRuntimeHelpers() {
  return absl::MakeConstSpan(kCelRuntimeHelpersArr);
}

absl::Span<const AbiHelper> CelHostFunctions() {
  return absl::MakeConstSpan(kCelHostFunctionsArr);
}

absl::Span<const AbiHelper> CelEnvFunctions() {
  return absl::MakeConstSpan(kCelEnvFunctionsArr);
}

absl::Status CheckRuntimeAbiVersion(const CelAbi& abi) {
  const uint32_t prog_v = abi.runtime_abi_version();
  const uint32_t engine_v = kRuntimeAbiVersion;
  if (prog_v == engine_v) return absl::OkStatus();
  if (prog_v == 0) {
    const bool empty = abi.variables_size() == 0 && abi.fields_size() == 0 &&
                       abi.attributes_size() == 0 && abi.types_size() == 0;
    if (empty) return absl::OkStatus();
    return absl::FailedPreconditionError(absl::StrCat(
        "cel.abi: program predates ABI versioning "
        "(runtime_abi_version=0); recompile against this engine's compiler "
        "(engine ABI v",
        engine_v, ")"));
  }
  return absl::FailedPreconditionError(absl::StrCat(
      "cel.abi: runtime ABI version mismatch — program compiled "
      "against v",
      prog_v, ", this engine ships v", engine_v,
      "; recompile the program against this engine's compiler"));
}

const AbiHelper* FindBuiltinHelper(AbiModule module, absl::string_view name) {
  const absl::flat_hash_map<absl::string_view, const AbiHelper*>* idx = nullptr;
  switch (module) {
    case AbiModule::kCelRuntime: idx = &RuntimeIndex(); break;
    case AbiModule::kCelHost:    idx = &HostIndex(); break;
    case AbiModule::kCelEnv:     idx = &EnvIndex(); break;
    case AbiModule::kCelFn:
      // Custom-fn helpers aren't in the catalogue; arity comes
      // from the per-compile registration in Compiler::Builder.
      return nullptr;
  }
  auto it = idx->find(name);
  return it == idx->end() ? nullptr : it->second;
}

}  // namespace celwasm::abi
