#include "compiler/host/host_loader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/codegen/cel_abi.pb.h"
#include "compiler/host/cel_host_wasmtime.h"
#include "compiler/host/cel_log.h"
#include "compiler/runtime/cel_runtime_wasm_bytes.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Pulls the message out of a wasmtime error (and frees it).  Returns
// an empty string if err is null; the caller checks for null first and
// only calls this on the error path.
std::string ErrorMessage(wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return out;
}

std::string TrapMessage(wasm_trap_t* trap) {
  wasm_message_t msg;
  wasm_trap_message(trap, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return out;
}

// Builds a wasmtime engine with the feature set the runtime module and
// the eval module require.  Keep in sync with the feature mask applied
// in WasmModule's constructor (module.cc) — if they drift, the runtime
// or eval bytes will decode cleanly in Binaryen and then be rejected
// here at instantiation time.
wasm_engine_t* NewEngine() {
  wasm_config_t* config = wasm_config_new();
  // reference-types is required for the `$cel_refs` externref table,
  // and function-references/gc are the follow-on proposals wasmtime
  // demands once a typed `ref.null externref` initializer appears.
  wasmtime_config_wasm_reference_types_set(config, true);
  wasmtime_config_wasm_function_references_set(config, true);
  wasmtime_config_wasm_gc_set(config, true);
  // Multi-value and bulk-memory ride along in wasmtime's default set;
  // sign-ext and mutable-globals are always on.  Leave defaults for
  // those rather than touching settings that have moved between
  // wasmtime releases.
  return wasm_engine_new_with_config(config);
}

// Small helper — the flow below allocates modules, instances, and the
// linker one step at a time, and any step can fail.  Rather than
// hand-write a goto chain, use a local struct + early returns, and let
// LoadedEval::Reset() (on the out-param) clean up on failure.
absl::Status CompileModule(wasm_engine_t* engine,
                           absl::Span<const uint8_t> bytes,
                           absl::string_view label, wasmtime_module_t** out) {
  wasmtime_error_t* err =
      wasmtime_module_new(engine, bytes.data(), bytes.size(), out);
  if (err != nullptr) {
    return absl::InternalError(
        absl::StrCat("wasmtime_module_new(", label, "): ", ErrorMessage(err)));
  }
  return absl::OkStatus();
}

// Looks up a function export on the given instance and invokes it with
// `args` / expecting `n_results` results.  Fails (NotFound) if the
// export is missing or not a function.
absl::Status CallInstanceFn(wasmtime_context_t* ctx,
                            const wasmtime_instance_t& instance,
                            absl::string_view name,
                            absl::Span<const wasmtime_val_t> args,
                            absl::Span<wasmtime_val_t> results) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &instance, name.data(), name.size(),
                                    &ext)) {
    return absl::NotFoundError(
        absl::StrCat("instance has no export named `", name, "`"));
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    wasmtime_extern_delete(&ext);
    return absl::FailedPreconditionError(
        absl::StrCat("export `", name, "` is not a function"));
  }
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &ext.of.func, args.data(), args.size(),
                         results.data(), results.size(), &trap);
  if (err != nullptr) {
    return absl::InternalError(
        absl::StrCat("wasmtime_func_call(", name, "): ", ErrorMessage(err)));
  }
  if (trap != nullptr) {
    return absl::InternalError(
        absl::StrCat(name, " trapped: ", TrapMessage(trap)));
  }
  return absl::OkStatus();
}

