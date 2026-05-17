// Demonstrates compiling a C++ stdlib regex into wasm via wasi-sdk.
// Same exported ABI as hello.cc's match(), but the body uses
// std::regex_search — proving the libc++ stdlib path works inside
// a wasm runtime without host-side regex code.

#include <cstdint>
#include <cstring>
#include <regex>
#include <string>

extern "C" {

// match(pattern_ptr, pattern_len, text_ptr, text_len) -> 1 if match, 0 if no, -1 on regex error.
int32_t match(const char* pattern, int32_t plen,
              const char* text,    int32_t tlen) {
  try {
    std::string p(pattern, pattern + plen);
    std::string t(text, text + tlen);
    std::regex re(p);
    return std::regex_search(t, re) ? 1 : 0;
  } catch (const std::regex_error&) {
    return -1;
  }
}

}  // extern "C"
