#include "compiler/host/cel_host_wasmtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/host/attribute.h"
#include "compiler/host/cel_host.h"
#include "compiler/runtime/cel_runtime.h"
#include "google/protobuf/message.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// ---- Export lookup helpers -------------------------------------------------

absl::Status LookupFunc(wasmtime_context_t* ctx,
                        const wasmtime_instance_t& instance,
                        absl::string_view name, wasmtime_func_t* out) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &instance, name.data(), name.size(),
                                    &ext)) {
    return absl::NotFoundError(
        absl::StrCat("runtime has no export named `", name, "`"));
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError(
        absl::StrCat("runtime export `", name, "` is not a function"));
  }
  *out = ext.of.func;
  return absl::OkStatus();
}

absl::Status LookupMemory(wasmtime_context_t* ctx,
                          const wasmtime_instance_t& instance,
                          absl::string_view name, wasmtime_memory_t* out) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &instance, name.data(), name.size(),
                                    &ext)) {
    return absl::NotFoundError(
        absl::StrCat("runtime has no export named `", name, "`"));
  }
  if (ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::FailedPreconditionError(
        absl::StrCat("runtime export `", name, "` is not a memory"));
  }
  *out = ext.of.memory;
  return absl::OkStatus();
}

// ---- Reentrant calls into the runtime from inside a trampoline -------------

// Calls `func` with one i32 arg, returns one i32 result.  On any wasmtime
// error / trap the trampoline trips an abort-style error (we propagate
// through a host trap below) — but in the "can't happen" call sites we
// currently hit (the runtime is our own module), we translate failure to
// 0 / null and let the CEL-level error propagate naturally as a CEL_ERROR
// downstream.  For M3 that's sufficient; richer trap plumbing can replace
// the `return 0` with `wasm_trap_new` if we start surfacing
// runtime-side errors.
uint32_t CallRuntimeI32ToI32(wasmtime_context_t* ctx,
                             const wasmtime_func_t& func, uint32_t arg) {
  wasmtime_val_t in{};
  in.kind = WASMTIME_I32;
  in.of.i32 = static_cast<int32_t>(arg);
  wasmtime_val_t out{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &func, &in, 1, &out, 1, &trap);
  if (err != nullptr) {
    wasmtime_error_delete(err);
    return 0;
  }
  if (trap != nullptr) {
    wasm_trap_delete(trap);
    return 0;
  }
  return static_cast<uint32_t>(out.of.i32);
}

uint32_t CallRuntimeNullaryI32(wasmtime_context_t* ctx,
                               const wasmtime_func_t& func) {
  wasmtime_val_t out{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &func, nullptr, 0, &out, 1, &trap);
  if (err != nullptr) {
    wasmtime_error_delete(err);
    return 0;
  }
  if (trap != nullptr) {
    wasm_trap_delete(trap);
    return 0;
  }
  return static_cast<uint32_t>(out.of.i32);
}

