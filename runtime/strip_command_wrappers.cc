// Build-time tool that rewrites `cel_runtime.wasm` to drop the
// wasi-libc command-mode wrappers around exported helpers, producing
// the static-link variant of the runtime artifact (`cel_runtime_
// stripped_wasm.bin`).
//
// Why this exists.  wasi-libc's command exec-model wraps every
// exported function as
//     <export>.command_export → __wasm_call_ctors; <body>; __wasm_call_dtors
// so each cross-module call into the runtime pays the full ctor/dtor
// chain — measured at ~83 ns/call on the host-side microbench
// (`wasm_compilation_experiments/wrapper_overhead/FINDINGS.md` §4.1).
// Configurable-linking's static mode wants the bare bodies as exports
// so a single `_initialize` (called once at instantiate) handles the
// init, and the per-call wrapper chain disappears.
//
// Why a build-time tool rather than a clang flag.  Confirmed by spike
// on 2026-06-07 (see `m28-configurable-linking.md` §5.2.0):
// `-mexec-model=reactor` is rejected by clang on `wasm32-wasi-threads`
// and `-wasip1-threads`; vanilla `wasm32-wasi` doesn't ship `<mutex>`
// in its libc++ headers, so cctz/absl won't compile against it.
// Post-processing is the only path within the wasi-sdk we ship.
//
// What it does, mechanically.  For every function export `X` whose
// target is the wrapper function `X.command_export`, remove the
// export and re-add it pointing at the bare-body function `X`.
// `BinaryenModuleOptimize` then DCEs the now-unreferenced wrapper
// functions plus the entire `__wasm_call_ctors`/`__wasm_call_dtors`
// chain (no live callers after the export retarget).
//
// Usage: `strip_command_wrappers <in.wasm> <out.wasm>`.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binaryen-c.h"

namespace {

constexpr std::string_view kSuffix = ".command_export";

std::vector<char> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "open(%s) failed\n", path);
    std::exit(1);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  return std::vector<char>(s.begin(), s.end());
}

void WriteFile(const char* path, const char* data, size_t size) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "open(%s, w) failed\n", path);
    std::exit(1);
  }
  f.write(data, static_cast<std::streamsize>(size));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <in.wasm> <out.wasm>\n", argv[0]);
    return 1;
  }
  std::vector<char> bytes = ReadFile(argv[1]);
  BinaryenModuleRef m = BinaryenModuleRead(bytes.data(), bytes.size());
  if (m == nullptr) {
    std::fprintf(stderr, "BinaryenModuleRead failed\n");
    return 1;
  }

  // Walk exports.  For each function export whose target is named
  // `X.command_export` and where the bare `X` exists, queue the
  // (external_name, bare_internal_name) pair for retarget.
  std::vector<std::pair<std::string, std::string>> retargets;
  BinaryenIndex n = BinaryenGetNumExports(m);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(m, i);
    if (BinaryenExportGetKind(e) != BinaryenExternalFunction()) continue;
    const char* exp_name = BinaryenExportGetName(e);
    const char* target = BinaryenExportGetValue(e);
    std::string fname(target);
    if (fname.size() <= kSuffix.size()) continue;
    if (fname.compare(fname.size() - kSuffix.size(), kSuffix.size(),
                      kSuffix) != 0) {
      continue;
    }
    std::string bare = fname.substr(0, fname.size() - kSuffix.size());
    if (BinaryenGetFunction(m, bare.c_str()) == nullptr) continue;
    retargets.emplace_back(std::string(exp_name), std::move(bare));
  }

  // Apply the retargets.  Remove the old export, add a replacement
  // pointing at the bare body.  Order of two-step matters: Binaryen
  // disallows duplicate exports.
  for (auto& [exp_name, bare_internal] : retargets) {
    BinaryenRemoveExport(m, exp_name.c_str());
    BinaryenAddFunctionExport(m, bare_internal.c_str(), exp_name.c_str());
  }

  // Optimize: triggers DCE which strips the now-unreachable
  // `.command_export` wrapper functions and the `__wasm_call_ctors` /
  // `__wasm_call_dtors` chain (no live callers after the retarget).
  BinaryenModuleOptimize(m);

  BinaryenModuleAllocateAndWriteResult res =
      BinaryenModuleAllocateAndWrite(m, /*sourceMapUrl=*/nullptr);
  if (res.binary == nullptr) {
    std::fprintf(stderr, "BinaryenModuleAllocateAndWrite failed\n");
    BinaryenModuleDispose(m);
    return 1;
  }
  WriteFile(argv[2], static_cast<const char*>(res.binary), res.binaryBytes);
  std::free(res.binary);
  if (res.sourceMap != nullptr) std::free(res.sourceMap);
  BinaryenModuleDispose(m);

  std::fprintf(stderr,
               "stripped %zu .command_export wrappers, wrote %zu bytes\n",
               retargets.size(), res.binaryBytes);
  return 0;
}