// Decodes the 24-byte CelValue at `mem + slot_off` (absolute byte
// offset into wasmtime linear memory) into a `wasmtime_val_t` mirroring
// the pre-sret return convention: bool → i32, int/uint → i64, double
// → f64, string/bytes/message → i32 carrying `slot_off_arena` so
// callers can re-decode from the slot as they would have from the old
// arena-relative offset.  CEL_ERROR / CEL_UNKNOWN surface as
// `InternalError` — there is no 3VL API on CallEval yet; callers that
// need to observe those values will migrate to a richer result type
// in a later slice.
absl::StatusOr<wasmtime_val_t> DecodeSlot(const uint8_t* absl_nonnull mem,
                                          uint32_t slot_off_arena,
                                          uint32_t mem_base) {
  const uint8_t* slot = mem + mem_base + slot_off_arena;
  uint32_t kind = 0;
  std::memcpy(&kind, slot, sizeof(kind));
  const uint8_t* payload = slot + 8;
  wasmtime_val_t out{};
  switch (kind) {
    case /*CEL_BOOL=*/1: {
      int32_t b = 0;
      std::memcpy(&b, payload, sizeof(b));
      out.kind = WASMTIME_I32;
      out.of.i32 = b;
      return out;
    }
    case /*CEL_INT=*/2: {
      int64_t i = 0;
      std::memcpy(&i, payload, sizeof(i));
      out.kind = WASMTIME_I64;
      out.of.i64 = i;
      return out;
    }
    case /*CEL_UINT=*/3: {
      uint64_t u = 0;
      std::memcpy(&u, payload, sizeof(u));
      out.kind = WASMTIME_I64;
      out.of.i64 = static_cast<int64_t>(u);
      return out;
    }
    case /*CEL_DOUBLE=*/4: {
      double d = 0;
      std::memcpy(&d, payload, sizeof(d));
      out.kind = WASMTIME_F64;
      out.of.f64 = d;
      return out;
    }
    case /*CEL_STRING=*/5:
    case /*CEL_BYTES=*/6:
    case /*CEL_MESSAGE=*/11:
      // The slot itself is the final CelValue; return the arena-relative
      // offset as i32 so callers can walk the span / message the same
      // way they did under the old "eval returns a CelValue offset"
      // convention.
      out.kind = WASMTIME_I32;
      out.of.i32 = static_cast<int32_t>(slot_off_arena);
      return out;
    case /*CEL_UNKNOWN=*/14:
      return absl::InternalError("CallEval: result is UNKNOWN");
    case /*CEL_ERROR=*/15:
      return absl::InternalError("CallEval: result is ERROR");
    default:
      return absl::InternalError(
          absl::StrCat("CallEval: slot has unsupported kind ", kind));
  }
}

// Reads a ULEB128 from `bytes` starting at `*pos`, advancing `*pos`.
// Returns false on truncation; on success `*out` holds the decoded value.
bool ReadUleb128(absl::Span<const uint8_t> bytes, size_t* pos, uint64_t* out) {
  uint64_t value = 0;
  int shift = 0;
  const uint8_t* const data = bytes.data();
  while (*pos < bytes.size()) {
    const uint8_t b = data[(*pos)++];
    value |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *out = value;
      return true;
    }
    shift += 7;
    if (shift > 63) return false;
  }
  return false;
}

// Walks `eval_wasm` looking for a custom section named `cel.abi` and
// returns its payload bytes (after the section name).  Returns an
// empty span if no such section exists — a valid state for
// back-compat evals built before the ABI section landed, and for
// selectless expressions (the codegen still emits the section but its
// `fields` table is empty).  Binary format reference:
// https://webassembly.github.io/spec/core/binary/modules.html.
absl::Span<const uint8_t> FindCelAbiSection(
    absl::Span<const uint8_t> eval_wasm) {
  if (eval_wasm.size() < 8) return {};
  const uint8_t* const data = eval_wasm.data();
  size_t pos = 8;  // skip magic + version
  while (pos < eval_wasm.size()) {
    const uint8_t id = data[pos++];
    uint64_t section_len = 0;
    if (!ReadUleb128(eval_wasm, &pos, &section_len)) return {};
    if (pos + section_len > eval_wasm.size()) return {};
    const size_t section_start = pos;
    const size_t section_end = section_start + section_len;
    if (id == 0) {  // custom section
      size_t p = section_start;
      uint64_t name_len = 0;
      if (!ReadUleb128(eval_wasm, &p, &name_len)) return {};
      if (p + name_len > section_end) return {};
      const absl::string_view name(reinterpret_cast<const char*>(data + p),
                                   name_len);
      p += name_len;
      if (name == "cel.abi") {
        return eval_wasm.subspan(p, section_end - p);
      }
    }
    pos = section_end;
  }
  return {};
}

