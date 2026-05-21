#include "compiler_v2/frontend/parse_and_check.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"
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
#include "common/source.h"
#include "common/type.h"
#include "compiler_v2/frontend/status_tags.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "extensions/bindings_ext.h"
#include "extensions/comprehensions_v2.h"
#include "extensions/strings.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "parser/macro_registry.h"
#include "parser/options.h"
#include "parser/parser.h"
#include "parser/standard_macros.h"

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

// Returns a non-empty `cel::Type` iff `name` is one of the CEL spec's
// primitive or well-known type keywords.  Kept separate from
// `TypeParser` so the parser's per-method line count stays small.
std::optional<cel::Type> ParsePrimitiveType(absl::string_view name) {
  if (name == "bool") return cel::Type(cel::BoolType{});
  if (name == "int") return cel::Type(cel::IntType{});
  if (name == "uint") return cel::Type(cel::UintType{});
  if (name == "double") return cel::Type(cel::DoubleType{});
  if (name == "string") return cel::Type(cel::StringType{});
  if (name == "bytes") return cel::Type(cel::BytesType{});
  if (name == "null_type") return cel::Type(cel::NullType{});
  if (name == "timestamp") return cel::Type(cel::TimestampType{});
  if (name == "duration") return cel::Type(cel::DurationType{});
  if (name == "any") return cel::Type(cel::AnyType{});
  if (name == "dyn") return cel::Type(cel::DynType{});
  // `type` declarable as a variable type (the type-of-types; see
  // `rewrite/m9-type-subsystem.md`).
  // The cel-cpp `TypeType` constructor takes an optional inner type;
  // a default-constructed TypeType corresponds to "untyped type" —
  // sufficient for variable declarations whose values are arbitrary
  // CEL_TYPE Values.
  if (name == "type") return cel::Type(cel::TypeType{});
  return std::nullopt;
}

struct TypeParser {
  absl::string_view src;
  size_t pos = 0;
  google::protobuf::Arena* arena = nullptr;
  const google::protobuf::DescriptorPool* pool = nullptr;