// Interns a host `Message*` into the module's `$cel_refs` table.  Each
// call produces a fresh wasmtime externref (the externref-table acts as
// the identity map at the wasm level), so identical Message* values
// appearing twice during one evaluation get distinct slots — matches the
// bump-allocator semantics of `cel_ref_intern`.
uint32_t InternMessageViaRefIntern(CelHostEnv& env,
                                   const google::protobuf::Message& msg) {
  wasmtime_val_t in{};
  in.kind = WASMTIME_EXTERNREF;
  if (!wasmtime_externref_new(
          env.ctx(),
          // The trampoline only uses the pointer as an identity handle;
          // `ReadField` / the test harness read through it before the
          // store is torn down.  No finalizer — the embedder owns the
          // Message.  `wasmtime_externref_new` takes `void*`, but the
          // host treats the Message as read-only (`ReadField` receives
          // `const Message&`), so the cast is a C-API accommodation.
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
          const_cast<google::protobuf::Message*>(&msg),
          /*finalizer=*/nullptr, &in.of.externref)) {
    return 0;
  }
  wasmtime_val_t out{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call(env.ctx(), &env.cel_ref_intern(),
                                             &in, 1, &out, 1, &trap);
  if (err != nullptr) {
    wasmtime_error_delete(err);
    return 0;
  }
  if (trap != nullptr) {
    wasm_trap_delete(trap);
    return 0;
  }
  return static_cast<uint32_t>(out.of.i32);
}

// Builds an ArenaAllocator that allocates via the runtime's `cel_alloc`
// export and hands back a host-addressable pointer into wasmtime's linear
// memory plus the arena-relative offset (which is exactly what
// `cel_alloc` returns directly — the arena is at linear-memory offset 0
// for the current runtime, but we still round-trip through `cel_mem_base`
// so the glue survives a future shared-memory layout change).
ArenaAllocator MakeArenaAllocator(CelHostEnv& env) {
  return [&env](size_t len, uint32_t* out_offset) -> uint8_t* {
    const uint32_t offset = CallRuntimeI32ToI32(env.ctx(), env.cel_alloc(),
                                                static_cast<uint32_t>(len));
    *out_offset = offset;
    if (len == 0) return nullptr;
    const uint32_t base = CallRuntimeNullaryI32(env.ctx(), env.cel_mem_base());
    uint8_t* mem = wasmtime_memory_data(env.ctx(), &env.memory());
    return mem + base + offset;
  };
}

InternMessage MakeInternMessage(CelHostEnv& env) {
  return [&env](const google::protobuf::Message& msg) -> uint32_t {
    return InternMessageViaRefIntern(env, msg);
  };
}

// Dereferences a `wasmtime_val_t` holding an externref into the
// host-owned `Message*` the embedder attached when it called
// `wasmtime_externref_new`.  The embedder is responsible for only ever
// passing Message-bearing externrefs through `cel_host.*` imports;
// anything else is a checker invariant violation, which we surface as a
// CEL_ERROR rather than a trap so the same shape can be inspected from
// a test.
const google::protobuf::Message* MessageFromExternref(
    wasmtime_context_t* ctx, const wasmtime_val_t& slot) {
  if (slot.kind != WASMTIME_EXTERNREF) return nullptr;
  return static_cast<const google::protobuf::Message*>(
      wasmtime_externref_data(ctx, &slot.of.externref));
}

// ---- Trampolines -----------------------------------------------------------

// Builds a `celwasm::Attribute` from a resolved `AttributeTableEntry`.
// Copies because the `Attribute` owns its strings and the table row
// may outlive the trampoline invocation (but is cheap either way —
// attributes are short paths).
Attribute AttributeFromEntry(const AttributeTableEntry& entry) {
  std::vector<AttributeQualifier> path;
  path.reserve(entry.qualifiers.size());
  for (const std::string& q : entry.qualifiers) {
    path.emplace_back(q);
  }
  return Attribute(entry.variable, std::move(path));
}

wasm_trap_t* GetFieldTrampoline(void* data, wasmtime_caller_t* /*caller*/,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* /*results*/,
                                size_t /*nresults*/) {
  CelHostEnv& env = *static_cast<CelHostEnv*>(data);
  if (nargs < 4) return nullptr;
  const google::protobuf::Message* msg =
      MessageFromExternref(env.ctx(), args[0]);
  const auto intern_id = static_cast<uint32_t>(args[1].of.i32);
  const auto attr_id = static_cast<uint32_t>(args[2].of.i32);
  const auto out_offset = static_cast<uint32_t>(args[3].of.i32);

  const uint32_t base = CallRuntimeNullaryI32(env.ctx(), env.cel_mem_base());
  uint8_t* mem = wasmtime_memory_data(env.ctx(), &env.memory());
  auto* out_cv = reinterpret_cast<CelValue*>(mem + base + out_offset);

  // Partial-eval check — before we even look at the field table.
  // A FULL-match unknown pattern supersedes the field read: we
  // carry the codegen attr_id as the single element of the
  // UnknownSet so the embedder can map it back to a source path
  // through `CelAbi.attributes`.  PARTIAL / NONE fall through to
  // the normal field-read path; a FULL match on a deeper select
  // will fire when the codegen walks to that call site.
  const AttributeTableEntry* attr_entry = env.LookupAttribute(attr_id);
  if (attr_entry != nullptr &&
      env.AttributeIsFullyUnknown(AttributeFromEntry(*attr_entry))) {
    out_cv->kind = CEL_UNKNOWN;
    // Point at an empty UnknownSet; we don't mint a new one here
    // because `cel_make_unknown` writes into the arena and the caller
    // only needs the UNKNOWN kind visible for 3VL absorption.  A
    // later slice that surfaces attr provenance through diagnostics
    // will swap this for a reentrant call into `cel_make_unknown`.
    out_cv->payload.unk = 0;
    return nullptr;
  }

  const FieldTableEntry* entry = env.LookupField(intern_id);
  if (msg == nullptr || entry == nullptr) {
    out_cv->kind = CEL_ERROR;
    out_cv->payload.err = 0;
    return nullptr;
  }
  ArenaAllocator alloc = MakeArenaAllocator(env);
  InternMessage intern = MakeInternMessage(env);
  ReadField(*msg, static_cast<int>(entry->field_number), entry->name, out_cv,
            alloc, intern);
  return nullptr;
}

wasm_trap_t* HasFieldTrampoline(void* data, wasmtime_caller_t* /*caller*/,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* results, size_t nresults) {
  CelHostEnv& env = *static_cast<CelHostEnv*>(data);
  if (nargs < 2 || nresults < 1) return nullptr;
  const google::protobuf::Message* msg =
      MessageFromExternref(env.ctx(), args[0]);
  const auto intern_id = static_cast<uint32_t>(args[1].of.i32);
  results[0].kind = WASMTIME_I32;
  const FieldTableEntry* entry = env.LookupField(intern_id);
  results[0].of.i32 = (msg != nullptr && entry != nullptr &&
                       HasField(*msg, entry->field_number, entry->name))
                          ? 1
                          : 0;
  return nullptr;
}

wasm_trap_t* MessageEqTrampoline(void* data, wasmtime_caller_t* /*caller*/,
                                 const wasmtime_val_t* args, size_t nargs,
                                 wasmtime_val_t* results, size_t nresults) {
  CelHostEnv& env = *static_cast<CelHostEnv*>(data);
  if (nargs < 2 || nresults < 1) return nullptr;
  const google::protobuf::Message* a = MessageFromExternref(env.ctx(), args[0]);
  const google::protobuf::Message* b = MessageFromExternref(env.ctx(), args[1]);
  results[0].kind = WASMTIME_I32;
  results[0].of.i32 =
      (a != nullptr && b != nullptr && MessageEq(*a, *b)) ? 1 : 0;
  return nullptr;
}

// ---- Type descriptors for the trampolines ----------------------------------

// Builds `(externref, i32, i32, i32) -> ()`.
// Params: message externref, field_intern_id, attr_id, out_offset.
wasm_functype_t* GetFieldType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[4];
  param_arr[0] = wasm_valtype_new(WASM_EXTERNREF);
  param_arr[1] = wasm_valtype_new(WASM_I32);
  param_arr[2] = wasm_valtype_new(WASM_I32);
  param_arr[3] = wasm_valtype_new(WASM_I32);
  wasm_valtype_vec_new(&params, 4, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

// Builds `(externref, i32) -> i32`.
wasm_functype_t* HasFieldType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[2];
  param_arr[0] = wasm_valtype_new(WASM_EXTERNREF);
  param_arr[1] = wasm_valtype_new(WASM_I32);
  wasm_valtype_t* result_arr[1];
  result_arr[0] = wasm_valtype_new(WASM_I32);
  wasm_valtype_vec_new(&params, 2, param_arr);
  wasm_valtype_vec_new(&results, 1, result_arr);
  return wasm_functype_new(&params, &results);
}

// Builds `(externref, externref) -> i32`.
wasm_functype_t* MessageEqType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[2];
  param_arr[0] = wasm_valtype_new(WASM_EXTERNREF);
  param_arr[1] = wasm_valtype_new(WASM_EXTERNREF);
  wasm_valtype_t* result_arr[1];
  result_arr[0] = wasm_valtype_new(WASM_I32);
  wasm_valtype_vec_new(&params, 2, param_arr);
  wasm_valtype_vec_new(&results, 1, result_arr);
  return wasm_functype_new(&params, &results);
}

