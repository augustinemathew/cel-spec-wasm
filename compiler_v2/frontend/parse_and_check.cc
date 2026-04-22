#include "compiler_v2/frontend/parse_and_check.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/variant.h"
#include "cel/expr/syntax.pb.h"
#include "checker/standard_library.h"
#include "checker/type_checker.h"
#include "checker/type_checker_builder.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/ast_proto.h"
#include "common/decl.h"
#include "common/expr.h"
#include "common/type.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "parser/parser.h"

namespace celwasm {

namespace {

// --- Schema loading ----------------------------------------------------------

struct DescriptorPoolBundle {
  std::unique_ptr<google::protobuf::SimpleDescriptorDatabase> schema_db;
  std::unique_ptr<google::protobuf::DescriptorPoolDatabase> generated_db;
  std::unique_ptr<google::protobuf::MergedDescriptorDatabase> merged_db;
  std::unique_ptr<google::protobuf::DescriptorPool> owned_pool;
  const google::protobuf::DescriptorPool* pool = nullptr;
};

class StringErrorCollector : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, int column, absl::string_view message) override {
    absl::StrAppend(&text_, "  line ", line + 1, ":", column + 1, " ", message,
                    "\n");
  }
  void RecordWarning(int /*line*/, int /*column*/,
                     absl::string_view /*message*/) override {}
  const std::string& text() const {
    return text_;
  }

 private:
  std::string text_;
};

absl::StatusOr<google::protobuf::FileDescriptorProto> ParseProtoSource(
    absl::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open proto source: ", path));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string contents = buf.str();

  google::protobuf::io::ArrayInputStream input(
      contents.data(), static_cast<int>(contents.size()));
  StringErrorCollector collector;
  google::protobuf::io::Tokenizer tokenizer(&input, &collector);

  google::protobuf::compiler::Parser parser;
  parser.RecordErrorsTo(&collector);

  google::protobuf::FileDescriptorProto file;
  if (!parser.Parse(&tokenizer, &file)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "failed to parse proto source ", path, ":\n", collector.text()));
  }
  file.set_name(std::string(path));
  return file;
}

absl::Status RegisterSchemaProtoSource(
    const SchemaProtoSource& src,
    google::protobuf::SimpleDescriptorDatabase& schema_db) {
  auto file = ParseProtoSource(src.path);
  if (!file.ok()) return file.status();
  if (!schema_db.Add(*file)) {
    return absl::InvalidArgumentError(
        absl::StrCat("could not register proto source: ", src.path));
  }
  return absl::OkStatus();
}

absl::Status RegisterSchemaDescriptorSet(
    const SchemaDescriptorSet& src,
    google::protobuf::SimpleDescriptorDatabase& schema_db) {
  std::ifstream in{src.path, std::ios::binary};
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open schema file: ", src.path));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string bytes = buf.str();

  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(bytes)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "schema file is not a valid FileDescriptorSet: ", src.path));
  }
  for (const auto& file : fds.file()) {
    if (!schema_db.Add(file)) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate file in FileDescriptorSet: ", file.name()));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<DescriptorPoolBundle> LoadDescriptorPool(
    const CheckOptions& opts) {
  DescriptorPoolBundle bundle;
  if (std::holds_alternative<std::monostate>(opts.schema)) {
    bundle.pool = google::protobuf::DescriptorPool::generated_pool();
    return bundle;
  }

  bundle.schema_db =
      std::make_unique<google::protobuf::SimpleDescriptorDatabase>();

  absl::Status register_status = std::visit(
      [&](const auto& src) -> absl::Status {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return absl::OkStatus();
        } else if constexpr (std::is_same_v<T, SchemaProtoSource>) {
          return RegisterSchemaProtoSource(src, *bundle.schema_db);
        } else if constexpr (std::is_same_v<T, SchemaDescriptorSet>) {
          return RegisterSchemaDescriptorSet(src, *bundle.schema_db);
        }
      },
      opts.schema);
  if (!register_status.ok()) return register_status;

  bundle.generated_db =
      std::make_unique<google::protobuf::DescriptorPoolDatabase>(
          *google::protobuf::DescriptorPool::generated_pool());
  bundle.merged_db =
      std::make_unique<google::protobuf::MergedDescriptorDatabase>(
          bundle.schema_db.get(), bundle.generated_db.get());
  bundle.owned_pool = std::make_unique<google::protobuf::DescriptorPool>(
      bundle.merged_db.get());
  bundle.pool = bundle.owned_pool.get();
  return bundle;
}

// --- Type spec parsing -------------------------------------------------------

struct TypeParser {
  absl::string_view src;
  size_t pos = 0;
  google::protobuf::Arena* arena;
  const google::protobuf::DescriptorPool* pool;

  void SkipWhitespace() {
    while (pos < src.size() && absl::ascii_isspace(src[pos]))
      ++pos;
  }

