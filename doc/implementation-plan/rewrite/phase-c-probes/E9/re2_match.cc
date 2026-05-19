// Probe E8 — link RE2 + absl into a wasm; verify a regex match.

#include <cstdint>
#include "absl/strings/string_view.h"
#include "re2/re2.h"

extern "C" {

__attribute__((export_name("match")))
int32_t match(const char* pat, int32_t pat_len,
              const char* text, int32_t text_len) {
  RE2 re(absl::string_view(pat, static_cast<size_t>(pat_len)));
  if (!re.ok()) return -1;
  return RE2::PartialMatch(
      absl::string_view(text, static_cast<size_t>(text_len)), re) ? 1 : 0;
}

}  // extern "C"
