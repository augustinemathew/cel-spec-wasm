// CEL runtime common data types.  Shared by every other runtime header
// and by any host-side code that needs to read / write the CelValue
// layout (codegen, cel_log pretty-printer, tests).  Header-only — no
// implementation file — so it can be included freely without dragging
// in function declarations.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_DATA_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_DATA_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Final CelKind set.  M3 split the single `CEL_MAP` kind into
// `CEL_MAP_ARENA` (literal-built, header in linear memory) and
// `CEL_MAP_HOST` (proto reflection / Activation::Bind, payload is a
// host-table ref_slot); M4 mirrors that split for lists:
// `CEL_LIST_ARENA` (literal-built, header in linear memory) and
// `CEL_LIST_HOST` (proto repeated / Activation::Bind list, payload
// is a host-table ref_slot).  See
// `doc/implementation-plan/rewrite/map-list-dispatch.md`.
//
// Numeric values are stable on the wire from this point forward:
// host pretty-printers, cel_host trampolines, and any persisted
// fixture fingerprints rely on them.  Append only; never renumber.
// `CEL_LIST_ARENA` reuses the pre-M4 `CEL_LIST = 7` slot to avoid
// ABI churn; `CEL_LIST_HOST = 17` is the new tail value.
typedef enum {
  CEL_NULL = 0,
  CEL_BOOL = 1,
  CEL_INT = 2,
  CEL_UINT = 3,
  CEL_DOUBLE = 4,
  CEL_STRING = 5,
  CEL_BYTES = 6,
  CEL_LIST_ARENA = 7,
  CEL_MAP_ARENA = 8,
  CEL_MAP_HOST = 9,
  CEL_MESSAGE = 10,
  CEL_TYPE = 11,
  CEL_DURATION = 12,
  CEL_TIMESTAMP = 13,
  CEL_OPTIONAL = 14,
  CEL_UNKNOWN = 15,
  CEL_ERROR = 16,
  CEL_LIST_HOST = 17,
} CelKind;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelSpan;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelArray;

// Arena-backed map header (dispatch-doc §4.1).  Pointed at by
// `CelValue.payload.arena_map.header_ptr` (a u32 byte offset into the
// shared linear memory).  `entries_offset` points at a contiguous
// `capacity` × 48-byte run — entries are { key:CelValue, val:CelValue }
// pairs back-to-back.  Growth re-allocates the entries run in the
// bump arena and updates `entries_offset` in place; the header itself
// stays put so `CelValue.payload.arena_map` is stable across grow.
typedef struct {
  uint32_t count;
  uint32_t capacity;
  uint32_t entries_offset;
  uint32_t _pad;
} ArenaMapHeader;

_Static_assert(sizeof(ArenaMapHeader) == 16,
               "ArenaMapHeader must be 16 bytes (dispatch-doc §4.1)");

typedef struct {
  uint32_t header_ptr;
} ArenaMapRef;

// Arena-backed list header (dispatch-doc §4.2).  Mirrors
// `ArenaMapHeader` but each element is a single CelValue (24 B), so
// the entry stride halves from 48 to 24.  `elements_offset` points at
// a contiguous `capacity` × 24-byte run; growth re-allocates the run
// and updates `elements_offset` in place.
typedef struct {
  uint32_t count;
  uint32_t capacity;
  uint32_t elements_offset;
  uint32_t _pad;
} ArenaListHeader;

_Static_assert(sizeof(ArenaListHeader) == 16,
               "ArenaListHeader must be 16 bytes (dispatch-doc §4.2)");

typedef struct {
  uint32_t header_ptr;
} ArenaListRef;

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
    ArenaMapRef arena_map;    // CEL_MAP_ARENA
    ArenaListRef arena_list;  // CEL_LIST_ARENA
    uint32_t ref_slot;        // CEL_MAP_HOST + CEL_LIST_HOST (and any
                              // future host aggregates; CEL_MESSAGE
                              // has its own `msg_slot` for now to
                              // keep cel_host call sites unchanged
                              // since M3.A).
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

