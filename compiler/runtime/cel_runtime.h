// CEL WASM runtime — data model and allocator shared between the AOT-
// generated module and the native unit tests. Authored in plain C so it can
// cross-compile to wasm32 (brew clang) without touching libc, and is linked
// directly by googletest on the host for fast iteration.
//
// Every "pointer" in the CEL value graph is a 32-bit byte offset into the
// module's linear memory. On the host build we back that memory with a
// static byte buffer; on wasm32 it will be `memory[0]`. The allocator is a
// bump arena the host rewinds with cel_reset() between evaluations.

#ifndef CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_
#define CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CEL_NULL      = 0,
  CEL_BOOL      = 1,
  CEL_INT       = 2,
  CEL_UINT      = 3,
  CEL_DOUBLE    = 4,
  CEL_STRING    = 5,
  CEL_BYTES     = 6,
  CEL_LIST      = 7,
  CEL_MAP       = 8,
  CEL_MESSAGE   = 9,
  CEL_TYPE      = 10,
  CEL_DURATION  = 11,
  CEL_TIMESTAMP = 12,
  CEL_OPTIONAL  = 13,
  CEL_UNKNOWN   = 14,
  CEL_ERROR     = 15,
} CelKind;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelSpan;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelArray;

typedef struct {
  uint32_t pairs_ptr;
  uint32_t len;
} CelMap;

typedef struct {
  int64_t seconds;
  int32_t nanos;
  int32_t _pad;
} CelDurTs;

typedef struct CelValue CelValue;
struct CelValue {
  uint32_t kind;
  uint32_t _pad;
  union {
    int32_t  b;
    int64_t  i;
    uint64_t u;
    double   d;
    CelSpan  s;
    CelSpan  bytes;
    CelArray list;
    CelMap   map;
    uint32_t msg_slot;
    uint32_t type_id;
    CelDurTs dur;
    CelDurTs ts;
    uint32_t opt;
    uint32_t unk;
    uint32_t err;
  } payload;
};

_Static_assert(sizeof(CelValue) == 24, "CelValue must remain 24 bytes");

typedef struct {
  uint32_t bump;
  uint32_t limit;
} CelArena;

extern CelArena g_cel_arena;

uint8_t* cel_mem_base(void);
uint32_t cel_mem_size(void);

uint32_t cel_alloc(uint32_t n);
void     cel_reset(void);

// Offset-to-pointer helper. Returns NULL when off == 0 so callers can treat
// a zero offset uniformly as "absent". Not valid after a cel_reset().
CelValue* cel_value_at(uint32_t off);

uint32_t cel_make_null(void);
uint32_t cel_make_bool(int32_t b);
uint32_t cel_make_int(int64_t i);
uint32_t cel_make_uint(uint64_t u);
uint32_t cel_make_double(double d);

uint32_t cel_make_string(const char* src, uint32_t len);
uint32_t cel_make_bytes(const void* src, uint32_t len);

// View variants wrap an already-arena-resident span without copying. The
// host uses these after streaming bytes into memory via cel_alloc.
uint32_t cel_make_string_view(uint32_t ptr, uint32_t len);
uint32_t cel_make_bytes_view(uint32_t ptr, uint32_t len);

uint32_t cel_make_message(uint32_t ref_slot);
uint32_t cel_make_type(uint32_t type_id);

uint32_t cel_make_duration(int64_t seconds, int32_t nanos);
uint32_t cel_make_timestamp(int64_t seconds, int32_t nanos);

uint32_t cel_make_optional_some(uint32_t inner);
uint32_t cel_make_optional_none(void);

uint32_t cel_make_unknown(uint32_t attribute_id);
uint32_t cel_make_error(uint32_t code, uint32_t msg_ptr, uint32_t msg_len);

int32_t cel_string_eq(uint32_t a, uint32_t b);
int32_t cel_bytes_eq(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_