  void SkipWhitespace() {
    while (pos < src.size() && absl::ascii_isspace(src[pos])) {
      ++pos;
    }
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

  absl::StatusOr<cel::Type> ParseListType() {
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

  absl::StatusOr<cel::Type> ParseMapType() {
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

  absl::StatusOr<cel::Type> ParseMessageType(absl::string_view name) const {
    const auto* descriptor = pool->FindMessageTypeByName(std::string(name));
    if (descriptor == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat(
          "unknown type in spec: '", name,
          "' (not a primitive, list<>, map<>, or known message type)"));
    }
    return cel::Type::Message(descriptor);
  }

  absl::StatusOr<cel::Type> ParseType() {
    SkipWhitespace();
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    if (auto primitive = ParsePrimitiveType(*name); primitive.has_value()) {
      return *primitive;
    }
    if (*name == "list") return ParseListType();
    if (*name == "map") return ParseMapType();
    return ParseMessageType(*name);
  }
};

struct ParsedSpec {
  cel::VariableDecl decl{};
  Repr repr = Repr::kUnknown;
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
  return ParsedSpec{cel::MakeVariableDecl(name, *type), repr};
}

// --- Static-subset enforcement (folded from v1's ir/static_subset) ---------

struct Violation {
  int64_t expr_id = 0;
  const char* kind = nullptr;
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
  // Recurse through container types — list<dyn>, map<_, dyn>, map<dyn, _>,
  // and abstract<...,dyn,...> all carry implicit dyn that the static-subset
  // gate must reject.
  if (type.has_list_type()) {
    return UnacceptableLabel(type.list_type().elem_type());
  }
  if (type.has_map_type()) {
    if (const char* l = UnacceptableLabel(type.map_type().key_type());
        l != nullptr) {
      return l;
    }
    return UnacceptableLabel(type.map_type().value_type());
  }
  if (type.has_abstract_type()) {
    for (const auto& p : type.abstract_type().parameter_types()) {
      if (const char* l = UnacceptableLabel(p); l != nullptr) return l;
    }
  }
  return nullptr;
}

void CheckSubsetNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out);

// `<string>.format(<list>)` (M12 string_ext extension) takes a
// heterogeneously-typed args list — empty list literals type as
// `list(dyn)`, mixed-element-type lists too, and nested
// list/map literals in the args carry the same dyn exposure.
// The runtime kernel dispatches per-element by CelKind via
// `RenderString` in `cel_string_format_render.cc`, so the dyn
// exposure is a checker-side artefact rather than a runtime
// concern.  Admit the args sub-tree without recursing — every
// child node is presumed safe under runtime per-kind dispatch.
//
// Scope: only the LITERAL-LIST shape `"...".format([...])`
// admits.  Other shapes (variable holding a list, comprehension
// result, …) stay rejected — they can't be statically validated
// for runtime kind safety without further analysis (e.g. a
// `list<dyn>`-typed variable might carry messages, which
// `RenderString` errors out on).
bool IsFormatCallWithListLiteralArgs(const cel::CallExpr& call) {
  if (!call.has_target()) return false;
  if (call.function() != "format") return false;
  if (call.args().size() != 1) return false;
  return call.args()[0].kind_case() == cel::ExprKindCase::kListExpr;
}

void CheckSubsetCall(const cel::CallExpr& call, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out) {
  if (call.has_target()) CheckSubsetNode(call.target(), types, out);
  if (IsFormatCallWithListLiteralArgs(call)) {
    // Args subtree fully admitted — see comment above.
    return;
  }
  for (const auto& arg : call.args()) {
    CheckSubsetNode(arg, types, out);
  }
}

void CheckSubsetMap(const cel::MapExpr& map, const cel::Ast::TypeMap& types,
                    std::vector<Violation>& out) {
  for (const auto& entry : map.entries()) {
    if (entry.has_key()) CheckSubsetNode(entry.key(), types, out);
    if (entry.has_value()) CheckSubsetNode(entry.value(), types, out);
  }
}

void CheckSubsetList(const cel::ListExpr& list, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out) {
  for (const auto& elem : list.elements()) {
    if (elem.has_expr()) CheckSubsetNode(elem.expr(), types, out);
  }
}

void CheckSubsetStruct(const cel::StructExpr& s, const cel::Ast::TypeMap& types,
                       std::vector<Violation>& out) {
  for (const auto& field : s.fields()) {
    if (field.has_value()) CheckSubsetNode(field.value(), types, out);
  }
}

// `cel.bind(name, value, body)` expands to a
// degenerate comprehension whose `iter_range` is an empty list
// literal `[]` (and `loop_condition` is `kConst(false)`).  Empty
// list literals type as `list(dyn)` — the checker has no
// element type to infer — and RejectDyn would otherwise refuse
// every cel.bind expression on that basis.  Skipping the
// iter_range subtree for this canonical shape is safe: the loop
// body never executes (loop_cond is `false`), so the
// iter_range's element type is never observed at runtime.
bool IsCelBindShape(const cel::ComprehensionExpr& c) {
  if (!c.has_iter_range() || !c.has_loop_condition()) return false;
  const cel::Expr& range = c.iter_range();
  if (range.kind_case() != cel::ExprKindCase::kListExpr) return false;
  if (!range.list_expr().elements().empty()) return false;
  const cel::Expr& cond = c.loop_condition();
  if (cond.kind_case() != cel::ExprKindCase::kConstant) return false;
  return cond.const_expr().has_bool_value() && !cond.const_expr().bool_value();
}

void CheckSubsetComprehension(const cel::ComprehensionExpr& c,
                              const cel::Ast::TypeMap& types,
                              std::vector<Violation>& out) {
  const bool cel_bind = IsCelBindShape(c);
  if (c.has_iter_range() && !cel_bind) {
    CheckSubsetNode(c.iter_range(), types, out);
  }
  if (c.has_accu_init()) CheckSubsetNode(c.accu_init(), types, out);
  if (c.has_loop_condition()) {
    CheckSubsetNode(c.loop_condition(), types, out);
  }
  if (c.has_loop_step() && !cel_bind) {
    // cel.bind's loop_step is `kIdent(accu_var)` per the
    // bindings_ext macro (never executed; loop_cond is false).
    // It may type as the accu_var's type which can be anything
    // including dyn-derived, but it's unreachable.
    CheckSubsetNode(c.loop_step(), types, out);
  }
  if (c.has_result()) CheckSubsetNode(c.result(), types, out);
}

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
    case cel::ExprKindCase::kCallExpr:
      CheckSubsetCall(node.call_expr(), types, out);
      return;
    case cel::ExprKindCase::kListExpr:
      CheckSubsetList(node.list_expr(), types, out);
      return;
    case cel::ExprKindCase::kStructExpr:
      CheckSubsetStruct(node.struct_expr(), types, out);
      return;
    case cel::ExprKindCase::kMapExpr:
      CheckSubsetMap(node.map_expr(), types, out);
      return;
    case cel::ExprKindCase::kComprehensionExpr:
      CheckSubsetComprehension(node.comprehension_expr(), types, out);
      return;
  }
}

