#include "testdata/cel_cpp_oracle.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "base/attribute.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "cel/expr/value.pb.h"
#include "common/ast_proto.h"
#include "common/decl.h"
#include "common/internal/value_conversion.h"
#include "common/type.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/optional.h"
#include "compiler/standard_library.h"
#include "extensions/math_ext.h"
#include "extensions/math_ext_decls.h"
#include "extensions/protobuf/enum_adapter.h"
#include "extensions/protobuf/runtime_adapter.h"
#include "extensions/strings.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/generated_message_reflection.h"
#include "runtime/activation.h"
#include "runtime/optional_types.h"
#include "runtime/reference_resolver.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"

namespace celwasm::testdata {
namespace {

// Force the proto2/proto3 conformance descriptors (incl. GlobalEnum /
// TestAllTypes.NestedEnum) into the generated descriptor pool, so the
// container-qualified names resolve.
const bool kLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto2::TestAllTypes>();
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto3::TestAllTypes>();
      return true;
    }();

// Parse a dotted unknown-pattern string into a cel::AttributePattern.
// A bare name (`"xs"`) is a whole-variable pattern; dotted segments
// (`"c.field.leaf"`) become string field qualifiers.
cel::AttributePattern ParseAttributePattern(absl::string_view dotted) {
  std::vector<absl::string_view> parts = absl::StrSplit(dotted, '.');
  std::string variable(parts.front());
  std::vector<cel::AttributeQualifierPattern> quals;
  for (size_t i = 1; i < parts.size(); ++i) {
    quals.push_back(
        cel::AttributeQualifierPattern::OfString(std::string(parts[i])));
  }
  return {std::move(variable), std::move(quals)};
}

// Build a cel-cpp runtime configured like the modern conformance
// service (`conformance/service.cc::ModernConformanceServiceImpl`):
// the option + enum-registration combination that makes
// container-qualified enum references resolve and proto constructors
// range-check.  Optimizations (constant folding, select optimization)
// are intentionally omitted — they change performance, not semantics.
// `enable_unknowns` turns on attribute-unknown processing for the
// partial-eval entry point.
// Register the runtime-side extensions the oracle evaluates, each
// paired with its checker library in `AddCompilerLibraries`:
//   - optional types (`optional.of` / `.hasValue()` / `?field:`),
//   - strings (`split` / `indexOf` / `substring` / `replace` / …),
//   - math (`math.abs` / `math.bitAnd` / `math.sqrt` / …).
// Without these the oracle's runtime rejects extension calls our
// pipeline evaluates — a harness asymmetry, not a real divergence.
absl::Status RegisterRuntimeExtensions(cel::RuntimeBuilder& builder,
                                       const cel::RuntimeOptions& opts) {
  if (auto s = cel::extensions::EnableOptionalTypes(builder); !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterStringsFunctions(
          builder.function_registry(), opts);
      !s.ok()) {
    return s;
  }
  return cel::extensions::RegisterMathExtensionFunctions(
      builder.function_registry(), opts);
}

absl::StatusOr<std::unique_ptr<const cel::Runtime>> BuildRuntime(
    const google::protobuf::DescriptorPool* pool, absl::string_view container,
    bool enable_unknowns) {
  cel::RuntimeOptions opts;
  opts.container = std::string(container);
  opts.enable_qualified_type_identifiers = true;
  opts.enable_timestamp_duration_overflow_errors = true;
  opts.enable_heterogeneous_equality = true;
  opts.enable_empty_wrapper_null_unboxing = true;
  // cel-cpp's default comprehension budget (10k iterations per eval)
  // is a DoS guard, not spec semantics — PBT-generated nested
  // comprehensions over 10-element ranges legitimately exceed it
  // (first hit: e2e/fuzz bool seed=683 depth=6, ~11k iterations).
  // Raise it so the oracle answers the value question on those
  // shapes; the conformance corpus never comes near either limit.
  // (cel-wasm itself has no equivalent eval budget — an m29
  // hardening question.)
  opts.comprehension_max_iterations = 10'000'000;
  if (enable_unknowns) {
    opts.unknown_processing = cel::UnknownProcessingOptions::kAttributeOnly;
  }

  auto builder = cel::CreateStandardRuntimeBuilder(pool, opts);
  if (!builder.ok()) return builder.status();
  if (auto s = cel::EnableReferenceResolver(
          *builder, cel::ReferenceResolverEnabled::kAlways);
      !s.ok()) {
    return s;
  }
  if (auto s = RegisterRuntimeExtensions(*builder, opts); !s.ok()) return s;
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

namespace {

// Compile (parse + type-check) `source`, declaring each `vars` entry so
// it resolves, and return the resolved CheckedExpr.  The checker's
// resolved enum references live in the CheckedExpr's reference_map;
// ProtobufRuntimeAdapter applies them at eval (the bare
// runtime->CreateProgram(ast) path does not, treating a qualified enum
// Register the checker libraries the oracle understands: the
// standard library, optional syntax + decls (`?field:`, the
// optional macros / `optional_type` decls — needed for the
// `optional.ofNonZeroValue` pins), and the strings extension
// (`split`/`indexOf`/… — pairs with `RegisterStringsFunctions` in
// BuildRuntime so the checker accepts what our compiler accepts).
absl::Status AddCompilerLibraries(cel::CompilerBuilder& builder) {
  if (auto s = builder.AddLibrary(cel::StandardCompilerLibrary()); !s.ok()) {
    return s;
  }
  if (auto s = builder.AddLibrary(cel::OptionalCompilerLibrary()); !s.ok()) {
    return s;
  }
  if (auto s = builder.AddLibrary(cel::extensions::StringsCompilerLibrary());
      !s.ok()) {
    return s;
  }
  return builder.AddLibrary(cel::extensions::MathCompilerLibrary());
}

// name as an unbound activation variable).
absl::StatusOr<cel::expr::CheckedExpr> CompileSource(
    const google::protobuf::DescriptorPool* pool, absl::string_view source,
    absl::string_view container, absl::Span<const OracleVar> vars) {
  auto compiler_builder = cel::NewCompilerBuilder(pool);
  if (!compiler_builder.ok()) return compiler_builder.status();
  if (auto s = AddCompilerLibraries(**compiler_builder); !s.ok()) return s;
  auto& checker = (*compiler_builder)->GetCheckerBuilder();
  checker.set_container(std::string(container));
  // Declare every free variable as `dyn`: the static type is irrelevant
  // to the unknown-propagation semantics under test, and `dyn` lets the
  // checker accept any usage without a per-case type spec.
  for (const OracleVar& v : vars) {
    if (auto s =
            checker.AddVariable(cel::MakeVariableDecl(v.name, cel::DynType()));
        !s.ok()) {
      return s;
    }
  }
  auto compiler = std::move(**compiler_builder).Build();
  if (!compiler.ok()) return compiler.status();
  auto compiled = (*compiler)->Compile(source);
  if (!compiled.ok()) return compiled.status();
  if (!compiled->IsValid()) {
    return absl::InvalidArgumentError(
        "cel-cpp oracle: compile/type-check failed");
  }
  cel::expr::CheckedExpr checked;
  if (auto s = cel::AstToCheckedExpr(*compiled->GetAst(), &checked); !s.ok()) {
    return s;
  }
  return checked;
}

// Bind every `vars` entry that carries a value, and install the
// unknown-attribute patterns.
absl::Status PopulateActivation(cel::Activation* activation,
                                absl::Span<const OracleVar> vars,
                                absl::Span<const std::string> unknown_patterns,
                                const cel::Runtime& runtime,
                                google::protobuf::Arena* arena) {
  for (const OracleVar& v : vars) {
    if (!v.value.has_value()) continue;  // unbound — legal under partial eval
    auto value = cel::test::FromExprValue(*v.value, runtime.GetDescriptorPool(),
                                          runtime.GetMessageFactory(), arena);
    if (!value.ok()) return value.status();
    activation->InsertOrAssignValue(v.name, *std::move(value));
  }
  std::vector<cel::AttributePattern> patterns;
  patterns.reserve(unknown_patterns.size());
  for (const std::string& dotted : unknown_patterns) {
    patterns.push_back(ParseAttributePattern(dotted));
  }
  activation->SetUnknownPatterns(std::move(patterns));
  return absl::OkStatus();
}

// Classify a successful cel-cpp evaluation into an OracleResult:
// UnknownValue / ErrorValue are first-class outcomes; everything else
// is encoded to the neutral `cel.expr.Value` exchange proto.
absl::StatusOr<OracleResult> ClassifyValue(const cel::Value& result,
                                           const cel::Runtime& runtime,
                                           google::protobuf::Arena* arena) {
  OracleResult out;
  if (result.IsUnknown()) {
    out.is_unknown = true;
    // Export every attribute identity the unknown carries (cel-cpp
    // merges unknown operands into ONE set; see
    // `AttributeUtility::MergeUnknowns`).  AsString fails only on
    // non-ident-rooted attributes, which partial-eval cannot produce —
    // treat that as a harness failure.
    for (const cel::Attribute& attr : result.GetUnknown().attribute_set()) {
      auto s = attr.AsString();
      if (!s.ok()) return s.status();
      out.unknown_attributes.push_back(*std::move(s));
    }
    return out;
  }
  if (result.Is<cel::ErrorValue>()) {
    out.is_error = true;
    out.error_message = std::string(result.GetError().ToStatus().message());
    return out;
  }
  auto exported = cel::test::ToExprValue(result, runtime.GetDescriptorPool(),
                                         runtime.GetMessageFactory(), arena);
  if (!exported.ok()) return exported.status();
  out.value = std::move(*exported);
  return out;
}

// Shared core for both public entry points.
absl::StatusOr<OracleResult> EvalCore(
    absl::string_view source, absl::string_view container,
    absl::Span<const OracleVar> vars,
    absl::Span<const std::string> unknown_patterns, bool enable_unknowns) {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  google::protobuf::Arena arena;

  auto checked = CompileSource(pool, source, container, vars);
  if (!checked.ok()) return checked.status();

  auto runtime = BuildRuntime(pool, container, enable_unknowns);
  if (!runtime.ok()) return runtime.status();
  auto program = cel::extensions::ProtobufRuntimeAdapter::CreateProgram(
      **runtime, *checked);
  if (!program.ok()) return program.status();

  cel::Activation activation;
  if (auto s = PopulateActivation(&activation, vars, unknown_patterns,
                                  **runtime, &arena);
      !s.ok()) {
    return s;
  }

  auto evaluated = (*program)->Evaluate(&arena, activation);
  // cel-cpp surfaces a CEL eval error two ways: a non-OK Evaluate()
  // status (e.g. the wrapper int64->int32 overflow) OR an OK status
  // wrapping an ErrorValue.  cel-cpp's own conformance harness treats
  // BOTH as a CEL error result, so the oracle does too.  A non-OK from
  // the EARLIER stages is a genuine harness failure and propagates.
  if (!evaluated.ok()) {
    OracleResult out;
    out.is_error = true;
    out.error_message = std::string(evaluated.status().message());
    return out;
  }
  return ClassifyValue(*evaluated, **runtime, &arena);
}

}  // namespace

absl::StatusOr<OracleResult> EvalWithCelCpp(absl::string_view source,
                                            absl::string_view container) {
  return EvalCore(source, container, /*vars=*/{}, /*unknown_patterns=*/{},
                  /*enable_unknowns=*/false);
}

absl::StatusOr<OracleResult> PartialEvalWithCelCpp(
    absl::string_view source, absl::string_view container,
    absl::Span<const OracleVar> vars,
    absl::Span<const std::string> unknown_patterns) {
  return EvalCore(source, container, vars, unknown_patterns,
                  /*enable_unknowns=*/true);
}

}  // namespace celwasm::testdata
