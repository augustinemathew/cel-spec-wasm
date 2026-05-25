// M12 Slice D — `<string>.format(<list>)` extension function.
//
// Cel-cpp's `extensions/formatting.cc` implements this as a single
// `Format(format_value, args, ...)` adapter that walks the format
// string once, dispatching per-directive against the arg list.  Our
// wasm-hosted runtime splits the work in two:
//
//   1. The parser (Slice D, this header's `cel_string_format_internal.h`
//      private companion) walks the format string and produces a
//      `DirectiveOp` sequence.  Single-pass over the format bytes;
//      malformed directives surface a status carrying cel-cpp's
//      diagnostic.
//   2. The renderer (Slice E) walks the parsed sequence against the
//      args list, dispatching to per-CelKind canonical formatters.
//
// Slice D ships the parser plus a stub renderer that
// `ABSL_CHECK(false)`-fails per the CLAUDE.md unimplemented-feature
// rule.  Slice E lifts the CHECK and lands the renderer.

#ifndef CELWASM_RUNTIME_CEL_STRING_FORMAT_H_
#define CELWASM_RUNTIME_CEL_STRING_FORMAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `s.format(args)`.  `s` is the format string; `args` is a
// `CEL_LIST_ARENA` of values to substitute.  Output bytes are
// arena-allocated.  Malformed format strings, arg-count mismatches,
// and arg-kind mismatches all poison `out` with
// `CEL_ERR_INVALID_ARGUMENT` (cel-cpp returns an `ErrorValue` carrying
// `absl::InvalidArgumentError` in the same conditions).
//
// 3VL absorb: ERROR / UNKNOWN in either input passes through.  Kind
// mismatch on `s` (non-string) or `args` (non-list) poisons with
// `CEL_ERR_TYPE_MISMATCH`.
void cel_string_format_at_vv(uint32_t out_slot, uint32_t s_slot,
                             uint32_t args_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_STRING_FORMAT_H_
