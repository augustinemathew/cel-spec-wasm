// Hand-curated stub of the `author.h` that real wit-bindgen would
// emit for a fns.wit covering every m24 §6 row.  Lives at fixtures/
// because it pairs with the genrule-emitted codec.h to prove the
// codec emitter's output compiles against the wit-bindgen-shaped
// author surface.
//
// **Not a substitute for real wit-bindgen output** — when the
// toolchain integration (m26 §5, H.1) lands, this file goes away
// and the cc_library depends on a wit-bindgen-emitted author.h
// directly.  Until then, this file IS the contract the codec
// emitter promises to match.
//
// Source of truth: the actual wit-bindgen 0.57.1 C output captured
// at the /tmp/witgen probe during m26 §3.5 design (2026-06-04);
// the field shapes here are byte-exact mirrors of that output.

#ifndef CELWASM_COMPILER_CELFN_CELFNC_EMIT_FIXTURES_AUTHOR_STUB_H_
#define CELWASM_COMPILER_CELFN_CELFNC_EMIT_FIXTURES_AUTHOR_STUB_H_

#include <stddef.h>
#include <stdint.h>

#include <cstdlib>  // malloc, free, abort

#ifdef __cplusplus
extern "C" {
#endif

// String — the (ptr, len) carrier wit-bindgen emits for `string`.
typedef struct author_string_t {
  uint8_t* ptr;
  size_t len;
} author_string_t;

// Lists.  Element type is always one of:
//   - uint8_t (list<u8> = bytes / proto)
//   - int64_t (list<s64>)
//   - author_string_t (list<string>)
//   - author_list_s64_t (list<list<s64>>)
//   - author_tuple2_*_t (list<tuple<...>> = a map)

typedef struct author_list_u8_t {
  uint8_t* ptr;
  size_t len;
} author_list_u8_t;

typedef struct author_list_s64_t {
  int64_t* ptr;
  size_t len;
} author_list_s64_t;

typedef struct author_list_string_t {
  author_string_t* ptr;
  size_t len;
} author_list_string_t;

typedef struct author_list_list_s64_t {
  author_list_s64_t* ptr;
  size_t len;
} author_list_list_s64_t;

// Tuples — the (f0, f1) carrier wit-bindgen emits for `tuple<a, b>`.
typedef struct author_tuple2_string_s64_t {
  author_string_t f0;
  int64_t f1;
} author_tuple2_string_s64_t;

typedef struct author_list_tuple2_string_s64_t {
  author_tuple2_string_s64_t* ptr;
  size_t len;
} author_list_tuple2_string_s64_t;

// Option<u8> — `is_some` + `val`.  The export-adapter uses
// pointer-as-maybe instead, but the codec emits the struct form.
typedef struct author_option_u8_t {
  bool is_some;
  uint8_t val;
} author_option_u8_t;

// Records.  wit-bindgen prefixes interface-scoped records with
// `exports_<package_normalized>_<interface>_`.  m26 fixture uses
// `cel:customfn` + `fns` → `exports_cel_customfn_fns_`.

typedef struct exports_cel_customfn_fns_duration_t {
  int64_t seconds;
  int32_t nanos;
} exports_cel_customfn_fns_duration_t;

typedef struct exports_cel_customfn_fns_timestamp_t {
  int64_t seconds;
  int32_t nanos;
} exports_cel_customfn_fns_timestamp_t;

// Canonical-ABI allocator.  Real wit-bindgen emits this as a __weak
// export linked against `realloc`; the stub mirrors that.
static inline void* cabi_realloc(void* ptr, size_t /*old_size*/,
                                 size_t align, size_t new_size) {
  (void)align;
  if (new_size == 0) return ptr;
  void* r = realloc(ptr, new_size);
  if (!r) abort();
  return r;
}

// String helpers used by codec lower.
static inline void author_string_dup_n(author_string_t* ret, const char* s,
                                       size_t len) {
  ret->len = len;
  ret->ptr =
      (uint8_t*)cabi_realloc(NULL, 0, 1, len);  // NOLINT(*-no-malloc)
  if (len > 0) {
    for (size_t i = 0; i < len; ++i) ret->ptr[i] = (uint8_t)s[i];
  }
}

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_CELFN_CELFNC_EMIT_FIXTURES_AUTHOR_STUB_H_
