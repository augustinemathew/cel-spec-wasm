#include "compiler/celfn/function_library.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "antlr4-runtime.h"
#include "compiler/celfn/CelfnLexer.h"
#include "compiler/celfn/CelfnParser.h"

namespace celwasm {

namespace celfn = celwasm_celfn;

// ── Argkind synthesis ────────────────────────────────────────────────

std::string ArgkindSlug(const CelType& type) {
  switch (type.kind()) {
    case CelType::Kind::kBool:
      return "bool";
    case CelType::Kind::kInt:
      return "int";
    case CelType::Kind::kUint:
      return "uint";
    case CelType::Kind::kDouble:
      return "double";
    case CelType::Kind::kString:
      return "string";
    case CelType::Kind::kBytes:
      return "bytes";
    case CelType::Kind::kNull:
      return "null";
    case CelType::Kind::kDuration:
      return "duration";
    case CelType::Kind::kTimestamp:
      return "timestamp";
    case CelType::Kind::kList:
      return absl::StrCat("list_", ArgkindSlug(type.list_element()));
    case CelType::Kind::kMap:
      return absl::StrCat("map_", ArgkindSlug(type.map_key()), "_",
                          ArgkindSlug(type.map_value()));
    case CelType::Kind::kMessage:
      return absl::StrCat(
          "message_", absl::StrReplaceAll(type.message_fully_qualified_name(),
                                          {{".", "_"}}));
    case CelType::Kind::kType:
      return "type";
    case CelType::Kind::kOptional:
      return absl::StrCat("optional_", ArgkindSlug(type.optional_element()));
    case CelType::Kind::kUnknown:
      break;
  }
  ABSL_CHECK(false) << "ArgkindSlug: kUnknown CelType (default-constructed "
                       "sentinel) has no argkind slug";
  return "";
}

// ── Backend spelling ─────────────────────────────────────────────────

absl::string_view BackendPrefix(CelfnDecl::Backend backend) {
  switch (backend) {
    case CelfnDecl::Backend::kHost:
      return "@host.";
    case CelfnDecl::Backend::kCelDefined:
      return "@native.";
    case CelfnDecl::Backend::kPlugin:
      return "@plugin.";
  }
  ABSL_CHECK(false) << "BackendPrefix: unhandled CelfnDecl::Backend = "
                    << static_cast<int>(backend);
  return "";
}

namespace {

// Mirrors a structural-recursion check across CelType for the
// permanently-out-of-scope kinds (`optional<T>` and `type`).  Used by
// Build() to
// reject kPlugin decls whose return / any param shape
// contains either — `optional<T>` and `type` are permanently out of
// scope as plugin declarable shapes (user direction; see
// m24 §14 "Permanently out of scope, not deferred").  Catching the
// violation at Build() turns the failure from a runtime
// kInvalidArgument deep inside Lift into a compile-time refusal at
// `Compiler::Builder::DeclareFunctions` time, with the offending decl named.  CEL
// `null` (kNull) is a distinct kind and stays supported — see the
// kNull arm in eval/internal/cel_plugin.cc.
bool MentionsOptional(const CelType& t) {
  if (t.kind() == CelType::Kind::kOptional) return true;
  if (t.kind() == CelType::Kind::kList) {
    return MentionsOptional(t.list_element());
  }
  if (t.kind() == CelType::Kind::kMap) {
    return MentionsOptional(t.map_key()) || MentionsOptional(t.map_value());
  }
  return false;
}

// Mirrors MentionsOptional for kType — the CEL type-of-types is
// permanently out of scope as a plugin declarable shape.
// `type` Lift/Lower stays implemented in cel_plugin.cc because
// other kCelFn / kHost paths can still use it; only the plugin
// decl surface is closed.
bool MentionsType(const CelType& t) {
  if (t.kind() == CelType::Kind::kType) return true;
  if (t.kind() == CelType::Kind::kList) {
    return MentionsType(t.list_element());
  }
  if (t.kind() == CelType::Kind::kMap) {
    return MentionsType(t.map_key()) || MentionsType(t.map_value());
  }
  if (t.kind() == CelType::Kind::kOptional) {
    return MentionsType(t.optional_element());
  }
  return false;
}

// langdef "Map type" restricts keys to {bool, int, uint, string}.
// Source-driven decls hit this at the grammar layer
// (ExtractType in this file's `mapType` arm);
// programmatically-built decls (AddHost / AddPlugin /
// AddPlugin with a constructed CelType) bypass the grammar and
// would otherwise surface the error at first Eval (kPlugin
// via Lift) or at codegen / runtime (kHost via the trampoline).
// Catching it here names the offending decl at registration time.
bool IsLegalMapKeyKind(CelType::Kind k) {
  return k == CelType::Kind::kBool || k == CelType::Kind::kInt ||
         k == CelType::Kind::kUint || k == CelType::Kind::kString;
}

// Returns the first illegal map-key kind found inside `t`, or
// nullopt when no illegal key exists.  Recurses through list
// element, map key+value, optional inner.
// Caller checks: `FirstIllegalMapKey(t).has_value()`.
std::optional<CelType::Kind> FirstIllegalMapKey(const CelType& t) {
  if (t.kind() == CelType::Kind::kMap) {
    if (!IsLegalMapKeyKind(t.map_key().kind())) return t.map_key().kind();
    if (auto k = FirstIllegalMapKey(t.map_value()); k.has_value()) return k;
  }
  if (t.kind() == CelType::Kind::kList) {
    return FirstIllegalMapKey(t.list_element());
  }
  if (t.kind() == CelType::Kind::kOptional) {
    return FirstIllegalMapKey(t.optional_element());
  }
  return std::nullopt;
}

std::string SynthesiseOverloadId(absl::string_view fn_name,
                                 const std::vector<CelfnParam>& params) {
  std::vector<std::string> parts;
  parts.emplace_back(fn_name);
  for (const auto& p : params) {
    parts.push_back(ArgkindSlug(p.type));
  }
  return absl::StrJoin(parts, "_");
}

absl::Status ValidateThisPlacement(absl::string_view fn_name,
                                   const std::vector<CelfnParam>& params) {
  for (size_t i = 1; i < params.size(); ++i) {
    if (params[i].is_receiver) {
      return absl::InvalidArgumentError(absl::StrCat(
          "`", fn_name,
          "`: `this` modifier is only allowed on the first parameter"));
    }
  }
  return absl::OkStatus();
}

void Finalise(CelfnDecl& d) {
  d.is_receiver = !d.params.empty() && d.params[0].is_receiver;
  d.overload_id = SynthesiseOverloadId(d.fn_name, d.params);
  d.num_args = static_cast<uint8_t>(d.params.size()) + 1u;
}

}  // namespace

// ── FunctionLibrary::Builder ────────────────────────────────────────

FunctionLibrary::Builder& FunctionLibrary::Builder::SetModuleName(
    absl::string_view module_name) {
  module_name_ = std::string(module_name);
  return *this;
}

FunctionLibrary::Builder& FunctionLibrary::Builder::AddHost(
    absl::string_view fn_name, CelType return_type,
    std::vector<CelfnParam> params) {
  CelfnDecl d{};
  d.backend = CelfnDecl::Backend::kHost;
  d.fn_name = std::string(fn_name);
  d.module_name = "cel_fn";
  d.return_type = std::move(return_type);
  d.params = std::move(params);
  Finalise(d);
  decls_.push_back(std::move(d));
  return *this;
}

FunctionLibrary::Builder& FunctionLibrary::Builder::AddPlugin(
    absl::string_view fn_name, CelType return_type,
    std::vector<CelfnParam> params) {
  CelfnDecl d{};
  d.backend = CelfnDecl::Backend::kPlugin;
  d.fn_name = std::string(fn_name);
  // Dispatch path is shared with @host (m24 §2: a plugin fn is a host
  // fn at the call site).  The wasm `(import "cel_fn" "<helper>" …)`
  // shape is therefore identical; plugin-ness is invisible to the
  // codegen / overload table / checker.
  d.module_name = "cel_fn";
  d.return_type = std::move(return_type);
  d.params = std::move(params);
  Finalise(d);
  decls_.push_back(std::move(d));
  return *this;
}

FunctionLibrary::Builder& FunctionLibrary::Builder::AddCelDefined(
    absl::string_view fn_name, CelType return_type,
    std::vector<CelfnParam> params, absl::string_view body) {
  CelfnDecl d{};
  d.backend = CelfnDecl::Backend::kCelDefined;
  d.fn_name = std::string(fn_name);
  // module_name resolved at Build() time once SetModuleName has been
  // applied; the Builder doesn't have it yet here.
  d.return_type = std::move(return_type);
  d.params = std::move(params);
  d.body = std::string(body);
  Finalise(d);
  decls_.push_back(std::move(d));
  return *this;
}

// Decl-shape gates that apply uniformly across every backend:
//   - langdef map-key restriction (FirstIllegalMapKey, return + params).
namespace {

absl::Status CheckUniversalDeclShape(const CelfnDecl& d) {
  if (auto bad = FirstIllegalMapKey(d.return_type); bad.has_value()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "`", d.fn_name, "` return type contains map<", CelTypeKindName(*bad),
        ", ...> — map keys must be bool|int|uint|string (langdef)"));
  }
  for (const auto& p : d.params) {
    if (auto bad = FirstIllegalMapKey(p.type); bad.has_value()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "`", d.fn_name, "` parameter `", p.name, "` contains map<",
          CelTypeKindName(*bad),
          ", ...> — map keys must be bool|int|uint|string (langdef)"));
    }
  }
  return absl::OkStatus();
}

