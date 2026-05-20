#include "compiler_v2/runtime/cel_matches.h"

#include <cstdint>
#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "re2/re2.h"

namespace {

// Per-Instance most-recent-pattern cache.  The common matches()
// workload is `list.exists(x, x.matches(pat))` style — the same
// pattern fires 0..N times in a row — so a single-slot cache hits
// every iteration past the first.  Multi-pattern call sites (rare
// in practice) recompile each switch, which is RE2-compile cost
// (~µs); not load-bearing.
//
// Module-static so each wasm Instance gets its own copy at
// instantiation time.  `cached_re == nullptr` after a successful
// `cached_initialized = true` means the cached pattern failed to
// compile — a repeat invocation of the same bad pattern returns
// sticky-error without recompiling.
//
// `cached_initialized` distinguishes "no pattern has been cached
// yet" from "the empty pattern is cached" — without the flag,
// the very first call with an empty pattern would spuriously
// match `CachedPattern() == p` against the default-constructed
// empty string, skip the compile path, and poison the result
// because `CachedRe()` is still null.  Caught by the conformance
// `matches/empty_arg` + `matches/empty_empty` rows.
bool& CachedInitialized() {
  static bool b = false;
  return b;
}
std::string& CachedPattern() {
  static auto* s = new std::string();
  return *s;
}
std::unique_ptr<RE2>& CachedRe() {
  static auto* p = new std::unique_ptr<RE2>();
  return *p;
}

inline void Poison(CelValue* out, uint32_t err_code) {
  out->kind = CEL_ERROR;
  out->payload.err = err_code;
}

// 3VL passthrough: ERROR / UNKNOWN.
inline bool Absorb3vl(CelValue* out, const CelValue* in) {
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;
    return true;
  }
  return false;
}

// Borrow input bytes from linear memory as a string_view.  Span
// offsets are relative to `cel_mem_base()` — 0 on wasm, the real
// backing-buffer base on native.
inline absl::string_view BorrowSpan(const CelSpan& s) {
  return {reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len};
}

}  // namespace

extern "C" void cel_matches_at_vv(uint32_t out_slot, uint32_t text_slot,
                                  uint32_t pat_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* text = cel_value_at(text_slot);
  const CelValue* pat = cel_value_at(pat_slot);

  if (Absorb3vl(out, text)) return;
  if (Absorb3vl(out, pat)) return;
  if (text->kind != CEL_STRING || pat->kind != CEL_STRING) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }

  const absl::string_view t = BorrowSpan(text->payload.s);
  const absl::string_view p = BorrowSpan(pat->payload.s);

  // Cache miss → recompile.  Failed compile stores nullptr; the
  // next call with the same pattern picks up the sticky error
  // without recompiling.  The `!CachedInitialized()` guard catches
  // the first-call-with-empty-pattern case where `CachedPattern()`
  // and `p` would both be empty by default and falsely "hit".
  if (!CachedInitialized() || CachedPattern() != p) {
    RE2::Options opts;
    opts.set_log_errors(false);  // user-provided patterns; don't spam stderr.
    auto re = std::make_unique<RE2>(p, opts);
    CachedRe() = re->ok() ? std::move(re) : nullptr;
    CachedPattern().assign(p);
    CachedInitialized() = true;
  }

  const RE2* re = CachedRe().get();
  if (re == nullptr) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  out->kind = CEL_BOOL;
  out->payload.b = RE2::PartialMatch(t, *re) ? 1 : 0;
}
