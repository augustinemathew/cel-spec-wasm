#include "bindings/c/cel_capi.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "shared/type.h"

namespace {

using ::celwasm::CelType;

// ---- absl::Status -> CelStatus + out_err -----------------------------------

CelStatus ToCelStatus(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kOk:
      return CEL_STATUS_OK;
    case absl::StatusCode::kInvalidArgument:
      return CEL_STATUS_INVALID_ARGUMENT;
    case absl::StatusCode::kUnimplemented:
      return CEL_STATUS_UNIMPLEMENTED;
    default:
      // Every other pipeline failure (FailedPrecondition, Internal,
      // …) is a compiler-internal invariant a C caller can't act on
      // beyond surfacing the message.
      return CEL_STATUS_INTERNAL;
  }
}

// Heap-copy `s` into a fresh NUL-terminated C string ownable by the
// caller via `cel_free` (which is plain `free`).  Returns null on
// allocation failure (the caller then sees a non-null status with a
// null diagnostic — acceptable degradation).
char* DupCString(absl::string_view s) {
  // malloc — not a container — because the C ABI contract is that the
  // caller frees with cel_free (plain free), which crosses the C
  // boundary where a smart pointer cannot.
  char* out = static_cast<char*>(
      std::malloc(s.size() + 1));  // NOLINT(cppcoreguidelines-no-malloc)
  if (out == nullptr) return nullptr;
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

// Write a diagnostic into `*out_err` (if the caller wants one) and
// return the mapped status code.  Centralises the status->C-ABI
// mapping so every failure path is uniform.
CelStatus Fail(const absl::Status& status, char** out_err) {
  if (out_err != nullptr) {
    *out_err = DupCString(status.message());
  }
  return ToCelStatus(status.code());
}

// ---- Type-spec parser (declaration-only) -----------------------------------
//
// Mirrors the `name:Type` type grammar that the `cel` CLI's `--var`
// flag and the compiler's variable declarations share
// (tools/cel/var_parser.cc).  This is the declaration half only —
// no value literals, so it carries no `eval:value` / protobuf
// dependency; the C ABI declares types, the eval binding binds
// values.
//
//   SCALAR ∈ { bool, int, uint, double, string, bytes, duration,
//              timestamp }
//   list< T >        map< K, V >        F.Q.N (message)

struct TypeCursor {
  absl::string_view src;
  size_t pos = 0;

  bool Eof() const {
    return pos >= src.size();
  }
  void Skip() {
    while (!Eof() &&
           absl::ascii_isspace(static_cast<unsigned char>(src[pos]))) {
      ++pos;
    }
  }
  bool ConsumeChar(char c) {
    Skip();
    if (Eof() || src[pos] != c) return false;
    ++pos;
    return true;
  }
  absl::Status Expect(char c) {
    if (ConsumeChar(c)) return absl::OkStatus();
    return absl::InvalidArgumentError(absl::StrCat(
        "expected '", std::string(1, c), "' at offset ", pos, " in: ", src));
  }
};

absl::StatusOr<CelType> ParseTypeRec(TypeCursor& c);

absl::StatusOr<std::string> ParseIdent(TypeCursor& c) {
  c.Skip();
  const size_t start = c.pos;
  while (!c.Eof()) {
    const char ch = c.src[c.pos];
    if (absl::ascii_isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
        ch == '.') {
      ++c.pos;
    } else {
      break;
    }
  }
  if (c.pos == start) {
    return absl::InvalidArgumentError(
        absl::StrCat("expected type identifier at offset ", c.pos,
                     " in type spec: ", c.src));
  }
  return std::string(c.src.substr(start, c.pos - start));
}

absl::StatusOr<CelType> ParseListT(TypeCursor& c) {
  if (auto s = c.Expect('<'); !s.ok()) return s;
  auto e = ParseTypeRec(c);
  if (!e.ok()) return e.status();
  if (auto s = c.Expect('>'); !s.ok()) return s;
  return CelType::List(*e);
}

absl::StatusOr<CelType> ParseMapT(TypeCursor& c) {
  if (auto s = c.Expect('<'); !s.ok()) return s;
  auto k = ParseTypeRec(c);
  if (!k.ok()) return k.status();
  if (auto s = c.Expect(','); !s.ok()) return s;
  auto v = ParseTypeRec(c);
  if (!v.ok()) return v.status();
  if (auto s = c.Expect('>'); !s.ok()) return s;
  return CelType::Map(*k, *v);
}

absl::StatusOr<CelType> ParseTypeRec(TypeCursor& c) {
  auto name = ParseIdent(c);
  if (!name.ok()) return name.status();
  const std::string& n = *name;
  if (n == "bool") return CelType::Bool();
  if (n == "int") return CelType::Int();
  if (n == "uint") return CelType::Uint();
  if (n == "double") return CelType::Double();
  if (n == "string") return CelType::String();
  if (n == "bytes") return CelType::Bytes();
  if (n == "duration") return CelType::Duration();
  if (n == "timestamp") return CelType::Timestamp();
  if (n == "list") return ParseListT(c);
  if (n == "map") return ParseMapT(c);
  // Anything else is a message FQN; the checker rejects it at compile
  // time if no such type exists.
  return CelType::Message(n);
}

absl::StatusOr<CelType> ParseTypeSpec(absl::string_view spec) {
  TypeCursor c{spec};
  auto t = ParseTypeRec(c);
  if (!t.ok()) return t.status();
  c.Skip();
  if (!c.Eof()) {
    return absl::InvalidArgumentError(
        absl::StrCat("unexpected trailing characters in type spec at offset ",
                     c.pos, ": `", spec, "`"));
  }
  return t;
}

// Heap-copy `bytes` into a fresh buffer the caller owns via
// `cel_free`.  Returns null on allocation failure.  malloc — not a
// std::vector — because the C ABI contract is that the caller frees
// with `cel_free` (plain `free`), which a vector's allocator
// can't guarantee.
uint8_t* DupBytes(absl::Span<const uint8_t> bytes) {
  // malloc — not a std::vector — because the C ABI contract is that the
  // caller frees with cel_free (plain free), which a vector's allocator
  // can't guarantee across the boundary.
  auto* out = static_cast<uint8_t*>(
      std::malloc(bytes.size()));  // NOLINT(cppcoreguidelines-no-malloc)
  if (out == nullptr) return nullptr;
  std::memcpy(out, bytes.data(), bytes.size());
  return out;
}

}  // namespace