// kPlugin permanently-out-of-scope rule (m24 §14): the
// foreign-fn author surface does not accept `optional<T>` or `type`
// as declarable shapes.  CEL `null` (kNull) stays supported.
absl::Status CheckPluginDeclShape(const CelfnDecl& d) {
  if (MentionsOptional(d.return_type)) {
    return absl::InvalidArgumentError(
        absl::StrCat("plugin `", d.fn_name,
                     "` has an optional<...> return type — optional<T> is not "
                     "supported as a plugin argument or return shape"));
  }
  if (MentionsType(d.return_type)) {
    return absl::InvalidArgumentError(
        absl::StrCat("plugin `", d.fn_name,
                     "` has a `type` return — `type` is not supported as a "
                     "plugin argument or return shape"));
  }
  for (const auto& p : d.params) {
    if (MentionsOptional(p.type)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "plugin `", d.fn_name, "` parameter `", p.name,
          "` is `optional<...>` — optional<T> is not supported as a "
          "plugin argument or return shape"));
    }
    if (MentionsType(p.type)) {
      return absl::InvalidArgumentError(
          absl::StrCat("plugin `", d.fn_name, "` parameter `", p.name,
                       "` is `type` — `type` is not supported as a "
                       "plugin argument or return shape"));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<FunctionLibrary> FunctionLibrary::Builder::Build() {
  bool has_cel_defined = false;
  for (auto& d : decls_) {
    if (d.backend == CelfnDecl::Backend::kCelDefined) {
      has_cel_defined = true;
      d.module_name = module_name_;
    }
  }

  if (has_cel_defined && module_name_.empty()) {
    return absl::InvalidArgumentError(
        "library contains CEL-defined functions but no module name was set "
        "(call SetModuleName() before Build())");
  }

  absl::flat_hash_set<std::string> seen_overload_ids;
  for (const auto& d : decls_) {
    if (auto s = ValidateThisPlacement(d.fn_name, d.params); !s.ok()) {
      return s;
    }
    if (!seen_overload_ids.insert(d.overload_id).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate declaration: overload-id `", d.overload_id,
                       "` already declared in this library"));
    }
    if (auto s = CheckUniversalDeclShape(d); !s.ok()) return s;
    if (d.backend == CelfnDecl::Backend::kPlugin) {
      if (auto s = CheckPluginDeclShape(d); !s.ok()) return s;
    }
  }

  FunctionLibrary lib;
  lib.module_name_ = std::move(module_name_);
  lib.wit_interface_ = std::move(wit_interface_);
  lib.decls_ = std::move(decls_);
  return lib;
}

FunctionLibrary::Builder& FunctionLibrary::Builder::SetWitInterface(
    absl::string_view wit_interface) {
  wit_interface_ = std::string(wit_interface);
  return *this;
}

// ── ParseCelfnSource ────────────────────────────────────────────────

namespace {

class CollectingErrorListener : public antlr4::BaseErrorListener {
 public:
  void syntaxError(antlr4::Recognizer* /*recognizer*/,
                   antlr4::Token* /*offending_symbol*/, size_t line,
                   size_t column, const std::string& msg,
                   std::exception_ptr /*e*/) override {
    errors_.push_back(absl::StrCat("line ", line, ":", column, " ", msg));
  }

  const std::vector<std::string>& errors() const {
    return errors_;
  }

 private:
  std::vector<std::string> errors_;
};

absl::StatusOr<CelType> ExtractType(celfn::CelfnParser::TypeContext* ctx);

absl::StatusOr<CelType> TypeFromPrimitiveText(absl::string_view txt) {
  if (txt == "bool") return CelType::Bool();
  if (txt == "int") return CelType::Int();
  if (txt == "uint") return CelType::Uint();
  if (txt == "double") return CelType::Double();
  if (txt == "string") return CelType::String();
  if (txt == "bytes") return CelType::Bytes();
  return absl::InternalError(absl::StrCat("unknown primitive type: ", txt));
}

absl::StatusOr<CelType> TypeFromWktText(absl::string_view txt) {
  if (txt == "Duration") return CelType::Duration();
  if (txt == "Timestamp") return CelType::Timestamp();
  return absl::InternalError(absl::StrCat("unknown wkt keyword: ", txt));
}

absl::StatusOr<CelType> TypeFromMapKeyText(absl::string_view txt) {
  if (txt == "bool") return CelType::Bool();
  if (txt == "int") return CelType::Int();
  if (txt == "uint") return CelType::Uint();
  if (txt == "string") return CelType::String();
  return absl::InvalidArgumentError(absl::StrCat(
      "map key type `", txt, "` not allowed (must be bool|int|uint|string)"));
}

absl::StatusOr<CelType> ExtractListType(celfn::CelfnParser::TypeContext* ctx) {
  auto inner = ExtractType(ctx->listType()->type());
  if (!inner.ok()) return inner.status();
  return CelType::List(*std::move(inner));
}

absl::StatusOr<CelType> ExtractMapType(celfn::CelfnParser::TypeContext* ctx) {
  auto k = TypeFromMapKeyText(ctx->mapType()->mapKeyType()->getText());
  if (!k.ok()) return k.status();
  auto v = ExtractType(ctx->mapType()->type());
  if (!v.ok()) return v.status();
  return CelType::Map(*std::move(k), *std::move(v));
}

absl::StatusOr<CelType> ExtractType(celfn::CelfnParser::TypeContext* ctx) {
  if (ctx == nullptr) {
    return absl::InternalError("ExtractType called with nullptr context");
  }
  if (ctx->primitiveType() != nullptr) {
    return TypeFromPrimitiveText(ctx->primitiveType()->getText());
  }
  if (ctx->wktKeyword() != nullptr) {
    return TypeFromWktText(ctx->wktKeyword()->getText());
  }
  if (ctx->listType() != nullptr) return ExtractListType(ctx);
  if (ctx->mapType() != nullptr) return ExtractMapType(ctx);
  if (ctx->protoType() != nullptr) {
    return CelType::Message(ctx->protoType()->qualifiedIdentifier()->getText());
  }
  if (ctx->getText() == "null") {
    return CelType::Null();
  }
  return absl::InternalError(absl::StrCat("unhandled type: ", ctx->getText()));
}

absl::StatusOr<std::vector<CelfnParam>> ExtractParams(
    celfn::CelfnParser::ParamsContext* ctx) {
  std::vector<CelfnParam> params;
  if (ctx == nullptr) return params;
  for (size_t i = 0; i < ctx->param().size(); ++i) {
    auto* p_ctx = ctx->param(i);
    CelfnParam p{};
    if (!p_ctx->children.empty()) {
      auto* first = p_ctx->children[0];
      if (dynamic_cast<antlr4::tree::TerminalNode*>(first) != nullptr &&
          first->getText() == "this") {
        p.is_receiver = true;
        if (i != 0) {
          return absl::InvalidArgumentError(
              "`this` modifier is only allowed on the first parameter");
        }
      }
    }
    auto t = ExtractType(p_ctx->type());
    if (!t.ok()) return t.status();
    p.type = std::move(*t);
    p.name = p_ctx->Identifier()->getText();
    params.push_back(std::move(p));
  }
  return params;
}

}  // namespace

absl::StatusOr<FunctionLibrary> ParseCelfnSource(absl::string_view source) {
  antlr4::ANTLRInputStream input{std::string(source)};
  celfn::CelfnLexer lexer{&input};
  antlr4::CommonTokenStream tokens{&lexer};
  celfn::CelfnParser parser{&tokens};

  CollectingErrorListener errors;
  lexer.removeErrorListeners();
  lexer.addErrorListener(&errors);
  parser.removeErrorListeners();
  parser.addErrorListener(&errors);

  auto* file_ctx = parser.file();
  if (!errors.errors().empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("parse error: ", absl::StrJoin(errors.errors(), "; ")));
  }

  FunctionLibrary::Builder b;
  if (file_ctx->moduleDirective() != nullptr) {
    b.SetModuleName(file_ctx->moduleDirective()->Identifier()->getText());
  }

  for (auto* item : file_ctx->fileItem()) {
    if (auto* host = item->hostFnDecl(); host != nullptr) {
      auto rt = ExtractType(host->type());
      if (!rt.ok()) return rt.status();
      auto ps = ExtractParams(host->params());
      if (!ps.ok()) return ps.status();
      b.AddHost(host->Identifier()->getText(), std::move(*rt), std::move(*ps));
    } else if (auto* comp = item->pluginFnDecl(); comp != nullptr) {
      auto rt = ExtractType(comp->type());
      if (!rt.ok()) return rt.status();
      auto ps = ExtractParams(comp->params());
      if (!ps.ok()) return ps.status();
      b.AddPlugin(comp->Identifier()->getText(), std::move(*rt),
                  std::move(*ps));
    } else if (auto* bare = item->bareHostDecl(); bare != nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("`host` is a reserved alias; use `@host.",
                       bare->Identifier()->getText(), "(…)` instead of `host.",
                       bare->Identifier()->getText(), "(…)`"));
    } else if (auto* def = item->nativeFnDecl(); def != nullptr) {
      auto rt = ExtractType(def->type());
      if (!rt.ok()) return rt.status();
      auto ps = ExtractParams(def->params());
      if (!ps.ok()) return ps.status();
      const std::string body = std::string(
          absl::StripAsciiWhitespace(def->celExprBody()->getText()));
      b.AddCelDefined(def->Identifier()->getText(), std::move(*rt),
                      std::move(*ps), body);
    } else {
      return absl::InternalError("fileItem matched no expected alternative");
    }
  }

  return std::move(b).Build();
}

// ── WIT name derivation ─────────────────────────────────────────────

std::string DeriveWitPackageName(absl::string_view module_name) {
  return absl::StrCat("cel:", module_name.empty()
                                  ? absl::string_view("customfn")
                                  : module_name);
}

std::string DeriveWitInterface(absl::string_view module_name) {
  return absl::StrCat(DeriveWitPackageName(module_name), "/fns@",
                      kWitPackageVersion);
}

}  // namespace celwasm
