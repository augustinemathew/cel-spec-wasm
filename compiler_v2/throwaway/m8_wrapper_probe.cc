// Throwaway empirical probe for M8 (wrapper types).  Runs an
// exhaustive matrix of CEL expressions through the vendored cel-cpp
// interpreter to pin down its behaviour for the 9 WKT wrappers
// (BoolValue, Int32Value, Int64Value, UInt32Value, UInt64Value,
// FloatValue, DoubleValue, StringValue, BytesValue) across:
//
//   GROUP 1 — Literal wrapper equality (does cel-cpp's `==` peel
//     wrapper literals?  And what about `== null`?)
//   GROUP 2 — Field reads on wrapper-typed message fields (auto-peel?
//     unset vs set-to-default?  proto2/proto3 agreement?  has()?)
//   GROUP 3 — Construction-side auto-wrap (Foo{w: scalar})
//   GROUP 4 — Any-contained wrapper (Any-unwrap chained to peel)
//   GROUP 5 — Edge / boundary values per wrapper kind
//   GROUP 6 — Cross-form symmetry (literal vs field-read equality)
//   GROUP 7 — Wrapper arithmetic / ordering / size / call coercion
//     (added 2026-05-17 to answer "does cel-cpp peel wrappers in
//     arithmetic / ordering / size / function-call contexts?").
//
// Output: one `[ROW] expr → result` line per row.  Read against
// `langdef.md` §wrapper-types to drive the M8 plan + test matrix.
//
// Throwaway: do NOT generalise.  Delete once M8 closes.
// manual-tagged in BUILD.bazel.
//
// Run: bazel run -c opt //compiler_v2/throwaway:m8_wrapper_probe

#include <iostream>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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

  cel::RuntimeOptions opts;
  opts.enable_timestamp_duration_overflow_errors = true;
  CEL_ASSIGN_OR_RETURN(
      auto runtime_builder,
      cel::CreateStandardRuntimeBuilder(
          cel::internal::GetTestingDescriptorPool(), opts));
  CEL_ASSIGN_OR_RETURN(auto runtime, std::move(runtime_builder).Build());

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
  std::cout << "[" << label << "] " << expr << "\n";
  auto r = EvalToString(expr);
  if (!r.ok()) {
    std::cout << "   -> STATUS-ERR: " << r.status() << "\n";
    return;
  }
  std::cout << "   -> " << *r << "\n";
}

constexpr absl::string_view kT3 = "cel.expr.conformance.proto3.TestAllTypes";
constexpr absl::string_view kT2 = "cel.expr.conformance.proto2.TestAllTypes";

void Group1_LiteralEq() {
  std::cout << "\n=== GROUP 1: Literal wrapper equality ===\n";
  // Int32Value across the key questions.
  Run("A1", "google.protobuf.Int32Value{value: 123} == 123");
  Run("A2", "google.protobuf.Int32Value{value: 0}   == 0");
  Run("A3", "google.protobuf.Int32Value{}            == 0");
  Run("A4", "google.protobuf.Int32Value{}            == null");
  Run("A5", "google.protobuf.Int32Value{value: 0}    == null");
  Run("A6", "google.protobuf.Int32Value{value: 1}    != null");
  Run("A7",
      "google.protobuf.Int32Value{value:1} == "
      "google.protobuf.Int32Value{value:1}");
  Run("A8", "google.protobuf.Int32Value{value:1} == \"1\"");

  // Repeat the most diagnostic rows (set-to-zero vs empty, == null) for
  // every other wrapper kind.  These are the rows that decide whether
  // M8 should treat "set-to-default-value" as "still a message".
  Run("A2-bool",   "google.protobuf.BoolValue{value: false} == false");
  Run("A3-bool",   "google.protobuf.BoolValue{}             == false");
  Run("A5-bool",   "google.protobuf.BoolValue{value: false} == null");
  Run("A1-i64",    "google.protobuf.Int64Value{value: 9} == 9");
  Run("A5-i64",    "google.protobuf.Int64Value{value: 0} == null");
  Run("A1-u32",    "google.protobuf.UInt32Value{value: 7u} == 7u");
  Run("A5-u32",    "google.protobuf.UInt32Value{value: 0u} == null");
  Run("A1-u64",    "google.protobuf.UInt64Value{value: 7u} == 7u");
  Run("A5-u64",    "google.protobuf.UInt64Value{value: 0u} == null");
  Run("A1-float",  "google.protobuf.FloatValue{value: 1.5}  == 1.5");
  Run("A3-float",  "google.protobuf.FloatValue{}            == 0.0");
  Run("A5-float",  "google.protobuf.FloatValue{value: 0.0}  == null");
  Run("A1-double", "google.protobuf.DoubleValue{value: 1.5} == 1.5");
  Run("A5-double", "google.protobuf.DoubleValue{value: 0.0} == null");
  Run("A1-str",    "google.protobuf.StringValue{value: \"x\"} == \"x\"");
  Run("A3-str",    "google.protobuf.StringValue{}             == \"\"");
  Run("A5-str",    "google.protobuf.StringValue{value: \"\"}  == null");
  Run("A1-bytes",  "google.protobuf.BytesValue{value: b\"x\"} == b\"x\"");
  Run("A3-bytes",  "google.protobuf.BytesValue{}             == b\"\"");
  Run("A5-bytes",  "google.protobuf.BytesValue{value: b\"\"} == null");
}

