// Conformance-harness marshaller from upstream test-fixture proto
// shapes (`cel.expr.ExprValue`, `cel.expr.Decl`, `cel.expr.Type`)
// into the public `cel::` surface (`celwasm::api::Value`, `celwasm::api::Activation`,
// the `name:type` spec strings consumed by
// `Compiler::Builder::DeclareVariable`).
//
// Every entry point returns a graceful `Unimplemented` (caller
// SKIPs as `kBindingUnsupported` / `kTypeEnvUnsupported`) for
// shapes the harness can't yet route, rather than crashing — the
// corpus mixes in-scope and out-of-scope shapes inside the same
// fixture file.
//
// Currently supported (`ValueFromProto` / `DeclareVariablesOnBuilder`):
//   - scalar values (null / bool / int / uint / double / string / bytes)
//   - `enum_value` (decoded as int per langdef §"Enumerated Types")
//   - `object_value` (Any-unpacked via the generated descriptor pool)
//   - `type_value` (decoded to `celwasm::api::Value::Type(name)`)
//   - primitive / message-type / type-of-types `Decl` ident types
//
// Still `Unimplemented` (graceful SKIP — see follow-ups in
// `compiler_v2/conformance/README.md`):
//   - `map_value` / `list_value` bindings (aggregate marshal)
//   - `error` / `unknown` `ExprValue` bindings (no per-test expr-id
//     → AttributeId plumbing)
//   - `wrapper`, `well_known`, `list_type`, `map_type`, `abstract_type`,
//     `type_param`, `error`, `dyn`, `function` decls

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

// Decode a `cel.expr.Value` (scalar kind only) into a `celwasm::api::Value`.
// Returns `Unimplemented` for any aggregate / enum / type kind, and
// `InvalidArgument` if no `kind` is set on the proto.
ABSL_MUST_USE_RESULT absl::StatusOr<celwasm::api::Value> ValueFromProto(
    const cel::expr::Value& v);

// M7: unpack an Any-style `object_value` payload into a heap-owned
// proto.  Caller's descriptor pool (the generated pool) must have
// the type registered — the conformance harness force-links
// TestAllTypes via `ForceLinkFixtureDescriptors` in `runner.cc`.
// Used both by `ValueFromProto`'s kObjectValue arm (binding side)
// and by `runner.cc::CompareMessage` (matcher side).
ABSL_MUST_USE_RESULT absl::StatusOr<std::unique_ptr<google::protobuf::Message>>
UnpackAny(const google::protobuf::Any& any);

// Convert a scalar `cel.expr.Decl` (IdentDecl with a primitive
// `Type`) into a `name:type` spec string consumed by
// `CheckOptions::variable_specs` (the lower-level frontend surface).
// The harness uses this for unit tests; for actually declaring a
// variable on a `celwasm::api::Compiler::Builder` use
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
    celwasm::api::Compiler::Builder& b);

// Iterate `t.bindings()` and for each entry decode the wrapped
// `Value` via `ValueFromProto`, then `Bind` it on `act`.
// `Unimplemented` on:
//   - any binding that is `error:` or `unknown:` (M2 has no
//     harness-side path to materialise an UnknownSet's expr-id list
//     into an AttributePattern, and never decodes errors)
//   - any binding whose nested `Value` returns `Unimplemented` from
//     `ValueFromProto` (aggregate / enum / type_value)
ABSL_MUST_USE_RESULT absl::Status PopulateActivation(
    const cel::expr::conformance::test::SimpleTest& t,
    celwasm::api::Activation& act);

// Iterate `t.type_env()` and append a spec string per decl into
// `out`.  Same Unimplemented contract as `VariableSpecFromDecl`.
ABSL_MUST_USE_RESULT absl::Status PopulateVariableSpecs(
    const cel::expr::conformance::test::SimpleTest& t,
    std::vector<std::string>& out);

}  // namespace celwasm::conformance

#endif  // CELWASM_COMPILER_V2_CONFORMANCE_BINDING_MARSHAL_H_
