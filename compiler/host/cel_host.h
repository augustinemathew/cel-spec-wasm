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
#include "absl/strings/string_view.h"
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
// out-parameter shape as a successful read.  Repeated fields (M5+) are
// likewise stubbed as CEL_ERROR for now; the checker should normally catch
// this earlier, but the runtime guard keeps the host honest.
using InternMessage =
    absl::AnyInvocable<uint32_t(const google::protobuf::Message& submessage)>;

// `field_number == 0` is the sentinel for "not proto-resolvable"
// (forward-compat path for JSON / map backings): in that case the
// host resolves the field by `field_name` instead.  For proto-backed
// messages the compiler always emits the concrete number, so the
// fast descriptor-lookup path is taken.  `field_name` is always
// populated — attribute-pattern matching in the partial-eval layer
// compares by name regardless of how the descriptor was resolved.
void ReadField(const google::protobuf::Message& msg, int field_number,
               absl::string_view field_name, CelValue* absl_nonnull out,
               ArenaAllocator& alloc, InternMessage& intern);

// Returns true iff `msg` has the field set.  Same `(field_number,
// field_name)` resolution contract as `ReadField`: prefer number
// when non-zero, fall back to name.  Mirrors `Reflection::HasField`,
// which matches CEL's `has()` semantics for proto messages: proto2
// reports explicit presence; proto3 singular scalar fields report
// true iff the field's value is not the type's default.  Unknown
// fields return false — the checker already rejects `has(msg.nope)`
// earlier, so `has_field` never sees a bogus name in practice.
bool HasField(const google::protobuf::Message& msg, int field_number,
              absl::string_view field_name);

// True iff `a` and `b` are proto-equal.  Delegates to protobuf's
// `MessageDifferencer::Equals`, which is descriptor-aware (handles
// unknown fields, repeated ordering, and map semantics correctly — the
// reason CEL routes message `==` through the host rather than computing
// it module-side).
bool MessageEq(const google::protobuf::Message& a,
               const google::protobuf::Message& b);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_CEL_HOST_H_
