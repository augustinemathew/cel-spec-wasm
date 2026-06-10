// A host function handler has three distinct ways to not return a
// plain value — and they mean different things:
//
//   1. Return Value::Error(...)        =>  a CEL *error value*.  The
//      expression keeps evaluating under CEL's absorption rules
//      (e.g. `true || error` is still true).  Use for domain errors
//      the policy semantics should see.
//   2. Return Value::Unknown(...)      =>  a CEL *unknown*.  Combines
//      with partial evaluation: "this function couldn't answer yet."
//   3. Return a non-OK absl::Status    =>  an infrastructure failure.
//      The whole Eval() call fails with that status — use for "my
//      backend is down", never for policy-visible conditions.
//
// To choose 1 or 2 the lambda's return type is StatusOr<Value>, which
// can carry any kind.
//
//   bazel run //examples:08_function_errors_and_unknowns
//
// Expected output:
//   quota("alice")  =>  int: 100
//   quota("")       =>  error value: runtime error code 18
//   quota("bob") || is_admin  =>  bool: true (the unknown was absorbed)
//   quota("carol")  =>  Eval failed with status: FAILED_PRECONDITION: ...
//                       Caused by: quota backend down
//
// (Note the second line: the ErrorPayload's *code* survives the wasm
// round-trip; the free-text `message` currently does not — the decoded
// error carries a synthesized "runtime error code N" string.)

#include <iostream>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/error.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

// One handler exercising all three outcomes, keyed on the input.
absl::StatusOr<celwasm::Value> Quota(absl::string_view user) {
  if (user == "carol") {
    // 3. Infrastructure failure: fail the whole Eval.
    return absl::InternalError("quota backend down");
  }
  if (user.empty()) {
    // 1. Domain error: a CEL error value the expression can absorb.
    celwasm::ErrorPayload payload;
    payload.code = celwasm::ErrorCode::kInvalidArgument;
    payload.message = "quota: user must not be empty";
    return celwasm::Value::Error(std::move(payload));
  }
  if (user == "bob") {
    // 2. Unknown: "I can't answer yet" — partial-eval composes with it.
    return celwasm::Value::Unknown(
        celwasm::AttributeId{celwasm::kFunctionUnknownSentinel});
  }
  return celwasm::Value::Int(100);
}

celwasm::Instance BuildInstance(absl::string_view expr) {
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("user", celwasm::CelType::String())
      .DeclareVariable("is_admin", celwasm::CelType::Bool());
  builder.AddFunction("int @host.quota(string user);");
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());
  auto program = compiler->Compile(expr);
  ABSL_QCHECK_OK(program.status());

  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  ABSL_QCHECK_OK(engine->AddTypedFunction("quota_string", Quota));
  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

void EvalQuota(celwasm::Instance& instance, absl::string_view user) {
  celwasm::Activation activation;
  activation.Bind("user", celwasm::Value::String(std::string(user)))
      .Bind("is_admin", celwasm::Value::Bool(false));
  auto result = instance.Eval(activation);
  if (!result.ok()) {
    std::cout << "quota(\"" << user
              << "\")  =>  Eval failed with status: " << result.status()
              << "\n";
    return;
  }
  if (result->IsError()) {
    std::cout << "quota(\"" << user << "\")  =>  error value: "
              << (*result->ErrorInfo())->message << "\n";
    return;
  }
  std::cout << "quota(\"" << user << "\")  =>  int: " << result->AsInt().value()
            << "\n";
}

}  // namespace

int main() {
  celwasm::Instance quota_only = BuildInstance("quota(user)");
  EvalQuota(quota_only, "alice");  // 1. plain value
  EvalQuota(quota_only, "");       // 2. CEL error value

  // 3. Unknown absorption: `unknown || true` is true — CEL's
  // three-valued logic lets the policy answer without the quota.
  celwasm::Instance with_or = BuildInstance("quota(user) > 50 || is_admin");
  celwasm::Activation activation;
  activation.Bind("user", celwasm::Value::String("bob"))
      .Bind("is_admin", celwasm::Value::Bool(true));
  auto absorbed = with_or.Eval(activation);
  ABSL_QCHECK_OK(absorbed.status());
  std::cout << "quota(\"bob\") || is_admin  =>  bool: "
            << (absorbed->AsBool().value() ? "true" : "false")
            << " (the unknown was absorbed)\n";

  // 4. Infrastructure failure fails the Eval itself.
  EvalQuota(quota_only, "carol");
  return 0;
}