// Slice 1.5 (dyn-passthrough-plan.md): admit `dyn(scalar)` as a no-op
// type-check escape so the conformance corpus's `dyn(scalar) == other_kind`
// rows reach the runtime kernel that already implements polymorphic
// equality.  The call site itself is typed `dyn` by cel-cpp's checker
// (verified via probe spike, plan §"Risk #3"), so we recurse into the
// argument only — the argument's type drives admissibility.  Every
// other `dyn`-typed shape (variables, list/map element types,
// aggregate operands, field reads) continues to fall through to the
// `UnacceptableLabel` dispatch and reject as before.
//
// Scope of admission (plan §"What gets admitted"):
//   - argument's checker-assigned type is a primitive scalar or null;
//   - or argument is itself a `dyn(...)` call (recursive collapse).
// Aggregate / message / dyn-typed arguments stay rejected: an admitted
// `dyn(msg)` would invite `dyn(msg).field` (late-bound message field
// reads, M7+ surface), which today crashes codegen.
bool ArgIsAdmissibleScalar(const cel::Expr& arg,
                           const cel::Ast::TypeMap& types) {
  auto it = types.find(arg.id());
  if (it == types.end()) return false;
  const auto& t = it->second;
  // `dyn(type-value)` admits — CEL_TYPE is a scalar wire shape
  // (`payload.s` is a CelSpan into linear memory), and downstream
  // operators (`==` via `cel_equals` CEL_TYPE arm) handle it
  // correctly.  Excluded: `dyn(message)` — out of scope (reflective
  // introspection; see `rewrite/m7-proto-literals.md` §2.2).
  return t.has_primitive() || t.has_null() || t.has_type();
}

// Admit a SelectExpr whose operand types as
// `google.protobuf.Any` even when the select itself types as `dyn`.
// cel-cpp's checker types reads through Any as dyn (the runtime
// unwrap target isn't statically known); v2's runtime DOES know how
// to unwrap at eval time (ProtoBacking::ReadField), so blocking
// these at the static-subset gate makes Any unusable for customers
// without buying any safety.
//
// Recursive: a select whose operand is itself a select-through-Any
// admits transitively, so `msg.single_any.x.y` lands after the
// first carve-out fires.
bool IsSelectThroughAny(const cel::Expr& node, const cel::Ast::TypeMap& types) {
  if (!node.has_select_expr()) return false;
  const auto& sel = node.select_expr();
  if (!sel.has_operand()) return false;
  auto it = types.find(sel.operand().id());
  if (it == types.end()) return false;
  const auto& t = it->second;
  if (t.has_well_known() && t.well_known() == cel::WellKnownTypeSpec::kAny) {
    return true;
  }
  if (t.has_message_type() &&
      t.message_type().type() == "google.protobuf.Any") {
    return true;
  }
  return IsSelectThroughAny(sel.operand(), types);
}

