// Conformance-harness marshaller from upstream test-fixture proto
// shapes (`cel.expr.ExprValue`, `cel.expr.Decl`, `cel.expr.Type`)
// into the public `cel::` surface (`cel::Value`, `cel::Activation`,
// the `name:type` spec strings consumed by
// `Compiler::Builder::DeclareVariable`).
//
// The harness uses these to widen the M2 envelope to admit tests
// carrying scalar `bindings:` / `type_env:` / `container:` — every
// gate is a graceful `Unimplemented` (caller SKIPs) rather than a
// crash, since the corpus mixes scalar shapes (M2-eligible) with
// aggregate shapes (M6/M7) inside the same fixture file.
//
// Scope at M2: scalar kinds (null/bool/int/uint/double/string/bytes)
// only.  Aggregate ExprValue (`object_value`, `map_value`,
// `list_value`, `enum_value`, `type_value`), aggregate `Decl` types
// (list/map/message), function decls, and unknown/error ExprValue
// all return `Unimplemented` — later milestones lift each gate.

#ifndef CELWASM_COMPILER_V2_CONFORMANCE_BINDING_MARSHAL_H_
#define CELWASM_COMPILER_V2_CONFORMANCE_BINDING_MARSHAL_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/message.h"

namespace celwasm::conformance {

// Decode a `cel.expr.Value` (scalar kind only) into a `cel::Value`.
// Returns `Unimplemented` for any aggregate / enum / type kind, and
// `InvalidArgument` if no `kind` is set on the proto.
ABSL_MUST_USE_RESULT absl::StatusOr<cel::Value> ValueFromProto(
    const cel::expr::Value& v);

// M7: unpack an Any-style `object_value` payload into a heap-owned
// proto.  Caller's descriptor pool (the generated pool) must have
// the type registered — the conformance harness force-links
// TestAllTypes via `ForceLinkFixtureDescriptors` in `runner.cc`.
// Used both by `ValueFromProto`'s kObjectValue arm (binding side)
// and by `runner.cc::CompareMessage` (matcher side).
ABSL_MUST_USE_RESULT absl::StatusOr<
    std::unique_ptr<google::protobuf::Message>>
UnpackAny(const google::protobuf::Any& any);

// Convert a scalar `cel.expr.Decl` (IdentDecl with a primitive
// `Type`) into a `name:type` spec string consumed by
// `CheckOptions::variable_specs` (the lower-level frontend surface).
// The harness uses this for unit tests; for actually declaring a
// variable on a `cel::Compiler::Builder` use
// `DeclareVariablesOnBuilder` below, which goes via the typed
// `CelType` surface and skips a round-trip through the spec parser.
//
// `Unimplemented` for:
//   - aggregate types (list_type / map_type / message_type)
//   - `wrapper`, `well_known`, `function`, `abstract_type`,
//     `type_param`, `type`, `error`, `dyn`
//   - function decls (no `ident` set or `function` set)
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> VariableSpecFromDecl(
    const cel::expr::Decl& d);

// Iterate `t.type_env()` and call
// `Compiler::Builder::DeclareVariable(name, CelType)` on `b` for
// each scalar IdentDecl.  Same Unimplemented contract as
// `VariableSpecFromDecl` (aggregates / functions SKIP).
ABSL_MUST_USE_RESULT absl::Status DeclareVariablesOnBuilder(
    const cel::expr::conformance::test::SimpleTest& t,
    cel::Compiler::Builder& b);

// Iterate `t.bindings()` and for each entry decode the wrapped
// `Value` via `ValueFromProto`, then `Bind` it on `act`.
// `Unimplemented` on:
//   - any binding that is `error:` or `unknown:` (M2 has no
//     harness-side path to materialise an UnknownSet's expr-id list
//     into an AttributePattern, and never decodes errors)
//   - any binding whose nested `Value` returns `Unimplemented` from
//     `ValueFromProto` (aggregate / enum / type_value)
ABSL_MUST_USE_RESULT absl::Status PopulateActivation(
    const cel::expr::conformance::test::SimpleTest& t, cel::Activation& act);

// Iterate `t.type_env()` and append a spec string per decl into
// `out`.  Same Unimplemented contract as `VariableSpecFromDecl`.
ABSL_MUST_USE_RESULT absl::Status PopulateVariableSpecs(
    const cel::expr::conformance::test::SimpleTest& t,
    std::vector<std::string>& out);

}  // namespace celwasm::conformance

#endif  // CELWASM_COMPILER_V2_CONFORMANCE_BINDING_MARSHAL_H_