// Parses the `cel.abi` payload into a field intern-id table suitable
// for `CelHostEnv::SetFieldTable`.  Fills row `i` from `CelAbi.fields(i)`
// if present; holes in the `id` space are padded with zeroed entries
// so `LookupField` keeps returning a non-null row for the compiler's
// dense-id invariant.  Returns an empty table on a malformed payload
// — the caller still instantiates, but `get_field` / `has_field`
// degrade to CEL_ERROR, which is the intended surface for a corrupt
// section.
std::vector<FieldTableEntry> ParseFieldTable(
    absl::Span<const uint8_t> abi_bytes) {
  CelAbi abi;
  if (abi_bytes.empty() ||
      !abi.ParseFromArray(abi_bytes.data(),
                          static_cast<int>(abi_bytes.size()))) {
    return {};
  }
  uint32_t max_id = 0;
  for (const auto& row : abi.fields()) {
    max_id = std::max(row.id(), max_id);
  }
  std::vector<FieldTableEntry> out;
  if (abi.fields_size() > 0) out.resize(max_id + 1);
  for (const auto& row : abi.fields()) {
    if (row.id() >= out.size()) continue;
    out.at(row.id()) = FieldTableEntry{row.field_number(), row.name()};
  }
  return out;
}

// Parses `cel.abi` into an attribute intern-id table for
// `CelHostEnv::SetAttributeTable`.  Same dense-id invariant as the
// field table — holes pad with empty entries so `LookupAttribute` never
// dereferences past the vector.  Empty on a malformed or absent section,
// which the trampoline treats as "never match an unknown pattern".
std::vector<AttributeTableEntry> ParseAttributeTable(
    absl::Span<const uint8_t> abi_bytes) {
  CelAbi abi;
  if (abi_bytes.empty() ||
      !abi.ParseFromArray(abi_bytes.data(),
                          static_cast<int>(abi_bytes.size()))) {
    return {};
  }
  uint32_t max_id = 0;
  for (const auto& row : abi.attributes()) {
    max_id = std::max(row.id(), max_id);
  }
  std::vector<AttributeTableEntry> out;
  if (abi.attributes_size() > 0) out.resize(max_id + 1);
  for (const auto& row : abi.attributes()) {
    if (row.id() >= out.size()) continue;
    AttributeTableEntry entry;
    entry.variable = row.variable();
    entry.qualifiers.reserve(row.qualifiers_size());
    for (const std::string& q : row.qualifiers()) {
      entry.qualifiers.push_back(q);
    }
    out.at(row.id()) = std::move(entry);
  }
  return out;
}

// Fetches the exported `memory` + `cel_mem_base` global from the
// runtime instance.  Separated from CallEval so the decode is a
// straight-line function under the lint threshold.
absl::StatusOr<std::pair<uint8_t*, uint32_t>> ReadRuntimeMemoryAndBase(
    wasmtime_context_t* absl_nonnull ctx, const wasmtime_instance_t& runtime) {
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &runtime, "memory",
                                    std::strlen("memory"), &mem_ext) ||
      mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::FailedPreconditionError(
        "runtime instance has no `memory` export");
  }
  uint8_t* data = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  wasmtime_val_t base{};
  absl::Span<wasmtime_val_t> results(&base, 1);
  if (auto s =
          CallInstanceFn(ctx, runtime, "cel_mem_base", /*args=*/{}, results);
      !s.ok()) {
    return s;
  }
  return std::make_pair(data, static_cast<uint32_t>(base.of.i32));
}

}  // namespace

void LoadedEval::Reset() noexcept {
  has_instances_ = false;
  // Delete the host env before the linker/store it borrows from: its
  // trampolines closed over `store_` via CelHostEnv::ctx_, and the
  // linker holds raw `this` pointers to CelHostEnv as callback data.
  host_env_.reset();
  if (linker_ != nullptr) {
    wasmtime_linker_delete(linker_);
    linker_ = nullptr;
  }
  if (eval_mod_ != nullptr) {
    wasmtime_module_delete(eval_mod_);
    eval_mod_ = nullptr;
  }
  if (runtime_mod_ != nullptr) {
    wasmtime_module_delete(runtime_mod_);
    runtime_mod_ = nullptr;
  }
  if (store_ != nullptr) {
    wasmtime_store_delete(store_);
    store_ = nullptr;
  }
  if (engine_ != nullptr) {
    wasm_engine_delete(engine_);
    engine_ = nullptr;
  }
  runtime_instance_ = {};
  eval_instance_ = {};
}

LoadedEval::~LoadedEval() {
  Reset();
}

LoadedEval::LoadedEval(LoadedEval&& other) noexcept {
  *this = std::move(other);
}

