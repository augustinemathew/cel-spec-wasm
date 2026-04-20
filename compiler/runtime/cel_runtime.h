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

// Binary checked arithmetic.  Each helper takes two CelValue offsets
// (kind must be CEL_INT / CEL_UINT for the happy path; any CEL_ERROR
// or CEL_UNKNOWN on either side propagates via `cel_status_either`)
// and returns a CelValue offset of kind CEL_INT / CEL_UINT on success,
// or CEL_ERROR with `CEL_ERR_OVERFLOW` / `CEL_ERR_DIVIDE_BY_ZERO` /
// `CEL_ERR_MODULUS_BY_ZERO` on failure.  Returns 0 on type error
// (operand kind not one of the supported ones) or OOM — callers get
// the usual zero-check for free.
//
// CEL §langdef.arithmetic: signed overflow, int `/` / `%` by zero,
// and `INT64_MIN / -1` (which also overflows) must produce ERROR, not
// trap.  Unsigned uses wrap-around detection (`a + b < a`) since
// `uint64_t` addition is well-defined in C.
uint32_t cel_int_add(uint32_t a, uint32_t b);
uint32_t cel_int_sub(uint32_t a, uint32_t b);
uint32_t cel_int_mul(uint32_t a, uint32_t b);
uint32_t cel_int_div(uint32_t a, uint32_t b);
uint32_t cel_int_mod(uint32_t a, uint32_t b);

uint32_t cel_uint_add(uint32_t a, uint32_t b);
uint32_t cel_uint_sub(uint32_t a, uint32_t b);
uint32_t cel_uint_mul(uint32_t a, uint32_t b);
uint32_t cel_uint_div(uint32_t a, uint32_t b);
uint32_t cel_uint_mod(uint32_t a, uint32_t b);

// Unary negate for int.  `-INT64_MIN` overflows, so this helper
// produces ERROR for that one input.  No `cel_uint_neg` because CEL
// has no unary-minus overload on uint.
uint32_t cel_int_neg(uint32_t a);

// Scalar-constructor variants the codegen can call from straight-line
// wasm without round-tripping a scalar through `cel_make_int`.  Same
// semantics as the boxed variants — the boxed version just does the
// unwrap internally — but shaped to match the common case where one
// or both operands are already scalars (constants, ident reads).
//
// We do not combinatorially expand every (boxed, scalar) pair: the
// codegen consistently boxes leaves first, so the boxed variants
// above are the load-bearing ones.  These scalar helpers are kept
// for completeness and for the benchmarks in the M4 plan doc.
uint32_t cel_int_add_ii(int64_t a, int64_t b);
uint32_t cel_int_sub_ii(int64_t a, int64_t b);
uint32_t cel_int_mul_ii(int64_t a, int64_t b);
uint32_t cel_int_div_ii(int64_t a, int64_t b);
uint32_t cel_int_mod_ii(int64_t a, int64_t b);

uint32_t cel_uint_add_uu(uint64_t a, uint64_t b);
uint32_t cel_uint_sub_uu(uint64_t a, uint64_t b);
uint32_t cel_uint_mul_uu(uint64_t a, uint64_t b);
uint32_t cel_uint_div_uu(uint64_t a, uint64_t b);
uint32_t cel_uint_mod_uu(uint64_t a, uint64_t b);

uint32_t cel_int_neg_i(int64_t a);