bool IsDynPassthroughCall(const cel::Expr& node,
                          const cel::Ast::TypeMap& types) {
  if (!node.has_call_expr()) return false;
  const auto& call = node.call_expr();
  if (call.function() != "dyn" || call.args().size() != 1 ||
      call.has_target()) {
    return false;
  }
  const auto& arg = call.args()[0];
  // Recursive collapse: `dyn(dyn(x))` admits iff the inner call admits.
  if (arg.has_call_expr() && arg.call_expr().function() == "dyn" &&
      arg.call_expr().args().size() == 1) {
    return IsDynPassthroughCall(arg, types);
  }
  return ArgIsAdmissibleScalar(arg, types);
}

void CheckSubsetNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
                     std::vector<Violation>& out) {
  if (IsDynPassthroughCall(node, types)) {
    CheckSubsetNode(node.call_expr().args()[0], types, out);
    return;
  }
  if (IsSelectThroughAny(node, types)) {
    // Skip the dyn violation at this node and recurse into the
    // operand (which by construction is admissible — either Any-typed
    // or itself a select-through-Any).  The runtime unwrap arm in
    // ProtoBacking::ReadField handles the resolution at eval time.
    CheckSubsetNode(node.select_expr().operand(), types, out);
    return;
  }
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
  std::vector<std::string> ids;
  ids.reserve(violations.size());
  for (const auto& v : violations) {
    lines.push_back(absl::StrCat("  expr id=", v.expr_id, " is ", v.kind, " (",
                                 v.detail, ")"));
    ids.push_back(absl::StrCat(v.expr_id));
  }
  absl::Status s = absl::InvalidArgumentError(absl::StrCat(
      "expression is not in the static subset:\n", absl::StrJoin(lines, "\n")));
  // Tag with status_tags.h::kStaticSubsetViolationUrl so the
  // conformance harness can classify by GetPayload rather than by
  // substring-matching the message text.  Body is the comma-joined
  // offending expr-id list ("3,17"); the harness ignores the body
  // today but the structure is here for diagnostics.
  s.SetPayload(kStaticSubsetViolationUrl, absl::Cord(absl::StrJoin(ids, ",")));
  return s;
}

