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

// Extracts the i32 bool payload from a CelValue*.  Returns 0 for a
// non-bool / zero-offset input; callers that need to distinguish
// false-from-not-a-bool must check the kind themselves.  The common
// caller is the lowered has() path where the enclosing codegen already
// guarantees the result is bool.
int32_t cel_bool_from_value(uint32_t v);

// Scalar-unbox siblings for the uniform boxed ABI: take a CelValue
// offset of the expected kind, return its raw wasm scalar payload.
// Returns 0 / 0.0 on kind mismatch or zero-offset — the type checker
// guarantees the kind at every statically-typed call site, so this is
// a defense-in-depth fallback rather than a supported user error path.
int64_t cel_int_from_value(uint32_t v);
uint64_t cel_uint_from_value(uint32_t v);
double cel_double_from_value(uint32_t v);

// ---- Uniform-boxed ABI (Slice F step 4) ----------------------------------
//
// String / bytes eq, concat, starts_with, ends_with, contains, and size —
// the sole codegen entrypoints for these ops since Slice F Step 4.  Each
// takes CelValue offsets for its operands and returns a CelValue offset:
//
//   * If any operand is UNKNOWN / ERROR, the dominant non-OK status
//     (per `cel_status_either`; ERROR > UNKNOWN) is returned verbatim so
//     a wrapping `&&` / `||` / `?:` absorber sees it.
//   * On kind mismatch (operand not the expected CEL_STRING / CEL_BYTES)
//     a fresh `CEL_ERROR{CEL_ERR_TYPE_MISMATCH}` offset is returned.
//   * On success the result is a freshly boxed CelValue of the expected
//     kind: CEL_BOOL for eq / starts_with / ends_with / contains,
//     CEL_STRING / CEL_BYTES for concat, CEL_INT for size.
uint32_t cel_string_eq_v(uint32_t a, uint32_t b);
uint32_t cel_bytes_eq_v(uint32_t a, uint32_t b);
uint32_t cel_string_concat_v(uint32_t a, uint32_t b);
uint32_t cel_bytes_concat_v(uint32_t a, uint32_t b);
uint32_t cel_string_starts_with_v(uint32_t s, uint32_t prefix);
uint32_t cel_string_ends_with_v(uint32_t s, uint32_t suffix);
uint32_t cel_string_contains_v(uint32_t s, uint32_t needle);
uint32_t cel_string_size_v(uint32_t s);
uint32_t cel_bytes_size_v(uint32_t b);

// Message-equality prologue (Slice F Step 5).  Codegen emits this
// before invoking the host's `message_eq(externref, externref)` so
// UNKNOWN / ERROR message operands surface as a value the wrapping
// 3VL absorber (`cel_or` / `cel_and` / `cel_cmp_bool_*`) can see:
//
//   * OK CEL_MESSAGE both sides → returns 0.  Caller proceeds to
//     `cel_make_bool(message_eq(cel_unwrap_message(a),
//                                cel_unwrap_message(b)))` (and
//     wraps with `cel_not` for `_!=_`).
//   * Either side UNKNOWN / ERROR → returns the dominant non-OK
//     offset (`cel_status_either`; ERROR > UNKNOWN) verbatim.
//   * Either side non-message / zero → returns a freshly boxed
//     `CEL_ERROR{CEL_ERR_TYPE_MISMATCH}` offset.
//
// Why a separate "prologue" rather than a single `cel_message_eq_v`
// like the string / bytes helpers: `message_eq` is a host-side
// descriptor-aware compare living in `cel_host`, not reachable from
// the runtime wasm module.  Splitting the absorption check out into a
// prologue keeps the host call shape unchanged (still `externref,
// externref → i32`) while letting codegen short-circuit through the
// prologue's offset result.  See `LowerMessageEqualityBoxed` in
// `compiler/codegen/expr_lower.cc`.
uint32_t cel_message_eq_prologue_v(uint32_t a, uint32_t b);

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
// int/uint.  Unary negate (`-_`) is emitted inline by codegen as a
// plain `i64.sub` / `f64.neg`, so no `cel_*_neg_at` helper exists.
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

