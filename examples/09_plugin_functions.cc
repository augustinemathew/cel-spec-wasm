// Custom functions, path 2: `@plugin.` — a SANDBOXED WebAssembly
// plugin.  The function body (adder_fns.cc) is compiled to
// wasm32-wasip2 and sealed in its own linear memory: it cannot read
// the embedder's memory, cannot syscall, cannot do I/O — safe for
// third-party plugins and not-yet-reviewed code.  Swap in new bytes
// at runtime by registering a new plugin on a fresh Engine.
//
// The build does the heavy lifting: the `cel_wasm_plugin` macro in
// BUILD.bazel turns adder.idl + adder_fns.cc into adder_plugin.wasm
// AND embeds the .idl declarations verbatim in a `cel.fns` custom
// section — the artifact describes itself.  This binary never
// re-declares add(): `Plugin::Load` reads the declarations out of
// the bytes, and the SAME `Plugin` object registers on both the
// Compiler (type-checking) and the Engine (dispatch).
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

#include "abi/plugin.h"
#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;

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

celwasm::Instance BuildInstance(const char* argv0, celwasm::Engine& engine) {
  // One noun carries everything: the wasm bytes, the declarations
  // parsed from the artifact's own `cel.fns` section, and a content
  // hash.  There is NO hand-written C++ mirror of adder.idl to
  // drift out of sync — the declarations provably describe the
  // deployed bytes.  A malformed artifact (core module instead of a
  // component, missing section, unparseable declarations) is
  // rejected right here.
  auto plugin = celwasm::Plugin::Load(LoadPluginBytes(argv0));
  ABSL_QCHECK_OK(plugin.status());

  // Compile side: Use(plugin) hands the artifact's declarations to
  // the type-checker, so `add(a, b)` type-checks like any builtin.
  // The compiled Program also records every plugin function it
  // calls (name + full signature) in its cel.abi, for Plan to
  // verify later.
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("a", celwasm::CelType::Int())
      .DeclareVariable("b", celwasm::CelType::Int())
      .Use(*plugin);
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile("add(a, b) * 2");
  ABSL_QCHECK_OK(program.status());

  // Eval side: the same Plugin registers as the sandboxed backend.
  // Registration statically checks the plugin actually exports
  // every function it declares — a bad plugin upload fails here,
  // not at traffic time — and nothing is instantiated yet.
  ABSL_QCHECK_OK(engine.Use(*plugin));

  // Plan verifies every function the program requires exists in the
  // Engine's registry with an exactly matching signature, then
  // instantiates only the plugins this program calls — each into
  // its own sandbox with its own linear memory.
  auto instance = engine.Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());

  celwasm::Instance instance = BuildInstance(argv[0], *engine);

  celwasm::Activation activation;
  activation.Bind("a", celwasm::Value::Int(40))
      .Bind("b", celwasm::Value::Int(2));
  auto result = instance.Eval(activation);
  ABSL_QCHECK_OK(result.status());
  std::cout << "add(a, b) * 2  =>  " << result->AsInt().value() << "\n";
  return 0;
}
