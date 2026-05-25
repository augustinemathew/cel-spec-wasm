#include "testdata/cel_cpp_oracle.h"

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "cel/expr/value.pb.h"
#include "common/ast_proto.h"
#include "common/internal/value_conversion.h"
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

namespace celwasm::testdata {
namespace {

// Force the proto2/proto3 conformance descriptors (incl. GlobalEnum /
// TestAllTypes.NestedEnum) into the generated descriptor pool, so the
// container-qualified names resolve.
const bool kLinked = [] {
  google::protobuf::LinkMessageReflection<
      ::cel::expr::conformance::proto2::TestAllTypes>();
  google::protobuf::LinkMessageReflection<
      ::cel::expr::conformance::proto3::TestAllTypes>();
  return true;
}();

// Build a cel-cpp runtime configured like the modern conformance
// service (`conformance/service.cc::ModernConformanceServiceImpl`):
// the option + enum-registration combination that makes
// container-qualified enum references resolve and proto constructors
// range-check.  Optimizations (constant folding, select optimization)
// are intentionally omitted — they change performance, not semantics.
absl::StatusOr<std::unique_ptr<const cel::Runtime>> BuildRuntime(
    const google::protobuf::DescriptorPool* pool, absl::string_view container) {
  cel::RuntimeOptions opts;
  opts.container = std::string(container);
  opts.enable_qualified_type_identifiers = true;
  opts.enable_timestamp_duration_overflow_errors = true;
  opts.enable_heterogeneous_equality = true;
  opts.enable_empty_wrapper_null_unboxing = true;

  auto builder = cel::CreateStandardRuntimeBuilder(pool, opts);
  if (!builder.ok()) return builder.status();
  if (auto s = cel::EnableReferenceResolver(
          *builder, cel::ReferenceResolverEnabled::kAlways);
      !s.ok()) {
    return s;
  }
  auto& registry = builder->type_registry();
  for (const google::protobuf::EnumDescriptor* enum_desc : {
           ::cel::expr::conformance::proto2::GlobalEnum_descriptor(),
           ::cel::expr::conformance::proto3::GlobalEnum_descriptor(),
           ::cel::expr::conformance::proto2::TestAllTypes::
               NestedEnum_descriptor(),
           ::cel::expr::conformance::proto3::TestAllTypes::
               NestedEnum_descriptor(),
       }) {
    if (auto s = cel::extensions::RegisterProtobufEnum(registry, enum_desc);
        !s.ok()) {
      return s;
    }
  }
  return std::move(*builder).Build();
}

}  // namespace

absl::StatusOr<OracleResult> EvalWithCelCpp(absl::string_view source,
                                            absl::string_view container) {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();

  // --- compile (parse + type-check) ---
  auto compiler_builder = cel::NewCompilerBuilder(pool);
  if (!compiler_builder.ok()) return compiler_builder.status();
  if (auto s = (*compiler_builder)->AddLibrary(cel::StandardCompilerLibrary());
      !s.ok()) {
    return s;
  }
  (*compiler_builder)->GetCheckerBuilder().set_container(std::string(container));
  auto compiler = std::move(**compiler_builder).Build();
  if (!compiler.ok()) return compiler.status();
  auto compiled = (*compiler)->Compile(source);
  if (!compiled.ok()) return compiled.status();
  if (!compiled->IsValid()) {
    return absl::InvalidArgumentError(
        "cel-cpp oracle: compile/type-check failed");
  }

  // The checker's resolved enum references live in the CheckedExpr's
  // reference_map; ProtobufRuntimeAdapter applies them at eval (the
  // bare runtime->CreateProgram(ast) path does not, treating a
  // qualified enum name as an unbound activation variable).
  cel::expr::CheckedExpr checked;
  if (auto s = cel::AstToCheckedExpr(*compiled->GetAst(), &checked); !s.ok()) {
    return s;
  }

  // --- runtime + program ---
  auto runtime = BuildRuntime(pool, container);
  if (!runtime.ok()) return runtime.status();
  auto program = cel::extensions::ProtobufRuntimeAdapter::CreateProgram(
      **runtime, checked);
  if (!program.ok()) return program.status();

  google::protobuf::Arena arena;
  cel::Activation activation;
  auto evaluated = (*program)->Evaluate(&arena, activation);

  OracleResult out;
  // cel-cpp surfaces a CEL eval error two ways: as a non-OK Evaluate()
  // status (e.g. the wrapper int64->int32 overflow) OR as an OK status
  // wrapping an ErrorValue (e.g. the enum SetEnumValue range error).
  // cel-cpp's own conformance harness (conformance/service.cc modern
  // impl) treats BOTH as a CEL error result, so the oracle does too.  A
  // non-OK Status from the EARLIER stages (compile / runtime build) is
  // still a genuine harness failure and propagates as the function's
  // own non-OK return.
  if (!evaluated.ok()) {
    out.is_error = true;
    out.error_message = std::string(evaluated.status().message());
    return out;
  }
  const cel::Value& result = *evaluated;
  if (result.Is<cel::ErrorValue>()) {
    out.is_error = true;
    out.error_message = std::string(result.GetError().NativeValue().message());
    return out;
  }

  // Encode the runtime value to the neutral `cel.expr.Value` proto —
  // the same exchange type the conformance corpus uses.
  auto exported = cel::test::ToExprValue(
      result, (*runtime)->GetDescriptorPool(),
      (*runtime)->GetMessageFactory(), &arena);
  if (!exported.ok()) return exported.status();
  out.value = std::move(*exported);
  return out;
}

}  // namespace celwasm::testdata