absl::Status ConfigureCheckerBuilder(
    cel::TypeCheckerBuilder& builder, const CheckOptions& opts,
    const google::protobuf::DescriptorPool* pool,
    std::vector<Variable>& variables_out) {
  if (auto s = builder.AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  // comprehensions_v2 also declares checker overloads for the
  // runtime-only functions cel-cpp's transformMap /
  // transformMapEntry macros emit (`cel.@mapInsert`,
  // `cel.@mapInsertOverwrite`).  Without these, type-checking
  // rejects every transformMap{,Entry} expression with
  // "undeclared reference to 'cel.@mapInsert'".
  if (auto s =
          builder.AddLibrary(cel::extensions::ComprehensionsV2CheckerLibrary());
      !s.ok()) {
    return s;
  }
  // M12 string_ext: registers the 13 cel-cpp `strings` extension
  // functions + the printf-style `format` directive.  Runtime
  // kernels are self-hosted in `cel_runtime.wasm`; codegen
  // routes through the 19 overload IDs seeded in
  // `overload_table.cc`.
  if (auto s = builder.AddLibrary(cel::extensions::StringsCheckerLibrary());
      !s.ok()) {
    return s;
  }
  if (!opts.container.empty()) {
    builder.set_container(opts.container);
  }
  variables_out.reserve(opts.variable_specs.size());
  for (const auto& spec : opts.variable_specs) {
    auto parsed = ParseVariableSpec(spec, builder.arena(), pool);
    if (!parsed.ok()) return parsed.status();
    std::string name = parsed->decl.name();
    if (auto s = builder.AddVariable(parsed->decl); !s.ok()) return s;
    variables_out.push_back(Variable{std::move(name), parsed->repr});
  }
  return absl::OkStatus();
}

// Macros the parser recognises beyond the bare
// CEL grammar.  Standard macros (`has`, `all`, `exists`,
// `exists_one`, `map`, `filter`) cover langdef-required
// expansions; bindings_ext adds `cel.bind`; comprehensions_v2
// adds the three-arg / two-iter-var forms (`exists(i, v, p)`,
// `transformList(i, v, t)`, etc.) that drive `macros2.textproto`.
absl::Status BuildMacroRegistry(cel::MacroRegistry& registry) {
  cel::ParserOptions opts;
  if (auto s = cel::RegisterStandardMacros(registry, opts); !s.ok()) return s;
  if (auto s = cel::extensions::RegisterBindingsMacros(registry, opts);
      !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterComprehensionsV2Macros(registry, opts);
      !s.ok()) {
    return s;
  }
  return absl::OkStatus();
}

// Extract the bare symbol from an "undeclared reference to 'X'"
// issue message, returning the root namespace component (everything
// up to the first '.').  Used to tag `kUndeclaredReferencesUrl` so
// the harness can match against ext-lib roots without parsing
// human-readable text itself.  Returns empty string_view if the
// message doesn't match the expected shape.
absl::string_view UndeclaredSymbolRoot(absl::string_view msg) {
  constexpr absl::string_view kPrefix = "undeclared reference to '";
  const auto start = msg.find(kPrefix);
  if (start == absl::string_view::npos) return {};
  const auto sym_begin = start + kPrefix.size();
  const auto sym_end = msg.find('\'', sym_begin);
  if (sym_end == absl::string_view::npos) return {};
  absl::string_view sym = msg.substr(sym_begin, sym_end - sym_begin);
  const auto dot = sym.find('.');
  return dot == absl::string_view::npos ? sym : sym.substr(0, dot);
}

absl::StatusOr<std::unique_ptr<cel::Ast>> RunTypeCheck(
    cel::TypeChecker& checker, absl::string_view expression,
    absl::string_view description) {
  cel::MacroRegistry registry;
  if (auto s = BuildMacroRegistry(registry); !s.ok()) return s;
  auto source = cel::NewSource(expression, std::string(description));
  if (!source.ok()) return source.status();
  auto parsed = google::api::expr::parser::Parse(**source, registry);
  if (!parsed.ok()) return parsed.status();
  auto ast = cel::CreateAstFromParsedExpr(*parsed);
  if (!ast.ok()) return ast.status();
  auto result = checker.Check(std::move(*ast));
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    absl::Status s = absl::InvalidArgumentError(
        absl::StrCat("type check failed:\n", result->FormatError()));
    // Collect the root namespace of every "undeclared reference to
    // '<sym>'" issue so the conformance harness can classify
    // ext-lib gaps (math.greatest, optional.of, …) without
    // substring-matching the full message text.
    absl::btree_set<absl::string_view> roots;
    for (const auto& issue : result->GetIssues()) {
      const absl::string_view root = UndeclaredSymbolRoot(issue.message());
      if (!root.empty()) roots.insert(root);
    }
    if (!roots.empty()) {
      s.SetPayload(kUndeclaredReferencesUrl,
                   absl::Cord(absl::StrJoin(roots, "\n")));
    }
    return s;
  }
  return result->ReleaseAst();
}

// Walk the AST in place and replace every kIdentExpr whose
// `reference_map` entry carries a Constant value with a kConstantExpr
// node holding that value.  cel-cpp's checker resolves enum-name
// references like `TestAllTypes.NestedEnum.BAR` to a `VariableReference`
// with `has_value()=true` and `value()` carrying the int constant; for
// our pipeline (which has no runtime variable lookup that would
// substitute a constant on read), the simplest correct path is to
// inline the constant into the AST so the existing kConst codegen
// (rodata pack + `i32.const` emit) handles it.
//
// Cross-reference: cel-cpp's runtime/reference_resolver does the same
// rewrite under its `ReferenceResolverEnabled::kAlways` mode, but
// requires pulling in the FlatExprBuilder runtime — we don't run that
// runtime, so we re-implement the walk here in ~30 lines.
//
// Idempotent — running this on an AST without any constant idents is
// a no-op.  Called after the checker returns and before
// `RejectDyn` / annotation population so all later passes see the
// rewritten kConstant nodes uniformly.
// Split out of VisitInlineConstantChildren to keep it under the
// readability-function-size gate.
void VisitComprehensionChildren(cel::Expr& expr,
                                const std::function<void(cel::Expr&)>& visit) {
  auto& c = expr.mutable_comprehension_expr();
  visit(c.mutable_iter_range());
  visit(c.mutable_accu_init());
  visit(c.mutable_loop_condition());
  visit(c.mutable_loop_step());
  visit(c.mutable_result());
}

