// E2E test harness (the `host` side). Builds CEL-shaped args, calls the
// foreign component's typed fns, asserts results incl. boundary cases.
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "host.h"
static int g_pass = 0, g_fail = 0;
#define CHECK(label, got, want)                                 \
  do {                                                          \
    long long _g = (long long)(got), _w = (long long)(want);    \
    if (_g == _w)                                               \
      g_pass++;                                                 \
    else {                                                      \
      g_fail++;                                                 \
      printf("FAIL %-22s got=%lld want=%lld\n", label, _g, _w); \
    }                                                           \
  } while (0)
static host_string_t S(const char* s) {
  return host_string_t{(uint8_t*)s, strlen(s)};
}

int main() {
  // primitives + boundaries
  CHECK("add_ints", cel_customfn_fns_add_ints(2, 3), 5);
  CHECK("add_ints MAX", cel_customfn_fns_add_ints(INT64_MAX, 0), INT64_MAX);
  CHECK("add_ints MIN", cel_customfn_fns_add_ints(INT64_MIN, 0), INT64_MIN);

  // string in/out + empty + embedded NUL
  {
    host_string_t in = S("hi"), out;
    cel_customfn_fns_shout(&in, &out);
    int ok = out.len == 3 && memcmp(out.ptr, "HI!", 3) == 0;
    if (ok)
      g_pass++;
    else {
      g_fail++;
      printf("FAIL shout\n");
    }
    host_string_free(&out);
  }
  {
    host_string_t in = S(""), out;
    cel_customfn_fns_shout(&in, &out);
    int ok = out.len == 1 && out.ptr[0] == '!';
    if (ok)
      g_pass++;
    else {
      g_fail++;
      printf("FAIL shout empty\n");
    }
    host_string_free(&out);
  }
  {
    host_string_t in{(uint8_t*)"a\0b", 3}, out;
    cel_customfn_fns_shout(&in, &out);
    int ok = out.len == 4 && memcmp(out.ptr, "A\0B!", 4) == 0;
    if (ok)
      g_pass++;
    else {
      g_fail++;
      printf("FAIL shout NUL len=%zu\n", out.len);
    }
    host_string_free(&out);
  }

  // list<int> + empty + INT64_MIN element
  {
    int64_t a[] = {1, 2, 3};
    host_list_s64_t l{a, 3};
    CHECK("sum_list", cel_customfn_fns_sum_list(&l), 6);
  }
  {
    host_list_s64_t l{nullptr, 0};
    CHECK("sum_list empty", cel_customfn_fns_sum_list(&l), 0);
  }
  {
    int64_t a[] = {INT64_MIN};
    host_list_s64_t l{a, 1};
    CHECK("sum_list MIN", cel_customfn_fns_sum_list(&l), INT64_MIN);
  }

  // map<string,int> + empty
  {
    host_tuple2_string_s64_t t[] = {{S("a"), 1}, {S("b"), 2}};
    host_list_tuple2_string_s64_t m{t, 2};
    CHECK("total", cel_customfn_fns_total(&m), 3);
  }
  {
    host_list_tuple2_string_s64_t m{nullptr, 0};
    CHECK("total empty", cel_customfn_fns_total(&m), 0);
  }

  // list<list<int>> + nested empty
  {
    int64_t a[] = {1, 2}, b[] = {3};
    host_list_s64_t inner[] = {{a, 2}, {b, 1}};
    host_list_list_s64_t ll{inner, 2};
    CHECK("grand_total", cel_customfn_fns_grand_total(&ll), 6);
  }
  {
    host_list_s64_t inner[] = {{nullptr, 0}};
    host_list_list_s64_t ll{inner, 1};
    CHECK("grand_total [[]]", cel_customfn_fns_grand_total(&ll), 0);
  }

  // map<string,list<int>> + missing key
  {
    int64_t x[] = {1, 2}, y[] = {3};
    host_tuple2_string_list_s64_t t[] = {{S("x"), {x, 2}}, {S("y"), {y, 1}}};
    host_list_tuple2_string_list_s64_t m{t, 2};
    host_string_t kx = S("x"), kz = S("z");
    CHECK("sum_by_key", cel_customfn_fns_sum_by_key(&m, &kx), 3);
    CHECK("sum_by_key miss", cel_customfn_fns_sum_by_key(&m, &kz), -1);
  }

  // bytes + empty
  {
    uint8_t b[] = {0, 1, 2};
    host_list_u8_t bl{b, 3};
    CHECK("byte_len", cel_customfn_fns_byte_len(&bl), 3);
  }
  {
    host_list_u8_t bl{nullptr, 0};
    CHECK("byte_len empty", cel_customfn_fns_byte_len(&bl), 0);
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
