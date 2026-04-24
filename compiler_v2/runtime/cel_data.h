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

// Final CelKind set.  Even kinds unused at M1 are declared so the wire
// encoding stays stable as later milestones light up new codegen arms.
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
};

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_DATA_H_
