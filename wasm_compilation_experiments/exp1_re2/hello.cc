#include <cstdint>
#include <cstring>

// Exported entry point that does not need any WASI imports at all:
// caller writes input to linear memory, calls match(), reads result.
// We start with a fixed-string "regex" stub to prove the toolchain
// produces a runnable wasm before we link any heavy library.
extern "C" {

// Returns 1 if `needle` is a substring of `haystack`, else 0.
int32_t match(const char* haystack, int32_t hlen,
              const char* needle,   int32_t nlen) {
  if (nlen == 0) return 1;
  if (nlen > hlen) return 0;
  for (int32_t i = 0; i + nlen <= hlen; ++i) {
    if (std::memcmp(haystack + i, needle, nlen) == 0) return 1;
  }
  return 0;
}

}  // extern "C"
