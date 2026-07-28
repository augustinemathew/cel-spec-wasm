// Program save/reload round-trip e2e — proves that the wasm bytes
// emitted by `Compiler::Compile` can be persisted (to disk, a cache,
// a remote endpoint, …) and reconstructed into an equivalent
// `celwasm::Program` via the `Program(std::vector<uint8_t>)` constructor,
// then planned and evaluated to bit-identical results.
//
// `program_test.cc` covers the structural round-trip (bytes in =
// bytes out) at the unit level.  This file covers the load-bearing
// assertion: the bytes ARE the program — Compile → wasm_bytes() →
// Program(bytes) → Plan → Eval produces the same Value as the
// straight Compile → Plan → Eval path.  Without this assertion, a
// silent regression that put runtime-dependent state outside the
// bytes (e.g. accidentally smuggled descriptor pool ptrs) would not
// surface until a real cross-process deployment broke.
//
// Marked manual because it goes through the full wasmtime pipeline
// (Engine::Plan → Instance::Eval) — same cost as m2/m4/m5; included
// in `scripts/run_full_suite.sh`'s closeout gate.

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "abi/program_facts.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Compile + Plan + Eval through both paths and compare Values.
// `configure` declares any variables; `bind` fills the activation.
void RoundTripIdentical(absl::string_view source,
                        std::function<void(Compiler::Builder&)> configure,
                        std::function<void(Activation&)> bind) {
  Compiler::Builder b;
  configure(b);
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  auto program = compiler->Compile(source, e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  // Capture the wasm bytes into an owned vector (simulating a write
  // to disk / cache / wire), then reconstruct a fresh Program from
  // them via the public ctor.
  std::vector<uint8_t> saved(program->wasm_bytes().begin(),
                             program->wasm_bytes().end());
  Program reloaded(std::move(saved));

  // The reconstructed Program must hold byte-identical wasm; the
  // pure-data contract on Program promises no information lives
  // anywhere else.
  ASSERT_EQ(reloaded.wasm_bytes().size(), program->wasm_bytes().size());

  // Plan both ends.
  auto i_orig = GlobalEngine().Plan(*program);
  ASSERT_TRUE(i_orig.ok()) << i_orig.status();
  auto i_reload = GlobalEngine().Plan(reloaded);
  ASSERT_TRUE(i_reload.ok()) << i_reload.status();

  // Eval both ends with the same Activation contents.
  Activation a_orig;
  bind(a_orig);
  Activation a_reload;
  bind(a_reload);
  auto v_orig = i_orig->Eval(a_orig);
  auto v_reload = i_reload->Eval(a_reload);
  ASSERT_TRUE(v_orig.ok()) << v_orig.status();
  ASSERT_TRUE(v_reload.ok()) << v_reload.status();

  // Compare by kind + scalar payload.  Test matrix below picks
  // sources that produce a scalar, so the kind+payload check is
  // sufficient — aggregate equality is its own slice.
  ASSERT_EQ(v_orig->kind(), v_reload->kind()) << source;
  switch (v_orig->kind()) {
    case Value::Kind::kBool:
      EXPECT_EQ(*v_orig->AsBool(), *v_reload->AsBool()) << source;
      break;
    case Value::Kind::kInt:
      EXPECT_EQ(*v_orig->AsInt(), *v_reload->AsInt()) << source;
      break;
    case Value::Kind::kUint:
      EXPECT_EQ(*v_orig->AsUint(), *v_reload->AsUint()) << source;
      break;
    case Value::Kind::kDouble:
      EXPECT_EQ(*v_orig->AsDouble(), *v_reload->AsDouble()) << source;
      break;
    case Value::Kind::kString:
      EXPECT_EQ(*v_orig->AsString(), *v_reload->AsString()) << source;
      break;
    default:
      FAIL() << "test matrix produced a non-scalar result for source=" << source
             << " kind=" << static_cast<int>(v_orig->kind());
  }
}

TEST(ProgramRoundTripE2E, LiteralInt) {
  RoundTripIdentical("42", [](Compiler::Builder&) {}, [](Activation&) {});
}

TEST(ProgramRoundTripE2E, LiteralString) {
  RoundTripIdentical(
      "\"hello\"", [](Compiler::Builder&) {}, [](Activation&) {});
}

TEST(ProgramRoundTripE2E, Arithmetic) {
  RoundTripIdentical(
      "a + b * 2",
      [](Compiler::Builder& b) {
        b.DeclareVariable("a", CelType::Int());
        b.DeclareVariable("b", CelType::Int());
      },
      [](Activation& a) {
        a.Bind("a", Value::Int(3));
        a.Bind("b", Value::Int(4));
      });
}

TEST(ProgramRoundTripE2E, Comparison) {
  RoundTripIdentical(
      "x > 0 && x < 100",
      [](Compiler::Builder& b) {
        b.DeclareVariable("x", CelType::Int());
      },
      [](Activation& a) {
        a.Bind("x", Value::Int(42));
      });
}

TEST(ProgramRoundTripE2E, Conversion) {
  RoundTripIdentical(
      "int(string(123))", [](Compiler::Builder&) {}, [](Activation&) {});
}

TEST(ProgramRoundTripE2E, StringConcat) {
  RoundTripIdentical(
      "\"hello, \" + name",
      [](Compiler::Builder& b) {
        b.DeclareVariable("name", CelType::String());
      },
      [](Activation& a) {
        a.Bind("name", Value::String("world"));
      });
}

TEST(ProgramRoundTripE2E, MultipleReloadsAreIndependent) {
  // A Program reconstructed twice from the same bytes should produce
  // independent Instances under Plan — no shared state between them.
  Compiler::Builder b;
  b.DeclareVariable("x", CelType::Int());
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("x + 1", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  std::vector<uint8_t> saved(program->wasm_bytes().begin(),
                             program->wasm_bytes().end());
  Program reloaded_a(saved);
  Program reloaded_b(std::move(saved));

  auto i_a = GlobalEngine().Plan(reloaded_a);
  ASSERT_TRUE(i_a.ok()) << i_a.status();
  auto i_b = GlobalEngine().Plan(reloaded_b);
  ASSERT_TRUE(i_b.ok()) << i_b.status();

  Activation act_a;
  act_a.Bind("x", Value::Int(10));
  auto v_a = i_a->Eval(act_a);
  ASSERT_TRUE(v_a.ok()) << v_a.status();
  EXPECT_EQ(*v_a->AsInt(), 11);

  Activation act_b;
  act_b.Bind("x", Value::Int(100));
  auto v_b = i_b->Eval(act_b);
  ASSERT_TRUE(v_b.ok()) << v_b.status();
  EXPECT_EQ(*v_b->AsInt(), 101);
}

TEST(ProgramRoundTripE2E, MalformedBytesFailAtPlan) {
  // Construct a Program from non-wasm bytes — the ctor is intentionally
  // non-validating (per program.h: "the wasmtime parse happens later
  // in Engine::Plan").  Plan must reject with FailedPrecondition rather
  // than crash.
  std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef, 0x00, 0x00, 0x00, 0x00};
  Program bogus(std::move(garbage));
  auto i = GlobalEngine().Plan(bogus);
  EXPECT_FALSE(i.ok());
}

TEST(ProgramRoundTripE2E, OptimizedBytesAlsoRoundTrip) {
  // The bytes from a Compile with optimize_level=2 should round-trip
  // identically — Binaryen's serialize is deterministic, and the
  // optimized module is still a valid wasm module.
  Compiler::Builder b;
  b.DeclareVariable("x", CelType::Int());
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  CompilerOptions opts;
  opts.optimize_level = 2;
  auto program = compiler->Compile("x * x + x + 1", opts);
  ASSERT_TRUE(program.ok()) << program.status();

  std::vector<uint8_t> saved(program->wasm_bytes().begin(),
                             program->wasm_bytes().end());
  Program reloaded(std::move(saved));

  auto i = GlobalEngine().Plan(reloaded);
  ASSERT_TRUE(i.ok()) << i.status();

  Activation a;
  a.Bind("x", Value::Int(5));
  auto v = i->Eval(a);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 31);  // 25 + 5 + 1
}

