// Protobuf messages as CEL variables: declare the variable with the
// message's fully-qualified name, bind a real proto with
// Value::Message, and the expression reads fields directly —
// including has() — with zero copying (fields decode lazily through
// the message's reflection).
//
//   bazel run //examples:06_proto_messages
//
// Expected output:
//   user=alice action=read quantity=10  =>  allowed: true
//   user=mallory action=delete quantity=10  =>  allowed: false

#include <iostream>

#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "examples/example_request.pb.h"
#include "shared/type.h"

int main() {
  // The message type resolves from protobuf's generated pool — any
  // cc_proto_library linked into the binary is visible automatically.
  auto builder = celwasm::Compiler::NewBuilder();
  builder.DeclareVariable(
      "request", celwasm::CelType::Message("celwasm.examples.Request"));
  auto compiler = std::move(builder).Build();
  ABSL_QCHECK_OK(compiler.status());

  auto program = compiler->Compile(
      "request.action in ['read', 'list'] && request.quantity <= 100");
  ABSL_QCHECK_OK(program.status());

  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_QCHECK_OK(engine.status());
  auto instance = engine->Plan(*program);
  ABSL_QCHECK_OK(instance.status());

  celwasm::examples::Request read_req;
  read_req.set_user("alice");
  read_req.set_action("read");
  read_req.set_quantity(10);

  celwasm::examples::Request delete_req;
  delete_req.set_user("mallory");
  delete_req.set_action("delete");
  delete_req.set_quantity(10);

  for (const auto* req : {&read_req, &delete_req}) {
    // Value::Message is non-owning: keep the proto alive through Eval.
    celwasm::Activation activation;
    activation.Bind("request", celwasm::Value::Message(*req));
    auto result = instance->Eval(activation);
    ABSL_QCHECK_OK(result.status());
    std::cout << "user=" << req->user() << " action=" << req->action()
              << " quantity=" << req->quantity() << "  =>  allowed: "
              << (result->AsBool().value() ? "true" : "false") << "\n";
  }
  return 0;
}
