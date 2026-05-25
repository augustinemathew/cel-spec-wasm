#include "compiler/celfn/function_library.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
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
  }
  return "unknown";
}

namespace {

bool MentionsProto(const CelfnType& t) {
  if (t.kind == CelfnType::Kind::kProto) return true;
  if (t.kind == CelfnType::Kind::kList && !t.list_element.empty()) {
    return MentionsProto(t.list_element[0]);
  }
  if (t.kind == CelfnType::Kind::kMap && t.map_kv.size() == 2) {
    return MentionsProto(t.map_kv[0]) || MentionsProto(t.map_kv[1]);
  }
  return false;
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

FunctionLibrary::Builder& FunctionLibrary::Builder::AddForeign(
    absl::string_view alias, absl::string_view fn_name, CelfnType return_type,
    std::vector<CelfnParam> params) {
  CelfnDecl d{};
  d.backend = CelfnDecl::Backend::kForeign;
  d.fn_name = std::string(fn_name);
  d.module_name = std::string(alias);
  d.return_type = std::move(return_type);
  d.params = std::move(params);
  Finalise(d);
  bool seen = false;
  for (const auto& a : foreign_aliases_) {
    if (a == alias) {
      seen = true;
      break;
    }
  }
  if (!seen) foreign_aliases_.emplace_back(alias);
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
    if (d.backend == CelfnDecl::Backend::kForeign) {
      if (MentionsProto(d.return_type)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "foreign-backed `", d.module_name, ".", d.fn_name,
            "` has a proto(...) return type — proto messages may not "
            "cross the foreign-wasm boundary in v1; see §4.5.1 of "
            "m13-custom-fns.md"));
      }
      for (const auto& p : d.params) {
        if (MentionsProto(p.type)) {
          return absl::InvalidArgumentError(absl::StrCat(
              "foreign-backed `", d.module_name, ".", d.fn_name,
              "` has a proto(...) parameter — proto messages may not "
              "cross the foreign-wasm boundary in v1; see §4.5.1 of "
              "m13-custom-fns.md"));
        }
      }
    }
  }

  if (!module_name_.empty()) {
    for (const auto& a : foreign_aliases_) {
      if (a == module_name_) {
        return absl::InvalidArgumentError(absl::StrCat(
            "foreign alias `", a, "` collides with the library's module name"));
      }
    }
  }

  FunctionLibrary lib;
  lib.module_name_ = std::move(module_name_);
  lib.decls_ = std::move(decls_);
  lib.foreign_aliases_ = std::move(foreign_aliases_);
  return lib;
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

absl::StatusOr<CelfnType> ExtractType(celfn::CelfnParser::TypeContext* ctx) {
  CelfnType t{};
  if (ctx == nullptr) {
    return absl::InternalError("ExtractType called with nullptr context");
  }
  if (ctx->primitiveType() != nullptr) {
    const std::string txt = ctx->primitiveType()->getText();
    if (txt == "bool")
      t.kind = CelfnType::Kind::kBool;
    else if (txt == "int")
      t.kind = CelfnType::Kind::kInt;
    else if (txt == "uint")
      t.kind = CelfnType::Kind::kUint;
    else if (txt == "double")
      t.kind = CelfnType::Kind::kDouble;
    else if (txt == "string")
      t.kind = CelfnType::Kind::kString;
    else if (txt == "bytes")
      t.kind = CelfnType::Kind::kBytes;
    else
      return absl::InternalError(absl::StrCat("unknown primitive type: ", txt));
    return t;
  }
  if (ctx->wktKeyword() != nullptr) {
    const std::string txt = ctx->wktKeyword()->getText();
    if (txt == "Duration")
      t.kind = CelfnType::Kind::kDuration;
    else if (txt == "Timestamp")
      t.kind = CelfnType::Kind::kTimestamp;
    else
      return absl::InternalError(absl::StrCat("unknown wkt keyword: ", txt));
    return t;
  }
  if (ctx->listType() != nullptr) {
    t.kind = CelfnType::Kind::kList;
    auto inner = ExtractType(ctx->listType()->type());
    if (!inner.ok()) return inner.status();
    t.list_element.push_back(std::move(*inner));
    return t;
  }
  if (ctx->mapType() != nullptr) {
    t.kind = CelfnType::Kind::kMap;
    CelfnType k{};
    const std::string ktxt = ctx->mapType()->mapKeyType()->getText();
    if (ktxt == "bool")
      k.kind = CelfnType::Kind::kBool;
    else if (ktxt == "int")
      k.kind = CelfnType::Kind::kInt;
    else if (ktxt == "uint")
      k.kind = CelfnType::Kind::kUint;
    else if (ktxt == "string")
      k.kind = CelfnType::Kind::kString;
    else
      return absl::InvalidArgumentError(
          absl::StrCat("map key type `", ktxt,
                       "` not allowed (must be bool|int|uint|string)"));
    auto v = ExtractType(ctx->mapType()->type());
    if (!v.ok()) return v.status();
    t.map_kv.push_back(std::move(k));
    t.map_kv.push_back(std::move(*v));
    return t;
  }
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
    } else if (auto* fgn = item->foreignFnDecl(); fgn != nullptr) {
      const std::string alias = fgn->Identifier(0)->getText();
      if (alias == "host") {
        return absl::InvalidArgumentError(absl::StrCat(
            "`host` is a reserved alias; use `@host.",
            fgn->Identifier(1)->getText(), "(…)` instead of `host.",
            fgn->Identifier(1)->getText(), "(…)`"));
      }
      auto rt = ExtractType(fgn->type());
      if (!rt.ok()) return rt.status();
      auto ps = ExtractParams(fgn->params());
      if (!ps.ok()) return ps.status();
      b.AddForeign(alias, fgn->Identifier(1)->getText(), std::move(*rt),
                   std::move(*ps));
    } else if (auto* bare = item->bareHostDecl(); bare != nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("`host` is a reserved alias; use `@host.",
                       bare->Identifier()->getText(), "(…)` instead of `host.",
                       bare->Identifier()->getText(), "(…)`"));
    } else if (auto* def = item->celFnDef(); def != nullptr) {
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

}  // namespace celwasm
