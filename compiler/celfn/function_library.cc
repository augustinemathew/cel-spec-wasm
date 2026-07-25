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

std::string CelfnType::Argkind() const {
  switch (kind) {
    case Kind::kBool:
      return "bool";
    case Kind::kInt:
      return "int";
    case Kind::kUint:
      return "uint";
    case Kind::kDouble:
      return "double";
    case Kind::kString:
      return "string";
    case Kind::kBytes:
      return "bytes";
    case Kind::kNull:
      return "null";
    case Kind::kDuration:
      return "duration";
    case Kind::kTimestamp:
      return "timestamp";
    case Kind::kList:
      return absl::StrCat("list_", list_element.empty()
                                       ? "unknown"
                                       : list_element[0].Argkind());
    case Kind::kMap:
      if (map_kv.size() != 2) return "map_unknown_unknown";
      return absl::StrCat("map_", map_kv[0].Argkind(), "_",
                          map_kv[1].Argkind());
    case Kind::kProto:
      return absl::StrCat("message_",
                          absl::StrReplaceAll(proto_fqn, {{".", "_"}}));
    case Kind::kType:
      return "type";
    case Kind::kOptional:
      return absl::StrCat("optional_", optional_element.empty()
                                           ? "unknown"
                                           : optional_element[0].Argkind());
  }
  return "unknown";
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

// Mirrors a structural-recursion check across CelfnType for the
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
bool MentionsOptional(const CelfnType& t) {
  if (t.kind == CelfnType::Kind::kOptional) return true;
  if (t.kind == CelfnType::Kind::kList && !t.list_element.empty()) {
    return MentionsOptional(t.list_element[0]);
  }
  if (t.kind == CelfnType::Kind::kMap && t.map_kv.size() == 2) {
    return MentionsOptional(t.map_kv[0]) || MentionsOptional(t.map_kv[1]);
  }
  return false;
}

// Mirrors MentionsOptional for kType — the CEL type-of-types is
// permanently out of scope as a plugin declarable shape.
// `type` Lift/Lower stays implemented in cel_plugin.cc because
// other kCelFn / kHost paths can still use it; only the plugin
// decl surface is closed.
bool MentionsType(const CelfnType& t) {
  if (t.kind == CelfnType::Kind::kType) return true;
  if (t.kind == CelfnType::Kind::kList && !t.list_element.empty()) {
    return MentionsType(t.list_element[0]);
  }
  if (t.kind == CelfnType::Kind::kMap && t.map_kv.size() == 2) {
    return MentionsType(t.map_kv[0]) || MentionsType(t.map_kv[1]);
  }
  if (t.kind == CelfnType::Kind::kOptional && !t.optional_element.empty()) {
    return MentionsType(t.optional_element[0]);
  }
  return false;
}

// langdef "Map type" restricts keys to {bool, int, uint, string}.
// Source-driven decls hit this at the grammar layer
// (ExtractType in this file's `mapType` arm, ~ line 322);
// programmatically-built decls (AddHost / AddPlugin /
// AddPlugin with a constructed CelfnType) bypass the grammar and
// would otherwise surface the error at first Eval (kPlugin
// via Lift) or at codegen / runtime (kHost via the trampoline).
// Catching it here names the offending decl at registration time.
bool IsLegalMapKeyKind(CelfnType::Kind k) {
  return k == CelfnType::Kind::kBool || k == CelfnType::Kind::kInt ||
         k == CelfnType::Kind::kUint || k == CelfnType::Kind::kString;
}

// Returns the first illegal map-key kind found inside `t`, or
// kBool (the canonical-legal sentinel) when no illegal key exists.
// Recurses through list element, map key+value, optional inner.
// Caller checks: `FirstIllegalMapKey(t).has_value()`.
std::optional<CelfnType::Kind> FirstIllegalMapKey(const CelfnType& t) {
  if (t.kind == CelfnType::Kind::kMap && t.map_kv.size() == 2) {
    if (!IsLegalMapKeyKind(t.map_kv[0].kind)) return t.map_kv[0].kind;
    if (auto k = FirstIllegalMapKey(t.map_kv[1]); k.has_value()) return k;
  }
  if (t.kind == CelfnType::Kind::kList && !t.list_element.empty()) {
    return FirstIllegalMapKey(t.list_element[0]);
  }
  if (t.kind == CelfnType::Kind::kOptional && !t.optional_element.empty()) {
    return FirstIllegalMapKey(t.optional_element[0]);
  }
  return std::nullopt;
}

absl::string_view MapKeyKindName(CelfnType::Kind k) {
  switch (k) {
    case CelfnType::Kind::kBool:
      return "bool";
    case CelfnType::Kind::kInt:
      return "int";
    case CelfnType::Kind::kUint:
      return "uint";
    case CelfnType::Kind::kDouble:
      return "double";
    case CelfnType::Kind::kString:
      return "string";
    case CelfnType::Kind::kBytes:
      return "bytes";
    case CelfnType::Kind::kNull:
      return "null";
    case CelfnType::Kind::kDuration:
      return "duration";
    case CelfnType::Kind::kTimestamp:
      return "timestamp";
    case CelfnType::Kind::kList:
      return "list";
    case CelfnType::Kind::kMap:
      return "map";
    case CelfnType::Kind::kProto:
      return "proto";
    case CelfnType::Kind::kType:
      return "type";
    case CelfnType::Kind::kOptional:
      return "optional";
  }
  return "unknown";
}

std::string SynthesiseOverloadId(absl::string_view fn_name,
                                 const std::vector<CelfnParam>& params) {
  std::vector<std::string> parts;
  parts.emplace_back(fn_name);
  for (const auto& p : params) {
    parts.push_back(p.type.Argkind());
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
    absl::string_view fn_name, CelfnType return_type,
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
    absl::string_view fn_name, CelfnType return_type,
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
    absl::string_view fn_name, CelfnType return_type,
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
        "`", d.fn_name, "` return type contains map<", MapKeyKindName(*bad),
        ", ...> — map keys must be bool|int|uint|string (langdef)"));
  }
  for (const auto& p : d.params) {
    if (auto bad = FirstIllegalMapKey(p.type); bad.has_value()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "`", d.fn_name, "` parameter `", p.name, "` contains map<",
          MapKeyKindName(*bad),
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
    return absl::InvalidArgumentError(absl::StrCat(
        "plugin `", d.fn_name,
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
      return absl::InvalidArgumentError(absl::StrCat(
          "plugin `", d.fn_name, "` parameter `", p.name,
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

absl::StatusOr<CelfnType> ExtractType(celfn::CelfnParser::TypeContext* ctx);

absl::StatusOr<CelfnType::Kind> KindFromPrimitiveText(absl::string_view txt) {
  if (txt == "bool") return CelfnType::Kind::kBool;
  if (txt == "int") return CelfnType::Kind::kInt;
  if (txt == "uint") return CelfnType::Kind::kUint;
  if (txt == "double") return CelfnType::Kind::kDouble;
  if (txt == "string") return CelfnType::Kind::kString;
  if (txt == "bytes") return CelfnType::Kind::kBytes;
  return absl::InternalError(absl::StrCat("unknown primitive type: ", txt));
}

absl::StatusOr<CelfnType::Kind> KindFromWktText(absl::string_view txt) {
  if (txt == "Duration") return CelfnType::Kind::kDuration;
  if (txt == "Timestamp") return CelfnType::Kind::kTimestamp;
  return absl::InternalError(absl::StrCat("unknown wkt keyword: ", txt));
}

absl::StatusOr<CelfnType::Kind> KindFromMapKeyText(absl::string_view txt) {
  if (txt == "bool") return CelfnType::Kind::kBool;
  if (txt == "int") return CelfnType::Kind::kInt;
  if (txt == "uint") return CelfnType::Kind::kUint;
  if (txt == "string") return CelfnType::Kind::kString;
  return absl::InvalidArgumentError(absl::StrCat(
      "map key type `", txt, "` not allowed (must be bool|int|uint|string)"));
}

absl::StatusOr<CelfnType> ExtractListType(
    celfn::CelfnParser::TypeContext* ctx) {
  CelfnType t{};
  t.kind = CelfnType::Kind::kList;
  auto inner = ExtractType(ctx->listType()->type());
  if (!inner.ok()) return inner.status();
  t.list_element.push_back(std::move(*inner));
  return t;
}

absl::StatusOr<CelfnType> ExtractMapType(celfn::CelfnParser::TypeContext* ctx) {
  CelfnType t{};
  t.kind = CelfnType::Kind::kMap;
  auto k = KindFromMapKeyText(ctx->mapType()->mapKeyType()->getText());
  if (!k.ok()) return k.status();
  CelfnType key{};
  key.kind = *k;
  auto v = ExtractType(ctx->mapType()->type());
  if (!v.ok()) return v.status();
  t.map_kv.push_back(std::move(key));
  t.map_kv.push_back(std::move(*v));
  return t;
}

absl::StatusOr<CelfnType> ExtractType(celfn::CelfnParser::TypeContext* ctx) {
  if (ctx == nullptr) {
    return absl::InternalError("ExtractType called with nullptr context");
  }
  CelfnType t{};
  if (ctx->primitiveType() != nullptr) {
    auto k = KindFromPrimitiveText(ctx->primitiveType()->getText());
    if (!k.ok()) return k.status();
    t.kind = *k;
    return t;
  }
  if (ctx->wktKeyword() != nullptr) {
    auto k = KindFromWktText(ctx->wktKeyword()->getText());
    if (!k.ok()) return k.status();
    t.kind = *k;
    return t;
  }
  if (ctx->listType() != nullptr) return ExtractListType(ctx);
  if (ctx->mapType() != nullptr) return ExtractMapType(ctx);
  if (ctx->protoType() != nullptr) {
    t.kind = CelfnType::Kind::kProto;
    t.proto_fqn = ctx->protoType()->qualifiedIdentifier()->getText();
    return t;
  }
  if (ctx->getText() == "null") {
    t.kind = CelfnType::Kind::kNull;
    return t;
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
  return absl::StrCat(
      "cel:", module_name.empty() ? absl::string_view("customfn")
                                  : module_name);
}

std::string DeriveWitInterface(absl::string_view module_name) {
  return absl::StrCat(DeriveWitPackageName(module_name), "/fns@",
                      kWitPackageVersion);
}

}  // namespace celwasm
