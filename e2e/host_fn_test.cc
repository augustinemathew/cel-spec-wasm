// End-to-end coverage for the `@host.` custom-function ("native
// function") backend.  Every test drives the WHOLE flow exactly as a
// customer would — no test-only wrappers or DTOs in the way:
//
//   Compiler::NewBuilder().DeclareVariable(...).AddFunction("<celfn IDL>")
//     .Build()                       // declare the host fn on the compiler
//   compiler->Compile("<expr>", e2e::DefaultOpts())  // compile a CEL
//     expr that CALLS it (link mode from the dual-mode build macro —
//     see e2e/link_mode_e2e_helpers.h)
//   Engine::NewBuilder().Build()
//   engine->AddTypedFunction(id, lambda)   // typed registration (Layer 2)
//     -- or --
//   engine->AddFunction(id, num_args, ctx-lambda)   // context registration
//   engine->Plan(*program)           // bind imports → Instance
//   instance->Eval(act)              // run through real wasmtime
//
// The two registration surfaces are both exercised across the full
// type matrix:
//
//   * AddTypedFunction — the typed sugar a customer reaches for first.
//     The closure takes native C++ params (int64_t, absl::string_view,
//     const Customer&, HostMapView, ...) and returns absl::StatusOr<R>.
//     A typed customer never writes an arg count or touches a raw slot.
//   * AddFunction — the context surface, for callbacks that need
//     per-argument kind control or want to emit an unknown / error
//     directly.  Here the customer does pass the real arg count and a
//     `HostCallContext&` closure.
//
// The exhaustive per-type decode / encode matrix (positive + negative +
// boundary, over fake primitives) lives in host_call_context_test.cc;
// this file proves the trampoline *wiring* — that the context is built
// with the real memory / externref table / arena and that 3VL operand
// absorption fires — end-to-end through wasmtime.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/host_call_context.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Customer;

// ════════════════════════════════════════════════════════════════
// Typed API (Engine::AddTypedFunction) — the customer's first reach.
// Full argument / return type matrix, end-to-end through wasmtime.
// ════════════════════════════════════════════════════════════════

TEST(HostFnTest, TypedIntLambdaDoubles) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("double_it_int",
                                     [](int64_t x) -> absl::StatusOr<int64_t> {
                                       return x * 2;
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(21));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 42);
}