void Group2_FieldReads() {
  std::cout << "\n=== GROUP 2: Wrapper field reads ===\n";
  std::string p = std::string(kT3);
  Run("B1", p + "{single_int32_wrapper: 1}.single_int32_wrapper");
  Run("B2", p + "{single_int32_wrapper: 1}.single_int32_wrapper == 1");
  Run("B3", p + "{}.single_int32_wrapper == null");
  Run("B4", p + "{}.single_int32_wrapper == 0");
  Run("B5", p + "{single_int32_wrapper: 0}.single_int32_wrapper == null");
  // B6: proto2 variant.
  std::string p2 = std::string(kT2);
  Run("B6-proto2",
      p2 + "{single_int32_wrapper: 0}.single_int32_wrapper == null");
  Run("B7", "has(" + p + "{single_int32_wrapper: 0}.single_int32_wrapper)");
  Run("B8", "has(" + p + "{}.single_int32_wrapper)");

  // Per-wrapper field-read peel sanity.
  Run("B-bool",   p + "{single_bool_wrapper: true}.single_bool_wrapper");
  Run("B-i64",    p + "{single_int64_wrapper: 9}.single_int64_wrapper");
  Run("B-u32",    p + "{single_uint32_wrapper: 7u}.single_uint32_wrapper");
  Run("B-u64",    p + "{single_uint64_wrapper: 7u}.single_uint64_wrapper");
  Run("B-float",  p + "{single_float_wrapper: 1.5}.single_float_wrapper");
  Run("B-double", p + "{single_double_wrapper: 1.5}.single_double_wrapper");
  Run("B-str",    p + "{single_string_wrapper: \"x\"}.single_string_wrapper");
  Run("B-bytes",  p + "{single_bytes_wrapper: b\"x\"}.single_bytes_wrapper");

  // Read field type (does cel-cpp report it as int or message?)
  Run("B-type-set",
      "type(" + p + "{single_int32_wrapper: 1}.single_int32_wrapper)");
  Run("B-type-unset",
      "type(" + p + "{}.single_int32_wrapper)");
}