LoadedEval& LoadedEval::operator=(LoadedEval&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  engine_ = other.engine_;
  store_ = other.store_;
  runtime_mod_ = other.runtime_mod_;
  eval_mod_ = other.eval_mod_;
  linker_ = other.linker_;
  host_env_ = std::move(other.host_env_);
  runtime_instance_ = other.runtime_instance_;
  eval_instance_ = other.eval_instance_;
  has_instances_ = other.has_instances_;
  other.engine_ = nullptr;
  other.store_ = nullptr;
  other.runtime_mod_ = nullptr;
  other.eval_mod_ = nullptr;
  other.linker_ = nullptr;
  other.runtime_instance_ = {};
  other.eval_instance_ = {};
  other.has_instances_ = false;
  return *this;
}

wasmtime_context_t* LoadedEval::context() const {
  return store_ == nullptr ? nullptr : wasmtime_store_context(store_);
}

absl::StatusOr<wasmtime_val_t> LoadedEval::CallEval(
    absl::Span<const wasmtime_val_t> args) {
  if (!has_instances_) {
    return absl::FailedPreconditionError(
        "LoadedEval::CallEval on an uninitialised instance "
        "(did LoadEval() fail or was the object moved-from?)");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(store_);

  // Per-call isolation: rewind the runtime's bump arena so repeated
  // invocations don't leak earlier allocations.  The runtime exports
  // `cel_reset` precisely for this.
  if (auto s = CallInstanceFn(ctx, runtime_instance_, "cel_reset",
                              /*args=*/{}, /*results=*/{});
      !s.ok()) {
    return s;
  }

  // Allocate the 24-byte sret slot in the runtime's arena.
  wasmtime_val_t alloc_arg = {WASMTIME_I32, {.i32 = 24}};
  wasmtime_val_t alloc_result{};
  absl::Span<const wasmtime_val_t> alloc_args(&alloc_arg, 1);
  absl::Span<wasmtime_val_t> alloc_results(&alloc_result, 1);
  if (auto s = CallInstanceFn(ctx, runtime_instance_, "cel_alloc", alloc_args,
                              alloc_results);
      !s.ok()) {
    return s;
  }
  const auto slot_off = static_cast<uint32_t>(alloc_result.of.i32);

  // Prepend the slot offset as param 0 and call eval (no results).
  std::vector<wasmtime_val_t> eval_args;
  eval_args.reserve(args.size() + 1);
  eval_args.push_back({WASMTIME_I32, {.i32 = static_cast<int32_t>(slot_off)}});
  for (const auto& a : args) {
    eval_args.push_back(a);
  }
  if (auto s = CallInstanceFn(ctx, eval_instance_, "eval", eval_args,
                              /*results=*/{});
      !s.ok()) {
    return s;
  }

  auto mem_and_base = ReadRuntimeMemoryAndBase(ctx, runtime_instance_);
  if (!mem_and_base.ok()) return mem_and_base.status();
  return DecodeSlot(mem_and_base->first, slot_off, mem_and_base->second);
}

absl::StatusOr<wasmtime_val_t> LoadedEval::CallNullaryEval() {
  return CallEval(/*args=*/{});
}

absl::Status LoadedEval::SetUnknownPatterns(
    std::vector<AttributePattern> patterns) {
  if (host_env_ == nullptr) {
    return absl::FailedPreconditionError(
        "LoadedEval::SetUnknownPatterns: host env not initialised");
  }
  host_env_->SetUnknownPatterns(std::move(patterns));
  return absl::OkStatus();
}

absl::Status LoadedEval::InitEngineStoreAndCompile(
    absl::Span<const uint8_t> eval_wasm_bytes) {
  engine_ = NewEngine();
  if (engine_ == nullptr) {
    return absl::InternalError("wasm_engine_new_with_config returned null");
  }
  store_ = wasmtime_store_new(engine_, /*data=*/nullptr, /*finalizer=*/nullptr);
  if (store_ == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(store_);

  const absl::Span<const uint8_t> runtime_bytes(kCelRuntimeWasmBytes,
                                                kCelRuntimeWasmBytesSize);
  if (auto s = CompileModule(engine_, runtime_bytes, "runtime", &runtime_mod_);
      !s.ok()) {
    return s;
  }
  if (auto s = CompileModule(engine_, eval_wasm_bytes, "eval", &eval_mod_);
      !s.ok()) {
    return s;
  }
  // Runtime imports `cel_env.cel_log` (see DeclareAllocAndSpanImports in
  // compiler/codegen/expr_lower.cc and `cel_runtime.h`'s
  // `import_module`/`import_name` attributes).  Create the shared
  // linker up front, register the log trampoline, and instantiate the
  // runtime through the same linker — the eval module will reuse it
  // for its own `cel_log` import plus the `cel` / `cel_host` namespaces.
  linker_ = wasmtime_linker_new(engine_);
  if (linker_ == nullptr) {
    return absl::InternalError("wasmtime_linker_new returned null");
  }
  if (auto s = RegisterCelLog(linker_); !s.ok()) {
    return s;
  }
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      linker_, ctx, runtime_mod_, &runtime_instance_, &trap);
  if (err != nullptr) {
    return absl::InternalError(absl::StrCat(
        "wasmtime_linker_instantiate(runtime): ", ErrorMessage(err)));
  }
  if (trap != nullptr) {
    return absl::InternalError(
        absl::StrCat("runtime start-function trap: ", TrapMessage(trap)));
  }
  return absl::OkStatus();
}

absl::Status LoadedEval::SetupLinkerAndInstantiateEval(
    wasmtime_context_t* absl_nonnull ctx) {
  // The linker was created in InitEngineStoreAndCompile; it already
  // carries the `cel_env.cel_log` trampoline needed by both the
  // runtime and the eval module.  Register the runtime's exports
  // under the namespace `"cel"` so the eval module's
  // `(import "cel" "memory" …)` and every `(import "cel" "cel_*" …)`
  // resolves without per-name plumbing.
  {
    const char kCelNs[] = "cel";
    wasmtime_error_t* err = wasmtime_linker_define_instance(
        linker_, ctx, kCelNs, sizeof(kCelNs) - 1, &runtime_instance_);
    if (err != nullptr) {
      return absl::InternalError(absl::StrCat(
          "wasmtime_linker_define_instance(cel): ", ErrorMessage(err)));
    }
  }

  // Register the host trampolines (`cel_host.get_field` etc.) on the
  // same linker.  The eval module declares these imports unconditionally
  // — wasmtime's linker accepts them as resolved whether or not the
  // body ever calls them.  Must happen before `wasmtime_linker_instantiate`.
  host_env_ = std::make_unique<CelHostEnv>();
  if (auto s = host_env_->Init(ctx, runtime_instance_); !s.ok()) {
    return s;
  }
  if (auto s = host_env_->Register(linker_); !s.ok()) {
    return s;
  }

  // Instantiate the eval module; the linker resolves every "cel.<name>"
  // import against the runtime instance registered above.
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(linker_, ctx, eval_mod_,
                                                        &eval_instance_, &trap);
    if (err != nullptr) {
      return absl::InternalError(absl::StrCat(
          "wasmtime_linker_instantiate(eval): ", ErrorMessage(err)));
    }
    if (trap != nullptr) {
      return absl::InternalError(
          absl::StrCat("eval start-function trap: ", TrapMessage(trap)));
    }
  }

  // Bind `cel_ref_intern` on the eval instance so `cel_host.get_field`
  // can intern submessages into the module's `$cel_refs` table when a
  // SelectExpr returns a message (G4).  The export only exists when
  // codegen pulled in `AddCelRefsTableAndHelpers`, i.e. when the eval
  // declared at least one message variable.  A missing export is fine
  // for scalar-only evals; any other failure propagates.
  if (auto s = host_env_->BindEvalInterner(eval_instance_);
      !s.ok() && !absl::IsNotFound(s)) {
    return s;
  }
  return absl::OkStatus();
}

absl::StatusOr<LoadedEval> LoadEval(absl::Span<const uint8_t> eval_wasm_bytes) {
  LoadedEval out;
  if (auto s = out.InitEngineStoreAndCompile(eval_wasm_bytes); !s.ok()) {
    return s;
  }
  if (auto s =
          out.SetupLinkerAndInstantiateEval(wasmtime_store_context(out.store_));
      !s.ok()) {
    return s;
  }
  // Hand the intern-id table parsed from the eval module's `cel.abi`
  // custom section over to the host env.  The trampolines consume it
  // via `CelHostEnv::LookupField`; an empty table is fine for
  // selectless evals (no `get_field` / `has_field` calls will be made).
  if (out.host_env_ != nullptr) {
    const absl::Span<const uint8_t> abi_bytes =
        FindCelAbiSection(eval_wasm_bytes);
    out.host_env_->SetFieldTable(ParseFieldTable(abi_bytes));
    out.host_env_->SetAttributeTable(ParseAttributeTable(abi_bytes));
  }
  out.has_instances_ = true;
  return out;
}

}  // namespace celwasm