// ---- The opaque options handle ---------------------------------------------
//
// Plain data: the staged variable declarations, host-fn declaration
// strings, and the per-compilation tunables.  `cel_compile`
// materialises a `Compiler::Builder` from this and runs the pipeline.
struct CelCompileOpts {
  std::vector<celwasm::VariableDeclaration> variables;
  std::vector<std::string> host_fns;
  std::string container;
  int optimize_level = 0;
  celwasm::CompilerOptions::LinkMode link_mode =
      celwasm::CompilerOptions::LinkMode::kStatic;

  // Descriptor pool built from a caller-supplied FileDescriptorSet (see
  // cel_compile_opts_set_descriptor_set), layered over the generated pool
  // so well-known types resolve.  The owning databases + pool live here so
  // they outlive the Compile in cel_compile; `descriptor_pool` is the
  // borrowed pointer handed to the Builder.  Declared last so they destruct
  // first (the pool references merged_db, which references the two dbs).
  std::unique_ptr<google::protobuf::SimpleDescriptorDatabase> schema_db;
  std::unique_ptr<google::protobuf::DescriptorPoolDatabase> generated_db;
  std::unique_ptr<google::protobuf::MergedDescriptorDatabase> merged_db;
  std::unique_ptr<google::protobuf::DescriptorPool> owned_pool;
  const google::protobuf::DescriptorPool* descriptor_pool = nullptr;
};

namespace {

// Materialise a `Compiler` from the staged declarations.  A null
// `opts` yields a declaration-free compiler (constant expressions).
absl::StatusOr<celwasm::Compiler> BuildCompiler(const CelCompileOpts* opts) {
  celwasm::Compiler::Builder builder;
  if (opts != nullptr) {
    for (const auto& var : opts->variables) {
      builder.DeclareVariable(var.name, var.type);
    }
    for (const auto& fn : opts->host_fns) {
      builder.AddFunction(fn);
    }
    if (opts->descriptor_pool != nullptr) {
      builder.SetDescriptorPool(opts->descriptor_pool);
    }
  }
  return std::move(builder).Build();
}

// Project the opaque handle's tunables onto the public
// `CompilerOptions`.  A null `opts` yields the compiler defaults.
celwasm::CompilerOptions MakeCompilerOptions(const CelCompileOpts* opts) {
  celwasm::CompilerOptions out;
  if (opts != nullptr) {
    out.container = opts->container;
    out.optimize_level = opts->optimize_level;
    out.link_mode = opts->link_mode;
  }
  return out;
}

}  // namespace

