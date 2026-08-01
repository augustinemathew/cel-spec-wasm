// Internal — host-side gcov (.gcda) collection for an instrumented
// cel_runtime.wasm.
//
// A runtime built with `--//runtime:instrument_wasm` (clang
// -fprofile-arcs -ftest-coverage, no wasm profile runtime available)
// imports its gcov write-out API from the host:
//
//   env::llvm_gcda_start_file(filename_ptr, version, checksum)
//   env::llvm_gcda_emit_function(ident, func_checksum, cfg_checksum)
//   env::llvm_gcda_emit_arcs(num_counters, counters_ptr)
//   env::llvm_gcda_summary_info()
//   env::llvm_gcda_end_file()
//   env::llvm_gcov_init(writeout_fn, reset_fn)   // table indices
//
// This component implements those imports.  `WasmGcovSink` is the
// pure .gcda writer (mirroring compiler-rt GCDAProfiling.c at
// llvmorg-19.1.5 — the LLVM wasi-sdk 25 builds from — including
// merge-by-rewrite semantics for pre-existing files).  `WasmGcovEnv`
// is the per-Instance state the wasmtime callbacks share.  See
// doc/implementation-plan/rewrite/m38-wasm-gcov-coverage.md.
//
// Collection is opt-in: callbacks no-op unless the process env var
// `CELWASM_WASM_GCOV_DIR` names an output directory.  Registering the
// imports on a linker is always safe — a non-instrumented module
// simply never calls them.

#ifndef CELWASM_EVAL_INTERNAL_WASM_GCOV_H_
#define CELWASM_EVAL_INTERNAL_WASM_GCOV_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "wasmtime.h"

namespace celwasm {

// Writes .gcda files from the record stream an instrumented module's
// write-out functions produce.  One file is "open" at a time
// (StartFile … EndFile), matching the guest protocol.  When the target
// file already exists its counters are merged in (summed positionally,
// the compiler-rt behaviour), so sequential runs accumulate.
//
// Not thread-safe; not crash-safe (no file locking).  Run collection
// workloads sequentially.
class WasmGcovSink {
 public:
  // `output_dir` empty ⇒ every call is a no-op (collection disabled).
  explicit WasmGcovSink(std::string output_dir);

  // Opens `<output_dir>/<basename(orig_filename)>` for merge-or-create
  // and writes the .gcda header.  `version` and `checksum` are copied
  // into the header verbatim.
  void StartFile(absl::string_view orig_filename, uint32_t version,
                 uint32_t checksum);
  void EmitFunction(uint32_t ident, uint32_t func_checksum,
                    uint32_t cfg_checksum);
  // Appends an arcs record, summing any positionally-matching record
  // from the pre-existing file.  Mismatched shape drops the merge and
  // keeps the fresh counters (loudly, via LOG(WARNING)).
  void EmitArcs(absl::Span<const uint64_t> counters);
  // Object-summary record; bumps the stored run count by one.
  void SummaryInfo();
  // Writes the buffered bytes to disk and closes the session.
  // Returns the first I/O error; collection continues on later files.
  absl::Status EndFile();

  bool enabled() const {
    return !output_dir_.empty();
  }

 private:
  uint32_t ReadOld32();  // next u32 from the old file, or ~0u
  void Write32(uint32_t v);
  void Write64(uint64_t v);

  std::string output_dir_;
  std::string current_path_;        // empty ⇔ no open session
  std::vector<uint8_t> old_bytes_;  // pre-existing file being merged
  // Scan offset for ReadOld32, relative to the position the next write
  // lands at (records in old and new files are positionally aligned —
  // the same trick GCDAProfiling.c gets from rewriting in place).
  size_t old_pos_ = 0;
  std::vector<uint8_t> new_bytes_;  // buffered output
  int gcov_version_ = 0;            // decoded from the version word
};

// Per-Instance collection state, shared by the six linker callbacks
// and the dump step.  Owned by InstanceImpl; addresses handed to
// wasmtime callbacks must stay stable for the instance's lifetime.
struct WasmGcovEnv {
  WasmGcovSink sink;
  // Table indices registered via llvm_gcov_init (one write-out fn per
  // instrumented TU, invoked host-side by DumpWasmGcov).
  std::vector<uint32_t> writeout_fns;
  // Borrowed stable base of the runtime's (shared) linear memory.
  // Null until Engine::Plan caches the memory; the gcda callbacks
  // only run during DumpWasmGcov, well after that point.
  const uint8_t* absl_nullable mem_base = nullptr;
  size_t mem_size = 0;

  WasmGcovEnv() : sink(OutputDirFromEnv()) {}
  // Test seam: collect into an explicit directory instead of the env var.
  explicit WasmGcovEnv(std::string output_dir) : sink(std::move(output_dir)) {}

  // Reads CELWASM_WASM_GCOV_DIR ("" when unset).
  static std::string OutputDirFromEnv();
};

// Defines the six env::llvm_* imports on `linker`, all backed by
// `env` (borrowed; must outlive the linker).
ABSL_MUST_USE_RESULT absl::Status RegisterWasmGcovImports(
    wasmtime_linker_t* absl_nonnull linker, WasmGcovEnv* absl_nonnull env);

// Invokes every write-out function the guest registered, driving the
// gcda imports against `env->sink`.  Resolves the module's exported
// `__indirect_function_table` on `helpers_instance` (present only in
// instrumented builds, which link `--export-table`).  No-op when
// collection is disabled or nothing was registered.  Call before the
// store is torn down — the counters live in guest memory.
ABSL_MUST_USE_RESULT absl::Status DumpWasmGcov(
    wasmtime_context_t* absl_nonnull context,
    const wasmtime_instance_t& helpers_instance, WasmGcovEnv* absl_nonnull env);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_WASM_GCOV_H_
