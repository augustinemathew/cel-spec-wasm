#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" int32_t match(const char*, int32_t, const char*, int32_t);

static void run(const char* pat, const char* txt) {
  int r = match(pat, (int32_t)std::strlen(pat), txt, (int32_t)std::strlen(txt));
  std::printf("match(%-20s | %-30s) = %d\n", pat, txt, r);
}

int main() {
  run("^hello",           "hello world");        // 1
  run("world$",           "hello world");        // 1
  run("[a-z]+@[a-z]+",    "x@y test");           // 1
  run("[0-9]{4}",         "year 2026 here");     // 1
  run("nope",             "no nope here");       // 1 (substring search)
  run("foo(",             "anything");           // -1 (regex error)
  return 0;
}
