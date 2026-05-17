// Throwaway empirical probe — runs four CEL expressions through the
// vendored cel-cpp interpreter to confirm cel-cpp's actual behavior
// for two M7B-polish corner cases:
//
//   1. ts(year-9999) - ts(year-1) — error or duration?
//   2. ts(year-1) - ts(year-9999) — error or duration?
//   3. timestamp("...") == google.protobuf.Timestamp{seconds: 1} —
//      true, false, or error?
//   4. duration("1s") == google.protobuf.Duration{seconds: 1} — same.
//
// Source-read of `third_party/cel-cpp/internal/overflow.cc:295`
// suggested cel-cpp uses int64-nanos for `Time - Time`; this binary
// verifies it.  Same for cross-form equivalence and the spec mapping
// of `google.protobuf.Timestamp` literal type.
//
// Throwaway: do NOT generalise.  Delete once the two M7B polish
// items are verified.  manual-tagged in BUILD.bazel.

#include <iostream>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/standard_library.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "internal/testing_descriptor_pool.h"
#include "cel/expr/checked.pb.h"
#include "runtime/activation.h"
#include "runtime/runtime.h"
#include "runtime/runtime_builder.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"

namespace {

absl::StatusOr<std::string> EvalToString(absl::string_view source) {
  using ::cel::Value;
  // Build a compiler with the standard library.
  CEL_ASSIGN_OR_RETURN(
      auto compiler_builder,
      cel::NewCompilerBuilder(
          cel::internal::GetTestingDescriptorPool()));
  CEL_RETURN_IF_ERROR(
      compiler_builder->AddLibrary(cel::StandardCompilerLibrary()));
  CEL_ASSIGN_OR_RETURN(auto compiler, std::move(*compiler_builder).Build());

  CEL_ASSIGN_OR_RETURN(auto validation_result,
                       compiler->Compile(source, /*description=*/"probe"));
  if (!validation_result.IsValid()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "compile failed: ", validation_result.FormatError()));
  }
  CEL_ASSIGN_OR_RETURN(auto ast, validation_result.ReleaseAst());

  // Build a runtime with the standard library + checked time math
  // (matches the conformance harness; default is unchecked).
  cel::RuntimeOptions opts;
  opts.enable_timestamp_duration_overflow_errors = true;
  CEL_ASSIGN_OR_RETURN(
      auto runtime_builder,
      cel::CreateStandardRuntimeBuilder(
          cel::internal::GetTestingDescriptorPool(), opts));
  CEL_ASSIGN_OR_RETURN(auto runtime, std::move(runtime_builder).Build());

  // Convert AST to checked expr and create program.
  cel::expr::CheckedExpr checked;
  CEL_RETURN_IF_ERROR(cel::AstToCheckedExpr(*ast, &checked));
  CEL_ASSIGN_OR_RETURN(
      auto another_ast,
      cel::CreateAstFromCheckedExpr(checked));
  CEL_ASSIGN_OR_RETURN(auto program,
                       runtime->CreateProgram(std::move(another_ast)));

  cel::Activation act;
  google::protobuf::Arena arena;
  CEL_ASSIGN_OR_RETURN(Value result, program->Evaluate(&arena, act));

  return result.DebugString();
}

void Run(absl::string_view label, absl::string_view expr) {
  std::cout << label << ":\n  expr: " << expr << "\n";
  auto r = EvalToString(expr);
  if (!r.ok()) {
    std::cout << "  STATUS-ERR: " << r.status() << "\n\n";
    return;
  }
  std::cout << "  result: " << *r << "\n\n";
}

}  // namespace

int main() {
  Run("BOUNDARY-1 (ts9999 - ts1)",
      "timestamp('9999-12-31T23:59:59Z') - "
      "timestamp('0001-01-01T00:00:00Z')");
  Run("BOUNDARY-2 (ts1 - ts9999)",
      "timestamp('0001-01-01T00:00:00Z') - "
      "timestamp('9999-12-31T23:59:59Z')");
  Run("CROSS-FORM-1 (ts string == proto literal)",
      "timestamp('1970-01-01T00:00:01Z') == "
      "google.protobuf.Timestamp{seconds: 1}");
  Run("CROSS-FORM-2 (dur string == proto literal)",
      "duration('1s') == google.protobuf.Duration{seconds: 1}");
  Run("CROSS-FORM-3 (proto literal type)",
      "type(google.protobuf.Timestamp{seconds: 1})");
  Run("CROSS-FORM-4 (constructor type)",
      "type(timestamp('1970-01-01T00:00:01Z'))");
  Run("CROSS-FORM-5 (cross type equality)",
      "type(timestamp('1970-01-01T00:00:01Z')) == "
      "type(google.protobuf.Timestamp{seconds: 1})");
  Run("CROSS-FORM-6 (ordering across forms)",
      "timestamp('1970-01-01T00:00:01Z') < "
      "google.protobuf.Timestamp{seconds: 2}");
  return 0;
}