absl::Status DefineTrampoline(wasmtime_linker_t* linker,
                              absl::string_view module, absl::string_view name,
                              wasm_functype_t* type,
                              wasmtime_func_callback_t cb, void* data) {
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, module.data(), module.size(), name.data(), name.size(), type, cb,
      data, /*finalizer=*/nullptr);
  wasm_functype_delete(type);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string text(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return absl::InternalError(absl::StrCat("wasmtime_linker_define_func(",
                                            module, ".", name, "): ", text));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status CelHostEnv::Init(wasmtime_context_t* ctx,
                              const wasmtime_instance_t& runtime) {
  ctx_ = ctx;
  if (auto s = LookupFunc(ctx, runtime, "cel_alloc", &cel_alloc_); !s.ok()) {
    return s;
  }
  if (auto s = LookupFunc(ctx, runtime, "cel_mem_base", &cel_mem_base_);
      !s.ok()) {
    return s;
  }
  if (auto s = LookupMemory(ctx, runtime, "memory", &memory_); !s.ok()) {
    return s;
  }
  // `cel_ref_intern` is not a runtime export — it's minted by the
  // codegen helpers in `compiler/codegen/cel_refs.cc` and lives on the
  // eval module.  G2 flat-select never needs it (scalar fields write
  // their payload directly into the out-CelValue), so we leave the
  // handle zero-initialised here.  The nested-message path (G4)
  // populates it after the eval module instantiates by calling
  // `BindEvalInterner` on that instance.
  return absl::OkStatus();
}

absl::Status CelHostEnv::BindEvalInterner(const wasmtime_instance_t& eval) {
  return LookupFunc(ctx_, eval, "cel_ref_intern", &cel_ref_intern_);
}

void CelHostEnv::SetFieldTable(std::vector<FieldTableEntry> table) {
  field_table_ = std::move(table);
}

const FieldTableEntry* CelHostEnv::LookupField(uint32_t intern_id) const {
  if (static_cast<size_t>(intern_id) >= field_table_.size()) return nullptr;
  return &field_table_.at(intern_id);
}

void CelHostEnv::SetAttributeTable(std::vector<AttributeTableEntry> table) {
  attribute_table_ = std::move(table);
}

const AttributeTableEntry* CelHostEnv::LookupAttribute(uint32_t attr_id) const {
  if (static_cast<size_t>(attr_id) >= attribute_table_.size()) return nullptr;
  return &attribute_table_.at(attr_id);
}

void CelHostEnv::SetUnknownPatterns(std::vector<AttributePattern> patterns) {
  unknown_patterns_ = std::move(patterns);
}

bool CelHostEnv::AttributeIsFullyUnknown(const Attribute& attr) const {
  for (const AttributePattern& pat : unknown_patterns_) {
    if (pat.IsMatch(attr) == AttributePattern::MatchType::FULL) return true;
  }
  return false;
}

absl::Status CelHostEnv::Register(wasmtime_linker_t* linker) {
  if (auto s = DefineTrampoline(linker, "cel_host", "get_field", GetFieldType(),
                                GetFieldTrampoline, this);
      !s.ok()) {
    return s;
  }
  if (auto s = DefineTrampoline(linker, "cel_host", "has_field", HasFieldType(),
                                HasFieldTrampoline, this);
      !s.ok()) {
    return s;
  }
  if (auto s = DefineTrampoline(linker, "cel_host", "message_eq",
                                MessageEqType(), MessageEqTrampoline, this);
      !s.ok()) {
    return s;
  }
  return absl::OkStatus();
}

}  // namespace celwasm
