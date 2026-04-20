// Host-side logic backing the `cel_host.{get_field, has_field, message_eq}`
// imports.  Kept free of any wasmtime dependency so it can be unit-tested
// natively and reused by any embedder that can unwrap a `Message*` from an
// externref.  The wasmtime-specific wiring lives in `cel_host_wasmtime.{h,cc}`.

#ifndef CELWASM_COMPILER_HOST_CEL_HOST_H_
#define CELWASM_COMPILER_HOST_CEL_HOST_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/nullability.h"
#include "absl/functional/any_invocable.h"
#include "compiler/runtime/cel_runtime.h"
#include "google/protobuf/message.h"

namespace celwasm {

// Reserves `len` bytes in the wasm arena (via an imported-back `cel_alloc`)
// and hands back a host-addressable pointer plus the arena-relative offset
// so the CelSpan's `ptr` field matches what the module sees.  Returns
// nullptr on allocation failure; `*out_offset` is undefined in that case.
using ArenaAllocator = absl::AnyInvocable<
    uint8_t* absl_nullable(size_t len, uint32_t* absl_nonnull out_offset)>;

// Writes `msg.<field_number>` into `*out`.  Scalar fields fill `*out` in
// place; string / bytes payloads are copied into a fresh arena allocation
// obtained from `alloc`; message fields write `kind = CEL_MESSAGE` plus the
// caller-provided `msg_slot` (obtained by interning the submessage into the
// module's externref table — see `RegisterCelHost`).
//
// If `field_number` does not resolve on `msg`'s descriptor, `*out` is set
// to `{kind = CEL_ERROR}` — CEL treats "field not present" as an evaluation
// error, not a host-level failure, so it travels through the same
// out-parameter shape as a successful read.  Repeated fields (M4+) are
// likewise stubbed as CEL_ERROR for now; the checker should normally catch
// this earlier, but the runtime guard keeps the host honest.
using InternMessage =
    absl::AnyInvocable<uint32_t(const google::protobuf::Message& submessage)>;

void ReadField(const google::protobuf::Message& msg, int field_number,
               CelValue* absl_nonnull out, ArenaAllocator& alloc,
               InternMessage& intern);

// Returns true iff `msg` has `field_number` set.  Mirrors
// `Reflection::HasField`, which matches CEL's `has()` semantics for proto
// messages: proto2 reports explicit presence; proto3 singular scalar
// fields report true iff the field's value is not the type's default
// (which is what CEL calls "has" on a proto3 field without presence).
// Unknown field numbers return false — consistent with the checker-side
// view that `has(msg.nope)` is an error, so the codegen path for
// `has_field` never sees this call with a bogus number in practice.
bool HasField(const google::protobuf::Message& msg, int field_number);

// True iff `a` and `b` are proto-equal.  Delegates to protobuf's
// `MessageDifferencer::Equals`, which is descriptor-aware (handles
// unknown fields, repeated ordering, and map semantics correctly — the
// reason CEL routes message `==` through the host rather than computing
// it module-side).
bool MessageEq(const google::protobuf::Message& a,
               const google::protobuf::Message& b);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_CEL_HOST_H_