// ---- Boxed-operand checked arithmetic (M4 Slice F Step 2) ----------------
//
// Sret helpers that take CelValue offsets for both operands.  Each:
//
//   1. Short-circuits on `cel_status_either(a, b)` — if either
//      operand is ERROR / UNKNOWN, the dominant non-OK value is
//      copied into `*out` and the helper returns.  The wrapping
//      3VL absorber (`cel_and` / `cel_or` or a boxed comparison)
//      then sees the non-OK value as a CelValue offset and applies
//      the spec's absorption rules.
//   2. Kind-checks both operands (expected kind: CEL_INT or
//      CEL_UINT depending on the helper).  Type-mismatch writes
//      CEL_ERROR{CEL_ERR_TYPE_MISMATCH}.
//   3. Delegates the scalar overflow / div-by-zero checks to the
//      existing `_at_ii` / `_at_uu` siblings by extracting the
//      payload.
//
// Paired with Step 2's always-boxed arith codegen — `LowerCheckedArithBoxed`
// calls these so a nested arith subtree of a boxed comparison (rows
// 4, 18, 22 in `doc/implementation-plan/m4-slice-f-3vl-absorption.md`)
// absorbs non-OK instead of early-returning from `$eval`.
void cel_int_add_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_int_sub_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_int_mul_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_int_div_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_int_mod_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_uint_add_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_uint_sub_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_uint_mul_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_uint_div_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);
void cel_uint_mod_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off);

// ---- 3VL-aware scalar comparison (M4 Slice F1) ---------------------------
//
// Boxed-operand comparisons used when either operand's subtree can
// produce CEL_UNKNOWN or CEL_ERROR.  Each helper takes two CelValue
// offsets and returns a CelValue offset:
//
//   - If either operand is non-OK, returns `cel_status_either(a, b)`
//     (ERROR dominates UNKNOWN; UNKNOWN operands are merged).  The
//     wrapping 3VL absorber (`cel_and` / `cel_or`) then sees the
//     non-OK value and applies the spec's absorption rules rather
//     than the compiler early-returning from `$eval`.
//   - If both operands are OK of the expected kind, reads the
//     payload and returns `cel_make_bool(raw_cmp)`.
//   - Returns 0 on type mismatch (operand kind not matching the
//     helper's scalar kind), matching the other runtime helpers so
//     codegen's zero-check shape keeps working.
//
// Ordered double compares additionally return
// `cel_make_error(CEL_ERR_TYPE_MISMATCH)` when either OK operand is
// NaN — the same spec rule `LowerDoubleOrderedCompare` enforces on
// the scalar path.  Equality (`==` / `!=`) on NaN is well-defined by
// IEEE 754 and goes through the normal `d == d` compare.
uint32_t cel_cmp_int_eq(uint32_t a, uint32_t b);
uint32_t cel_cmp_int_ne(uint32_t a, uint32_t b);
uint32_t cel_cmp_int_lt(uint32_t a, uint32_t b);
uint32_t cel_cmp_int_le(uint32_t a, uint32_t b);
uint32_t cel_cmp_int_gt(uint32_t a, uint32_t b);
uint32_t cel_cmp_int_ge(uint32_t a, uint32_t b);

uint32_t cel_cmp_uint_eq(uint32_t a, uint32_t b);
uint32_t cel_cmp_uint_ne(uint32_t a, uint32_t b);
uint32_t cel_cmp_uint_lt(uint32_t a, uint32_t b);
uint32_t cel_cmp_uint_le(uint32_t a, uint32_t b);
uint32_t cel_cmp_uint_gt(uint32_t a, uint32_t b);
uint32_t cel_cmp_uint_ge(uint32_t a, uint32_t b);

uint32_t cel_cmp_double_eq(uint32_t a, uint32_t b);
uint32_t cel_cmp_double_ne(uint32_t a, uint32_t b);
uint32_t cel_cmp_double_lt(uint32_t a, uint32_t b);
uint32_t cel_cmp_double_le(uint32_t a, uint32_t b);
uint32_t cel_cmp_double_gt(uint32_t a, uint32_t b);
uint32_t cel_cmp_double_ge(uint32_t a, uint32_t b);

