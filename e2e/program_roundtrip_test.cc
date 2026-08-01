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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
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
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

// Force generated-pool registration of Customer's descriptor for the
// `proto(...)`-typed declaration row below.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<testdata::Customer>();
      return 0;
    }();

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Same scalar payload, by kind.  The round-trip matrix picks sources
// that produce a scalar, so this check is sufficient — aggregate
// equality is its own slice.  Non-scalar kinds compare unequal so the
// caller's EXPECT fails loudly.
bool SameScalarPayload(const Value& a, const Value& b) {
  switch (a.kind()) {
    case Value::Kind::kBool:
      return *a.AsBool() == *b.AsBool();
    case Value::Kind::kInt:
      return *a.AsInt() == *b.AsInt();
    case Value::Kind::kUint:
      return *a.AsUint() == *b.AsUint();
    case Value::Kind::kDouble:
      return *a.AsDouble() == *b.AsDouble();
    case Value::Kind::kString:
      return *a.AsString() == *b.AsString();
    default:
      return false;
  }
}

// Compile `source`, capture the wasm bytes into an owned vector
// (simulating a write to disk / cache / wire), reconstruct a fresh
// Program via the public ctor, and plan BOTH ends.  The pure-data
// contract on Program promises no information lives outside the
// bytes, so the reloaded instance must behave identically.
std::pair<Instance, Instance> PlanOriginalAndReloaded(
    absl::string_view source,
    const std::function<void(Compiler::Builder&)>& configure) {
  Compiler::Builder b;
  configure(b);
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler.status());
  auto program = compiler->Compile(source, e2e::DefaultOpts());
  ABSL_CHECK_OK(program.status()) << source;
  std::vector<uint8_t> saved(program->wasm_bytes().begin(),
                             program->wasm_bytes().end());
  Program reloaded(std::move(saved));
  ABSL_CHECK(reloaded.wasm_bytes().size() == program->wasm_bytes().size());
  auto i_orig = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(i_orig.status()) << source;
  auto i_reload = GlobalEngine().Plan(reloaded);
  ABSL_CHECK_OK(i_reload.status()) << source;
  return {*std::move(i_orig), *std::move(i_reload)};
}

