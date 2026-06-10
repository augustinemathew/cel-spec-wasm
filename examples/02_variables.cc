// Variables: declare types at compile time, bind values at eval time.
// One Compiler compiles many Programs; one Program evals many times
// with different Activations.
//
//   bazel run //examples:02_variables
//
// Expected output:
//   age=25 country="US"  =>  allowed: true
//   age=15 country="US"  =>  allowed: false

#include <cstdint>
#include <iostream>
#include <utility>

#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

// Compile the policy and JIT it into a ready-to-eval Instance.  An
// Instance keeps the engine state alive internally, so it can be
// returned by value and outlive the local Engine handle.
celwasm::Instance BuildPolicyInstance() {
  // Variables are statically typed, declared up front.  Referencing an
  // undeclared variable — or using one at the wrong type — fails at
  // Compile() time, not at eval time.
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("age", celwasm::CelType::Int())
      .DeclareVariable("country", celwasm::CelType::String());
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());

  auto program = compiler->Compile("age >= 18 && country in ['US', 'CA']");
  ABSL_QCHECK_OK(program.status());

  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

}  // namespace

int main() {
  celwasm::Instance instance = BuildPolicyInstance();

  // Bind concrete values per evaluation.  The same Instance is reused;
  // its arena resets automatically at the top of every Eval.
  for (int64_t age : {25, 15}) {
    celwasm::Activation activation;
    activation.Bind("age", celwasm::Value::Int(age))
        .Bind("country", celwasm::Value::String("US"));
    auto result = instance.Eval(activation);
    ABSL_QCHECK_OK(result.status());
    std::cout << "age=" << age << " country=\"US\"  =>  allowed: "
              << (result->AsBool().value() ? "true" : "false") << "\n";
  }
  return 0;
}
