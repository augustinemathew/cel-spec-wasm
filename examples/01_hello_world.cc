// The smallest possible cel-wasm embed: compile a CEL expression
// ahead-of-time to a wasm module, JIT it to native code, evaluate it.
//
//   bazel run //examples:01_hello_world
//
// Expected output:
//   1 + 2 + 3  =>  6

#include <iostream>

#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"

int main() {
  // A Compiler turns CEL source into a Program: portable wasm bytes.
  // No engine, no JIT — pure compile-time.
  auto compiler = celwasm::Compiler::NewBuilder().Build();
  if (!compiler.ok()) {
    std::cerr << compiler.status() << "\n";
    return 1;
  }
  auto program = compiler->Compile("1 + 2 + 3");
  if (!program.ok()) {
    std::cerr << program.status() << "\n";
    return 1;
  }

  // An Engine owns the wasm runtime machinery. Build one per process,
  // share it across threads.
  auto engine = celwasm::Engine::NewBuilder().Build();
  if (!engine.ok()) {
    std::cerr << engine.status() << "\n";
    return 1;
  }

  // Plan() JITs the Program to native machine code via Cranelift —
  // once per program. Eval() then runs native code; no interpreter.
  auto instance = engine->Plan(*program);
  if (!instance.ok()) {
    std::cerr << instance.status() << "\n";
    return 1;
  }
  auto result = instance->Eval();
  if (!result.ok()) {
    std::cerr << result.status() << "\n";
    return 1;
  }

  std::cout << "1 + 2 + 3  =>  " << result->AsInt().value() << "\n";
  return 0;
}