  bool Consume(char c) {
    SkipWhitespace();
    if (pos < src.size() && src[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }

  absl::StatusOr<std::string> ParseIdent() {
    SkipWhitespace();
    const size_t start = pos;
    while (pos < src.size() && (absl::ascii_isalnum(src[pos]) ||
                                src[pos] == '_' || src[pos] == '.')) {
      ++pos;
    }
    if (pos == start) {
      return absl::InvalidArgumentError(absl::StrCat(
          "expected identifier at offset ", start, " in type spec: ", src));
    }
    return std::string(src.substr(start, pos - start));
  }

  absl::StatusOr<cel::Type> ParseType() {
    SkipWhitespace();
    auto name = ParseIdent();
    if (!name.ok()) return name.status();

    if (*name == "bool") return cel::Type(cel::BoolType{});
    if (*name == "int") return cel::Type(cel::IntType{});
    if (*name == "uint") return cel::Type(cel::UintType{});
    if (*name == "double") return cel::Type(cel::DoubleType{});
    if (*name == "string") return cel::Type(cel::StringType{});
    if (*name == "bytes") return cel::Type(cel::BytesType{});
    if (*name == "null_type") return cel::Type(cel::NullType{});
    if (*name == "timestamp") return cel::Type(cel::TimestampType{});
    if (*name == "duration") return cel::Type(cel::DurationType{});
    if (*name == "any") return cel::Type(cel::AnyType{});
    if (*name == "dyn") return cel::Type(cel::DynType{});

    if (*name == "list") {
      if (!Consume('<')) {
        return absl::InvalidArgumentError("expected '<' after 'list'");
      }
      auto elem = ParseType();
      if (!elem.ok()) return elem.status();
      if (!Consume('>')) {
        return absl::InvalidArgumentError("expected '>' to close 'list<...>'");
      }
      return cel::Type(cel::ListType(arena, *elem));
    }
    if (*name == "map") {
      if (!Consume('<')) {
        return absl::InvalidArgumentError("expected '<' after 'map'");
      }
      auto key = ParseType();
      if (!key.ok()) return key.status();
      if (!Consume(',')) {
        return absl::InvalidArgumentError("expected ',' in map<...>");
      }
      auto val = ParseType();
      if (!val.ok()) return val.status();
      if (!Consume('>')) {
        return absl::InvalidArgumentError("expected '>' to close 'map<...>'");
      }
      return cel::Type(cel::MapType(arena, *key, *val));
    }

    const auto* descriptor = pool->FindMessageTypeByName(*name);
    if (descriptor == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat(
          "unknown type in spec: '", *name,
          "' (not a primitive, list<>, map<>, or known message type)"));
    }
    return cel::Type::Message(descriptor);
  }
};

struct ParsedSpec {
  cel::VariableDecl decl;
  Repr repr;
};

absl::StatusOr<ParsedSpec> ParseVariableSpec(
    absl::string_view spec, google::protobuf::Arena* arena,
    const google::protobuf::DescriptorPool* pool) {
  const size_t colon = spec.find(':');
  if (colon == absl::string_view::npos) {
    return absl::InvalidArgumentError(
        absl::StrCat("variable spec must be 'name:Type': ", spec));
  }
  std::string name =
      std::string(absl::StripAsciiWhitespace(spec.substr(0, colon)));
  absl::string_view type_src =
      absl::StripAsciiWhitespace(spec.substr(colon + 1));
  if (name.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("variable spec has empty name: ", spec));
  }
  TypeParser parser{type_src, 0, arena, pool};
  auto type = parser.ParseType();
  if (!type.ok()) return type.status();
  parser.SkipWhitespace();
  if (parser.pos != type_src.size()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "trailing garbage in type spec: '", type_src.substr(parser.pos), "'"));
  }
  Repr repr = ReprOf(*type);
  return ParsedSpec{cel::MakeVariableDecl(name, std::move(*type)), repr};
}

// --- Static-subset enforcement (folded from v1's ir/static_subset) ---------

struct Violation {
  int64_t expr_id;
  const char* kind;
  std::string detail;
};

const char* UnacceptableLabel(const cel::TypeSpec& type) {
  if (type.has_dyn()) return "dyn";
  if (type.has_error()) return "error";
  using cel::TypeSpecKind;
  const TypeSpecKind& k = type.type_kind();
  if (absl::holds_alternative<cel::FunctionTypeSpec>(k)) return "function";
  if (absl::holds_alternative<cel::ParamTypeSpec>(k)) return "type-param";
  if (absl::holds_alternative<cel::UnsetTypeSpec>(k)) return "unset";
  return nullptr;
}

void CheckSubsetNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out);

