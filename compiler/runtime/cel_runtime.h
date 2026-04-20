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

// ---- Checked arithmetic (M4 Slice B) -------------------------------------
//
// Error codes carried in `cel_make_error(code, ...)` by the checked
// arithmetic helpers.  Kept numeric rather than stringly-typed so the
// wasm side stays allocation-free in the happy path — the host-side
// pretty-printer maps code → message when formatting a failed eval.
enum {
  CEL_ERR_OVERFLOW = 10,
  CEL_ERR_DIVIDE_BY_ZERO = 11,
  CEL_ERR_MODULUS_BY_ZERO = 12,
  // Static-type-checker-guaranteed-impossible mismatch that still made it
  // to runtime.  Reaching one at runtime is a codegen bug, not a user
  // bug — surfaced as CEL_ERROR so a buggy run degrades gracefully
  // rather than forging a phantom OK result.
  CEL_ERR_TYPE_MISMATCH = 13,
};

// ---- Scratch-slot (sret) ABI (M4 Slice C) --------------------------------
//
// These helpers write a 24-byte CelValue result *into a caller-provided
// slot* rather than bump-allocating from the arena on every call.  The
// `out` parameter is a 32-bit byte offset into linear memory pointing at
// an 8-byte-aligned, 24-byte region — typically a software-stack frame
// slot reserved at `$eval` entry (see `doc/wasm-compiler-design.md`
// §7.4.2), though the runtime is agnostic to any aligned region.
//
// **Total-function guarantee.**  Every helper always writes a well-formed
// 24-byte CelValue into `*out` before returning:
//
//   - Arithmetic error.  Overflow → `CEL_ERROR{CEL_ERR_OVERFLOW}`;
//     int `/` by 0 → `CEL_ERR_DIVIDE_BY_ZERO`; int `%` by 0 →
//     `CEL_ERR_MODULUS_BY_ZERO`.  `INT64_MIN / -1` overflows;
//     `INT64_MIN % -1` is defined as 0 (matching cel-go).
//     `-INT64_MIN` overflows.
//   - Happy path.  `CEL_INT{result}` or `CEL_UINT{result}`.
//
// Because `*out` is always well-formed, codegen emits no branch on "did
// the helper write anything?" — it just loads `[out+0]` after the call
// and dispatches on the kind tag.  The return type is `void` for the
// same reason.
//
// `out == 0` points at the null sentinel at memory offset 0.  That's
// treated as a caller bug (no frame slot is ever allocated there) and
// the helpers early-return without writing, leaving the sentinel intact.
//
// The sret ABI lets codegen reuse a small pool of frame slots sized to
// the expression's tree depth, keeping temporary-value memory O(depth)
// instead of O(#subexpressions).

// ---- Primitive → slot writers -------------------------------------------
//
// Used at scalar→boxed boundaries where codegen has a raw wasm
// scalar on the stack (a literal, an ident read, or the unboxed
// result of a non-checked op) and needs to hand it to something
// that speaks boxed CelValue (e.g. the 3VL `cel_and` / `cel_or`).
// Bool doesn't have an sret writer of its own because Repr::kBool
// travels as a CelValue offset post-3b2 — codegen goes through
// `cel_make_bool(raw)` and then `cel_copy_celvalue_at(out, offset)`
// instead of a dedicated `cel_box_bool(out, raw)`.
void cel_box_int(uint32_t out, int64_t i);
void cel_box_uint(uint32_t out, uint64_t u);
void cel_box_double(uint32_t out, double d);

// Copies a pre-built 24-byte CelValue from `src` to `out` (both are
// arena offsets into g_memory).  Used at the sret-eval boundary for
// roots whose Repr is already carried as a CelValue offset (string,
// bytes, message wrapped via `cel_wrap_message`) — in those cases
// there is no raw scalar to box, so the copy is all we need.  The
// `out == 0` check matches every other `_at` helper (a zero slot is
// a well-defined no-op so the total-function guarantee holds even
// when codegen forgets to pre-allocate).  `src == 0` means "no value
// available" — we write CEL_ERROR{TYPE_MISMATCH} into the slot so a
// forgotten source propagates as a proper CEL error instead of a
// phantom OK.
void cel_copy_celvalue_at(uint32_t out, uint32_t src);

// Writes CEL_ERROR{code, no-msg} into the 24-byte slot at `*out`.
// Used by codegen as the "oh no, computation failed" escape hatch:
// the NaN-in-ordered-compare guard emits
// `cel_set_error_at(sret, CEL_ERR_TYPE_MISMATCH); return;` so the
// host sees an observable error instead of a wasm trap.  `out == 0`
// is a no-op — same total-function guarantee as the box helpers.
void cel_set_error_at(uint32_t out, uint32_t code);

// ---- Checked-arithmetic sret helpers -------------------------------------
//
// Semantics: see the "total-function guarantee" block above.  One
// helper per CEL `_+_` / `_-_` / `_*_` / `_/_` / `_%_` overload on
// int/uint, plus `cel_int_neg_at` for unary minus.  (CEL has no
// unary-minus overload on uint, so there is no `cel_uint_neg_at`.)
// Scalar-arg sret variants.  Operands arrive as raw wasm scalars
// (i64 / u64) rather than arena offsets, so there is no operand-status
// to propagate (pure OK/ERROR outcome).  These are the ones codegen
// emits from straight-line arithmetic where both operands are already
// wasm scalars (literals, ident reads, payload.i loads).  The same
// total-function guarantee as the other sret helpers holds.  Paired
// with `cel_box_*` at the boundaries where a scalar needs to enter
// the boxed API (e.g. a 3VL `cel_and` call).
void cel_int_add_at_ii(uint32_t out, int64_t a, int64_t b);
void cel_int_sub_at_ii(uint32_t out, int64_t a, int64_t b);
void cel_int_mul_at_ii(uint32_t out, int64_t a, int64_t b);
void cel_int_div_at_ii(uint32_t out, int64_t a, int64_t b);
void cel_int_mod_at_ii(uint32_t out, int64_t a, int64_t b);

void cel_uint_add_at_uu(uint32_t out, uint64_t a, uint64_t b);
void cel_uint_sub_at_uu(uint32_t out, uint64_t a, uint64_t b);
void cel_uint_mul_at_uu(uint32_t out, uint64_t a, uint64_t b);
void cel_uint_div_at_uu(uint32_t out, uint64_t a, uint64_t b);
void cel_uint_mod_at_uu(uint32_t out, uint64_t a, uint64_t b);

void cel_int_neg_at_i(uint32_t out, int64_t a);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_