// Compile + Plan + Eval through both paths and compare Values.
// `configure` declares any variables; `bind` fills the activation.
void RoundTripIdentical(
    absl::string_view source,
    const std::function<void(Compiler::Builder&)>& configure,
    const std::function<void(Activation&)>& bind) {
  auto [i_orig, i_reload] = PlanOriginalAndReloaded(source, configure);
  Activation a_orig;
  bind(a_orig);
  Activation a_reload;
  bind(a_reload);
  auto v_orig = i_orig.Eval(a_orig);
  auto v_reload = i_reload.Eval(a_reload);
  ASSERT_TRUE(v_orig.ok()) << v_orig.status();
  ASSERT_TRUE(v_reload.ok()) << v_reload.status();
  ASSERT_EQ(v_orig->kind(), v_reload->kind()) << source;
  EXPECT_TRUE(SameScalarPayload(*v_orig, *v_reload)) << source;
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

// Every declarable variable kind's `type_spec` as DescribeProgram
// renders it (abi/program_facts.cc RenderVarTypeSpec).  This string is
// user-visible — `cel inspect` prints it and `TypeSpecForBinding`
// round-trips it into a `--var` spec — so a wrong spelling ships as a
// wrong CLI contract.
TEST(ProgramFactsE2E, DescribeRendersEveryDeclarableVarTypeSpec) {
  struct Row {
    absl::string_view name;
    CelType type;
    absl::string_view spec;
  };
  const Row kRows[] = {
      {"vb", CelType::Bool(), "bool"},
      {"vi", CelType::Int(), "int"},
      {"vu", CelType::Uint(), "uint"},
      {"vd", CelType::Double(), "double"},
      {"vs", CelType::String(), "string"},
      {"vy", CelType::Bytes(), "bytes"},
      {"vdur", CelType::Duration(), "duration"},
      {"vts", CelType::Timestamp(), "timestamp"},
      {"vt", CelType::Type(), "type"},
      {"vm", CelType::Message("celwasm.testdata.Customer"),
       "celwasm.testdata.Customer"},
      {"vl", CelType::List(CelType::Bytes()), "list<bytes>"},
      {"vmap", CelType::Map(CelType::Uint(), CelType::Double()),
       "map<uint,double>"},
      // Nesting recurses through the same renderer.
      {"vnest", CelType::List(CelType::Map(CelType::String(), CelType::Int())),
       "list<map<string,int>>"},
  };
  auto b = Compiler::NewBuilder();
  for (const Row& row : kRows) {
    b.DeclareVariable(std::string(row.name), row.type);
  }
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  // cel.abi records only the variables the expression REFERENCES, so
  // every row has to appear in the source.
  auto program = compiler->Compile(
      "vb && vi == 1 && vu == 1u && vd == 1.0 && vs == '' && vy == b'' && "
      "vdur == duration('0s') && "
      "vts == timestamp('1970-01-01T00:00:00Z') && vt == int && "
      "vm.name == '' && size(vl) == 0 && size(vmap) == 0 && size(vnest) == 0",
      e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto facts = abi::DescribeProgram(program->wasm_bytes());
  ASSERT_TRUE(facts.ok()) << facts.status();
  // Match by name — the ABI's ordering is its own business.
  for (const Row& row : kRows) {
    const auto it = std::find_if(facts->vars.begin(), facts->vars.end(),
                                 [&](const abi::DeclaredVar& v) {
                                   return v.name == row.name;
                                 });
    ASSERT_NE(it, facts->vars.end()) << row.name << " missing from cel.abi";
    EXPECT_EQ(it->type_spec, row.spec) << row.name;
  }
}

TEST(ProgramFactsE2E, DescribeRendersSignaturesAcrossTypeFamilies) {
  // One compiled program per declarable type family; the described
  // signature is `abi::RenderSignature` over the `cel.abi`
  // required-functions row, so each row pins the wire spelling of a
  // family (scalars, temporals, aggregates, receiver form, proto).
  struct Row {
    absl::string_view decl;
    absl::string_view expr;
    absl::string_view want_signature;
  };
  const Row kRows[] = {
      {"bool @host.flag(uint u, double d);", "flag(1u, 1.5)",
       "bool flag(uint, double)"},
      {"bytes @host.digest(string s);", "digest('x')", "bytes digest(string)"},
      {"null @host.nil();", "nil()", "null nil()"},
      {"Duration @host.lag(Timestamp t);",
       "lag(timestamp('2024-01-01T00:00:00Z'))", "Duration lag(Timestamp)"},
      {"list<int> @host.ids(map<string, int> m);", "ids({'a': 1})",
       "list<int> ids(map<string, int>)"},
      {"string @host.upper(this string s);", "'x'.upper()",
       "string upper(this string)"},
      {"int @host.rank(proto(celwasm.testdata.Customer) c);", "rank(cust)",
       "int rank(proto(celwasm.testdata.Customer))"},
  };
  for (const Row& row : kRows) {
    auto b = Compiler::NewBuilder();
    b.AddFunction(row.decl);
    b.DeclareVariable("cust", CelType::Message("celwasm.testdata.Customer"));
    auto compiler = std::move(b).Build();
    ASSERT_TRUE(compiler.ok()) << row.decl << ": " << compiler.status();
    auto program = compiler->Compile(row.expr, e2e::DefaultOpts());
    ASSERT_TRUE(program.ok()) << row.expr << ": " << program.status();
    auto facts = abi::DescribeProgram(program->wasm_bytes());
    ASSERT_TRUE(facts.ok()) << facts.status();
    ASSERT_EQ(facts->required_fns.size(), 1u) << row.decl;
    EXPECT_TRUE(facts->required_fns[0].is_host) << row.decl;
    EXPECT_EQ(facts->required_fns[0].signature, row.want_signature);
  }
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
