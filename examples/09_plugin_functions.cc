// Custom functions, path 2: `@plugin.` — a SANDBOXED WebAssembly
// component.  The function body (adder_fns.cc) is compiled to
// wasm32-wasip2 and sealed in its own linear memory: it cannot read
// the embedder's memory, cannot syscall, cannot do I/O — safe for
// third-party plugins and not-yet-reviewed code.  Swap in new bytes
// at runtime by registering a new plugin on a fresh Engine.
//
// The build does the heavy lifting: the `cel_wasm_plugin` macro in
// BUILD.bazel turns adder.idl + adder_fns.cc into
// adder_plugin.wasm.  This binary loads those bytes, registers
// them, and calls add() from CEL.
//
//   bazel run //examples:09_plugin_functions
//
// Expected output:
//   add(a, b) * 2  =>  84

#include <cstdint>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;

celwasm::CelfnType Prim(celwasm::CelfnType::Kind k) {
  celwasm::CelfnType t;
  t.kind = k;
  return t;
}

// The plugin bytes the cel_wasm_plugin macro produced, located
// via bazel runfiles (works under both `bazel run` and `bazel test`).
std::vector<uint8_t> LoadPluginBytes(const char* argv0) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(argv0, &error));
  ABSL_QCHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path =
      runfiles->Rlocation("_main/examples/adder_plugin.wasm");
  std::ifstream f(path, std::ios::binary);
  ABSL_QCHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

// The embedder's mirror of adder.idl.  AddPlugin validates the
// plugin's exports against these declarations — a missing or
// mis-typed export is rejected at registration, not at eval.
celwasm::FunctionLibrary BuildAdderLibrary() {
  auto lib =
      celwasm::FunctionLibrary::Builder()
          .SetWitInterface("cel:greeter/fns@0.1.0")
          .AddPlugin(
              "add", Prim(celwasm::CelfnType::Kind::kInt),
              {celwasm::CelfnParam{false, Prim(celwasm::CelfnType::Kind::kInt),
                                   "a"},
               celwasm::CelfnParam{false, Prim(celwasm::CelfnType::Kind::kInt),
                                   "b"}})
          .Build();
  ABSL_QCHECK_OK(lib.status());
  return *std::move(lib);
}

celwasm::Instance BuildInstance(const char* argv0,
                                const celwasm::FunctionLibrary& lib,
                                celwasm::Engine& engine) {
  ABSL_QCHECK_OK(engine.AddPlugin(LoadPluginBytes(argv0), lib));

  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("a", celwasm::CelType::Int())
      .DeclareVariable("b", celwasm::CelType::Int())
      .DeclareFunctions(lib);
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile("add(a, b) * 2");
  ABSL_QCHECK_OK(program.status());

  auto instance = engine.Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  const celwasm::FunctionLibrary lib = BuildAdderLibrary();
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());

  celwasm::Instance instance = BuildInstance(argv[0], lib, *engine);

  celwasm::Activation activation;
  activation.Bind("a", celwasm::Value::Int(40))
      .Bind("b", celwasm::Value::Int(2));
  auto result = instance.Eval(activation);
  ABSL_QCHECK_OK(result.status());
  std::cout << "add(a, b) * 2  =>  " << result->AsInt().value() << "\n";
  return 0;
}