uint32_t cel_cmp_bool_eq(uint32_t a, uint32_t b);
uint32_t cel_cmp_bool_ne(uint32_t a, uint32_t b);

// ---- Debug / audit logging (cel_log) -------------------------------------
//
// Shared by two use cases:
//
//   1. Dead-code audit.  Every public runtime helper begins with a
//      `CEL_LOG("enter", ...)` call; a full run of the compiler test
//      suite records which helpers fired.  Helpers that never appear
//      in the capture are candidates for deletion.  macOS's Apple
//      clang has no `llvm-cov --show-functions` equivalent, so the
//      log-and-grep substitute lives in-tree.
//   2. Ad-hoc runtime tracing.  Codegen does not emit `cel_log` calls
//      today, but the import is declared unconditionally on every
//      eval module so a trace can be inserted at any call site
//      without a re-link.
//
// ABI (matches the import declared by `DeclareAllocAndSpanImports` in
// `compiler/codegen/expr_lower.cc`):
//
//   void cel_log(uint32_t file_ptr, uint32_t file_len,
//                uint32_t fn_ptr,   uint32_t fn_len,
//                uint32_t line,
//                uint32_t fmt_ptr,  uint32_t fmt_len,
//                uint32_t argv_ptr, uint32_t argc);
//
// `argv_ptr` points at `argc` contiguous 16-byte slots.  Each slot is
// two u64 words: the first word's low 32 bits carry the tag (high 32
// reserved), the second word is the per-tag payload (see
// `CEL_LOG_TAG_*` below).  The format string is parsed host-side — the
// wasm runtime has no printf — so any byte that is not part of a known
// directive is treated as a literal.  Directives:
//
//   %s   string span, payload = (u32 ptr, u32 len) packed into u64
//   %d   signed i64
//   %u   unsigned u64
//   %f   f64 (bit-cast into payload)
//   %b   i32 bool (prints "true" / "false")
//   %v   u32 CelValue offset; host pretty-prints kind + payload
//   %%   literal percent
//
// Anything else (a bare `%x`, an unmatched trailing `%`, an argc /
// directive mismatch) is printed verbatim — logging is diagnostic-only
// and must never trap.
enum {
  CEL_LOG_TAG_STR = 1,
  CEL_LOG_TAG_INT = 2,
  CEL_LOG_TAG_UINT = 3,
  CEL_LOG_TAG_DOUBLE = 4,
  CEL_LOG_TAG_BOOL = 5,
  CEL_LOG_TAG_VALUE = 6,
};

// Host import.  On wasm32 the `import_module` / `import_name`
// attributes force the symbol into an `(import "cel_env" "cel_log"
// …)` entry that the host loader satisfies via `RegisterCelLog`
// before the runtime instance stands up.  On the native-host build
// `cel_runtime.c` provides a weak no-op so linking as a plain C
// library for unit tests "just works"; embedders that want to
// capture runtime-native log lines can define a strong override.
#ifdef __wasm__
__attribute__((import_module("cel_env"), import_name("cel_log")))
#endif
void cel_log(uint32_t file_ptr, uint32_t file_len, uint32_t fn_ptr,
             uint32_t fn_len, uint32_t line, uint32_t fmt_ptr, uint32_t fmt_len,
             uint32_t argv_ptr, uint32_t argc);

