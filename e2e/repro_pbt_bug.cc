// One-off reproducer for the PBT-discovered divergence.
#include <iostream>
#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

using namespace celwasm;

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: repro <source>\n"; return 1; }
  const std::string source = argv[1];

  Compiler::Builder b;
  b.DeclareVariable("i_a", CelType::Int());
  b.DeclareVariable("i_b", CelType::Int());
  b.DeclareVariable("b_a", CelType::Bool());
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);

  auto program = compiler->Compile(source);
  if (!program.ok()) { std::cerr << "Compile failed: " << program.status() << "\n"; return 2; }

  static Engine* engine = []() {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  auto instance = engine->Plan(*program);
  if (!instance.ok()) { std::cerr << "Plan failed: " << instance.status() << "\n"; return 3; }

  Activation a;
  a.Bind("i_a", Value::Int(7));
  a.Bind("i_b", Value::Int(11));
  a.Bind("b_a", Value::Bool(true));

  auto v = instance->Eval(a);
  if (!v.ok()) { std::cerr << "Eval status fail: " << v.status() << "\n"; return 4; }

  std::cout << "kind=" << static_cast<int>(v->kind()) << " (";
  switch (v->kind()) {
    case Value::Kind::kBool:   std::cout << "Bool=" << (*v->AsBool() ? "true" : "false"); break;
    case Value::Kind::kInt:    std::cout << "Int=" << *v->AsInt(); break;
    case Value::Kind::kUint:   std::cout << "Uint=" << *v->AsUint(); break;
    case Value::Kind::kDouble: std::cout << "Double=" << *v->AsDouble(); break;
    case Value::Kind::kError:  std::cout << "Error"; break;
    default: std::cout << "(unprinted kind)"; break;
  }
  std::cout << ")\n";
  return 0;
}
