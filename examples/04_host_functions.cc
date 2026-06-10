// Custom functions, path 1: `@host.` — a trusted C++ lambda running
// in your process.  ONE `.celfn` declaration string is the whole
// contract: the Compiler type-checks call sites against it, and
// Engine::BindFunction registers your lambda under it, validating the
// lambda's signature against the declaration at registration time.
//
// (Path 2 — `@component.`, sandboxed wasm components for code you
// DON'T trust — is examples/09_component_functions.cc.)
//
//   bazel run //examples:04_host_functions
//
// Expected output:
//   tier="gold"   price 100  =>  pay 80
//   tier="silver" price 100  =>  pay 90

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace {

// The single source of truth for the function's CEL signature.
constexpr absl::string_view kDiscountDecl =
    "int @host.discount_pct(string tier);";

celwasm::Instance BuildInstance() {
  // Compile side: the declaration makes `discount_pct` type-check
  // like any builtin.
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable("tier", celwasm::CelType::String())
      .DeclareVariable("price", celwasm::CelType::Int());
  builder.AddFunction(kDiscountDecl);
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());

  auto program = compiler->Compile("price - price * discount_pct(tier) / 100");
  ABSL_QCHECK_OK(program.status());

  // Runtime side: bind the implementation with the SAME declaration.
  // The lambda takes native C++ types; a signature that doesn't match
  // the declaration is rejected right here, not at eval time.
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  ABSL_QCHECK_OK(engine->BindFunction(
      kDiscountDecl, [](absl::string_view tier) -> absl::StatusOr<int64_t> {
        return tier == "gold" ? 20 : 10;
      }));

  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());
  return *std::move(instance);
}

}  // namespace

int main() {
  celwasm::Instance instance = BuildInstance();

  for (absl::string_view tier : {"gold", "silver"}) {
    celwasm::Activation activation;
    activation.Bind("tier", celwasm::Value::String(std::string(tier)))
        .Bind("price", celwasm::Value::Int(100));
    auto result = instance.Eval(activation);
    ABSL_QCHECK_OK(result.status());
    std::cout << "tier=\"" << tier << "\"\tprice 100  =>  pay "
              << result->AsInt().value() << "\n";
  }
  return 0;
}