void Group3_ConstructionAutowrap() {
  std::cout << "\n=== GROUP 3: Construction-side autowrap ===\n";
  std::string p = std::string(kT3);
  Run("C1", p + "{single_int32_wrapper: 5}.single_int32_wrapper == 5");
  Run("C2", p + "{single_int32_wrapper: null}.single_int32_wrapper == null");
  Run("C3",
      p + "{single_int32_wrapper: google.protobuf.Int32Value{value:5}}"
          ".single_int32_wrapper == 5");
  Run("C4", p + "{single_int32_wrapper: \"5\"}.single_int32_wrapper == 5");
  Run("C5", p + "{single_uint32_wrapper: -1}.single_uint32_wrapper == 0u");
  Run("C6", p + "{single_bool_wrapper: true}.single_bool_wrapper == true");

  // Auto-wrap per other kinds.
  Run("C-i64",   p + "{single_int64_wrapper: 9}.single_int64_wrapper == 9");
  Run("C-u32",   p + "{single_uint32_wrapper: 7u}.single_uint32_wrapper == 7u");
  Run("C-u64",   p + "{single_uint64_wrapper: 7u}.single_uint64_wrapper == 7u");
  Run("C-float", p + "{single_float_wrapper: 1.5}.single_float_wrapper == 1.5");
  Run("C-double",
      p + "{single_double_wrapper: 1.5}.single_double_wrapper == 1.5");
  Run("C-str",
      p + "{single_string_wrapper: \"x\"}.single_string_wrapper == \"x\"");
  Run("C-bytes",
      p + "{single_bytes_wrapper: b\"x\"}.single_bytes_wrapper == b\"x\"");

  // null-into-wrapper-field per kind.
  Run("C-null-bool",   p + "{single_bool_wrapper: null}.single_bool_wrapper == null");
  Run("C-null-str",    p + "{single_string_wrapper: null}.single_string_wrapper == null");
  Run("C-null-bytes",  p + "{single_bytes_wrapper: null}.single_bytes_wrapper == null");
}

void Group4_AnyContainedWrapper() {
  std::cout << "\n=== GROUP 4: Any-contained wrapper (chain peel) ===\n";
  std::string p = std::string(kT3);
  // Per-wrapper: pack into Any, read field, compare to scalar.
  Run("D-bool",
      p + "{single_any: google.protobuf.BoolValue{value: true}}"
          ".single_any == true");
  Run("D-i32",
      p + "{single_any: google.protobuf.Int32Value{value: 1}}"
          ".single_any == 1");
  Run("D-i64",
      p + "{single_any: google.protobuf.Int64Value{value: 9}}"
          ".single_any == 9");
  Run("D-u32",
      p + "{single_any: google.protobuf.UInt32Value{value: 7u}}"
          ".single_any == 7u");
  Run("D-u64",
      p + "{single_any: google.protobuf.UInt64Value{value: 7u}}"
          ".single_any == 7u");
  Run("D-float",
      p + "{single_any: google.protobuf.FloatValue{value: 1.5}}"
          ".single_any == 1.5");
  Run("D-double",
      p + "{single_any: google.protobuf.DoubleValue{value: 1.5}}"
          ".single_any == 1.5");
  Run("D-str",
      p + "{single_any: google.protobuf.StringValue{value: \"x\"}}"
          ".single_any == \"x\"");
  Run("D-bytes",
      p + "{single_any: google.protobuf.BytesValue{value: b\"x\"}}"
          ".single_any == b\"x\"");

  // type() of an Any-contained wrapper post-unwrap.
  Run("D-type-i32",
      "type(" + p +
          "{single_any: google.protobuf.Int32Value{value: 1}}.single_any)");
}

