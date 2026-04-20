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
  CEL_NULL = 0,
  CEL_BOOL = 1,
  CEL_INT = 2,
  CEL_UINT = 3,
  CEL_DOUBLE = 4,
  CEL_STRING = 5,
  CEL_BYTES = 6,
  CEL_LIST = 7,
  CEL_MAP = 8,
  CEL_MESSAGE = 9,
  CEL_TYPE = 10,
  CEL_DURATION = 11,
  CEL_TIMESTAMP = 12,
  CEL_OPTIONAL = 13,
  CEL_UNKNOWN = 14,
  CEL_ERROR = 15,
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
    int32_t b;
    int64_t i;
    uint64_t u;
    double d;
    CelSpan s;
    CelSpan bytes;
    CelArray list;
    CelMap map;
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
void cel_reset(void);

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

// Concatenates two CEL_STRING values and returns a new CelValue* offset.
// Returns 0 if either operand is zero-offset or not a CEL_STRING, or if the
// arena is out of memory.  The result copies both payloads into the arena so
// it is independent of the inputs' storage (their spans may be arena-resident
// views that will not survive a cel_reset()).
uint32_t cel_string_concat(uint32_t a, uint32_t b);

// Bytes-side concat counterpart.  Same semantics as `cel_string_concat`
// but gated on CEL_BYTES kind — the payload is an opaque byte span and
// there is no UTF-8 story, so the implementation shares every step with
// the string version except the kind tag.
uint32_t cel_bytes_concat(uint32_t a, uint32_t b);

// Returns the number of UTF-8 code points in a CEL_STRING.  Returns -1 on
// type error (non-string or zero offset) so the caller can distinguish
// "string of length 0" from "not a string".  CEL §1110 defines size(string)
// as code-point count, NOT byte count; counting continuation bytes
// (0b10xxxxxx) is the portable way to do that without a full decoder.
int64_t cel_string_size(uint32_t s);

// Returns the number of bytes in a CEL_BYTES value.  CEL §1110 defines
// size(bytes) as byte count (no UTF-8 interpretation), so unlike
// cel_string_size this is a direct payload-length read.  Returns -1 on
// type error so callers can distinguish "bytes of length 0" from "not
// bytes".
int64_t cel_bytes_size(uint32_t b);

// Extracts the i32 bool payload from a CelValue*.  Returns 0 for a
// non-bool / zero-offset input; callers that need to distinguish
// false-from-not-a-bool must check the kind themselves.  The common
// caller is the lowered has() path where the enclosing codegen already
// guarantees the result is bool.
int32_t cel_bool_from_value(uint32_t v);

// String member-call helpers (CEL §9 string extension): all three take
// two CEL_STRING operands and return 0/1 as an i32, matching how
// `cel_string_eq` speaks ABI.  Semantics follow the spec: the empty
// string is a prefix/suffix/substring of every string; a longer
// needle than haystack is never found.  Returns 0 on type mismatch
// (non-string, zero offset) so a codegen bug never forges `true`.
int32_t cel_string_starts_with(uint32_t s, uint32_t prefix);
int32_t cel_string_ends_with(uint32_t s, uint32_t suffix);
int32_t cel_string_contains(uint32_t s, uint32_t needle);

// ---- Three-valued logic helpers ------------------------------------------
//
// Implements CEL's OK / UNKNOWN / ERROR tri-state for the logical
// operators and for propagating UNKNOWN / ERROR status through
// arithmetic.  All inputs and outputs are arena offsets; a zero return
// encodes "type error" (operand kind was not one of CEL_BOOL /
// CEL_UNKNOWN / CEL_ERROR for the boolean helpers) so codegen can hoist
// the usual zero-check that every other runtime helper already emits.
//
// CEL `&&` and `||` short-circuit past ERROR / UNKNOWN when an operand
// is OK(false) / OK(true) respectively.  Outside short-circuit, ERROR
// dominates UNKNOWN, which dominates OK — see `doc/langdef.md`
// §partial-evaluation.

// 3VL AND.  Short-circuits on OK(false): `false && error = false`,
// `false && unknown = false`.  When both operands are UNKNOWN, returns
// a fresh UNKNOWN whose UnknownSet is the sorted-dedup'd union of the
// two operand sets.  Returns 0 on type error (operand kind not bool /
// unknown / error).
uint32_t cel_and(uint32_t a, uint32_t b);

// 3VL OR.  Short-circuits on OK(true).  Symmetric to `cel_and`.
uint32_t cel_or(uint32_t a, uint32_t b);

// 3VL NOT.  Flips a CEL_BOOL; passes CEL_UNKNOWN and CEL_ERROR through
// unchanged.  Returns 0 on type error.
uint32_t cel_not(uint32_t a);

// Sorted-dedup'd union of two UnknownSets, returned as a fresh
// CEL_UNKNOWN value.  Either operand being non-UNKNOWN is a type error
// (returns 0).  Deterministic — the merged order does not depend on
// which operand is passed first.
uint32_t cel_unknown_merge(uint32_t a, uint32_t b);

// Arithmetic status propagation.  Given two operand values, returns:
//   - `a` if `a` is ERROR;
//   - `b` if `b` is ERROR and `a` is not;
//   - `cel_unknown_merge(a, b)` if both are UNKNOWN;
//   - `a` if `a` is UNKNOWN and `b` is OK;
//   - `b` if `b` is UNKNOWN and `a` is OK;
//   - 0 if both are OK (the caller performs the arithmetic).
// Encodes the ERROR > UNKNOWN > OK dominance ordering; deterministic
// (prefers the left operand when both sides have the same dominant
// status).
uint32_t cel_status_either(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_