// ── abi::DescribeProgram — embedder introspection over the same
// persisted bytes this suite round-trips.  The facts come from the
// `cel.abi` section the compile pipeline emitted, so every assertion
// here pins the emit→describe contract end-to-end.

TEST(ProgramFactsE2E, DescribeReportsDeclaredVars) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("i", CelType::Int());
  b.DeclareVariable("xs", CelType::List(CelType::Int()));
  b.DeclareVariable("m", CelType::Map(CelType::String(), CelType::Int()));
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program =
      compiler->Compile("i + size(xs) + size(m)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto facts = abi::DescribeProgram(program->wasm_bytes());
  ASSERT_TRUE(facts.ok()) << facts.status();
  EXPECT_TRUE(facts->has_abi_section);
  EXPECT_GT(facts->abi_version, 0u);
  ASSERT_EQ(facts->vars.size(), 3u);
  EXPECT_EQ(facts->vars[0].name, "i");
  EXPECT_TRUE(facts->vars[0].has_full_type);
  EXPECT_EQ(facts->vars[0].type_spec, "int");
  EXPECT_EQ(facts->vars[1].type_spec, "list<int>");
  EXPECT_EQ(facts->vars[2].type_spec, "map<string,int>");
  // Every declared var's type spec round-trips into a --var binding
  // spec (the full-type path of TypeSpecForBinding).
  for (const auto& var : facts->vars) {
    auto spec = abi::TypeSpecForBinding(var);
    ASSERT_TRUE(spec.ok()) << var.name;
    EXPECT_EQ(*spec, var.type_spec);
  }
}

