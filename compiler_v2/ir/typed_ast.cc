#include "compiler_v2/ir/typed_ast.h"

#include "absl/types/variant.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/expr.h"
#include "common/type.h"
#include "common/type_kind.h"
#include "compiler_v2/ir/annotations.h"
#include "google/protobuf/descriptor.h"

namespace celwasm {

namespace {

Repr ReprOfPrimitive(cel::PrimitiveType p) {
  switch (p) {
    case cel::PrimitiveType::kBool:
      return Repr::kBool;
    case cel::PrimitiveType::kInt64:
      return Repr::kInt;
    case cel::PrimitiveType::kUint64:
      return Repr::kUint;
    case cel::PrimitiveType::kDouble:
      return Repr::kDouble;
    case cel::PrimitiveType::kString:
      return Repr::kString;
    case cel::PrimitiveType::kBytes:
      return Repr::kBytes;
    case cel::PrimitiveType::kPrimitiveTypeUnspecified:
      return Repr::kUnknown;
  }
  return Repr::kUnknown;
}

Repr ReprOfWellKnown(cel::WellKnownTypeSpec w) {
  switch (w) {
    case cel::WellKnownTypeSpec::kTimestamp:
      return Repr::kTimestamp;
    case cel::WellKnownTypeSpec::kDuration:
      return Repr::kDuration;
    case cel::WellKnownTypeSpec::kAny:
      return Repr::kMessage;
    case cel::WellKnownTypeSpec::kWellKnownTypeUnspecified:
      return Repr::kUnknown;
  }
  return Repr::kUnknown;
}

}  // namespace

Repr ReprOf(const cel::TypeSpec& type) {
  if (type.has_primitive()) return ReprOfPrimitive(type.primitive());
  if (type.has_wrapper()) return ReprOfPrimitive(type.wrapper());
  if (type.has_well_known()) return ReprOfWellKnown(type.well_known());
  if (type.has_null()) return Repr::kNull;
  if (type.has_list_type()) return Repr::kList;
  if (type.has_map_type()) return Repr::kMap;
  if (type.has_message_type()) return Repr::kMessage;
  if (type.has_type()) return Repr::kType;
  if (type.has_abstract_type() &&
      type.abstract_type().name() == "optional_type") {
    return Repr::kOptional;
  }
  // DynTypeSpec, ErrorTypeSpec, other AbstractType names, FunctionTypeSpec,
  // ParamTypeSpec: unsupported in the static subset.  Leave as kUnknown so the
  // validator can flag them.
  return Repr::kUnknown;
}

Repr ReprOf(const cel::Type& type) {
  switch (type.kind()) {
    case cel::TypeKind::kNull:
      return Repr::kNull;
    case cel::TypeKind::kBool:
      return Repr::kBool;
    case cel::TypeKind::kInt:
      return Repr::kInt;
    case cel::TypeKind::kUint:
      return Repr::kUint;
    case cel::TypeKind::kDouble:
      return Repr::kDouble;
    case cel::TypeKind::kString:
      return Repr::kString;
    case cel::TypeKind::kBytes:
      return Repr::kBytes;
    case cel::TypeKind::kDuration:
      return Repr::kDuration;
    case cel::TypeKind::kTimestamp:
      return Repr::kTimestamp;
    case cel::TypeKind::kList:
      return Repr::kList;
    case cel::TypeKind::kMap:
      return Repr::kMap;
    case cel::TypeKind::kStruct:
      return Repr::kMessage;
    case cel::TypeKind::kEnum:
      return Repr::kEnum;
    case cel::TypeKind::kType:
      return Repr::kType;
    // `any` checks as a message at runtime; passing it as an externref
    // lines up with how we'll wire `google.protobuf.Any` in M3 proto work.
    case cel::TypeKind::kAny:
      return Repr::kMessage;
    // `optional<T>` is modelled in cel-cpp's strong type API as an
    // `OptionalType`, a sub-shape of `OpaqueType`.  Detect via the
    // `Is<OptionalType>` predicate (avoids matching other OpaqueType
    // names that may appear in future cel-cpp releases).
    case cel::TypeKind::kOpaque:
      return type.Is<cel::OptionalType>() ? Repr::kOptional : Repr::kUnknown;
    case cel::TypeKind::kBoolWrapper:
      return Repr::kBool;
    case cel::TypeKind::kIntWrapper:
      return Repr::kInt;
    case cel::TypeKind::kUintWrapper:
      return Repr::kUint;
    case cel::TypeKind::kDoubleWrapper:
      return Repr::kDouble;
    case cel::TypeKind::kStringWrapper:
      return Repr::kString;
    case cel::TypeKind::kBytesWrapper:
      return Repr::kBytes;
    // Dyn / error / type-param / function / opaque / unknown: no scalar ABI.
    default:
      return Repr::kUnknown;
  }
}

namespace {

// Walks every `SelectExpr` and writes the resolved proto field number into
// its `NodeAnnotation`.  cel-cpp's `reference_map` only records entries for
// `IdentExpr` / `CallExpr` / `StructExpr`, so the field number is not in the
// checked AST — we re-resolve it here while the descriptor pool is still
// live.  Unresolvable nodes keep `field_number = 0`; codegen treats zero as
// "no info" and falls back to an error path.
class FieldNumberVisitor : public cel::AstVisitorBase {
 public:
  FieldNumberVisitor(const cel::Ast::TypeMap& type_map,
                     const google::protobuf::DescriptorPool* absl_nonnull pool,
                     WasmAnnotations& annotations)
      : type_map_(type_map), pool_(pool), annotations_(annotations) {}

  // `AstVisitorBase` leaves the expr-level hooks abstract; we have no use
  // for them, so define no-op overrides to keep the class instantiable.
  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PreVisitSelect(const cel::Expr& expr,
                      const cel::SelectExpr& select) override {
    auto it = type_map_.find(select.operand().id());
    if (it == type_map_.end()) return;
    const cel::TypeSpec& operand_type = it->second;
    if (!operand_type.has_message_type()) return;
    const std::string& fqn = operand_type.message_type().type();
    const google::protobuf::Descriptor* descriptor =
        pool_->FindMessageTypeByName(fqn);
    if (descriptor == nullptr) return;
    const google::protobuf::FieldDescriptor* field =
        descriptor->FindFieldByName(select.field());
    if (field == nullptr) return;
    annotations_[expr.id()].field_number =
        static_cast<uint32_t>(field->number());
  }

 private:
  const cel::Ast::TypeMap& type_map_;
  const google::protobuf::DescriptorPool* absl_nonnull pool_;
  WasmAnnotations& annotations_;
};

}  // namespace

void PopulateAnnotations(const cel::Ast& ast,
                         const google::protobuf::DescriptorPool* pool,
                         WasmAnnotations& annotations) {
  for (const auto& [expr_id, type] : ast.type_map()) {
    annotations[expr_id].repr = ReprOf(type);
  }
  if (pool != nullptr) {
    FieldNumberVisitor visitor(ast.type_map(), pool, annotations);
    cel::AstTraverse(ast.root_expr(), visitor);
  }
}

}  // namespace celwasm
