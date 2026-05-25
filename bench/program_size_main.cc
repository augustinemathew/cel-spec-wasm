// program_size_main — prints expr-module byte sizes across a
// representative expression matrix at optimize_level 0 and 2.  Used
// to populate the "Program size" table in `README.md`
// and `bench/README.md`; not a benchmark per se.
//
// Manual; run with `bazel run -c opt //bench:program_size_main`.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "common/ast_proto.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "common/type.h"
#include "eval/value.h"
#include "compiler/frontend/parse_and_check.h"
#include "runtime/cel_data.h"

namespace {

struct Row {
  const char* label;
  const char* source;
  // (var_name, public CelType, checker spec string) tuples.  Two
  // representations because the public Compiler API takes a CelType
  // while ParseAndCheck (used for the AST-proto column) takes a
  // string spec.  Both name the same variable.
  struct Decl {
    std::string name;
    celwasm::api::CelType type;
    std::string spec;  // e.g. "int", "string", "list<int>"
  };
  std::vector<Decl> decls;
};

// Serialize the AST proto for `source` after running the same
// frontend (parser + type-checker) the compiler uses.  Returns the
// wire size of the resulting `cel.expr.CheckedExpr`.
std::size_t CheckedExprSize(const Row& row) {
  celwasm::CheckOptions opts;
  for (const auto& d : row.decls) {
    opts.variable_specs.push_back(d.name + ":" + d.spec);
  }
  auto typed_ast = celwasm::ParseAndCheck(row.source, opts);
  ABSL_CHECK_OK(typed_ast);
  cel::expr::CheckedExpr pb;
  ABSL_CHECK_OK(cel::AstToCheckedExpr(typed_ast->ast(), &pb));
  return pb.ByteSizeLong();
}

void Emit(const Row& row) {
  celwasm::api::Compiler::Builder b;
  for (const auto& d : row.decls) {
    b.DeclareVariable(d.name, d.type);
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);

  celwasm::api::CompilerOptions o0;
  o0.optimize_level = 0;
  auto p0 = compiler->Compile(row.source, o0);
  ABSL_CHECK_OK(p0);

  celwasm::api::CompilerOptions o2;
  o2.optimize_level = 2;
  auto p2 = compiler->Compile(row.source, o2);
  ABSL_CHECK_OK(p2);

  const std::size_t s_ast = CheckedExprSize(row);
  const std::size_t s0 = p0->wasm_bytes().size();
  const std::size_t s2 = p2->wasm_bytes().size();
  const double ratio_ast_to_opt2 =
      static_cast<double>(s2) / static_cast<double>(s_ast);

  std::printf("%-32s  ast=%5zu B  unopt=%6zu B  opt2=%6zu B  opt2/ast=%4.2fx\n",
              row.label, s_ast, s0, s2, ratio_ast_to_opt2);
}

}  // namespace

int main() {
  // Per-row variable decls.  Helper lambdas keep the call sites
  // readable; tuples are (name, public CelType, checker spec string).
  auto INT = [](std::string name) -> Row::Decl {
    return {std::move(name), celwasm::api::CelType::Int(), "int"};
  };
  auto STR = [](std::string name) -> Row::Decl {
    return {std::move(name), celwasm::api::CelType::String(), "string"};
  };

  std::printf(
      "Expression                         "
      "ast        opt=0          opt=2          ratio\n");
  std::printf(
      "------------------------------------------------------------"
      "----------------------------\n");

  Emit({"literal_int", "42", {}});
  Emit({"literal_string", "\"hello\"", {}});
  Emit({"3-term_arith", "a + b + c", {INT("a"), INT("b"), INT("c")}});
  Emit({"20-term_compare_chain",
        "a < b && b < c && c < d && d < e && e < f && f < g && g < h && "
        "h < i && i < j && j < k && k < l && l < m && m < n && n < o && "
        "o < p && p < q && q < r && r < s && s < t",
        {INT("a"), INT("b"), INT("c"), INT("d"), INT("e"), INT("f"), INT("g"),
         INT("h"), INT("i"), INT("j"), INT("k"), INT("l"), INT("m"), INT("n"),
         INT("o"), INT("p"), INT("q"), INT("r"), INT("s"), INT("t")}});
  Emit({"type_of_eq_int", "type(x) == int", {INT("x")}});
  Emit({"int_from_string", "int(string(123))", {}});
  Emit({"create_list_5", "[1, 2, 3, 4, 5]", {}});
  Emit({"create_map_2", R"({"a": 1, "b": 2})", {}});
  Emit({"map_lookup_arena", R"({"a": 1, "b": 2, "c": 3}["b"])", {}});
  Emit({"string_concat", "\"hello, \" + s", {STR("s")}});
  Emit({"string_contains", "s.contains(\"world\")", {STR("s")}});

  std::printf("\n");
  std::printf("In-memory C++ object sizes (sizeof):\n");
  std::printf("  celwasm::api::Value         %zu B\n",
              sizeof(celwasm::api::Value));
  std::printf("  celwasm::api::Activation    %zu B\n",
              sizeof(celwasm::api::Activation));
  std::printf(
      "  celwasm::api::Program       %zu B (plus the wasm bytes vector data)\n",
      sizeof(celwasm::api::Program));
  std::printf(
      "  celwasm::api::Compiler      %zu B (plus the declared-variable "
      "vector)\n",
      sizeof(celwasm::api::Compiler));
  std::printf("  celwasm::api::CompilerOptions  %zu B\n",
              sizeof(celwasm::api::CompilerOptions));
  std::printf(
      "  CelValue (wire)    %zu B  // arena-resident; pinned by "
      "static_assert\n",
      sizeof(CelValue));
  return 0;
}