// Recurse into the immediate children of `expr` and apply `visit` to
// each — split out of `InlineConstantReferences` to keep that function
// under the lint size gate.  Caller is responsible for handling the
// kIdent rewrite at the parent; this only descends.
void VisitInlineConstantChildren(cel::Expr& expr,
                                 const std::function<void(cel::Expr&)>& visit) {
  if (expr.has_select_expr()) {
    visit(expr.mutable_select_expr().mutable_operand());
    return;
  }
  if (expr.has_call_expr()) {
    auto& call = expr.mutable_call_expr();
    if (call.has_target()) visit(call.mutable_target());
    for (auto& arg : call.mutable_args()) {
      visit(arg);
    }
    return;
  }
  if (expr.has_list_expr()) {
    for (auto& elem : expr.mutable_list_expr().mutable_elements()) {
      visit(elem.mutable_expr());
    }
    return;
  }
  if (expr.has_map_expr()) {
    for (auto& e : expr.mutable_map_expr().mutable_entries()) {
      visit(e.mutable_key());
      visit(e.mutable_value());
    }
    return;
  }
  if (expr.has_struct_expr()) {
    for (auto& f : expr.mutable_struct_expr().mutable_fields()) {
      visit(f.mutable_value());
    }
    return;
  }
  if (expr.has_comprehension_expr()) VisitComprehensionChildren(expr, visit);
  // kConstant / kUnspecified / kIdent — leaf, nothing to recurse into.
}

void InlineConstantReferences(cel::Ast& ast) {
  const cel::Ast::ReferenceMap& refs = ast.reference_map();
  // Recursive in-place walker.  kIdent is always a leaf, so once
  // rewritten to kConstant the recursion bottoms out without
  // descending — children of a kConstant are empty.  Every other
  // kind delegates to `VisitInlineConstantChildren`.
  std::function<void(cel::Expr&)> visit = [&](cel::Expr& expr) {
    if (expr.has_ident_expr()) {
      auto it = refs.find(expr.id());
      if (it != refs.end()) {
        const cel::Reference& ref = it->second;
        if (ref.has_value()) {
          expr.set_const_expr(ref.value());
        }
      }
      return;
    }
    VisitInlineConstantChildren(expr, visit);
  };
  visit(ast.mutable_root_expr());
}

// Resolve the spec type-name for a `cel.expr.Type` whose outer
// kind is `kType` (the type-of-types).  Returns the spec type-name
// per langdef §"Type Values" and `rewrite/m9-type-subsystem.md` §3.1.
//
// Returns std::nullopt for inner kinds we deliberately do NOT
// rewrite at this slice — `function` / `error` / `dyn` / abstract
// type-params we haven't yet pinned a name for.  Caller treats
// nullopt as "leave the kIdent alone; downstream codegen will
// surface its own diagnostic for a bound-variable miss".  This
// is a small, named set; everything else has a concrete name and
// CHECK-fails on an unrecognised enum value (CLAUDE.md
// "unreachable switch defaults" rule).
std::optional<std::string> PrimitiveTypeName(cel::PrimitiveType p) {
  switch (p) {
    case cel::PrimitiveType::kBool:
      return std::string("bool");
    case cel::PrimitiveType::kInt64:
      return std::string("int");
    case cel::PrimitiveType::kUint64:
      return std::string("uint");
    case cel::PrimitiveType::kDouble:
      return std::string("double");
    case cel::PrimitiveType::kString:
      return std::string("string");
    case cel::PrimitiveType::kBytes:
      return std::string("bytes");
    case cel::PrimitiveType::kPrimitiveTypeUnspecified:
      // The checker doesn't emit UNSPECIFIED for resolved types;
      // reaching here is an invariant violation upstream.
      ABSL_CHECK(false)
          << "InlineTypeIdentifierReferences: PrimitiveType is UNSPECIFIED";
  }
  ABSL_CHECK(false)
      << "InlineTypeIdentifierReferences: unhandled PrimitiveType="
      << static_cast<int>(p);
}