TEST(HostFnTest, TypedUintLambdaIncrements) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Uint());
  b.AddFunction("uint @host.inc(uint x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("inc(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("inc_uint",
                             [](uint64_t x) -> absl::StatusOr<uint64_t> {
                               return x + 1;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Uint(99u));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsUint(), 100u);
}

TEST(HostFnTest, TypedDoubleLambdaHalves) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Double());
  b.AddFunction("double @host.half(double x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("half(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("half_double",
                                     [](double x) -> absl::StatusOr<double> {
                                       return x / 2.0;
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Double(7.0));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_DOUBLE_EQ(*v->AsDouble(), 3.5);
}

TEST(HostFnTest, TypedBoolLambdaNegates) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bool());
  b.AddFunction("bool @host.negate(bool b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("negate(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("negate_bool",
                                     [](bool v) -> absl::StatusOr<bool> {
                                       return !v;
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("b", Value::Bool(true));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_FALSE(*v->AsBool());
}

// string → string: the typed closure builds a brand-new std::string and
// returns it; the trampoline arena-allocates the bytes back into linear
// memory.  The headline m21 win — the pre-adapter ABI had no arena and
// could only transform a string in place.
TEST(HostFnTest, TypedStringLambdaAllocatesReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("string @host.upper(this string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("s.upper()", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "upper_string",
                      [](absl::string_view s) -> absl::StatusOr<std::string> {
                        return absl::AsciiStrToUpper(s);
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String("hello"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "HELLO");
}

// bytes arg through the typed API: a CEL `bytes` value is accepted by an
// `absl::string_view` parameter (string and bytes share the C++ type;
// the trait accepts either kind).
TEST(HostFnTest, TypedBytesLambdaComputesLength) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bytes());
  b.AddFunction("int @host.blen(bytes b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("blen(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "blen_bytes",
                      [](absl::string_view b) -> absl::StatusOr<int64_t> {
                        return static_cast<int64_t>(b.size());
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("b", Value::Bytes(std::string("\x01\x02\x03\x04", 4)));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 4);
}

// A typed lambda CAN return `bytes`-kinded data — but it must do so
// through the `Value` escape hatch (`StatusOr<Value>` → `Value::Bytes`),
// because a `std::string` return is `string`-kinded by design.  The
// embedded NUL proves the {ptr,len} span carries length explicitly.
TEST(HostFnTest, TypedLambdaReturnsBytesViaValue) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bytes());
  b.AddFunction("bytes @host.echo_bytes(bytes b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_bytes(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("echo_bytes_bytes",
                             [](absl::string_view b) -> absl::StatusOr<Value> {
                               return Value::Bytes(std::string(b));
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::string payload;
  payload.push_back('a');
  payload.push_back('\0');
  payload.push_back('b');
  payload.push_back('\0');
  payload.push_back('c');
  Activation act;
  act.Bind("b", Value::Bytes(payload));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes);
  EXPECT_EQ(std::string(*v->AsBytes()), payload);
}

TEST(HostFnTest, TypedDurationLambdaDoubles) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("d", CelType::Duration());
  b.AddFunction("Duration @host.twice(Duration d);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("twice(d)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "twice_duration",
                      [](absl::Duration d) -> absl::StatusOr<absl::Duration> {
                        return d * 2;
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("d", Value::Duration(absl::Seconds(5)));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsDuration(), absl::Seconds(10));
}

TEST(HostFnTest, TypedTimestampLambdaRoundTrips) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("t", CelType::Timestamp());
  b.AddFunction("Timestamp @host.echo_ts(Timestamp t);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_ts(t)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("echo_ts_timestamp",
                             [](absl::Time t) -> absl::StatusOr<absl::Time> {
                               return t;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  const absl::Time t = absl::FromUnixSeconds(1700000000);
  Activation act;
  act.Bind("t", Value::Timestamp(t));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsTimestamp(), t);
}

// proto arg via a concrete `const Customer&` parameter.
TEST(HostFnTest, TypedConcreteProtoArg) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  b.AddFunction("bool @host.is_premium(proto(celwasm.testdata.Customer) c);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("is_premium(c)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("is_premium_message_celwasm_testdata_Customer",
                             [](const Customer& c) -> absl::StatusOr<bool> {
                               return c.is_premium();
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Customer c;
  c.set_is_premium(true);
  Activation act;
  act.Bind("c", Value::Message(c));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

// proto arg via a polymorphic `const google::protobuf::Message*`.
TEST(HostFnTest, TypedPolymorphicProtoArg) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  b.AddFunction("string @host.tname(proto(celwasm.testdata.Customer) c);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("tname(c)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("tname_message_celwasm_testdata_Customer",
                                     [](const google::protobuf::Message* m)
                                         -> absl::StatusOr<std::string> {
                                       return std::string(m->GetTypeName());
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Customer c;
  Activation act;
  act.Bind("c", Value::Message(c));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "celwasm.testdata.Customer");
}

// proto return: the closure builds a Customer host-side and returns a
// unique_ptr; CEL then reads a field off the interned message.
TEST(HostFnTest, TypedProtoReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("n", CelType::String());
  b.AddFunction(
      "proto(celwasm.testdata.Customer) @host.make_customer(string name);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("make_customer(n).name", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("make_customer_string",
                             [](absl::string_view name)
                                 -> absl::StatusOr<std::unique_ptr<Customer>> {
                               auto m = std::make_unique<Customer>();
                               m->set_name(std::string(name));
                               return m;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("n", Value::String("Grace"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "Grace");
}

// list arg via a `HostListView` parameter; the argument is an
// in-expression list LITERAL (arena-list representation).
TEST(HostFnTest, TypedListViewArg) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.sum(list<int> xs);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("sum([4, 5, 6])", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("sum_list_int",
                             [](HostListView xs) -> absl::StatusOr<int64_t> {
                               int64_t total = 0;
                               for (size_t i = 0; i < xs.Size(); ++i) {
                                 auto e = xs.At(i);
                                 if (!e.ok()) return e.status();
                                 total += *e->AsInt();
                               }
                               return total;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 15);
}

// map arg via a `HostMapView` parameter, bound-variable representation.
TEST(HostFnTest, TypedMapViewArg) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("m", CelType::Map(CelType::String(), CelType::Int()));
  b.DeclareVariable("k", CelType::String());
  b.AddFunction("int @host.lookup(map<string, int> m, string k);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("lookup(m, k)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "lookup_map_string_int_string",
                      [](HostMapView m,
                         absl::string_view k) -> absl::StatusOr<int64_t> {
                        auto got = m.Get(Value::String(std::string(k)));
                        if (!got.ok()) return got.status();
                        if (got->IsError()) return int64_t{-1};
                        return got->AsInt();
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("m", Value::Map({{Value::String("a"), Value::Int(10)},
                            {Value::String("b"), Value::Int(20)}}));
  act.Bind("k", Value::String("b"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);
}

// map arg via a LITERAL map — the wire shape is CEL_MAP_ARENA, so the
// context decodes entries straight out of the eval arena
// (`DecodeArenaMapEntries`, host_call_context.cc) rather than the
// host-backed table walk the bound-variable case above takes.  (The
// list sibling is covered by TypedListViewArg's `sum([4, 5, 6])`.)
TEST(HostFnTest, TypedMapViewArgFromLiteralMap) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.lookup(map<string, int> m, string k);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program =
      compiler->Compile("lookup({'a': 10, 'b': 20}, 'b')", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "lookup_map_string_int_string",
                      [](HostMapView m,
                         absl::string_view k) -> absl::StatusOr<int64_t> {
                        auto got = m.Get(Value::String(std::string(k)));
                        if (!got.ok()) return got.status();
                        if (got->IsError()) return int64_t{-1};
                        return got->AsInt();
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);
}

// A context callback eagerly decoding a LITERAL map arg via
// `ArgValue` — end-to-end through the real arena wire shape
// (host_call_context.cc's DecodeArenaMapEntries), where the
// HostMapView cases above decode lazily.  The callback sums the
// values through the decoded Value's map backing.
TEST(HostFnTest, ContextArgValueDecodesLiteralMapArg) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.msum(map<string, int> m);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program =
      compiler->Compile("msum({'a': 10, 'b': 20})", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("msum_map_string_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto v = ctx.ArgValue(0);
                                  if (!v.ok()) return v.status();
                                  auto backing = v->MapBacking();
                                  if (!backing.ok()) return backing.status();
                                  int64_t sum = 0;
                                  for (absl::string_view k : {"a", "b"}) {
                                    auto e = (*backing)->Get(
                                        Value::String(std::string(k)),
                                        CelType::Int());
                                    if (!e.ok()) return e.status();
                                    sum += *e->AsInt();
                                  }
                                  return ctx.ReturnInt(sum);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 30);
}

// An embedder callback reading an arg with the WRONG typed accessor:
// the mismatch diagnostic must name the actual wire kind (the
// WireKindName arms in host_call_context.cc) and surface as an eval
// failure — end-to-end proof the context's kind check is not
// bypassable from real wire values.
TEST(HostFnTest, ContextWrongAccessorDiagnosticNamesWireKind) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("d", CelType::Duration());
  b.AddFunction("int @host.dprobe(Duration d);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("dprobe(d)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("dprobe_duration", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  // Deliberate embedder bug: int read
                                  // of a duration slot.
                                  return ctx.ArgInt(0).status();
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  act.Bind("d", Value::Duration(absl::Seconds(5)));
  auto v = instance->Eval(act);
  ASSERT_FALSE(v.ok());
  EXPECT_TRUE(absl::StrContains(v.status().message(), "duration"))
      << v.status();
}

// What reaches the callback when an ARGUMENT evaluates to a runtime
// error: pins the dispatch contract for error-valued args through the
// full pipeline ([1][5] is int-typed but errors at eval).
TEST(HostFnTest, ErrorValuedArgContract) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.echo7(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo7([1][5])", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  int calls = 0;
  ASSERT_TRUE(engine
                  ->AddFunction("echo7_int", 2,
                                [&calls](HostCallContext& ctx) -> absl::Status {
                                  ++calls;
                                  return ctx.ReturnInt(7);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  // 3VL absorption: the error propagates without dispatching the
  // callback (mirrors cel-cpp strict-function semantics).
  EXPECT_TRUE(v->IsError());
  EXPECT_EQ(calls, 0);
}

// The unknown-valued analog of ErrorValuedArgContract: strict
// dispatch absorbs CEL_UNKNOWN args the same way it absorbs errors,
// so the callback never runs and the unknown propagates as the
// result (langdef partial-eval: unknown arguments short-circuit
// strict functions).
TEST(HostFnTest, UnknownValuedArgContract) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.echo8(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo8(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  int calls = 0;
  ASSERT_TRUE(engine
                  ->AddFunction("echo8_int", 2,
                                [&calls](HostCallContext& ctx) -> absl::Status {
                                  ++calls;
                                  return ctx.ReturnInt(8);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  act.Bind("x", Value::Int(41));
  auto pattern = AttributePattern::Parse("x");
  ASSERT_TRUE(pattern.ok()) << pattern.status();
  AttributePattern patterns[] = {*std::move(pattern)};
  auto v = instance->PartialEval(act, patterns);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown()) << static_cast<int>(v->kind());
  EXPECT_EQ(calls, 0);
}

// A literal-list argument decoded through the context's ArgValue:
// the arena-resident run decodes element-by-element (the
// DecodeArenaList walk in host_call_context.cc), end-to-end.
TEST(HostFnTest, ContextArgValueDecodesLiteralListArg) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.lsum(list<int> xs);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("lsum([1, 2, 3])", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("lsum_list_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto v = ctx.ArgValue(0);
                                  if (!v.ok()) return v.status();
                                  auto backing = v->ListBacking();
                                  if (!backing.ok()) return backing.status();
                                  int64_t sum = 0;
                                  for (size_t i = 0; i < (*backing)->Size();
                                       ++i) {
                                    auto e = (*backing)->At(i, CelType::Int());
                                    if (!e.ok()) return e.status();
                                    sum += *e->AsInt();
                                  }
                                  return ctx.ReturnInt(sum);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 6);
}

// HostMapView::ContainsKey from inside a callback — both the present
// and absent probes, against a literal (arena-resident) map.
TEST(HostFnTest, ContextMapViewContainsKey) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("bool @host.haskey(map<string, int> m, string k);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile(
      "haskey({'a': 1}, 'a') && "
      "!haskey({'a': 1}, 'z')",
      e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("haskey_map_string_int_string", 3,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto m = ctx.ArgMap(0);
                                  if (!m.ok()) return m.status();
                                  auto k = ctx.ArgString(1);
                                  if (!k.ok()) return k.status();
                                  return ctx.ReturnBool(m->ContainsKey(
                                      Value::String(std::string(*k))));
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

// Compile `kprobe(v)` with `v` declared as `type` / IDL `idl_type`.
Program CompileKprobeProgram(absl::string_view idl_type, const CelType& type) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("v", type);
  b.AddFunction(absl::StrCat("int @host.kprobe(", idl_type, " v);"));
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler.status());
  auto program = compiler->Compile("kprobe(v)", e2e::DefaultOpts());
  ABSL_CHECK_OK(program.status()) << idl_type;
  return *std::move(program);
}

// Register the wrong-accessor callback under `overload_id` and return
// the eval status for `bind`.
absl::Status WrongAccessorEvalStatus(const Program& program,
                                     absl::string_view overload_id,
                                     Value bind) {
  auto engine = Engine::NewBuilder().Build();
  ABSL_CHECK_OK(engine.status());
  ABSL_CHECK_OK(engine->AddFunction(  // Deliberate misread: int accessor.
      overload_id, 2, [](HostCallContext& ctx) -> absl::Status {
        return ctx.ArgInt(0).status();
      }));
  auto instance = engine->Plan(program);
  ABSL_CHECK_OK(instance.status()) << overload_id;
  Activation act;
  act.Bind("v", std::move(bind));
  return instance->Eval(act).status();
}

// One wrong-accessor probe: the ArgInt(0) misread must fail eval with
// a message naming `expect_kind` (that wire kind's WireKindName arm).
void ExpectWrongAccessorNames(absl::string_view idl_type, const CelType& type,
                              absl::string_view overload_id, Value bind,
                              absl::string_view expect_kind) {
  Program program = CompileKprobeProgram(idl_type, type);
  absl::Status s =
      WrongAccessorEvalStatus(program, overload_id, std::move(bind));
  ASSERT_FALSE(s.ok()) << idl_type;
  EXPECT_TRUE(absl::StrContains(s.message(), expect_kind))
      << idl_type << ": " << s;
}

// The WireKindName matrix: every wire kind a wrong accessor can name
// in its mismatch diagnostic, driven end-to-end from real bound
// values (the Duration arm is pinned above).
TEST(HostFnTest, WrongAccessorDiagnosticKindMatrix) {
  ExpectWrongAccessorNames("uint", CelType::Uint(), "kprobe_uint",
                           Value::Uint(5), "uint");
  ExpectWrongAccessorNames("double", CelType::Double(), "kprobe_double",
                           Value::Double(1.5), "double");
  ExpectWrongAccessorNames("bool", CelType::Bool(), "kprobe_bool",
                           Value::Bool(true), "bool");
  ExpectWrongAccessorNames("string", CelType::String(), "kprobe_string",
                           Value::String("s"), "string");
  ExpectWrongAccessorNames("bytes", CelType::Bytes(), "kprobe_bytes",
                           Value::Bytes("b"), "bytes");
  ExpectWrongAccessorNames(
      "Timestamp", CelType::Timestamp(), "kprobe_timestamp",
      Value::Timestamp(absl::FromUnixSeconds(1)), "timestamp");
  ExpectWrongAccessorNames("list<int>", CelType::List(CelType::Int()),
                           "kprobe_list_int", Value::List({Value::Int(1)}),
                           "list");
  ExpectWrongAccessorNames(
      "map<string, int>", CelType::Map(CelType::String(), CelType::Int()),
      "kprobe_map_string_int",
      Value::Map({{Value::String("a"), Value::Int(1)}}), "map");
}

// ── shared pieces for the deepest-composite tests ─────────────────
//
// Typed (TypedNestedMapStringListProtoArg) and context
// (ContextNestedMapStringListProtoArg) registration both exercise the
// same map<string, list<proto>> program shape and the same counting
// walk; the pieces live here so each TEST stays within the
// function-size lint thresholds.

// Compiles `count_premium(teams, k)` against the nested-composite decl.
absl::StatusOr<Program> CompileCountPremiumProgram() {
  const CelType team_map = CelType::Map(
      CelType::String(),
      CelType::List(CelType::Message("celwasm.testdata.Customer")));
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("teams", team_map);
  b.DeclareVariable("k", CelType::String());
  b.AddFunction(
      "int @host.count_premium("
      "map<string, list<proto(celwasm.testdata.Customer)>> teams, string k);");
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  return compiler->Compile("count_premium(teams, k)", e2e::DefaultOpts());
}

// teams = { "eng": [premium, not premium], "sales": [premium] }; k =
// "eng".  `Value::Message` is non-owning, so the fixture owns the
// Customers alongside the Activation that borrows them; it is
// heap-allocated so the bound message addresses stay stable.
struct TeamsFixture {
  Customer premium_eng;
  Customer regular_eng;
  Customer premium_sales;
  Activation act;
};

std::unique_ptr<TeamsFixture> MakeTeamsActivation() {
  auto f = std::make_unique<TeamsFixture>();
  f->premium_eng.set_is_premium(true);
  f->regular_eng.set_is_premium(false);
  f->premium_sales.set_is_premium(true);
  f->act.Bind("teams",
              Value::Map({{Value::String("eng"),
                           Value::List({Value::Message(f->premium_eng),
                                        Value::Message(f->regular_eng)})},
                          {Value::String("sales"),
                           Value::List({Value::Message(f->premium_sales)})}}));
  f->act.Bind("k", Value::String("eng"));
  return f;
}

// Looks up `k` in `teams` and counts premium Customers in the nested
// list<proto>: the list is reached via the Value that Get yields, then
// each proto element is reflected on.  -1 signals a missing key (the
// map Get yielded a CEL error value).
absl::StatusOr<int64_t> CountPremiumInTeam(const HostMapView& teams,
                                           absl::string_view k) {
  auto list_v = teams.Get(Value::String(std::string(k)));
  if (!list_v.ok()) return list_v.status();
  if (list_v->IsError()) return int64_t{-1};
  auto lb = list_v->ListBacking();
  if (!lb.ok()) return lb.status();
  int64_t n = 0;
  for (size_t i = 0; i < (*lb)->Size(); ++i) {
    auto e = (*lb)->At(i, CelType{});
    if (!e.ok()) return e.status();
    auto mb = e->MessageBacking();
    if (!mb.ok()) return mb.status();
    const auto* cust = dynamic_cast<const Customer*>((*mb)->message());
    if (cust != nullptr && cust->is_premium()) ++n;
  }
  return n;
}

// The deepest composite through the typed API: a
// map<string, list<proto>> param arrives as a `HostMapView`.
TEST(HostFnTest, TypedNestedMapStringListProtoArg) {
  auto program = CompileCountPremiumProgram();
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction(
              "count_premium_map_string_list_message_celwasm_testdata_Customer_"
              "string",
              [](HostMapView teams,
                 absl::string_view k) -> absl::StatusOr<int64_t> {
                return CountPremiumInTeam(teams, k);
              })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  auto teams = MakeTeamsActivation();
  auto v = instance->Eval(teams->act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 1);  // "eng" has one premium customer
}

// Typed receiver (`this`) dispatch: the receiver becomes arg 0.
TEST(HostFnTest, TypedReceiverIsFirstArg) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("bool @host.is_number(this string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("s.is_number()", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("is_number_string",
                             [](absl::string_view s) -> absl::StatusOr<bool> {
                               for (const char c : s) {
                                 if (c < '0' || c > '9') return false;
                               }
                               return !s.empty();
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String("12345"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

// Two typed fns compose: inc(double_it(x)).
TEST(HostFnTest, TypedFunctionsCompose) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  b.AddFunction("int @host.inc(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("inc(double_it(x))", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("double_it_int",
                                     [](int64_t x) -> absl::StatusOr<int64_t> {
                                       return x * 2;
                                     })
                  .ok());
  ASSERT_TRUE(engine
                  ->AddTypedFunction("inc_int",
                                     [](int64_t x) -> absl::StatusOr<int64_t> {
                                       return x + 1;
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(20));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 41);  // (20*2)+1
}

// Layer 3 sugar (Engine::BindFunction) — declaration-first form: the
// SAME `.celfn` string drives both the compiler and engine sides, so
// the overload-id and wasm arity are never hand-spelled.  The
// registration-level validation matrix lives in eval/engine_test.cc
// (EngineBindFunctionTest); this is the single end-to-end dispatch
// proof through wasmtime.
TEST(HostFnTest, BindFunctionDeclFirstRoundTrip) {
  constexpr absl::string_view kDecl = "int @host.discount_pct(string tier);";
  auto b = Compiler::NewBuilder();
  b.AddFunction(kDecl);
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("discount_pct('gold')", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->BindFunction(kDecl,
                         [](absl::string_view tier) -> absl::StatusOr<int64_t> {
                           return tier == "gold" ? 20 : 5;
                         })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  auto v = instance->Eval();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);
}

// A typed closure returning StatusOr<Value> can emit a function-origin
// unknown: the result is unknown carrying the reserved sentinel.
TEST(HostFnTest, TypedLambdaReturnsUnknown) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.make_unknown(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("make_unknown(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("make_unknown_int",
                                     [](int64_t) -> absl::StatusOr<Value> {
                                       return Value::Unknown(AttributeId{
                                           kFunctionUnknownSentinel});
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_TRUE(v->IsUnknown());
  ASSERT_TRUE(v->UnknownAttribute().ok());
  EXPECT_EQ(v->UnknownAttribute()->id, kFunctionUnknownSentinel);
}

// A typed closure returning StatusOr<Value> can emit a CEL error value.
TEST(HostFnTest, TypedLambdaReturnsErrorValue) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.always_err(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("always_err(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("always_err_int",
                                     [](int64_t) -> absl::StatusOr<Value> {
                                       ErrorPayload p;
                                       p.code = ErrorCode::kDivideByZero;
                                       return Value::Error(std::move(p));
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  if (v.ok()) {
    EXPECT_EQ(v->kind(), Value::Kind::kError);
  } else {
    SUCCEED() << "error surfaced as status: " << v.status();
  }
}

// A typed closure returning a non-OK status traps the eval.
TEST(HostFnTest, TypedLambdaNonOkStatusTraps) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.boom(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("boom(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("boom_int",
                                     [](int64_t) -> absl::StatusOr<int64_t> {
                                       return absl::InternalError(
                                           "typed callback exploded");
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  EXPECT_FALSE(v.ok()) << "a trapping typed callback must surface non-OK";
}

// Typed fn under PartialEval: a known arg dispatches and the closure
// runs; the same arg marked unknown auto-propagates without invoking the
// closure.
TEST(HostFnTest, TypedPartialEvalKnownDispatchesUnknownPropagates) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto invoked = std::make_shared<bool>(false);
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("double_it_int",
                             [invoked](int64_t x) -> absl::StatusOr<int64_t> {
                               *invoked = true;
                               return x * 2;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(21));

  // Known: dispatches, returns 42.
  auto known = instance->Eval(act);
  ASSERT_TRUE(known.ok()) << known.status();
  EXPECT_EQ(*known->AsInt(), 42);
  EXPECT_TRUE(*invoked);

  // Unknown: x marked unknown → typed closure NOT invoked.
  *invoked = false;
  auto pat = AttributePattern::Parse("x");
  ASSERT_TRUE(pat.ok()) << pat.status();
  std::vector<AttributePattern> unknowns;
  unknowns.push_back(*std::move(pat));
  auto v = instance->PartialEval(act, unknowns);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown());
  EXPECT_FALSE(*invoked) << "typed closure must not run on an unknown arg";
}

// ════════════════════════════════════════════════════════════════
// Context API (Engine::AddFunction) — for callbacks that need
// per-argument kind control or emit an unknown / error directly.  Here
// the customer passes the real arg count and a `HostCallContext&`.
// ════════════════════════════════════════════════════════════════

TEST(HostFnTest, ContextIntArgIntReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("double_it_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgInt(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnInt(*x * 2);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(21));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 42);
}

TEST(HostFnTest, ContextStringArgIntReturnComputesLength) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("int @host.length(string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("length(s)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("length_string", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto s = ctx.ArgString(0);
                                  if (!s.ok()) return s.status();
                                  return ctx.ReturnInt(
                                      static_cast<int64_t>(s->size()));
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String("hello"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 5);
}

TEST(HostFnTest, ContextBytesArgSumsBytes) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bytes());
  b.AddFunction("int @host.byte_sum(bytes b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("byte_sum(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("byte_sum_bytes", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto bytes = ctx.ArgBytes(0);
                                  if (!bytes.ok()) return bytes.status();
                                  int64_t sum = 0;
                                  for (const unsigned char c : *bytes) {
                                    sum += c;
                                  }
                                  return ctx.ReturnInt(sum);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("b", Value::Bytes(std::string("\x01\x02\x03", 3)));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 6);
}

TEST(HostFnTest, ContextBoolArgBoolReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bool());
  b.AddFunction("bool @host.negate(bool b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("negate(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("negate_bool", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto v = ctx.ArgBool(0);
                                  if (!v.ok()) return v.status();
                                  return ctx.ReturnBool(!*v);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("b", Value::Bool(true));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_FALSE(*v->AsBool());
}

TEST(HostFnTest, ContextUintArgUintReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Uint());
  b.AddFunction("uint @host.inc(uint x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("inc(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("inc_uint", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgUint(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnUint(*x + 1);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Uint(99u));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsUint(), 100u);
}

TEST(HostFnTest, ContextDoubleArgDoubleReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Double());
  b.AddFunction("double @host.half(double x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("half(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("half_double", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgDouble(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnDouble(*x / 2.0);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Double(7.0));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_DOUBLE_EQ(*v->AsDouble(), 3.5);
}

// string → string via the context API: the callback uppercases into a
// freshly arena-allocated return string (ReturnString).
TEST(HostFnTest, ContextStringArgStringReturnAllocates) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("string @host.upper(this string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("s.upper()", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("upper_string", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto s = ctx.ArgString(0);
                                  if (!s.ok()) return s.status();
                                  return ctx.ReturnString(
                                      absl::AsciiStrToUpper(*s));
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String("Hello"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "HELLO");
}

TEST(HostFnTest, ContextDurationArgDurationReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("d", CelType::Duration());
  b.AddFunction("Duration @host.twice(Duration d);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("twice(d)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("twice_duration", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto d = ctx.ArgDuration(0);
                                  if (!d.ok()) return d.status();
                                  return ctx.ReturnDuration(*d * 2);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("d", Value::Duration(absl::Seconds(5)));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsDuration(), absl::Seconds(10));
}

TEST(HostFnTest, ContextTimestampArgTimestampReturn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("t", CelType::Timestamp());
  b.AddFunction("Timestamp @host.echo_ts(Timestamp t);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_ts(t)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("echo_ts_timestamp", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto t = ctx.ArgTimestamp(0);
                                  if (!t.ok()) return t.status();
                                  return ctx.ReturnTimestamp(*t);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  const absl::Time t = absl::FromUnixSeconds(1700000000);
  Activation act;
  act.Bind("t", Value::Timestamp(t));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsTimestamp(), t);
}

// proto arg through the context API: read a field off a bound Customer
// via ArgProto.
TEST(HostFnTest, ContextProtoArgReadsField) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  b.AddFunction("bool @host.is_premium(proto(celwasm.testdata.Customer) c);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("is_premium(c)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddFunction("is_premium_message_celwasm_testdata_Customer", 2,
                        [](HostCallContext& ctx) -> absl::Status {
                          auto m = ctx.ArgProto(0);
                          if (!m.ok()) return m.status();
                          const auto* c = dynamic_cast<const Customer*>(*m);
                          if (c == nullptr) {
                            return absl::InvalidArgumentError("not a Customer");
                          }
                          return ctx.ReturnBool(c->is_premium());
                        })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Customer c;
  c.set_is_premium(true);
  Activation act;
  act.Bind("c", Value::Message(c));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

// proto return through the context API: build a Customer host-side via
// ReturnProto, then read a field off the returned message in CEL.
TEST(HostFnTest, ContextProtoReturnBuildsMessage) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("n", CelType::String());
  b.AddFunction(
      "proto(celwasm.testdata.Customer) @host.make_customer(string name);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("make_customer(n).name", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("make_customer_string", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto name = ctx.ArgString(0);
                                  if (!name.ok()) return name.status();
                                  auto m = std::make_unique<Customer>();
                                  m->set_name(std::string(*name));
                                  return ctx.ReturnProto(std::move(m));
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("n", Value::String("Grace"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "Grace");
}

// list arg as a bound variable (host/externref representation, vs. the
// arena-list literal the typed test feeds).
TEST(HostFnTest, ContextListArgSumsElements) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("xs", CelType::List(CelType::Int()));
  b.AddFunction("int @host.sum(list<int> xs);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("sum(xs)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("sum_list_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto lv = ctx.ArgList(0);
                                  if (!lv.ok()) return lv.status();
                                  int64_t total = 0;
                                  for (size_t i = 0; i < lv->Size(); ++i) {
                                    auto e = lv->At(i);
                                    if (!e.ok()) return e.status();
                                    auto n = e->AsInt();
                                    if (!n.ok()) return n.status();
                                    total += *n;
                                  }
                                  return ctx.ReturnInt(total);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("xs", Value::List({Value::Int(1), Value::Int(2), Value::Int(3)}));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 6);
}

// Same fn, but the list argument is an in-expression LITERAL (arena-list
// representation reaching a host fn, vs. the bound-variable host/externref
// representation above).
TEST(HostFnTest, ContextListLiteralArgSumsElements) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("int @host.sum(list<int> xs);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("sum([10, 20, 30])", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("sum_list_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto lv = ctx.ArgList(0);
                                  if (!lv.ok()) return lv.status();
                                  int64_t total = 0;
                                  for (size_t i = 0; i < lv->Size(); ++i) {
                                    auto e = lv->At(i);
                                    if (!e.ok()) return e.status();
                                    auto n = e->AsInt();
                                    if (!n.ok()) return n.status();
                                    total += *n;
                                  }
                                  return ctx.ReturnInt(total);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 60);
}

// map arg as a bound variable (host/externref representation, vs. the
// arena-map literal below).
TEST(HostFnTest, ContextMapArgLooksUpKey) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("m", CelType::Map(CelType::String(), CelType::Int()));
  b.DeclareVariable("k", CelType::String());
  b.AddFunction("int @host.lookup(map<string, int> m, string k);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("lookup(m, k)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("lookup_map_string_int_string", 3,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto mv = ctx.ArgMap(0);
                                  if (!mv.ok()) return mv.status();
                                  auto k = ctx.ArgString(1);
                                  if (!k.ok()) return k.status();
                                  auto got =
                                      mv->Get(Value::String(std::string(*k)));
                                  if (!got.ok()) return got.status();
                                  if (got->IsError()) return ctx.ReturnInt(-1);
                                  return ctx.ReturnValue(*got);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("m", Value::Map({{Value::String("a"), Value::Int(10)},
                            {Value::String("b"), Value::Int(20)}}));
  act.Bind("k", Value::String("b"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);
}

// map arg as an in-expression LITERAL (arena-map representation).
TEST(HostFnTest, ContextMapLiteralArgLooksUpKey) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("k", CelType::String());
  b.AddFunction("int @host.lookup(map<string, int> m, string k);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program =
      compiler->Compile("lookup({'a': 10, 'b': 20}, k)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("lookup_map_string_int_string", 3,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto mv = ctx.ArgMap(0);
                                  if (!mv.ok()) return mv.status();
                                  auto k = ctx.ArgString(1);
                                  if (!k.ok()) return k.status();
                                  auto got =
                                      mv->Get(Value::String(std::string(*k)));
                                  if (!got.ok()) return got.status();
                                  if (got->IsError()) return ctx.ReturnInt(-1);
                                  return ctx.ReturnValue(*got);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("k", Value::String("b"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);
}

// The deepest composite through the context API: map<string, list<proto>>
// — the callback decodes the map + key args itself, then runs the same
// counting walk as the typed variant (CountPremiumInTeam above).
TEST(HostFnTest, ContextNestedMapStringListProtoArg) {
  auto program = CompileCountPremiumProgram();
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddFunction(
              "count_premium_map_string_list_message_celwasm_testdata_Customer_"
              "string",
              3,
              [](HostCallContext& ctx) -> absl::Status {
                auto m = ctx.ArgMap(0);
                if (!m.ok()) return m.status();
                auto k = ctx.ArgString(1);
                if (!k.ok()) return k.status();
                auto n = CountPremiumInTeam(*m, *k);
                if (!n.ok()) return n.status();
                return ctx.ReturnInt(*n);
              })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  auto teams = MakeTeamsActivation();
  auto v = instance->Eval(teams->act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 1);  // "eng" has one premium customer
}

// Argument ORDER is asserted with a non-commutative op: a - b.
TEST(HostFnTest, ContextGlobalTwoArgPreservesOrder) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("a", CelType::Int());
  b.DeclareVariable("b", CelType::Int());
  b.AddFunction("int @host.sub(int a, int b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("sub(a, b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("sub_int_int", 3,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto a = ctx.ArgInt(0);
                                  auto b = ctx.ArgInt(1);
                                  if (!a.ok()) return a.status();
                                  if (!b.ok()) return b.status();
                                  return ctx.ReturnInt(*a - *b);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("a", Value::Int(10));
  act.Bind("b", Value::Int(3));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 7);  // 10 - 3, NOT 3 - 10
}

// A host-fn result feeds a CEL operator.
TEST(HostFnTest, ContextResultFeedsComparisonOperator) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x) > 40", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("double_it_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgInt(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnInt(*x * 2);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(21));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());  // 42 > 40
}

// Context receiver (`this`) dispatch: the receiver becomes arg 0.
TEST(HostFnTest, ContextReceiverIsFirstArg) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("bool @host.is_number(this string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("s.is_number()", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("is_number_string", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto s = ctx.ArgString(0);
                                  if (!s.ok()) return s.status();
                                  bool all_digits = !s->empty();
                                  for (const char c : *s) {
                                    if (c < '0' || c > '9') {
                                      all_digits = false;
                                      break;
                                    }
                                  }
                                  return ctx.ReturnBool(all_digits);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String("12345"));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

// A host-fn result feeds another host fn: inc(double_it(x)).
TEST(HostFnTest, ContextResultFeedsAnotherHostFn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  b.AddFunction("int @host.inc(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("inc(double_it(x))", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("double_it_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgInt(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnInt(*x * 2);
                                })
                  .ok());
  ASSERT_TRUE(engine
                  ->AddFunction("inc_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto x = ctx.ArgInt(0);
                                  if (!x.ok()) return x.status();
                                  return ctx.ReturnInt(*x + 1);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(20));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 41);  // (20*2)+1
}

// A multi-decl `.celfn` library parsed once, both fns registered, both
// fire in one expression.
TEST(HostFnTest, MultiDeclLibraryBothFire) {
  constexpr absl::string_view kSrc =
      "int @host.double_it(int x);\n"
      "int @host.triple_it(int x);\n";
  auto lib = ParseCelfnSource(kSrc);
  ASSERT_TRUE(lib.ok()) << lib.status();
  ASSERT_EQ(lib->decls().size(), 2u);

  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.DeclareFunctions(*std::move(lib));
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program =
      compiler->Compile("double_it(x) + triple_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto mul = [](int64_t k) {
    return [k](HostCallContext& ctx) -> absl::Status {
      auto x = ctx.ArgInt(0);
      if (!x.ok()) return x.status();
      return ctx.ReturnInt(*x * k);
    };
  };
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine->AddFunction("double_it_int", 2, mul(2)).ok());
  ASSERT_TRUE(engine->AddFunction("triple_it_int", 2, mul(3)).ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(4));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);  // 8 + 12
}

// A function whose args are all known may still decide its result is
// unknown — ReturnUnknown stamps the reserved function-origin sentinel.
TEST(HostFnTest, ContextReturnUnknownStampsFunctionOriginSentinel) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.make_unknown(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("make_unknown(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("make_unknown_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  return ctx.ReturnUnknown();
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_TRUE(v->IsUnknown());
  ASSERT_TRUE(v->UnknownAttribute().ok());
  EXPECT_EQ(v->UnknownAttribute()->id, kFunctionUnknownSentinel);
}

// The function-origin sentinel must survive the runtime's 3VL merge /
// short-circuit paths unchanged: feed a function-returned unknown through
// a CEL operator and confirm the result is still unknown carrying the
// same sentinel, not a bogus attribute id.
TEST(HostFnTest, FunctionOriginUnknownSurvivesOperatorMerge) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.make_unknown(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("make_unknown(x) + 1", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("make_unknown_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  return ctx.ReturnUnknown();
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_TRUE(v->IsUnknown());
  ASSERT_TRUE(v->UnknownAttribute().ok());
  EXPECT_EQ(v->UnknownAttribute()->id, kFunctionUnknownSentinel);
}

// A callback emitting a CEL error value via ReturnError → the error kind
// decodes through to the result Value.
TEST(HostFnTest, ContextReturnErrorPropagates) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.always_err(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("always_err(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("always_err_int", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  ErrorPayload p;
                                  p.code = ErrorCode::kDivideByZero;
                                  return ctx.ReturnError(std::move(p));
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  if (v.ok()) {
    EXPECT_EQ(v->kind(), Value::Kind::kError) << "expected an error Value";
  } else {
    SUCCEED() << "error surfaced as status: " << v.status();
  }
}

// A callback returning a non-OK absl::Status → traps the wasm and
// surfaces as a non-OK Eval status (not a crash, not a wrong value).
TEST(HostFnTest, ContextNonOkStatusTraps) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.boom(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("boom(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("boom_int", 2,
                                [](HostCallContext&) -> absl::Status {
                                  return absl::InternalError(
                                      "callback exploded");
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = instance->Eval(act);
  EXPECT_FALSE(v.ok()) << "a trapping callback must surface a non-OK status";
}

// Known args under PartialEval: the host fn dispatches and evaluates
// exactly as under Eval (an unrelated unknown pattern doesn't disturb
// it).  Proves host-fn dispatch works on the PartialEval path.
TEST(HostFnTest, PartialEvalKnownArgInvokesHostFn) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto invoked = std::make_shared<bool>(false);
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddFunction("double_it_int", 2,
                        [invoked](HostCallContext& ctx) -> absl::Status {
                          *invoked = true;
                          auto x = ctx.ArgInt(0);
                          if (!x.ok()) return x.status();
                          return ctx.ReturnInt(*x * 2);
                        })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(21));
  // Pattern names a different variable, so `x` stays known.
  auto pat = AttributePattern::Parse("other");
  ASSERT_TRUE(pat.ok()) << pat.status();
  std::vector<AttributePattern> unknowns;
  unknowns.push_back(*std::move(pat));
  auto v = instance->PartialEval(act, unknowns);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 42);
  EXPECT_TRUE(*invoked) << "host fn must run on the PartialEval path";
}

// ════════════════════════════════════════════════════════════════
// PartialEval auto-propagation across representative arg kinds.
// A string-, proto-, list-, and map-typed argument each marked unknown →
// the trampoline absorbs it before decode (kind-independently), the
// callback is NOT invoked, and the unknown propagates carrying a real
// attribute id (NOT the function-origin sentinel).
// ════════════════════════════════════════════════════════════════

struct UnknownArgCase {
  std::string decl;         // .celfn IDL
  std::string overload_id;  // real AddFunction id
  uint8_t num_args;
  std::string var;  // variable to mark unknown
  CelType var_type;
  Value binding;     // its (would-be) value
  std::string expr;  // call expr
};

class PartialEvalUnknownArgTest
    : public testing::TestWithParam<UnknownArgCase> {};

TEST_P(PartialEvalUnknownArgTest, UnknownArgPropagatesAndSkipsCallback) {
  const UnknownArgCase& c = GetParam();
  auto b = Compiler::NewBuilder();
  b.DeclareVariable(c.var, c.var_type);
  b.AddFunction(c.decl);
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile(c.expr, e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto invoked = std::make_shared<bool>(false);
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddFunction(c.overload_id, c.num_args,
                        [invoked](HostCallContext& ctx) -> absl::Status {
                          *invoked = true;
                          return ctx.ReturnInt(0);
                        })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind(c.var, c.binding);
  auto pat = AttributePattern::Parse(c.var);
  ASSERT_TRUE(pat.ok()) << pat.status();
  std::vector<AttributePattern> unknowns;
  unknowns.push_back(*std::move(pat));
  auto v = instance->PartialEval(act, unknowns);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown()) << "expected unknown to propagate";
  EXPECT_FALSE(*invoked) << "callback must not run on an unknown arg";
  ASSERT_TRUE(v->UnknownAttribute().ok());
  EXPECT_NE(v->UnknownAttribute()->id, kFunctionUnknownSentinel);
}

INSTANTIATE_TEST_SUITE_P(
    AcrossKinds, PartialEvalUnknownArgTest,
    testing::Values(
        UnknownArgCase{"int @host.use_s(string s);", "use_s_string", 2, "s",
                       CelType::String(), Value::String("hi"), "use_s(s)"},
        UnknownArgCase{"int @host.use_c(proto(celwasm.testdata.Customer) c);",
                       "use_c_message_celwasm_testdata_Customer", 2, "c",
                       CelType::Message("celwasm.testdata.Customer"),
                       Value::Message(Customer{}), "use_c(c)"},
        UnknownArgCase{"int @host.use_xs(list<int> xs);", "use_xs_list_int", 2,
                       "xs", CelType::List(CelType::Int()),
                       Value::List({Value::Int(1)}), "use_xs(xs)"},
        UnknownArgCase{
            "int @host.use_m(map<string, int> m);", "use_m_map_string_int", 2,
            "m", CelType::Map(CelType::String(), CelType::Int()),
            Value::Map({{Value::String("a"), Value::Int(1)}}), "use_m(m)"}));

// ════════════════════════════════════════════════════════════════
// Negative / error-propagation paths at compile / plan time
// ════════════════════════════════════════════════════════════════

TEST(HostFnTest, MalformedIdlFailsAtCompilerBuild) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("string @host.upper(this string s)");  // missing `;`
  auto compiler = std::move(b).Build();
  EXPECT_FALSE(compiler.ok());
  EXPECT_EQ(compiler.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(HostFnTest, ParseCelfnSourceRejectsGarbage) {
  auto lib = ParseCelfnSource("this is not celfn");
  EXPECT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(HostFnTest, MissingCallbackFailsAtPlanNoCrash) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.double_it(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("double_it(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  // No callback registered for double_it_int.
  auto instance = engine->Plan(*program);
  EXPECT_FALSE(instance.ok())
      << "Plan must reject a program importing an unbound cel_fn.*";
}

// ════════════════════════════════════════════════════════════════
// Boundary values — round-trip through the callback
// ════════════════════════════════════════════════════════════════

class IntBoundaryTest : public testing::TestWithParam<int64_t> {};

TEST_P(IntBoundaryTest, RoundTripsThroughCallback) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @host.echo_int(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_int(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction("echo_int_int",
                                     [](int64_t x) -> absl::StatusOr<int64_t> {
                                       return x;
                                     })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Int(GetParam()));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, IntBoundaryTest,
                         testing::Values(int64_t{0}, int64_t{-1}, int64_t{1},
                                         INT64_MIN, INT64_MAX));

class UintBoundaryTest : public testing::TestWithParam<uint64_t> {};

TEST_P(UintBoundaryTest, RoundTripsThroughCallback) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Uint());
  b.AddFunction("uint @host.echo_uint(uint x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_uint(x)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine
          ->AddTypedFunction("echo_uint_uint",
                             [](uint64_t x) -> absl::StatusOr<uint64_t> {
                               return x;
                             })
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("x", Value::Uint(GetParam()));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsUint(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, UintBoundaryTest,
                         testing::Values(uint64_t{0}, uint64_t{1}, UINT64_MAX));

// Echo a string arg back as a freshly allocated return string — empty,
// single, and multi-byte UTF-8 all survive the arena round trip.
class StringBoundaryTest : public testing::TestWithParam<std::string> {};

TEST_P(StringBoundaryTest, RoundTripsThroughCallback) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("string @host.echo_string(string s);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_string(s)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddTypedFunction(
                      "echo_string_string",
                      [](absl::string_view s) -> absl::StatusOr<std::string> {
                        return std::string(s);
                      })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  Activation act;
  act.Bind("s", Value::String(GetParam()));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, StringBoundaryTest,
                         testing::Values(std::string(""),       // empty
                                         std::string("a"),      // single
                                         std::string("héllo"),  // multi-byte
                                         std::string("日本語")));

// Echo a bytes arg (newly allocated return) with an embedded NUL — proves
// the {ptr,len} span carries length explicitly so interior NULs survive.
// A `bytes`-kinded return needs the context API: a typed `std::string`
// return always encodes as CEL string, so `ReturnBytes` is the surface
// that preserves the bytes kind.
TEST(HostFnTest, BytesArgWithEmbeddedNulRoundTrips) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("b", CelType::Bytes());
  b.AddFunction("bytes @host.echo_bytes(bytes b);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("echo_bytes(b)", e2e::DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(engine
                  ->AddFunction("echo_bytes_bytes", 2,
                                [](HostCallContext& ctx) -> absl::Status {
                                  auto bytes = ctx.ArgBytes(0);
                                  if (!bytes.ok()) return bytes.status();
                                  return ctx.ReturnBytes(*bytes);
                                })
                  .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::string payload;
  payload.push_back('a');
  payload.push_back('\0');
  payload.push_back('b');
  payload.push_back('\0');
  payload.push_back('c');
  Activation act;
  act.Bind("b", Value::Bytes(payload));
  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes);
  EXPECT_EQ(std::string(*v->AsBytes()), payload);
}

}  // namespace
}  // namespace celwasm
