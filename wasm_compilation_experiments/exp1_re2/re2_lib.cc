// Wrapper that exposes a flat `match()` C ABI backed by RE2.
// Compiled to wasm32-wasi-threads, linked against libre2.a + libabsl_*.a.

#include <cstdint>
#include <string>
#include "re2/re2.h"

extern "C" {

// Returns 1 if PartialMatch succeeds, 0 if not, -1 if the regex
// fails to compile.  Doesn't need exceptions — RE2 uses bool
// returns + RE2::ok() for compile errors.
int32_t match(const char* pattern, int32_t plen,
              const char* text,    int32_t tlen) {
  RE2 re(absl::string_view(pattern, plen));
  if (!re.ok()) return -1;
  return RE2::PartialMatch(absl::string_view(text, tlen), re) ? 1 : 0;
}

}  // extern "C"