std::optional<std::string> WrapperTypeName(cel::PrimitiveType p) {
  switch (p) {
    case cel::PrimitiveType::kBool:
      return std::string("google.protobuf.BoolValue");
    case cel::PrimitiveType::kInt64:
      return std::string("google.protobuf.Int64Value");
    case cel::PrimitiveType::kUint64:
      return std::string("google.protobuf.UInt64Value");
    case cel::PrimitiveType::kDouble:
      return std::string("google.protobuf.DoubleValue");
    case cel::PrimitiveType::kString:
      return std::string("google.protobuf.StringValue");
    case cel::PrimitiveType::kBytes:
      return std::string("google.protobuf.BytesValue");
    case cel::PrimitiveType::kPrimitiveTypeUnspecified:
      ABSL_CHECK(false)
          << "InlineTypeIdentifierReferences: wrapper PrimitiveType "
             "is UNSPECIFIED";
  }
  ABSL_CHECK(false)
      << "InlineTypeIdentifierReferences: unhandled wrapper PrimitiveType="
      << static_cast<int>(p);
}

std::optional<std::string> WellKnownTypeName(cel::WellKnownTypeSpec w) {
  switch (w) {
    case cel::WellKnownTypeSpec::kAny:
      return std::string("google.protobuf.Any");
    case cel::WellKnownTypeSpec::kTimestamp:
      return std::string("google.protobuf.Timestamp");
    case cel::WellKnownTypeSpec::kDuration:
      return std::string("google.protobuf.Duration");
    case cel::WellKnownTypeSpec::kWellKnownTypeUnspecified:
      ABSL_CHECK(false)
          << "InlineTypeIdentifierReferences: WellKnownType is UNSPECIFIED";
  }
  ABSL_CHECK(false)
      << "InlineTypeIdentifierReferences: unhandled WellKnownType="
      << static_cast<int>(w);
}

std::optional<std::string> SpecTypeName(const cel::TypeSpec& inner) {
  if (inner.has_primitive()) return PrimitiveTypeName(inner.primitive());
  if (inner.has_wrapper()) return WrapperTypeName(inner.wrapper());
  if (inner.has_well_known()) return WellKnownTypeName(inner.well_known());
  if (inner.has_null()) return std::string("null_type");
  if (inner.has_message_type()) return std::string(inner.message_type().type());
  if (inner.has_list_type()) return std::string("list");
  if (inner.has_map_type()) return std::string("map");
  if (inner.has_type()) return std::string("type");
  if (inner.has_type_param()) {
    // `list` / `map` / `type` standalone idents wrap their abstract
    // `T` parameter through TypeType.  The outer-being-TypeType
    // means the value IS a type — render as `type` (the only
    // standalone ident whose body is a bare type-param).
    return std::string("type");
  }
  // Out-of-scope inner kinds (`function`, `error`, `dyn`,
  // `abstract_type`).  Don't rewrite; let the original kIdent
  // surface its own downstream diagnostic.
  return std::nullopt;
}