extern "C" {

CelCompileOpts* cel_compile_opts_new(void) {
  return new (std::nothrow) CelCompileOpts();
}

void cel_compile_opts_free(CelCompileOpts* opts) {
  delete opts;
}

CelStatus cel_compile_opts_declare_var(CelCompileOpts* opts, const char* decl,
                                       char** out_err) {
  if (out_err != nullptr) *out_err = nullptr;
  if (opts == nullptr || decl == nullptr) {
    if (out_err != nullptr) {
      *out_err = DupCString("cel_compile_opts_declare_var: null argument");
    }
    return CEL_STATUS_INTERNAL;
  }

  const absl::string_view flag(decl);
  const size_t colon = flag.find(':');
  if (colon == absl::string_view::npos) {
    return Fail(absl::InvalidArgumentError(absl::StrCat(
                    "expected `name:Type`, missing ':' in: ", flag)),
                out_err);
  }
  const absl::string_view name = flag.substr(0, colon);
  if (name.empty()) {
    return Fail(absl::InvalidArgumentError(
                    absl::StrCat("empty variable name in: ", flag)),
                out_err);
  }
  auto type = ParseTypeSpec(flag.substr(colon + 1));
  if (!type.ok()) {
    return Fail(absl::InvalidArgumentError(absl::StrCat(
                    "declare_var ", name, ": ", type.status().message())),
                out_err);
  }
  opts->variables.push_back(
      celwasm::VariableDeclaration{std::string(name), *std::move(type)});
  return CEL_STATUS_OK;
}

CelStatus cel_compile_opts_declare_host_fn(CelCompileOpts* opts,
                                           const char* celfn_decl) {
  if (opts == nullptr || celfn_decl == nullptr) return CEL_STATUS_INTERNAL;
  opts->host_fns.emplace_back(celfn_decl);
  return CEL_STATUS_OK;
}

void cel_compile_opts_set_container(CelCompileOpts* opts,
                                    const char* container) {
  if (opts == nullptr) return;
  opts->container =
      container == nullptr ? std::string() : std::string(container);
}

void cel_compile_opts_set_optimize_level(CelCompileOpts* opts, int level) {
  if (opts == nullptr) return;
  opts->optimize_level = level;
}

void cel_compile_opts_set_link_mode(CelCompileOpts* opts, CelLinkMode mode) {
  if (opts == nullptr) return;
  opts->link_mode = mode == CEL_LINK_MODE_DYNAMIC
                        ? celwasm::CompilerOptions::LinkMode::kDynamic
                        : celwasm::CompilerOptions::LinkMode::kStatic;
}

CelStatus cel_compile_opts_set_descriptor_set(CelCompileOpts* opts,
                                              const uint8_t* fds, int len) {
  if (opts == nullptr) return CEL_STATUS_INVALID_ARGUMENT;
  // A null / empty set clears any previously-supplied pool (→ generated).
  if (fds == nullptr || len <= 0) {
    opts->owned_pool.reset();
    opts->merged_db.reset();
    opts->generated_db.reset();
    opts->schema_db.reset();
    opts->descriptor_pool = nullptr;
    return CEL_STATUS_OK;
  }
  google::protobuf::FileDescriptorSet set;
  if (!set.ParseFromArray(fds, len)) {
    return CEL_STATUS_INVALID_ARGUMENT;
  }
  // Build a pool that resolves the supplied files first, falling back to the
  // generated pool (so well-known types resolve) — the caller-builds-the-
  // fallback contract the compiler expects.
  auto schema_db = std::make_unique<google::protobuf::SimpleDescriptorDatabase>();
  for (const auto& file : set.file()) {
    if (!schema_db->Add(file)) {
      return CEL_STATUS_INVALID_ARGUMENT;  // duplicate file name
    }
  }
  auto generated_db =
      std::make_unique<google::protobuf::DescriptorPoolDatabase>(
          *google::protobuf::DescriptorPool::generated_pool());
  auto merged_db = std::make_unique<google::protobuf::MergedDescriptorDatabase>(
      schema_db.get(), generated_db.get());
  auto owned_pool =
      std::make_unique<google::protobuf::DescriptorPool>(merged_db.get());

  opts->schema_db = std::move(schema_db);
  opts->generated_db = std::move(generated_db);
  opts->merged_db = std::move(merged_db);
  opts->owned_pool = std::move(owned_pool);
  opts->descriptor_pool = opts->owned_pool.get();
  return CEL_STATUS_OK;
}

CelStatus cel_compile(const char* source, const CelCompileOpts* opts,
                      uint8_t** out_wasm, size_t* out_len, char** out_err) {
  if (out_err != nullptr) *out_err = nullptr;
  if (out_wasm != nullptr) *out_wasm = nullptr;
  if (out_len != nullptr) *out_len = 0;
  if (source == nullptr || out_wasm == nullptr || out_len == nullptr) {
    if (out_err != nullptr) {
      *out_err = DupCString("cel_compile: null required argument");
    }
    return CEL_STATUS_INTERNAL;
  }

  absl::StatusOr<celwasm::Compiler> compiler = BuildCompiler(opts);
  if (!compiler.ok()) {
    return Fail(compiler.status(), out_err);
  }

  absl::StatusOr<celwasm::Program> program =
      compiler->Compile(source, MakeCompilerOptions(opts));
  if (!program.ok()) {
    return Fail(program.status(), out_err);
  }

  const absl::Span<const uint8_t> bytes = program->wasm_bytes();
  uint8_t* buf = DupBytes(bytes);
  if (buf == nullptr) {
    return Fail(absl::ResourceExhaustedError(
                    absl::StrCat("cel_compile: out of memory copying ",
                                 bytes.size(), " wasm bytes")),
                out_err);
  }
  *out_wasm = buf;
  *out_len = bytes.size();
  return CEL_STATUS_OK;
}

void cel_free(void* ptr) {
  // The inverse of the malloc'd buffers this library hands out across
  // the C ABI; a caller in C/JS frees through here.
  std::free(ptr);  // NOLINT(cppcoreguidelines-no-malloc)
}

}  // extern "C"
