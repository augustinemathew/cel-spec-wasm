#include "compiler/frontend/parse_and_check.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/syntax.pb.h"
#include "checker/standard_library.h"
#include "checker/type_checker.h"
#include "checker/type_checker_builder.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/decl.h"
#include "common/type.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "parser/parser.h"

namespace celwasm {

namespace {

// --- Schema loading ----------------------------------------------------------

struct DescriptorPoolBundle {
  // Populated when the caller provided a schema.  When these are null we use
  // the generated pool directly.
  std::unique_ptr<google::protobuf::SimpleDescriptorDatabase> schema_db;
  std::unique_ptr<google::protobuf::DescriptorPoolDatabase> generated_db;
  std::unique_ptr<google::protobuf::MergedDescriptorDatabase> merged_db;
  std::unique_ptr<google::protobuf::DescriptorPool> owned_pool;
  const google::protobuf::DescriptorPool* pool = nullptr;
};

absl::StatusOr<DescriptorPoolBundle> LoadDescriptorPool(
    absl::string_view schema_path) {
  DescriptorPoolBundle bundle;
  if (schema_path.empty()) {
    bundle.pool = google::protobuf::DescriptorPool::generated_pool();
    return bundle;
  }

  std::ifstream in{std::string(schema_path), std::ios::binary};
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open schema file: ", schema_path));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  std::string bytes = buf.str();

  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(bytes)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "schema file is not a valid FileDescriptorSet: ", schema_path));
  }

  bundle.schema_db =
      std::make_unique<google::protobuf::SimpleDescriptorDatabase>();
  for (const auto& file : fds.file()) {
    if (!bundle.schema_db->Add(file)) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate file in FileDescriptorSet: ", file.name()));
    }
  }
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

    // Primitives and well-knowns by bare name.
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

    // Otherwise, treat it as a fully-qualified message type.
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

}  // namespace

absl::StatusOr<TypedAst> ParseAndCheck(absl::string_view expression,
                                       const CheckOptions& opts) {
  // 1. Descriptor pool (WKTs + optional user schema).
  auto pool_bundle = LoadDescriptorPool(opts.schema_path);
  if (!pool_bundle.ok()) return pool_bundle.status();

  // 2. Type-checker builder.
  auto builder = cel::CreateTypeCheckerBuilder(pool_bundle->pool);
  if (!builder.ok()) return builder.status();

  if (auto s = (*builder)->AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  if (!opts.container.empty()) {
    (*builder)->set_container(opts.container);
  }

  // 3. Parse user variable decls using the builder's arena.  Retain the
  // (name, Repr) pair alongside the checker's decl so downstream codegen
  // can lay out eval-function parameters in declaration order without
  // re-parsing the user's type strings.
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

  // 4. Build the checker.
  auto checker = (*builder)->Build();
  if (!checker.ok()) return checker.status();

  // 5. Parse source.
  auto parsed = google::api::expr::parser::Parse(expression, opts.description);
  if (!parsed.ok()) return parsed.status();

  // 6. Convert to runtime AST.
  auto ast = cel::CreateAstFromParsedExpr(*parsed);
  if (!ast.ok()) return ast.status();

  // 7. Type-check.
  auto result = (*checker)->Check(std::move(*ast));
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    return absl::InvalidArgumentError(
        absl::StrCat("type check failed:\n", result->FormatError()));
  }
  auto checked_ast = result->ReleaseAst();
  if (!checked_ast.ok()) return checked_ast.status();

  // 8. Seed annotations.
  WasmAnnotations annotations;
  PopulateAnnotations(**checked_ast, annotations);

  return TypedAst(std::move(*checked_ast), std::move(annotations),
                  std::move(variables));
}

}  // namespace celwasm
