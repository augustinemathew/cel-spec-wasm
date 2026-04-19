// celwasmc: CEL → WebAssembly AOT compiler.
//
// M0: parse a CEL expression via cel-cpp's parser and print the resulting
// cel::expr::ParsedExpr textproto.
//
// M1: add --check to also run the type checker.  With --schema <path.pb.bin>
// and --var name:Type, the CLI injects a descriptor pool + variable decls into
// the checker, then prints the CheckedExpr plus a summary of the static-subset
// ABI annotations we compute for codegen.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/syntax.pb.h"
#include "common/ast_proto.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/static_subset.h"
#include "compiler/ir/typed_ast.h"
#include "parser/parser.h"

ABSL_FLAG(std::string, e, "", "CEL expression to compile");
ABSL_FLAG(std::string, description, "<input>",
          "Source description used in parser/checker diagnostics");
ABSL_FLAG(bool, check, false,
          "Run the type checker in addition to the parser");
ABSL_FLAG(bool, reject_dyn, true,
          "When --check is set, reject expressions with any DYN-typed nodes");
ABSL_FLAG(std::string, schema, "",
          "Path to a FileDescriptorSet (binary) describing proto message types "
          "referenced by --var");
ABSL_FLAG(std::string, var, "",
          "Variable declarations 'name:Type' separated by ';'. "
          "Type is a primitive (bool, int, uint, double, string, bytes, "
          "null_type), a well-known (timestamp, duration, any), a composite "
          "(list<T>, map<K,V>), or a proto message FQN.  Commas are allowed "
          "inside type specs; use ';' to separate multiple vars.");
ABSL_FLAG(std::string, container, "",
          "Name resolution container (like CEL-Go's container option)");

namespace {

int ParseOnly(absl::string_view expression, absl::string_view description) {
  auto parsed =
      google::api::expr::parser::Parse(expression, description);
  if (!parsed.ok()) {
    std::fprintf(stderr, "parse error: %s\n",
                 std::string(parsed.status().message()).c_str());
    return 1;
  }
  std::fputs(parsed->DebugString().c_str(), stdout);
  return 0;
}

void PrintAnnotationSummary(const celwasm::TypedAst& typed) {
  // Sort by expr id for deterministic output.
  std::map<int64_t, celwasm::Repr> ordered;
  for (const auto& [id, note] : typed.annotations().nodes()) {
    ordered.emplace(id, note.repr);
  }
  std::fputs("# WasmAnnotations (expr_id -> repr)\n", stdout);
  for (const auto& [id, repr] : ordered) {
    std::fprintf(stdout, "#   %6lld  %s\n", static_cast<long long>(id),
                 std::string(celwasm::ReprName(repr)).c_str());
  }
}

int CheckAndPrint(absl::string_view expression,
                  const celwasm::CheckOptions& opts, bool reject_dyn) {
  auto typed = celwasm::ParseAndCheck(expression, opts);
  if (!typed.ok()) {
    std::fprintf(stderr, "%s\n",
                 std::string(typed.status().message()).c_str());
    return 1;
  }

  if (reject_dyn) {
    if (auto s = celwasm::RejectDyn(typed->ast()); !s.ok()) {
      std::fprintf(stderr, "%s\n", std::string(s.message()).c_str());
      return 1;
    }
  }

  // Print the CheckedExpr textproto.
  cel::expr::CheckedExpr checked_pb;
  if (auto s = cel::AstToCheckedExpr(typed->ast(), &checked_pb); !s.ok()) {
    std::fprintf(stderr, "failed to serialize checked AST: %s\n",
                 std::string(s.message()).c_str());
    return 1;
  }
  std::fputs(checked_pb.DebugString().c_str(), stdout);

  PrintAnnotationSummary(*typed);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  const std::string& expression = absl::GetFlag(FLAGS_e);
  if (expression.empty()) {
    std::fprintf(stderr,
                 "usage: celwasmc -e \"<cel expression>\" "
                 "[--check [--schema=<fds.pb.bin>] [--var=name:Type ...] "
                 "[--container=<pkg>]]\n");
    return 2;
  }

  const std::string& description = absl::GetFlag(FLAGS_description);

  if (!absl::GetFlag(FLAGS_check)) {
    return ParseOnly(expression, description);
  }

  celwasm::CheckOptions opts;
  opts.schema_path = absl::GetFlag(FLAGS_schema);
  const std::string var_flag = absl::GetFlag(FLAGS_var);
  if (!var_flag.empty()) {
    for (absl::string_view spec :
         absl::StrSplit(var_flag, ';', absl::SkipEmpty{})) {
      opts.variable_specs.emplace_back(spec);
    }
  }
  opts.container = absl::GetFlag(FLAGS_container);
  opts.description = description;
  return CheckAndPrint(expression, opts, absl::GetFlag(FLAGS_reject_dyn));
}