void CheckSubsetChildren(const cel::Expr& node, const cel::Ast::TypeMap& types,
                         std::vector<Violation>& out) {
  switch (node.kind_case()) {
    case cel::ExprKindCase::kUnspecifiedExpr:
    case cel::ExprKindCase::kConstant:
    case cel::ExprKindCase::kIdentExpr:
      return;
    case cel::ExprKindCase::kSelectExpr:
      if (node.select_expr().has_operand()) {
        CheckSubsetNode(node.select_expr().operand(), types, out);
      }
      return;
    case cel::ExprKindCase::kCallExpr: {
      const auto& call = node.call_expr();
      if (call.has_target()) CheckSubsetNode(call.target(), types, out);
      for (const auto& arg : call.args())
        CheckSubsetNode(arg, types, out);
      return;
    }
    case cel::ExprKindCase::kListExpr:
      for (const auto& elem : node.list_expr().elements()) {
        if (elem.has_expr()) CheckSubsetNode(elem.expr(), types, out);
      }
      return;
    case cel::ExprKindCase::kStructExpr:
      for (const auto& field : node.struct_expr().fields()) {
        if (field.has_value()) CheckSubsetNode(field.value(), types, out);
      }
      return;
    case cel::ExprKindCase::kMapExpr:
      for (const auto& entry : node.map_expr().entries()) {
        if (entry.has_key()) CheckSubsetNode(entry.key(), types, out);
        if (entry.has_value()) CheckSubsetNode(entry.value(), types, out);
      }
      return;
    case cel::ExprKindCase::kComprehensionExpr: {
      const auto& c = node.comprehension_expr();
      if (c.has_iter_range()) CheckSubsetNode(c.iter_range(), types, out);
      if (c.has_accu_init()) CheckSubsetNode(c.accu_init(), types, out);
      if (c.has_loop_condition())
        CheckSubsetNode(c.loop_condition(), types, out);
      if (c.has_loop_step()) CheckSubsetNode(c.loop_step(), types, out);
      if (c.has_result()) CheckSubsetNode(c.result(), types, out);
      return;
    }
  }
}

void CheckSubsetNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out) {
  const int64_t id = node.id();
  if (id != 0) {
    auto it = types.find(id);
    if (it == types.end()) {
      out.push_back({id, "dyn", "no type_map entry"});
    } else if (const char* label = UnacceptableLabel(it->second);
               label != nullptr) {
      out.push_back({id, label, cel::FormatTypeSpec(it->second)});
    }
  }
  CheckSubsetChildren(node, types, out);
}

absl::Status RejectDyn(const cel::Ast& ast) {
  if (!ast.is_checked()) {
    return absl::FailedPreconditionError(
        "RejectDyn requires a checked AST (type_map must be populated)");
  }
  std::vector<Violation> violations;
  CheckSubsetNode(ast.root_expr(), ast.type_map(), violations);
  if (violations.empty()) return absl::OkStatus();

  std::vector<std::string> lines;
  lines.reserve(violations.size());
  for (const auto& v : violations) {
    lines.push_back(absl::StrCat("  expr id=", v.expr_id, " is ", v.kind, " (",
                                 v.detail, ")"));
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "expression is not in the static subset:\n", absl::StrJoin(lines, "\n")));
}

}  // namespace

absl::StatusOr<TypedAst> ParseAndCheck(absl::string_view expression,
                                       const CheckOptions& opts) {
  auto pool_bundle = LoadDescriptorPool(opts);
  if (!pool_bundle.ok()) return pool_bundle.status();

  auto builder = cel::CreateTypeCheckerBuilder(pool_bundle->pool);
  if (!builder.ok()) return builder.status();

  if (auto s = (*builder)->AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  if (!opts.container.empty()) {
    (*builder)->set_container(opts.container);
  }

  std::vector<Variable> variables;
  variables.reserve(opts.variable_specs.size());
  for (const auto& spec : opts.variable_specs) {
    auto parsed =
        ParseVariableSpec(spec, (*builder)->arena(), pool_bundle->pool);
    if (!parsed.ok()) return parsed.status();
    std::string name = parsed->decl.name();
    if (auto s = (*builder)->AddVariable(parsed->decl); !s.ok()) return s;
    variables.push_back(Variable{std::move(name), parsed->repr});
  }

  auto checker = (*builder)->Build();
  if (!checker.ok()) return checker.status();

  auto parsed = google::api::expr::parser::Parse(expression, opts.description);
  if (!parsed.ok()) return parsed.status();

  auto ast = cel::CreateAstFromParsedExpr(*parsed);
  if (!ast.ok()) return ast.status();

  auto result = (*checker)->Check(std::move(*ast));
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    return absl::InvalidArgumentError(
        absl::StrCat("type check failed:\n", result->FormatError()));
  }
  auto checked_ast = result->ReleaseAst();
  if (!checked_ast.ok()) return checked_ast.status();

  // Static-subset gate: no DYN / ERROR / type-param / function / unset nodes.
  if (auto s = RejectDyn(**checked_ast); !s.ok()) return s;

  WasmAnnotations annotations;
  PopulateAnnotations(**checked_ast, pool_bundle->pool, annotations);

  return TypedAst(std::move(*checked_ast), std::move(annotations),
                  std::move(variables));
}

}  // namespace celwasm