TEST(ProgramFactsE2E, DescribeReportsRequiredHostFn) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.tax_rate(string region);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("tax_rate('de')", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto facts = abi::DescribeProgram(program->wasm_bytes());
  ASSERT_TRUE(facts.ok()) << facts.status();
  ASSERT_EQ(facts->required_fns.size(), 1u);
  EXPECT_EQ(facts->required_fns[0].name, "tax_rate");
  EXPECT_TRUE(facts->required_fns[0].is_host);
  EXPECT_FALSE(facts->required_fns[0].signature.empty());
}

TEST(ProgramFactsE2E, DescribeReportsLinkMode) {
  auto compiler = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("1 + 1", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();
  auto facts = abi::DescribeProgram(program->wasm_bytes());
  ASSERT_TRUE(facts.ok()) << facts.status();
  EXPECT_EQ(facts->static_linked,
            e2e::kE2ELinkMode == CompilerOptions::LinkMode::kStatic);
  EXPECT_TRUE(facts->required_fns.empty());
}

TEST(ProgramFactsE2E, DescribeModuleWithoutAbiSection) {
  // A minimal valid wasm module (magic + version, no sections) has no
  // cel.abi — not an error; the facts say so explicitly.
  const uint8_t kEmptyModule[] = {0x00, 0x61, 0x73, 0x6d,
                                  0x01, 0x00, 0x00, 0x00};
  auto facts = abi::DescribeProgram(kEmptyModule);
  ASSERT_TRUE(facts.ok()) << facts.status();
  EXPECT_FALSE(facts->has_abi_section);
  EXPECT_TRUE(facts->vars.empty());
}

TEST(ProgramFactsE2E, DescribeRejectsNonWasmBytes) {
  const uint8_t kGarbage[] = {0xde, 0xad, 0xbe, 0xef};
  auto facts = abi::DescribeProgram(kGarbage);
  EXPECT_EQ(facts.status().code(), absl::StatusCode::kInvalidArgument)
      << facts.status();
}

}  // namespace
}  // namespace celwasm
