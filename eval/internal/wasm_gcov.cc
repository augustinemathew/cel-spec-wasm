#include "eval/internal/wasm_gcov.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// .gcda wire constants — compiler-rt GCDAProfiling.c (llvmorg-19.1.5).
constexpr uint32_t kGcovDataMagic = 0x67636461;      // "gcda"
constexpr uint32_t kGcovTagFunction = 0x01000000;
constexpr uint32_t kGcovTagCounterArcs = 0x01a10000;
constexpr uint32_t kGcovTagObjectSummary = 0xa1000000;
constexpr uint32_t kGcovTagProgramSummary = 0xa3000000;
constexpr uint32_t kNoValue = 0xFFFFFFFFu;

// Decodes the packed version word ("B02*" and friends) the same way
// GCDAProfiling.c does: letters count from 'A' as hundreds.
int DecodeGcovVersion(uint32_t version) {
  const uint8_t c3 = version >> 24;
  const uint8_t c2 = (version >> 16) & 255;
  const uint8_t c1 = (version >> 8) & 255;
  return c3 >= 'A' ? (c3 - 'A') * 100 + (c2 - '0') * 10 + (c1 - '0')
                   : (c3 - '0') * 10 + (c1 - '0');
}

}  // namespace

WasmGcovSink::WasmGcovSink(std::string output_dir)
    : output_dir_(std::move(output_dir)) {}

// Reads the u32 in the pre-existing file at the position the next
// write would land — the positional-alignment trick GCDAProfiling.c
// gets from rewriting the mmap'd file in place.  `peek_offset_` is
// reset by callers before a record-shaped read sequence.
uint32_t WasmGcovSink::ReadOld32() {
  const size_t pos = new_bytes_.size() + old_pos_;
  old_pos_ += 4;
  if (pos + 4 > old_bytes_.size()) return kNoValue;
  uint32_t v = 0;
  std::memcpy(&v, old_bytes_.data() + pos, 4);
  return v;
}

void WasmGcovSink::Write32(uint32_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  new_bytes_.insert(new_bytes_.end(), p, p + 4);
}

void WasmGcovSink::Write64(uint64_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  new_bytes_.insert(new_bytes_.end(), p, p + 8);
}

