// A Program is portable bytes. Compile in one process (or on one
// machine), write the wasm to disk, ship it, and evaluate it in a
// process that never links the compiler.
//
// This example plays both roles: it compiles + saves, then reloads
// the bytes from disk into a fresh Program and evaluates that.
//
//   bazel run //examples:03_compile_once_run_anywhere
//
// Expected output:
//   saved 'price * quantity' to <tmp>/expr.wasm (NNN bytes)
//   reloaded and evaluated  =>  210

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

std::string TempPath() {
  const char* tmpdir = std::getenv("TEST_TMPDIR");  // set under bazel test
  if (tmpdir == nullptr) tmpdir = std::getenv("TMPDIR");
  if (tmpdir == nullptr) tmpdir = "/tmp";
  return std::string(tmpdir) + "/expr.wasm";
}

// ——— The "build box": compile and save. Only needs //compiler. ———
void CompileAndSave(const std::string& path) {
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("price", celwasm::CelType::Int())
      .DeclareVariable("quantity", celwasm::CelType::Int());
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile("price * quantity");
  ABSL_QCHECK_OK(program.status());

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(program->wasm_bytes().data()),
            static_cast<std::streamsize>(program->wasm_bytes().size()));
  ABSL_QCHECK(out.good()) << "failed writing " << path;
  std::cout << "saved 'price * quantity' to " << path << " ("
            << program->wasm_bytes().size() << " bytes)\n";
}

// ——— The "serving box": load and eval. Only needs //eval. ———
void LoadAndEval(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  ABSL_QCHECK(!bytes.empty()) << "failed reading " << path;

  celwasm::Program program(std::move(bytes));  // no validation until Plan
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  auto instance = engine->Plan(program);
  ABSL_QCHECK_OK(instance.status());

  celwasm::Activation activation;
  activation.Bind("price", celwasm::Value::Int(42))
      .Bind("quantity", celwasm::Value::Int(5));
  auto result = instance->Eval(activation);
  ABSL_QCHECK_OK(result.status());
  std::cout << "reloaded and evaluated  =>  " << result->AsInt().value()
            << "\n";
}

}  // namespace

int main() {
  const std::string path = TempPath();
  CompileAndSave(path);
  LoadAndEval(path);
  return 0;
}