// ---- Scratch-slot (sret) ABI (M4 Slice C) --------------------------------
//
// Parallel flavor of the checked-arithmetic API above.  Instead of
// bump-allocating a fresh 24-byte CelValue in the arena on every call,
// these helpers write the 24-byte result *into a caller-provided slot*.
//
// The `out` parameter is a 32-bit byte offset into linear memory
// pointing at an 8-byte-aligned, 24-byte region — one CelValue's
// worth.  Typically it is a software-stack frame slot reserved at
// `$eval` entry (see `doc/wasm-compiler-design.md` §7.4.2), though
// the runtime is agnostic: any aligned region in the module's linear
// memory works.  `a` and `b` are the usual offset-to-boxed-CelValue
// arguments that the boxed API already speaks, so callers can freely
// mix the two ABIs (e.g. pass an arena-resident ident read as `a`
// and a freshly-computed stack slot as `b`).
//
// Contrast:
//     uint32_t cel_int_add(a, b);        // allocates, returns offset.
//     void     cel_int_add_at(out,a,b);  // caller pre-owns `out`.
//
// **Total-function guarantee.**  Every helper always writes a
// well-formed 24-byte CelValue into `*out` before returning.  This
// holds across all four outcomes:
//
//   1. Status propagation.  If either operand carries non-OK status,
//      the helper does NOT do arithmetic — it copies the dominant
//      status value into `*out`.  Dominance is ERROR > UNKNOWN > OK
//      with left-wins tie-breaking (same as `cel_status_either`);
//      two UNKNOWNs merge their sets via `cel_unknown_merge`.
//   2. Type mismatch.  If an operand's kind doesn't match the
//      expected arithmetic kind (int for `cel_int_*_at`, uint for
//      `cel_uint_*_at`), `*out` gets `CEL_ERROR{CEL_ERR_TYPE_MISMATCH}`.
//      The static checker should make this unreachable; surfacing it
//      as ERROR means a codegen bug degrades gracefully instead of
//      forging a phantom OK result.
//   3. Arithmetic error.  Overflow → `CEL_ERROR{CEL_ERR_OVERFLOW}`;
//      int `/` by 0 → `CEL_ERR_DIVIDE_BY_ZERO`; int `%` by 0 →
//      `CEL_ERR_MODULUS_BY_ZERO`.  `INT64_MIN / -1` overflows;
//      `INT64_MIN % -1` is defined as 0 (matching cel-go).  `-INT64_MIN`
//      overflows.
//   4. Happy path.  `CEL_INT{result}` or `CEL_UINT{result}`.
//
// Because `*out` is always well-formed, codegen emits no branch on
// "did the helper write anything?" — it just loads `[out+0]` after
// the call and dispatches on the kind tag (OK → read payload;
// ERROR → propagate up; UNKNOWN → propagate up).  The return type
// is `void` for the same reason.
//
// `out == 0` points at the null sentinel at memory offset 0.  That's
// treated as a caller bug (no frame slot is ever allocated there) and
// the helpers early-return without writing, leaving the sentinel intact.
//
// **Why not just reuse the boxed ABI?**  The boxed variants burn an
// arena CelValue per call — for an expression like `(a + b) + (c + d)`
// that's 3 allocations for the intermediate results alone, all of which
// outlive the expression.  The sret ABI lets codegen reuse a small
// pool of frame slots sized to the expression's tree depth, keeping
// temporary-value memory O(depth) instead of O(#subexpressions).

// ---- Primitive → slot writers -------------------------------------------
//
// Used at scalar→boxed boundaries where codegen has a raw wasm
// scalar on the stack (a literal, an ident read, or the unboxed
// result of a non-checked op) and needs to hand it to something
// that speaks boxed CelValue (e.g. the 3VL `cel_and` / `cel_or`).
// `cel_box_bool` normalizes any non-zero int input to 1; the others
// are straight payload writes.
void cel_box_bool(uint32_t out, int32_t b);
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

// ---- Checked-arithmetic sret helpers -------------------------------------
//
// Semantics: see the "total-function guarantee" block above.  One
// helper per CEL `_+_` / `_-_` / `_*_` / `_/_` / `_%_` overload on
// int/uint, plus `cel_int_neg_at` for unary minus.  (CEL has no
// unary-minus overload on uint, so there is no `cel_uint_neg_at`.)
void cel_int_add_at(uint32_t out, uint32_t a, uint32_t b);
void cel_int_sub_at(uint32_t out, uint32_t a, uint32_t b);
void cel_int_mul_at(uint32_t out, uint32_t a, uint32_t b);
void cel_int_div_at(uint32_t out, uint32_t a, uint32_t b);
void cel_int_mod_at(uint32_t out, uint32_t a, uint32_t b);

void cel_uint_add_at(uint32_t out, uint32_t a, uint32_t b);
void cel_uint_sub_at(uint32_t out, uint32_t a, uint32_t b);
void cel_uint_mul_at(uint32_t out, uint32_t a, uint32_t b);
void cel_uint_div_at(uint32_t out, uint32_t a, uint32_t b);
void cel_uint_mod_at(uint32_t out, uint32_t a, uint32_t b);

void cel_int_neg_at(uint32_t out, uint32_t a);

// Scalar-arg sret variants.  Same semantics as the boxed-arg `_at`
// helpers above — the total-function guarantee still holds — except
// the operands arrive as raw wasm scalars instead of arena offsets,
// so there is no operand-status to propagate (pure OK/ERROR outcome).
// These are the ones codegen emits from straight-line arithmetic
// where both operands are already wasm i64 / u64 values (literals,
// ident reads, payload.i loads).  Paired with `cel_box_*` at the
// boundaries where a scalar needs to enter the boxed API (e.g. a 3VL
// `cel_and` call).
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