// Each map entry is two CelValues (key, value).  Pinned here so the
// runtime + every host consumer agree on the stride; `cel_map_grow`
// uses this to size the entries arena.
enum {
  kCelMapEntryStride = 48,
};

// Each list element is a single CelValue (no key).  `cel_list_create`
// uses this to size the elements arena; codegen + decoders rely on
// it to walk arena lists.
enum {
  kCelListEntryStride = 24,
};

_Static_assert(sizeof(CelValue) == kCelListEntryStride,
               "kCelListEntryStride must equal sizeof(CelValue)");

// Wasm memory is always little-endian (spec).  Host↔wasm CelValue
// transfer uses bitwise memcpy, which only works when the host is
// also LE — otherwise kind/payload bytes arrive swapped.  Every
// wasmtime-supported host is LE in practice; this guard fails the
// build loudly on a hypothetical BE port rather than silently
// miscoding.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "CEL WASM requires a little-endian host"
#endif

// Error codes carried in `cel_make_error(code, ...)`.  Kept numeric so
// the wasm side stays allocation-free in the happy path — the host
// pretty-printer maps code → message when formatting a failed eval.
//
// Numeric values are stable on the wire and mirror
// `cel::ErrorCode` (api/error.h).  Append only; never renumber.
enum {
  CEL_ERR_OVERFLOW = 10,
  CEL_ERR_DIVIDE_BY_ZERO = 11,
  CEL_ERR_MODULUS_BY_ZERO = 12,
  CEL_ERR_TYPE_MISMATCH = 13,
  // Returned by `ProtoBacking::ReadField` for MAP / REPEATED fields
  // until M6 flips them to host-backed aggregates.  Named explicitly
  // as the M2→M6 graduation contract (m2-ident-select-unknowns.md
  // §2.8 / §6.1.1 envelope boundary row).
  CEL_ERR_TYPE_UNSUPPORTED = 14,
  // Returned by `cel_map_lookup_arena` (and the kDynamic dispatcher)
  // when the key is absent from the map.  Per langdef §"Indexing":
  // map indexing on a missing key is a no_such_key error, not null.
  CEL_ERR_NO_SUCH_KEY = 15,
  // Returned by `cel_map_insert` when the literal contains a duplicate
  // key — per langdef §"Map literals", repeated keys are an error
  // captured at construction time.
  CEL_ERR_DUPLICATE_KEY = 16,
  // Returned by `cel_list_at_arena` (and the kDynamic dispatcher's
  // arena arm) when the index is outside `[0, count)`.  Per langdef
  // §"Indexing": list indexing on a negative index or `>= size` is
  // an error (not Python-style wrap-around).  Wire value mirrors
  // `cel::ErrorCode::kIndexOutOfBounds` (api/error.h).
  CEL_ERR_INDEX_OUT_OF_BOUNDS = 17,
  // M2.C: Layer-2 trampoline (`CelGetFieldImpl` / `CelHasFieldImpl`)
  // returns this when the resolver can't find a FieldDescriptor for
  // the (field_number, field_name) pair the AST referenced — usually
  // because field_ref_id is out-of-range against `cel.abi.fields[]`.
  // Mirrors `cel::ErrorCode::kFieldNotFound` (api/error.h).
  CEL_ERR_FIELD_NOT_FOUND = 20,
  // M2.C: Layer-2 trampoline returns this when the externref slot
  // pointed at by a CEL_MESSAGE CelValue has not been interned (or
  // was interned in a different generation that has since been
  // Reset()).  Distinct from kTypeMismatch — the operand kind was
  // valid, the host-side dereference failed.  Mirrors
  // `cel::ErrorCode::kHostAdapterError`.
  CEL_ERR_HOST_ADAPTER_ERROR = 41,
};

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_DATA_H_