void WasmGcovSink::StartFile(absl::string_view orig_filename,
                             uint32_t version, uint32_t checksum) {
  if (!enabled()) return;
  const std::string base =
      std::filesystem::path(std::string(orig_filename)).filename().string();
  current_path_ = absl::StrCat(output_dir_, "/", base);

  old_bytes_.clear();
  old_pos_ = 0;
  new_bytes_.clear();
  std::ifstream in(current_path_, std::ios::binary);
  if (in) {
    old_bytes_.assign((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  }

  gcov_version_ = DecodeGcovVersion(version);
  Write32(kGcovDataMagic);
  Write32(version);
  Write32(checksum);
}

void WasmGcovSink::EmitFunction(uint32_t ident, uint32_t func_checksum,
                                uint32_t cfg_checksum) {
  if (current_path_.empty()) return;
  const bool extra = gcov_version_ >= 47;
  Write32(kGcovTagFunction);
  Write32(extra ? 3 : 2);
  Write32(ident);
  Write32(func_checksum);
  if (extra) Write32(cfg_checksum);
}

void WasmGcovSink::EmitArcs(absl::Span<const uint64_t> counters) {
  if (current_path_.empty()) return;
  std::vector<uint64_t> old_ctrs;
  old_pos_ = 0;
  const uint32_t tag = ReadOld32();
  if (tag != kNoValue) {
    const uint32_t len = ReadOld32();
    if (tag != kGcovTagCounterArcs || len / 2 != counters.size()) {
      ABSL_LOG(WARNING) << "wasm_gcov: " << current_path_
                        << ": cannot merge previous GCDA arcs record "
                           "(shape mismatch); keeping fresh counters";
    } else {
      old_ctrs.resize(counters.size());
      const size_t pos = new_bytes_.size() + 8;
      std::memcpy(old_ctrs.data(), old_bytes_.data() + pos,
                  counters.size() * 8);
    }
  }
  Write32(kGcovTagCounterArcs);
  Write32(static_cast<uint32_t>(counters.size() * 2));
  for (size_t i = 0; i < counters.size(); ++i) {
    Write64(counters[i] + (old_ctrs.empty() ? 0 : old_ctrs[i]));
  }
}

// Object/program summary.  Deviation from GCDAProfiling.c: compiler-rt
// bumps the run count once per *process* (a static `run_counted`),
// which under-counts every file after the first; here each dump bumps
// every file it touches — one "run" per collection session, which is
// the truthful count for per-Instance dumps.
void WasmGcovSink::SummaryInfo() {
  if (current_path_.empty()) return;
  uint32_t runs = 1;
  old_pos_ = 0;
  const uint32_t tag = ReadOld32();
  const uint32_t want_tag =
      gcov_version_ >= 90 ? kGcovTagObjectSummary : kGcovTagProgramSummary;
  if (tag != kNoValue) {
    if (tag != want_tag) {
      ABSL_LOG(WARNING) << "wasm_gcov: " << current_path_
                        << ": cannot merge previous run count "
                           "(unexpected tag); restarting at 1";
    } else {
      ReadOld32();  // length
      uint32_t prev_runs;
      if (gcov_version_ < 90) {
        ReadOld32();
        ReadOld32();
        prev_runs = ReadOld32();
      } else {
        prev_runs = ReadOld32();
      }
      runs = prev_runs + 1;
    }
  }
  if (gcov_version_ >= 90) {
    Write32(kGcovTagObjectSummary);
    Write32(2);
    Write32(runs);
    Write32(0);  // sum_max
  } else {
    Write32(kGcovTagProgramSummary);
    Write32(3);
    Write32(0);
    Write32(0);
    Write32(runs);
  }
}

absl::Status WasmGcovSink::EndFile() {
  if (current_path_.empty()) return absl::OkStatus();
  const std::string path = std::exchange(current_path_, std::string());
  // 8-zero-byte EOF marker, then flush the whole buffer.
  Write64(0);

  std::error_code ec;
  std::filesystem::create_directories(output_dir_, ec);
  if (ec) {
    return absl::InternalError(absl::StrCat(
        "wasm_gcov: cannot create ", output_dir_, ": ", ec.message()));
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return absl::InternalError(absl::StrCat("wasm_gcov: cannot open ", path));
  }
  out.write(reinterpret_cast<const char*>(new_bytes_.data()),
            static_cast<std::streamsize>(new_bytes_.size()));
  out.close();
  if (!out) {
    return absl::InternalError(absl::StrCat("wasm_gcov: write failed: ", path));
  }
  return absl::OkStatus();
}

std::string WasmGcovEnv::OutputDirFromEnv() {
  const char* dir = std::getenv("CELWASM_WASM_GCOV_DIR");
  return dir == nullptr ? std::string() : std::string(dir);
}

namespace {

// Bounds-checked reads out of the instrumented module's linear memory.
// A violation logs and yields an empty/zero result — coverage plumbing
// must never trap an Eval.

absl::string_view GuestCString(const WasmGcovEnv& env, uint32_t ptr) {
  if (env.mem_base == nullptr || ptr >= env.mem_size) {
    ABSL_LOG(WARNING) << "wasm_gcov: filename pointer out of bounds";
    return "";
  }
  const char* start = reinterpret_cast<const char*>(env.mem_base) + ptr;
  const size_t max_len = env.mem_size - ptr;
  const size_t len = ::strnlen(start, max_len);
  if (len == max_len) {
    ABSL_LOG(WARNING) << "wasm_gcov: unterminated filename string";
    return "";
  }
  return absl::string_view(start, len);
}

std::vector<uint64_t> GuestCounters(const WasmGcovEnv& env, uint32_t n,
                                    uint32_t ptr) {
  std::vector<uint64_t> out;
  const uint64_t bytes = static_cast<uint64_t>(n) * 8;
  if (env.mem_base == nullptr || ptr > env.mem_size ||
      bytes > env.mem_size - ptr) {
    ABSL_LOG(WARNING) << "wasm_gcov: counter array out of bounds";
    return out;
  }
  out.resize(n);
  std::memcpy(out.data(), env.mem_base + ptr, bytes);
  return out;
}

WasmGcovEnv* EnvOf(void* raw) { return static_cast<WasmGcovEnv*>(raw); }

wasm_trap_t* GcdaStartFile(void* raw, wasmtime_caller_t*,
                           const wasmtime_val_t* args, size_t,
                           wasmtime_val_t*, size_t) {
  WasmGcovEnv* env = EnvOf(raw);
  if (!env->sink.enabled()) return nullptr;
  env->sink.StartFile(GuestCString(*env, args[0].of.i32),
                      static_cast<uint32_t>(args[1].of.i32),
                      static_cast<uint32_t>(args[2].of.i32));
  return nullptr;
}

wasm_trap_t* GcdaEmitFunction(void* raw, wasmtime_caller_t*,
                              const wasmtime_val_t* args, size_t,
                              wasmtime_val_t*, size_t) {
  EnvOf(raw)->sink.EmitFunction(static_cast<uint32_t>(args[0].of.i32),
                                static_cast<uint32_t>(args[1].of.i32),
                                static_cast<uint32_t>(args[2].of.i32));
  return nullptr;
}

wasm_trap_t* GcdaEmitArcs(void* raw, wasmtime_caller_t*,
                          const wasmtime_val_t* args, size_t,
                          wasmtime_val_t*, size_t) {
  WasmGcovEnv* env = EnvOf(raw);
  if (!env->sink.enabled()) return nullptr;
  const auto counters =
      GuestCounters(*env, static_cast<uint32_t>(args[0].of.i32),
                    static_cast<uint32_t>(args[1].of.i32));
  env->sink.EmitArcs(absl::MakeSpan(counters));
  return nullptr;
}

wasm_trap_t* GcdaSummaryInfo(void* raw, wasmtime_caller_t*,
                             const wasmtime_val_t*, size_t, wasmtime_val_t*,
                             size_t) {
  EnvOf(raw)->sink.SummaryInfo();
  return nullptr;
}

wasm_trap_t* GcdaEndFile(void* raw, wasmtime_caller_t*, const wasmtime_val_t*,
                         size_t, wasmtime_val_t*, size_t) {
  const absl::Status s = EnvOf(raw)->sink.EndFile();
  if (!s.ok()) ABSL_LOG(WARNING) << s;
  return nullptr;
}

wasm_trap_t* GcovInit(void* raw, wasmtime_caller_t*,
                      const wasmtime_val_t* args, size_t, wasmtime_val_t*,
                      size_t) {
  // args = (writeout_fn, reset_fn) table indices; only writeout is
  // ever host-invoked (reset would zero counters we're about to drop).
  EnvOf(raw)->writeout_fns.push_back(static_cast<uint32_t>(args[0].of.i32));
  return nullptr;
}

wasm_functype_t* NI32sToVoid(size_t n) {
  std::vector<wasm_valtype_t*> params(n);
  for (auto& p : params) p = wasm_valtype_new(WASM_I32);
  wasm_valtype_vec_t params_vec;
  wasm_valtype_vec_t results_vec;
  wasm_valtype_vec_new(&params_vec, n, params.data());
  wasm_valtype_vec_new_empty(&results_vec);
  return wasm_functype_new(&params_vec, &results_vec);
}

absl::Status DefineEnvFunc(wasmtime_linker_t* linker, absl::string_view name,
                           size_t arity, wasmtime_func_callback_t cb,
                           WasmGcovEnv* env) {
  wasm_functype_t* ty = NI32sToVoid(arity);
  const char kModule[] = "env";
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, kModule, sizeof(kModule) - 1, name.data(), name.size(), ty, cb,
      env, /*finalizer=*/nullptr);
  wasm_functype_delete(ty);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    const std::string text(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return absl::InternalError(
        absl::StrCat("wasmtime_linker_define_func(env.", name, "): ", text));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status RegisterWasmGcovImports(wasmtime_linker_t* absl_nonnull linker,
                                     WasmGcovEnv* absl_nonnull env) {
  struct Row {
    absl::string_view name;
    size_t arity;
    wasmtime_func_callback_t cb;
  };
  constexpr Row kRows[] = {
      {"llvm_gcda_start_file", 3, GcdaStartFile},
      {"llvm_gcda_emit_function", 3, GcdaEmitFunction},
      {"llvm_gcda_emit_arcs", 2, GcdaEmitArcs},
      {"llvm_gcda_summary_info", 0, GcdaSummaryInfo},
      {"llvm_gcda_end_file", 0, GcdaEndFile},
      {"llvm_gcov_init", 2, GcovInit},
  };
  for (const Row& row : kRows) {
    if (auto s = DefineEnvFunc(linker, row.name, row.arity, row.cb, env);
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

absl::Status DumpWasmGcov(wasmtime_context_t* absl_nonnull context,
                          const wasmtime_instance_t& helpers_instance,
                          WasmGcovEnv* absl_nonnull env) {
  if (!env->sink.enabled() || env->writeout_fns.empty()) {
    return absl::OkStatus();
  }
  const char kTable[] = "__indirect_function_table";
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(context, &helpers_instance, kTable,
                                    sizeof(kTable) - 1, &ext) ||
      ext.kind != WASMTIME_EXTERN_TABLE) {
    return absl::FailedPreconditionError(
        "wasm_gcov: module exports no __indirect_function_table; was the "
        "instrumented runtime linked with -Wl,--export-table?");
  }
  for (const uint32_t idx : env->writeout_fns) {
    wasmtime_val_t val;
    if (!wasmtime_table_get(context, &ext.of.table, idx, &val) ||
        val.kind != WASMTIME_FUNCREF) {
      return absl::InternalError(absl::StrCat(
          "wasm_gcov: table slot ", idx, " is not a funcref"));
    }
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(context, &val.of.funcref,
                                               nullptr, 0, nullptr, 0, &trap);
    // No wasmtime_val_unroot needed: funcref values are exempt.
    if (err != nullptr || trap != nullptr) {
      if (trap != nullptr) wasm_trap_delete(trap);
      if (err != nullptr) wasmtime_error_delete(err);
      return absl::InternalError(
          absl::StrCat("wasm_gcov: write-out fn at table slot ", idx,
                       " failed"));
    }
  }
  return absl::OkStatus();
}

}  // namespace celwasm
