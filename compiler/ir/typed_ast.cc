#include "compiler/ir/typed_ast.h"

#include "absl/types/variant.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "compiler/ir/annotations.h"

namespace celwasm {

namespace {

Repr ReprOfPrimitive(cel::PrimitiveType p) {
  switch (p) {
    case cel::PrimitiveType::kBool:   return Repr::kBool;
    case cel::PrimitiveType::kInt64:  return Repr::kInt;
    case cel::PrimitiveType::kUint64: return Repr::kUint;
    case cel::PrimitiveType::kDouble: return Repr::kDouble;
    case cel::PrimitiveType::kString: return Repr::kString;
    case cel::PrimitiveType::kBytes:  return Repr::kBytes;
    case cel::PrimitiveType::kPrimitiveTypeUnspecified:
      return Repr::kUnknown;
  }
  return Repr::kUnknown;
}

Repr ReprOfWellKnown(cel::WellKnownTypeSpec w) {
  switch (w) {
    case cel::WellKnownTypeSpec::kTimestamp: return Repr::kTimestamp;
    case cel::WellKnownTypeSpec::kDuration:  return Repr::kDuration;
    case cel::WellKnownTypeSpec::kAny:       return Repr::kMessage;
    case cel::WellKnownTypeSpec::kWellKnownTypeUnspecified:
      return Repr::kUnknown;
  }
  return Repr::kUnknown;
}

}  // namespace

Repr ReprOf(const cel::TypeSpec& type) {
  if (type.has_primitive())    return ReprOfPrimitive(type.primitive());
  if (type.has_wrapper())      return ReprOfPrimitive(type.wrapper());
  if (type.has_well_known())   return ReprOfWellKnown(type.well_known());
  if (type.has_null())         return Repr::kNull;
  if (type.has_list_type())    return Repr::kList;
  if (type.has_map_type())     return Repr::kMap;
  if (type.has_message_type()) return Repr::kMessage;
  if (type.has_type())         return Repr::kType;
  // DynTypeSpec, ErrorTypeSpec, AbstractType, FunctionTypeSpec, ParamTypeSpec:
  // unsupported in the static subset.  Leave as kUnknown so the validator can
  // flag them.
  return Repr::kUnknown;
}

void PopulateAnnotations(const cel::Ast& ast, WasmAnnotations& annotations) {
  for (const auto& [expr_id, type] : ast.type_map()) {
    annotations[expr_id].repr = ReprOf(type);
  }
}

}  // namespace celwasm