// Ergonomic call-site helpers.  Each expands to a `(uint64_t) tag_word,
// (uint64_t) payload` pair so `CEL_LOG` can slam them into a
// `uint64_t[]` compound literal.  The tag word's low 32 bits hold the
// `CEL_LOG_TAG_*` discriminator; the high 32 bits are reserved (zero
// today — the host decoder reads only the low 32).  Total slot size
// is 16 bytes (tag u32, pad u32, payload u64) — matching the
// `kArgvSlotBytes` constant and the `(u32 tag, u32 pad, u64 payload)`
// comment on `CelLogWireArgs`.
//
//   CEL_LOG_STR(ptr, len)  — %s
//   CEL_LOG_INT(i)         — %d
//   CEL_LOG_UINT(u)        — %u
//   CEL_LOG_DBL(d)         — %f
//   CEL_LOG_BOOL(b)        — %b
//   CEL_LOG_V(off)         — %v
//
// The string-span packer keeps ptr in the low 32 bits and len in the
// high 32 — the host decoder splits them back out via a mask / shift.
#define CEL_LOG_STR(ptr, len) \
  (uint64_t)CEL_LOG_TAG_STR,  \
      ((uint64_t)(uint32_t)(ptr)) | ((uint64_t)(uint32_t)(len) << 32)

#define CEL_LOG_INT(i) (uint64_t)CEL_LOG_TAG_INT, (uint64_t)(int64_t)(i)

#define CEL_LOG_UINT(u) (uint64_t)CEL_LOG_TAG_UINT, (uint64_t)(u)

// Bit-cast the double through a union-equivalent so strict-aliasing
// stays happy on every target.  Picks a ULL payload the host reads
// back via `memcpy(&d, &payload, 8)`.
#define CEL_LOG_DBL(d) \
  (uint64_t)CEL_LOG_TAG_DOUBLE, __builtin_bit_cast(uint64_t, (double)(d))

#define CEL_LOG_BOOL(b) (uint64_t)CEL_LOG_TAG_BOOL, (uint64_t)((b) ? 1 : 0)

#define CEL_LOG_V(off) (uint64_t)CEL_LOG_TAG_VALUE, (uint64_t)(uint32_t)(off)

// Trampoline from the ergonomic `(const char*, ...)` call-site shape
// to the `(uint32_t, ...)` wire ABI.  Keeps the macro a single
// expression and hides the `uintptr_t`→`uint32_t` cast so callers
// don't have to think about the wasm-vs-native layering.  On wasm32
// a C pointer is a uint32 linear-memory offset already; on native
// host the cast truncates 64-bit pointers to 32 bits — the weak
// default `cel_log` body is a no-op so that's harmless, but embedders
// wiring a native-host sink should intercept this call directly (not
// the trampoline) to get the unfiltered pointers.
void cel_log_emit(const char* file, uint32_t file_len, const char* fn,
                  uint32_t fn_len, uint32_t line, const char* fmt,
                  uint32_t fmt_len, const uint64_t* argv, uint32_t argc);

// Fire a log line.  Compiles to nothing when CEL_LOG_DISABLED is set,
// so tests can disable the hook for perf-sensitive runs.  Default is
// enabled — the audit driver wants it on.
#ifdef CEL_LOG_DISABLED
#define CEL_LOG(fmt_literal, ...) ((void)0)
#else
// Each `CEL_LOG_*` call-site helper expands to a `(tag_word, payload)`
// pair of `uint64_t` values — a 16-byte slot once staged in memory.
// Stage them into a compound-literal array sized `argc * 2 + 1` (the
// trailing sentinel 0 makes an empty arg list legal — a zero-element
// C array is non-standard).  Divide the element count minus the
// sentinel by 2 to recover argc.
#define CEL_LOG(fmt_literal, ...)                                        \
  do {                                                                   \
    static const char kCelLogFmt_[] = (fmt_literal);                     \
    uint64_t argv_[] = {__VA_ARGS__ 0};                                  \
    uint32_t argc_ =                                                     \
        (uint32_t)(((sizeof(argv_) / sizeof(uint64_t)) - 1u) / 2u);      \
    cel_log_emit(__FILE__, (uint32_t)sizeof(__FILE__) - 1u, __func__,    \
                 cel_strlen_(__func__), (uint32_t)__LINE__, kCelLogFmt_, \
                 (uint32_t)sizeof(kCelLogFmt_) - 1u, argv_, argc_);      \
  } while (0)
#endif

// In-runtime strlen.  Small enough to inline; also keeps the
// freestanding wasm build self-contained.  Not exported.
static inline uint32_t cel_strlen_(const char* s) {
  uint32_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_RUNTIME_CEL_RUNTIME_H_
