// Probe runner — drives a MANUALLY-BUILT (non-macro) plugin through
// the full one-noun flow: Plugin::Load -> Compiler::Builder::Use ->
// Compile -> Engine::Use -> Plan -> Eval.  The plugin under test is
// the Rust-authored component in addcase/ (built by build.sh); the
// point of the probe is that NOTHING here knows or cares the plugin
// was written in Rust.
//
// Usage: probe_runner <plugin.wasm>
// Prints the eval result and the plugin's identity facts; exits
// non-zero on any failure.

#include <cstdint>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
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

namespace {

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  ABSL_QCHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
  ABSL_QCHECK(argc == 2) << "usage: probe_runner <plugin.wasm>";

  auto plugin = celwasm::Plugin::Load(ReadFile(argv[1]));
  ABSL_QCHECK_OK(plugin.status());
  std::cout << "Plugin::Load ok — " << plugin->decls().size()
            << " decl(s), interface " << plugin->wit_interface()
            << ", hash " << plugin->hash_hex().substr(0, 12) << "\n";

  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("a", celwasm::CelType::Int())
      .DeclareVariable("b", celwasm::CelType::Int())
      .Use(*plugin);
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile("add(a, b) * 2");
  ABSL_QCHECK_OK(program.status());
  std::cout << "Compile ok — call site type-checked against the "
               "artifact's own decls\n";

  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  ABSL_QCHECK_OK(engine->Use(*plugin));
  std::cout << "Engine::Use ok — static export check passed\n";

  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  std::cout << "Plan ok — required-function signatures verified\n";

  celwasm::Activation activation;
  activation.Bind("a", celwasm::Value::Int(40))
      .Bind("b", celwasm::Value::Int(2));
  auto result = instance->Eval(activation);
  ABSL_QCHECK_OK(result.status());
  std::cout << "Eval: add(a, b) * 2  =>  " << result->AsInt().value() << "\n";
  ABSL_QCHECK(result->AsInt().value() == 84) << "expected 84";
  std::cout << "PROBE PASS — Rust-authored plugin round-trips end-to-end\n";
  return 0;
}
