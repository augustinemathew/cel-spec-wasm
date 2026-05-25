// Parser for the `cel` CLI's `--var name:Type=value` flag.
//
// The grammar mirrors the type-spec grammar that
// `frontend/parse_and_check.cc::TypeParser` consumes for the
// compiler's variable declarations, so a value bound with
// `--var x:int=42` is type-compatible with the variable the
// compiler declares from the same `--var` flag.  Forms:
//
//   name : SCALAR_TYPE = ATOM
//   name : list< T >  = "[" ATOM ("," ATOM)* "]"
//   name : map<K, V>  = "{" K_ATOM ":" V_ATOM ("," ...)* "}"
//   name : F.Q.N      = @path.{txtpb|json|pb}
//   name : F.Q.N      = txtpb:<inline>
//   name : F.Q.N      = json:<inline>
//
// SCALAR_TYPE ∈ { bool, int, uint, double, string, bytes,
//                 duration, timestamp }.
//
// ATOM literals (per scalar type):
//   bool        true | false
//   int         signed decimal
//   uint        unsigned decimal (optional 'u' suffix tolerated)
//   double      decimal with '.' or scientific notation
//   string      JSON-style double or single-quoted string (\\, \n,
//               \t, \r, \", \', \xHH, \uXXXX, \uXXXX\uYYYY surrogate)
//   bytes       same as string but with a `b` prefix, OR `@path.bin`
//   duration    quoted golang duration string (e.g. "3s", "1h2m3s")
//   timestamp   quoted RFC3339 (e.g. "2024-01-01T00:00:00Z")
//
// All errors are surfaced as `absl::InvalidArgumentError` with a
// pointer at the offending column in the value text.
//
// The parser is type-directed: knowing the declared `CelType`
// removes every ambiguity (e.g. `42` is `Value::Int(42)` under
// `:int=`, `Value::Uint(42)` under `:uint=`, `Value::Double(42.0)`
// under `:double=`).  No silent coercion — `:int=3.14` errors.

#ifndef CELWASM_TOOLS_CEL_VAR_PARSER_H_
#define CELWASM_TOOLS_CEL_VAR_PARSER_H_

#include <string>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "shared/type.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"

namespace celwasm::tools::cel {

// Parsed `--var name:Type=value` flag.  `name`, `type`, and `value`
// are produced together so the compiler can declare the variable
// while the activation can bind it.
//
// `owned_messages` carries any `DynamicMessage` instances created
// while parsing message-typed values (`@u.txtpb`, `json:{...}`,
// etc.).  `Value::Message(...)` references the message by const&;
// the messages must outlive every `Instance::Eval` that observes
// them, so the caller pins the parsed-var vector for that lifetime.
struct ParsedVar {
  std::string name;
  ::celwasm::api::CelType type;
  // `value` is meaningful iff `has_value` is true.  The `name:Type`
  // (declaration-only) form leaves `has_value == false` and `value`
  // default-constructed.  Used by `cel check` / `cel compile` to
  // declare a variable's type without binding it.
  bool has_value = false;
  ::celwasm::api::Value value;
};

// Parse a single `--var name:Type=value` flag.  The descriptor
// pool is consulted only when `type.kind() == kMessage`; scalar /
// list / map vars don't need it.
//
// `factory` constructs message instances for message-typed values;
// must outlive the returned `ParsedVar` (and any Eval that observes
// it) because the `Value` holds the message by const reference via
// the OwnedMessage backing.
ABSL_MUST_USE_RESULT absl::StatusOr<ParsedVar> ParseVarFlag(
    absl::string_view flag, const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory);

// Parse just the type-spec portion (everything between `:` and `=`).
// Exposed for tests and for the `cel check` subcommand, which
// declares variables without binding values.
ABSL_MUST_USE_RESULT absl::StatusOr<::celwasm::api::CelType> ParseTypeSpec(
    absl::string_view spec);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_TOOLS_CEL_VAR_PARSER_H_
