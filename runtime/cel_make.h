// Scalar CelValue constructors.  Each allocates a fresh 24-byte
// CelValue in the arena (via `arena_alloc`) and returns its byte offset,
// or 0 on OOM.  String / bytes variants copy `len` source bytes into
// the arena.

#ifndef CELWASM_RUNTIME_CEL_MAKE_H_
#define CELWASM_RUNTIME_CEL_MAKE_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t cel_make_null(void);
uint32_t cel_make_bool(int32_t b);
uint32_t cel_make_int(int64_t i);
uint32_t cel_make_uint(uint64_t u);
uint32_t cel_make_double(double d);

uint32_t cel_make_string(const char* src, uint32_t len);
uint32_t cel_make_bytes(const void* src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_MAKE_H_
