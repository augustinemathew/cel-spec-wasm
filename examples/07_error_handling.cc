// The three failure layers, and where each surfaces:
//
//   1. Compile time — type errors, undeclared variables, anything
//      outside the static subset. `Compile()` returns a non-OK Status.
//   2. Eval time, CEL-level — division by zero, overflow, missing map
//      key. CEL defines these as *values*: Eval succeeds and returns a
//      Value of kind kError carrying the spec error message.
//   3. Host-side inspection — calling the wrong accessor (AsString on
//      an int) is a recoverable StatusOr error, never a crash.
//
//   bazel run //examples:07_error_handling
//
// Expected output:
//   compile error:  ... found no matching overload for '_+_' ...
//   eval of '10 / divisor' with divisor=0  =>  error value: divide by zero
//   AsString() on an int Value  =>  INVALID_ARGUMENT

#include <iostream>

#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

// Layer 1: a type error never produces a Program.
void ShowCompileError(const celwasm::Compiler& compiler) {
  auto bad = compiler.Compile("1 + 'one'");
  ABSL_QCHECK(!bad.ok());
  std::cout << "compile error:  " << bad.status().message() << "\n";
}

// Layer 2 + 3: CEL eval errors are values; accessor mismatches are
// statuses.
void ShowEvalError(const celwasm::Compiler& compiler,
                   const celwasm::Engine& engine) {
  auto program = compiler.Compile("10 / divisor");
  ABSL_QCHECK_OK(program.status());
  auto instance = engine.Plan(*program);
  ABSL_QCHECK_OK(instance.status());

  celwasm::Activation activation;
  activation.Bind("divisor", celwasm::Value::Int(0));
  auto result = instance->Eval(activation);
  ABSL_QCHECK_OK(result.status());  // Eval itself succeeded …
  ABSL_QCHECK(result->IsError());   // … the CEL *result* is the error.
  auto error = result->ErrorInfo();
  ABSL_QCHECK_OK(error.status());
  std::cout << "eval of '10 / divisor' with divisor=0  =>  error value: "
            << (*error)->message << "\n";

  std::cout << "AsString() on an int Value  =>  "
            << celwasm::Value::Int(7).AsString().status().code() << "\n";
}

}  // namespace

int main() {
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("divisor", celwasm::CelType::Int());
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());

  ShowCompileError(*compiler);
  ShowEvalError(*compiler, *engine);
  return 0;
}
