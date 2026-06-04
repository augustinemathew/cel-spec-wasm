// celfnc_emit/cpp_codec_emitter — emit the C++ `codec.h` translation
// layer.  Pairs with the WIT emitter: the WIT emitter declares the
// interface, the codec emitter writes the lift/lower fns that
// reshape wit-bindgen's `customfn_*` / `exports_<pkg>_<iface>_*_t`
// structs into native C++ `std::` containers and back.
//
// Why one header (not multiple): codec.h is consumed by the
// generated_stub.cc (which is the only file that knows how to wire
// `codec::lift(*in)` → `user::Fn(...)` → `codec::lower(out, ...)`).
// Author never includes it.
//
// Ownership rule (m24 §4.0, m26 §4.0): the author's user_fns NEVER
// calls _free.  The `lower(out, ...)` impls populate `*out` via
// `customfn_string_dup_n` (strings) or `cabi_realloc` (lists), and the
// canonical-ABI runtime calls the per-export `cabi_post_*` cleanup
// hook after the host has copied the value out.
//
// Identifier naming (m26 §3.5.1 — settled by probing wit-bindgen
// 0.57 at design time):
//   - Collection types: `customfn_<wit_t>` (e.g. `customfn_list_s64_t`).
//   - Tuple types: `customfn_tuple2_<a>_<b>_t`.
//   - Records (duration / timestamp): prefix
//     `exports_<package_normalized>_<interface>_<record>_t`, where
//     package-normalized replaces `:` with `_`.
//
// Recursion: every list / map / nested aggregate calls back into
// `lift` / `lower` for its element type, so the emitted overload
// set is closed and total over the types actually used in `lib`.
// Unused types are NOT emitted (avoids template-instantiation churn
// and missing-include cascades).

#ifndef CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_CODEC_EMITTER_H_
#define CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_CODEC_EMITTER_H_

#include <string>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {

// Emit the text of a `codec.h` file driven by `lib`.
//
//   - `lib`: only kForeignComponent decls drive the emitter; other
//     backends have no WIT surface (m24 §A).
//   - `cpp_namespace`: the C++ namespace the codec functions live
//     in.  Per m26 §2.1, this is the IDL's `<module>` identifier
//     (e.g. `rules` for `Module rules;`).  Empty string → emit at
//     the global namespace (test/embedding use only).
//   - `wit_package_name`: the WIT package name used to derive the
//     record-type prefix `exports_<pkg_normalized>_fns_*_t`.  The
//     normalization replaces `:` with `_` (e.g. `cel:customfn` →
//     `exports_cel_customfn_fns_*_t`).
//
// Returns the rendered text.  Error: `FailedPrecondition` if a
// permanently-rejected type (`optional<T>` or `type`) reaches the
// emitter (regression tripwire — the Builder gates should have
// blocked it upstream).
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> EmitCodecH(
    const FunctionLibrary& lib, absl::string_view cpp_namespace,
    absl::string_view wit_package_name);

}  // namespace celwasm::celfnc_emit

#endif  // CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_CODEC_EMITTER_H_
