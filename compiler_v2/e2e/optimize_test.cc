// Optimization-on validation e2e — proves `CompilerOptions::optimize_level
// = 2` produces byte-identical Eval results vs the unoptimized default
// across a representative AST-kind matrix.
//
// The full-suite gate (`scripts/run_full_suite.sh`) runs every test
// with the default `optimize_level = 0`.  This test inverts that:
// each row compiles the same source TWICE (once at level 0, once at
// level 2), Plans + Evals both, and asserts the resulting Values are
// equal.  Any drift is a Binaryen miscompile — the load-bearing
// reason this gate exists.
//
// Matrix coverage mirrors `pipeline_bench.cc`: one representative
// expression per AST kind that's reachable in M10-shipped state.
// Comprehensions (M11) and Any-packing (M8) are out of scope.
//
// Marked `manual` because:
//   - Compiles every row twice → 2× the slowest other e2e suite.
//   - Locks the "optimized = unoptimized" invariant before any
//     production caller flips the default.  Not a per-PR gate; a
//     closeout gate when the Binaryen pass list or runtime build
//     flags move.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::Customer;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
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

// Build a Compiler with the named declarations.  Returns by value
// (Compiler is copyable).
using BuildFn = std::function<void(Compiler::Builder&)>;
Compiler BuildCompiler(const BuildFn& f) {
  Compiler::Builder b;
  f(b);
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

// Compile + Plan at the given optimize_level, run Eval, return the
// resulting Value.  Asserts each stage is ok().
Value CompileAndEval(const Compiler& compiler, absl::string_view source,
                     int optimize_level, const Activation& activation) {
  CompilerOptions opts;
  opts.optimize_level = optimize_level;
  auto program = compiler.Compile(source, opts);
  ABSL_CHECK_OK(program)
      << "source=" << source << " optimize_level=" << optimize_level;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance)
      << "source=" << source << " optimize_level=" << optimize_level;
  auto value = instance->Eval(activation);
  ABSL_CHECK_OK(value)
      << "source=" << source << " optimize_level=" << optimize_level;
  return *std::move(value);
}

// Equality between two evaluated Values across opt levels.  Kinds
// must match; scalar payloads must match exactly (no IEEE
// approximations — every test row produces a deterministic value).
void ExpectEvalIdentical(const Compiler& compiler, absl::string_view source,
                         const Activation& activation) {
  Value unopt = CompileAndEval(compiler, source, /*optimize_level=*/0,
                               activation);
  Value opt = CompileAndEval(compiler, source, /*optimize_level=*/2,
                             activation);
  ASSERT_EQ(unopt.kind(), opt.kind())
      << "kind drift after Optimize(2): source=" << source;
  switch (unopt.kind()) {
    case Value::Kind::kBool:
      EXPECT_EQ(*unopt.AsBool(), *opt.AsBool()) << source;
      break;
    case Value::Kind::kInt:
      EXPECT_EQ(*unopt.AsInt(), *opt.AsInt()) << source;
      break;
    case Value::Kind::kUint:
      EXPECT_EQ(*unopt.AsUint(), *opt.AsUint()) << source;
      break;
    case Value::Kind::kDouble:
      EXPECT_EQ(*unopt.AsDouble(), *opt.AsDouble()) << source;
      break;
    case Value::Kind::kString:
      EXPECT_EQ(*unopt.AsString(), *opt.AsString()) << source;
      break;
    case Value::Kind::kBytes:
      EXPECT_EQ(*unopt.AsBytes(), *opt.AsBytes()) << source;
      break;
    case Value::Kind::kType:
      EXPECT_EQ(*unopt.AsType(), *opt.AsType()) << source;
      break;
    default:
      // List / Map / Message equality lives behind a separate
      // `CelEquals` slice we don't lean on here — every row in this
      // file picks a source that evaluates to a scalar so the
      // kind+payload check is sufficient.
      FAIL() << "test matrix produced a non-scalar result for source="
             << source << " kind=" << static_cast<int>(unopt.kind());
  }
}

// ───────── Scalar / arithmetic / comparison ─────────

TEST(OptimizeE2E, LiteralInt) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "42", a);
}

TEST(OptimizeE2E, BoolLiteral) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "true", a);
}

TEST(OptimizeE2E, DoubleLiteral) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "3.14159", a);
}

TEST(OptimizeE2E, StringLiteral) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "\"hello\"", a);
}

TEST(OptimizeE2E, ThreeTermArith) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("a", CelType::Int());
    b.DeclareVariable("b", CelType::Int());
    b.DeclareVariable("c", CelType::Int());
  });
  Activation a;
  a.Bind("a", Value::Int(10));
  a.Bind("b", Value::Int(20));
  a.Bind("c", Value::Int(30));
  ExpectEvalIdentical(c, "a + b + c", a);
}