void Group5_BoundaryValues() {
  std::cout << "\n=== GROUP 5: Boundary values per wrapper ===\n";
  // BoolValue
  Run("E-bool-true",  "google.protobuf.BoolValue{value: true}  == true");
  Run("E-bool-false", "google.protobuf.BoolValue{value: false} == false");
  // Int32Value boundaries.
  Run("E-i32-min", "google.protobuf.Int32Value{value: -2147483648} == -2147483648");
  Run("E-i32-max", "google.protobuf.Int32Value{value: 2147483647}  == 2147483647");
  Run("E-i32-neg1","google.protobuf.Int32Value{value: -1}          == -1");
  // Int64Value boundaries.
  Run("E-i64-min", "google.protobuf.Int64Value{value: -9223372036854775808} "
                   "== -9223372036854775808");
  Run("E-i64-max", "google.protobuf.Int64Value{value: 9223372036854775807}  "
                   "== 9223372036854775807");
  // UInt32Value / UInt64Value boundaries.
  Run("E-u32-max", "google.protobuf.UInt32Value{value: 4294967295u} == 4294967295u");
  Run("E-u64-max", "google.protobuf.UInt64Value{value: 18446744073709551615u} "
                   "== 18446744073709551615u");
  // DoubleValue / FloatValue: signed zero, infinity, NaN.
  Run("E-d-zero",  "google.protobuf.DoubleValue{value:  0.0} ==  0.0");
  Run("E-d-nzero", "google.protobuf.DoubleValue{value: -0.0} == -0.0");
  Run("E-d-pinf",  "google.protobuf.DoubleValue{value: 1.0/0.0} == 1.0/0.0");
  Run("E-d-ninf",  "google.protobuf.DoubleValue{value: -1.0/0.0} == -1.0/0.0");
  Run("E-d-nan",   "google.protobuf.DoubleValue{value: 0.0/0.0} == 0.0/0.0");
  // StringValue: empty, ASCII, embedded NUL, multi-byte UTF-8.
  Run("E-s-empty", "google.protobuf.StringValue{value: \"\"}  == \"\"");
  Run("E-s-nul",   "google.protobuf.StringValue{value: \"a\\x00b\"} == \"a\\x00b\"");
  Run("E-s-utf8",  "google.protobuf.StringValue{value: \"\\u00e9\"} == \"\\u00e9\"");
  // BytesValue: empty, NUL-only.
  Run("E-b-empty", "google.protobuf.BytesValue{value: b\"\"}    == b\"\"");
  Run("E-b-nul",   "google.protobuf.BytesValue{value: b\"\\x00\"} == b\"\\x00\"");
}

void Group6_CrossForm() {
  std::cout << "\n=== GROUP 6: Cross-form symmetry ===\n";
  std::string p = std::string(kT3);
  Run("F1",
      "google.protobuf.Int32Value{value: 5} == " + p +
          "{single_int32_wrapper: 5}.single_int32_wrapper");
  Run("F1-rev",
      p + "{single_int32_wrapper: 5}.single_int32_wrapper == "
          "google.protobuf.Int32Value{value: 5}");
  // Field == literal wrapper of zero, with field unset.
  Run("F-unset-vs-litzero",
      p + "{}.single_int32_wrapper == google.protobuf.Int32Value{value: 0}");
  // Field set to zero == literal wrapper of zero.
  Run("F-setzero-vs-litzero",
      p + "{single_int32_wrapper: 0}.single_int32_wrapper == "
          "google.protobuf.Int32Value{value: 0}");
}

