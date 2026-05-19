#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
// Pure-malloc exports — no startup, no main.
int32_t* mk_array(int32_t n) {
  auto* p = (int32_t*)std::malloc(n * sizeof(int32_t));
  for (int32_t i = 0; i < n; ++i) p[i] = i * i;
  return p;
}
int32_t sum_and_free(int32_t* p, int32_t n) {
  int32_t s = 0;
  for (int32_t i = 0; i < n; ++i) s += p[i];
  std::free(p);
  return s;
}
}