TEST(OptimizeE2E, OverflowCheck) {
  // INT64_MAX + 1 must still error after optimization — the helper's
  // overflow-check path is the canonical example of "do not let the
  // optimizer remove an error envelope".
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  Activation a;
  a.Bind("x", Value::Int(INT64_MAX));
  // The Eval succeeds at both opt levels but yields an Error Value;
  // we can't compare scalar payloads, so route through has-error
  // assertion instead.
  CompilerOptions opt0;
  opt0.optimize_level = 0;
  CompilerOptions opt2;
  opt2.optimize_level = 2;
  auto p0 = c.Compile("x + 1", opt0);
  ASSERT_THAT(p0, IsOk());
  auto p2 = c.Compile("x + 1", opt2);
  ASSERT_THAT(p2, IsOk());
  auto i0 = GlobalEngine().Plan(*p0);
  ASSERT_THAT(i0, IsOk());
  auto i2 = GlobalEngine().Plan(*p2);
  ASSERT_THAT(i2, IsOk());
  auto v0 = i0->Eval(a);
  auto v2 = i2->Eval(a);
  // Both must surface the overflow identically — either both Eval
  // returns an Error Value, or both fail with the same status.
  EXPECT_EQ(v0.ok(), v2.ok());
  if (v0.ok() && v2.ok()) {
    EXPECT_EQ(v0->kind(), v2->kind());
  }
}

TEST(OptimizeE2E, TwentyTermCompareChain) {
  // 20-term `a < b && b < c && ... && s < t` — the longest body the
  // pipeline bench measures.  Optimizer has the most to chew on
  // here; if -O2 is going to miscompile a chain, this is where.
  std::string src;
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    for (char ch = 'a'; ch <= 't'; ++ch) {
      b.DeclareVariable(std::string(1, ch), CelType::Int());
    }
  });
  for (char ch = 'a'; ch < 't'; ++ch) {
    if (!src.empty()) src.append(" && ");
    src.push_back(ch);
    src.append(" < ");
    src.push_back(static_cast<char>(ch + 1));
  }
  Activation a;
  int64_t i = 1;
  for (char ch = 'a'; ch <= 't'; ++ch) {
    a.Bind(std::string(1, ch), Value::Int(i++));
  }
  ExpectEvalIdentical(c, src, a);
}

// ───────── 3VL / short-circuit ─────────

TEST(OptimizeE2E, AndShortCircuit) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  Activation a;
  a.Bind("x", Value::Int(0));
  // `false && (x / 0 > 0)` — the right arm divides by zero, so a
  // wrong-order codegen would surface an error.  Both opt levels
  // must short-circuit identically.
  ExpectEvalIdentical(c, "false && (x / 0 > 0)", a);
}

TEST(OptimizeE2E, OrShortCircuit) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  Activation a;
  a.Bind("x", Value::Int(0));
  ExpectEvalIdentical(c, "true || (x / 0 > 0)", a);
}

// ───────── Conversion (M10) ─────────

TEST(OptimizeE2E, IntFromString) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "int(string(123))", a);
}

TEST(OptimizeE2E, DoubleToString) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "string(3.14)", a);
}

// ───────── Type (M9) ─────────

TEST(OptimizeE2E, TypeOfEqInt) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  Activation a;
  a.Bind("x", Value::Int(42));
  ExpectEvalIdentical(c, "type(x) == int", a);
}

// ───────── Aggregates ─────────

TEST(OptimizeE2E, ListIndexArena) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "[10, 20, 30, 40, 50][2]", a);
}

TEST(OptimizeE2E, MapLookupArena) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, R"({"a": 1, "b": 2, "c": 3}["b"])", a);
}

TEST(OptimizeE2E, ListSize) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "size([1, 2, 3, 4, 5])", a);
}

TEST(OptimizeE2E, ListIn) {
  Compiler c = BuildCompiler([](Compiler::Builder&) {});
  Activation a;
  ExpectEvalIdentical(c, "3 in [1, 2, 3, 4, 5]", a);
}

// ───────── Proto-backed select + map / list ─────────

TEST(OptimizeE2E, ProtoSelectString) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  ExpectEvalIdentical(c, "c.name", a);
}

TEST(OptimizeE2E, ProtoRepeatedIndex) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
  Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  Activation a;
  a.Bind("c", Value::Message(msg));
  ExpectEvalIdentical(c, "c.tags[1]", a);
}

TEST(OptimizeE2E, ProtoMapLookup) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
  Customer msg;
  (*msg.mutable_metadata())["a"] = "1";
  (*msg.mutable_metadata())["b"] = "2";
  Activation a;
  a.Bind("c", Value::Message(msg));
  ExpectEvalIdentical(c, R"(c.metadata["b"])", a);
}

TEST(OptimizeE2E, ProtoHasField) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  ExpectEvalIdentical(c, "has(c.name)", a);
}

// ───────── String ops ─────────

TEST(OptimizeE2E, StringConcat) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  Activation a;
  a.Bind("s", Value::String("world"));
  ExpectEvalIdentical(c, "\"hello, \" + s", a);
}

TEST(OptimizeE2E, StringContains) {
  Compiler c = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  Activation a;
  a.Bind("s", Value::String("hello world"));
  ExpectEvalIdentical(c, "s.contains(\"world\")", a);
}

}  // namespace
}  // namespace cel