void Group7_ArithmeticOrderingSizeCalls() {
  std::cout << "\n=== GROUP 7: Wrapper arithmetic / ordering / size / "
               "calls ===\n";
  std::string p = std::string(kT3);

  // --- Arithmetic (+, -, *, /, %) -------------------------------
  Run("G-add-i32-lit-scalar",
      "google.protobuf.Int32Value{value:1} + 2");
  Run("G-sub-i32-wrap-wrap",
      "google.protobuf.Int32Value{value:5} - "
      "google.protobuf.Int32Value{value:2}");
  Run("G-mul-i64-lit-scalar",
      "google.protobuf.Int64Value{value:10} * 3");
  Run("G-add-double-lit-scalar",
      "google.protobuf.DoubleValue{value:1.5} + 2.5");
  Run("G-sub-u32-lit-scalar",
      "google.protobuf.UInt32Value{value:10u} - 3u");
  Run("G-add-bool-int",
      "google.protobuf.BoolValue{value:true} + 1");
  Run("G-concat-str-lit-scalar",
      "google.protobuf.StringValue{value:\"x\"} + \"y\"");
  Run("G-concat-bytes-lit-scalar",
      "google.protobuf.BytesValue{value:b\"x\"} + b\"y\"");
  Run("G-empty-wrapper-add",
      "google.protobuf.Int32Value{} + 2");
  Run("G-cross-form-add",
      "google.protobuf.Int32Value{value:1} + "
      "google.protobuf.Int32Value{value:2}");
  Run("G-div-i32-scalar",
      "google.protobuf.Int32Value{value:10} / 2");
  Run("G-mod-i64-scalar",
      "google.protobuf.Int64Value{value:7} % 3");
  Run("G-add-float-scalar",
      "google.protobuf.FloatValue{value:1.5} + 2.5");

  // --- Ordering (<, <=, >, >=) ---------------------------------
  Run("G-lt-i32-lit-scalar",
      "google.protobuf.Int32Value{value:1} < 2");
  Run("G-gt-i32-wrap-wrap",
      "google.protobuf.Int32Value{value:5} > "
      "google.protobuf.Int32Value{value:3}");
  Run("G-le-double-lit-scalar",
      "google.protobuf.DoubleValue{value:1.5} <= 1.5");
  Run("G-lt-str-lit-scalar",
      "google.protobuf.StringValue{value:\"abc\"} < \"abd\"");
  Run("G-ge-u64-lit-scalar",
      "google.protobuf.UInt64Value{value:10u} >= 5u");

  // --- size() / membership -------------------------------------
  Run("G-size-str-wrap",
      "size(google.protobuf.StringValue{value:\"hello\"})");
  Run("G-size-bytes-wrap",
      "size(google.protobuf.BytesValue{value:b\"foo\"})");
  Run("G-in-list-wrap-elem",
      "google.protobuf.Int32Value{value:1} in [1, 2, 3]");

  // --- Function calls ------------------------------------------
  Run("G-string-of-i32-wrap",
      "string(google.protobuf.Int32Value{value:42})");
  Run("G-int-of-double-wrap",
      "int(google.protobuf.DoubleValue{value:42.0})");
  Run("G-double-of-i32-wrap",
      "double(google.protobuf.Int32Value{value:7})");
  // toString-on-wrapper (does this method exist?)
  Run("G-i32-toString",
      "google.protobuf.Int32Value{value:1}.toString()");

  // --- Negative / disallowed ----------------------------------
  Run("G-add-wrap-string",
      "google.protobuf.Int32Value{value:1} + \"2\"");
  Run("G-add-i32-double",
      "google.protobuf.Int32Value{value:1} + "
      "google.protobuf.DoubleValue{value:1.5}");
  Run("G-add-i32-i64",
      "google.protobuf.Int32Value{value:1} + "
      "google.protobuf.Int64Value{value:2}");
  Run("G-add-i32-u32",
      "google.protobuf.Int32Value{value:1} + "
      "google.protobuf.UInt32Value{value:2u}");

  // --- Field-read arithmetic (already in our M8.B scope) -------
  Run("G-fieldread-i32-add",
      p + "{single_int32_wrapper: 5}.single_int32_wrapper + 1");
  Run("G-fieldread-i32-unset-add",
      p + "{}.single_int32_wrapper + 1");
  Run("G-fieldread-i64-add",
      p + "{single_int64_wrapper: 9}.single_int64_wrapper + 1");
  Run("G-fieldread-u32-add",
      p + "{single_uint32_wrapper: 7u}.single_uint32_wrapper + 1u");
  Run("G-fieldread-double-add",
      p + "{single_double_wrapper: 1.5}.single_double_wrapper + 2.5");
  Run("G-fieldread-str-add",
      p + "{single_string_wrapper: \"x\"}.single_string_wrapper + \"y\"");
  Run("G-fieldread-bool-and",
      p + "{single_bool_wrapper: true}.single_bool_wrapper && true");
  Run("G-fieldread-i32-lt",
      p + "{single_int32_wrapper: 5}.single_int32_wrapper < 10");
  Run("G-fieldread-size-str",
      "size(" + p +
          "{single_string_wrapper: \"hello\"}.single_string_wrapper)");
}

}  // namespace

int main() {
  Group1_LiteralEq();
  Group2_FieldReads();
  Group3_ConstructionAutowrap();
  Group4_AnyContainedWrapper();
  Group5_BoundaryValues();
  Group6_CrossForm();
  Group7_ArithmeticOrderingSizeCalls();
  std::cout << "\n[done]\n";
  return 0;
}