// Walk the AST and rewrite every `kIdentExpr` whose
// `reference_map` entry resolves to a checker-registered global
// type-name variable (`int`, `bool`, `<message-FQN>`, ...) to a
// `kConstantExpr` carrying `string_value = <spec type-name>`.
//
// Detection criterion: the `Reference` for the kIdent has NO
// `value()` set (`InlineConstantReferences` already handled the
// constant-value case — enum-name resolution), AND the node's
// checker-assigned type in `type_map` is `TypeType(<inner>)`
// (i.e. `has_type()` returns true on the outer TypeSpec).  These
// are precisely the type-identifier idents — the standard library
// registers them at `checker/standard_library.cc:799-829`.
//
// Downstream: PopulateAnnotations reads `type_map` and stamps the
// rewritten kConstant node with `Repr::kType` (already correct via
// the existing `ReprOf(TypeSpec)` path).  PackPass then uses the
// `Repr::kType` annotation to write a CEL_TYPE-kinded rodata
// CelValue (instead of the default CEL_STRING).  See
// `rewrite/m9-type-subsystem.md` §4.2 / §3.3.
//
// MUST run AFTER `InlineConstantReferences` — the constant-value
// rewrite happens first; type-ident rewriter sees only kIdent
// nodes whose Reference is value-less.
//
// Idempotent — running this on an AST without any type-ident
// idents is a no-op.
// Rewrite a single kIdent node into a kConstant carrying its spec
// type-name, if the reference/type maps identify it as a type-ident.
// Returns true if a rewrite happened (caller skips the children
// recursion).  Lifted out of `InlineTypeIdentifierReferences` to keep
// that function under the lint size gate.
bool MaybeRewriteTypeIdent(cel::Expr& expr, const cel::Ast::ReferenceMap& refs,
                           const cel::Ast::TypeMap& types) {
  if (!expr.has_ident_expr()) return false;
  auto refs_it = refs.find(expr.id());
  auto types_it = types.find(expr.id());
  if (refs_it == refs.end() || types_it == types.end()) return true;
  const cel::Reference& ref = refs_it->second;
  const cel::TypeSpec& outer = types_it->second;
  // Defensive: InlineConstantReferences already handled the
  // value-bearing path; assert we don't double-rewrite.
  if (ref.has_value()) return true;
  if (!outer.has_type()) return true;
  auto name = SpecTypeName(outer.type());
  if (!name.has_value()) return true;  // out-of-scope inner kind
  // Replace the kIdent with a kConstantExpr carrying the spec
  // type-name.  PopulateAnnotations + PackPass will see a kConstant
  // whose stamped Repr is kType (because the type_map entry is
  // unchanged — still TypeType(inner) — and ReprOf(TypeSpec) already
  // maps `has_type()` → Repr::kType).
  cel::Constant c;
  c.set_string_value(*std::move(name));
  expr.set_const_expr(std::move(c));
  return true;
}

void InlineTypeIdentifierReferences(cel::Ast& ast) {
  const cel::Ast::ReferenceMap& refs = ast.reference_map();
  const cel::Ast::TypeMap& types = ast.type_map();
  std::function<void(cel::Expr&)> visit = [&](cel::Expr& expr) {
    if (MaybeRewriteTypeIdent(expr, refs, types)) return;
    VisitInlineConstantChildren(expr, visit);
  };
  visit(ast.mutable_root_expr());
}

}  // namespace

absl::StatusOr<TypedAst> ParseAndCheck(absl::string_view expression,
                                       const CheckOptions& opts) {
  auto pool_bundle = LoadDescriptorPool(opts);
  if (!pool_bundle.ok()) return pool_bundle.status();

  auto builder = cel::CreateTypeCheckerBuilder(pool_bundle->pool);
  if (!builder.ok()) return builder.status();

  std::vector<Variable> variables;
  if (auto s = ConfigureCheckerBuilder(**builder, opts, pool_bundle->pool,
                                       variables);
      !s.ok()) {
    return s;
  }

  auto checker = (*builder)->Build();
  if (!checker.ok()) return checker.status();

  auto checked_ast = RunTypeCheck(**checker, expression, opts.description);
  if (!checked_ast.ok()) return checked_ast.status();

  // Inline `VariableReference::value()` constants (enum-name
  // references) into the AST as kConstant nodes — must run before
  // RejectDyn (which inspects every node's kind) and
  // PopulateAnnotations (which seeds Repr from the kind-stamped
  // type_map).
  InlineConstantReferences(**checked_ast);

  // Rewrite type-identifier idents (`int`, `bool`,
  // `<message-FQN>` standalone) to kConstantExpr nodes carrying
  // the spec type-name string.  MUST run AFTER
  // InlineConstantReferences (see `rewrite/m9-type-subsystem.md` §7 R2).
  InlineTypeIdentifierReferences(**checked_ast);

  // Static-subset gate: no DYN / ERROR / type-param / function / unset nodes.
  if (auto s = RejectDyn(**checked_ast); !s.ok()) return s;

  WasmAnnotations annotations;
  PopulateAnnotations(**checked_ast, pool_bundle->pool, annotations);

  return TypedAst(std::move(*checked_ast), std::move(annotations),
                  std::move(variables));
}

}  // namespace celwasm
