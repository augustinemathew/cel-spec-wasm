// Probe: does cel-cpp itself implement "strong" enum semantics, or
// does it decay enums to int?  This links cel-cpp's REAL compiler +
// runtime (the same libraries the production checker reuses) and runs
// the exact expressions from the `strong_proto2` section of
// `tests/simple/testdata/enums.textproto` end-to-end.
//
// The conformance fixture's `strong_proto2` rows expect:
//   - `GlobalEnum.GAZ`        -> enum_value{type:"...GlobalEnum", value:2}
//   - `type(GlobalEnum.GOO)`  -> type_value:"...GlobalEnum"
//   - `TestAllTypes.NestedEnum(2)` -> enum_value{...NestedEnum, value:2}
//
// If cel-cpp returns plain `int` / the `int` type / a compile error for
// these, then cel-cpp does NOT pass its own conformance corpus's strong
// rows — which is the evidence behind M20 descoping strong enums (see
// `doc/implementation-plan/rewrite/m20-enum-field-range.md` §6).
//
// Disposable: delete at M20 closeout (per CLAUDE.md probe discipline).

#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "common/ast_proto.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/standard_library.h"
#include "extensions/protobuf/enum_adapter.h"
#include "extensions/protobuf/runtime_adapter.h"
#include "runtime/activation.h"
#include "runtime/reference_resolver.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/generated_message_reflection.h"
#include "gtest/gtest.h"

namespace {

// Force the proto2 conformance descriptors (incl. GlobalEnum /
// TestAllTypes.NestedEnum) into the generated descriptor pool.
const bool kLinked = [] {
  google::protobuf::LinkMessageReflection<
      ::cel::expr::conformance::proto2::TestAllTypes>();
  return true;
}();

// Parse + type-check + evaluate `source` through cel-cpp's own
// pipeline, with the proto2 conformance container, and return the
// runtime Value (cel-cpp's `cel::Value`).
absl::StatusOr<cel::Value> EvalThroughCelCpp(absl::string_view source,
                                             google::protobuf::Arena* arena) {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();

  // --- compile (parse + check) ---
  auto builder = cel::NewCompilerBuilder(pool);
  if (!builder.ok()) return builder.status();
  if (auto s = (*builder)->AddLibrary(cel::StandardCompilerLibrary());
      !s.ok()) {
    return s;
  }
  (*builder)->GetCheckerBuilder().set_container(
      "cel.expr.conformance.proto2");
  auto compiler = std::move(**builder).Build();
  if (!compiler.ok()) return compiler.status();
  auto result = (*compiler)->Compile(source);
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    return absl::InvalidArgumentError("compile/type-check failed");
  }

  // --- checked AST -> CheckedExpr proto -> runtime program.  The
  // CheckedExpr's reference_map carries the checker's resolved enum
  // references, which the ProtobufRuntimeAdapter applies at eval (the
  // bare runtime->CreateProgram(ast) path does not, and treats the
  // qualified enum name as an unbound activation variable).
  cel::expr::CheckedExpr checked;
  if (auto s = cel::AstToCheckedExpr(*result->GetAst(), &checked); !s.ok()) {
    return s;
  }

  // Mirror cel-cpp's own conformance harness setup
  // (conformance/service.cc): the container + qualified-type
  // identifiers + a reference resolver + registered proto enums are
  // what let the runtime resolve `GlobalEnum.GAZ` to its int value.
  cel::RuntimeOptions opts;
  opts.container = "cel.expr.conformance.proto2";
  opts.enable_qualified_type_identifiers = true;
  auto rt_builder = cel::CreateStandardRuntimeBuilder(pool, opts);
  if (!rt_builder.ok()) return rt_builder.status();
  if (auto s = cel::EnableReferenceResolver(
          *rt_builder, cel::ReferenceResolverEnabled::kAlways);
      !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterProtobufEnum(
          rt_builder->type_registry(),
          ::cel::expr::conformance::proto2::GlobalEnum_descriptor());
      !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterProtobufEnum(
          rt_builder->type_registry(),
          ::cel::expr::conformance::proto2::TestAllTypes::
              NestedEnum_descriptor());
      !s.ok()) {
    return s;
  }
  auto runtime = std::move(*rt_builder).Build();
  if (!runtime.ok()) return runtime.status();
  auto program =
      cel::extensions::ProtobufRuntimeAdapter::CreateProgram(**runtime, checked);
  if (!program.ok()) return program.status();

  cel::Activation activation;
  return (*program)->Evaluate(arena, activation);
}

// Structural evidence, independent of any eval: cel-cpp's value-kind
// enum (`common/value_kind.h`) has NO `kEnum` member — only the
// type-system kind (`common/kind.h:57`) does.  So a cel-cpp runtime
// Value can hold an enum *type*, but never an enum *value*; an enum
// at runtime is always an `int`.  That is the root reason the
// `strong_*` rows can't pass: there is no value to compare against
// the fixture's `enum_value` matcher.

// `GlobalEnum.GAZ` — fixture wants enum_value{...GlobalEnum, 2}.
TEST(CelCppStrongEnum, EnumLiteralDecaysToInt) {
  google::protobuf::Arena arena;
  auto v = EvalThroughCelCpp("GlobalEnum.GAZ", &arena);
  ASSERT_TRUE(v.ok()) << v.status();
  // EVIDENCE: cel-cpp yields a plain int (kInt), not an enum value.
  EXPECT_EQ(v->kind(), cel::ValueKind::kInt)
      << "cel-cpp DebugString=" << v->DebugString();
}

// `type(GlobalEnum.GOO)` — fixture wants type_value:"...GlobalEnum".
TEST(CelCppStrongEnum, TypeOfEnumIsIntNotEnumType) {
  google::protobuf::Arena arena;
  auto v = EvalThroughCelCpp("type(GlobalEnum.GOO)", &arena);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), cel::ValueKind::kType) << v->DebugString();
  // EVIDENCE: the wrapped type is `int`, NOT
  // `cel.expr.conformance.proto2.GlobalEnum`.
  EXPECT_EQ(v->GetType().name(), "int")
      << "cel-cpp type() of an enum yielded: " << v->GetType().name()
      << " (fixture wants cel.expr.conformance.proto2.GlobalEnum)";
}

// `TestAllTypes.NestedEnum(2)` — fixture wants enum_value{...NestedEnum,2}.
// Either cel-cpp rejects the enum constructor at compile time, or it
// evaluates to a plain int — either way it does not produce an enum value.
TEST(CelCppStrongEnum, EnumConstructorNotStronglyTyped) {
  google::protobuf::Arena arena;
  auto v = EvalThroughCelCpp("TestAllTypes.NestedEnum(2)", &arena);
  if (!v.ok()) {
    GTEST_SUCCEED() << "cel-cpp rejects the enum constructor at "
                       "compile/eval: "
                    << v.status();
    return;
  }
  // EVIDENCE: if it compiles at all, the result is a plain int.
  EXPECT_EQ(v->kind(), cel::ValueKind::kInt)
      << "cel-cpp enum-constructor result: " << v->DebugString();
}

}  // namespace
