// Partial evaluation: evaluate with some inputs marked UNKNOWN and
// learn whether the policy even needs them.  Classic use: short-cut
// authorization before fetching expensive data.
//
//   bazel run //examples:05_partial_eval
//
// Expected output:
//   is_admin=true,  purchase_total unknown  =>  kind: bool, allowed: true
//   is_admin=false, purchase_total unknown  =>  kind: unknown (need the total!)
//   is_admin=false, purchase_total=250      =>  kind: bool, allowed: true

#include <iostream>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/types/span.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

celwasm::Instance BuildPolicyInstance() {
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("is_admin", celwasm::CelType::Bool())
      .DeclareVariable("purchase_total", celwasm::CelType::Int());
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile("is_admin || purchase_total > 100");
  ABSL_QCHECK_OK(program.status());

  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

// PartialEval with `purchase_total` unknown.  If `||` short-circuits
// on is_admin, the unknown is never consulted and we get a concrete
// bool; otherwise the result is kUnknown — "go fetch that data".
void EvalWithUnknownTotal(
    celwasm::Instance& instance, bool is_admin,
    absl::Span<const celwasm::AttributePattern> unknowns) {
  celwasm::Activation activation;
  activation.Bind("is_admin", celwasm::Value::Bool(is_admin));
  auto result = instance.PartialEval(activation, unknowns);
  ABSL_QCHECK_OK(result.status());
  if (result->IsUnknown()) {
    std::cout << "is_admin=false, purchase_total unknown  =>  kind: unknown "
              << "(need the total!)\n";
    return;
  }
  std::cout << "is_admin=" << (is_admin ? "true, " : "false,")
            << " purchase_total unknown  =>  kind: bool, allowed: "
            << (result->AsBool().value() ? "true" : "false") << "\n";
}

// Once the data is fetched, a plain Eval settles it.
void EvalWithFetchedTotal(celwasm::Instance& instance) {
  celwasm::Activation activation;
  activation.Bind("is_admin", celwasm::Value::Bool(false))
      .Bind("purchase_total", celwasm::Value::Int(250));
  auto result = instance.Eval(activation);
  ABSL_QCHECK_OK(result.status());
  std::cout << "is_admin=false, purchase_total=250      =>  kind: bool, "
            << "allowed: " << (result->AsBool().value() ? "true" : "false")
            << "\n";
}

}  // namespace

int main() {
  celwasm::Instance instance = BuildPolicyInstance();

  auto pattern = celwasm::AttributePattern::Parse("purchase_total");
  ABSL_QCHECK_OK(pattern.status());
  const celwasm::AttributePattern unknowns[] = {*pattern};

  EvalWithUnknownTotal(instance, /*is_admin=*/true, unknowns);
  EvalWithUnknownTotal(instance, /*is_admin=*/false, unknowns);
  EvalWithFetchedTotal(instance);
  return 0;
}
