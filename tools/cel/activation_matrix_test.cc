// End-to-end coverage for binding lists / maps / proto messages via
// `celwasm::api::Activation` — the exact shape the `cel` CLI's `--var
// name:Type=value` flag produces.
//
// Why this lives under `tools/cel/` rather than `compiler_v2/e2e/`:
// the test matrix is specifically the "user hands the compiler an
// activation containing aggregates" use case the CLI was built to
// surface.  Compiler / pipeline / runtime checks all happen via the
// public API (`celwasm::api::Compiler` + `celwasm::api::Engine` + `celwasm::api::Activation`),
// same path the CLI walks.
//
// Coverage axes (every cell exercised at least once):
//
//   element kind  | container shape         | usage
//   --------------+-------------------------+----------------------------
//   scalar int    | list<int>               | index, size, exists/all
//   scalar string | list<string>            | comprehension, in-operator
//   scalar int    | map<string,int>         | lookup, size, `in`
//   scalar int    | map<int,string>         | int-key lookup
//   message       | bound singleton         | field read (every wire type)
//   message       | proto map field         | nested map<string,_>
//   message       | proto repeated field    | nested repeated
//   message       | nested submessage       | a.b.c chain
//   mixed         | message + list + scalar | multi-binding eval

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/testdata/e2e_fixture.pb.h"
#include "compiler_v2/testdata/host_fixture_proto3.pb.h"
#include "gtest/gtest.h"

namespace celwasm::tools::cel {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::api::Activation;
using ::celwasm::api::CelType;
using ::celwasm::api::Compiler;
using ::celwasm::api::Engine;
using ::celwasm::api::Value;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg3;

// Process-wide Engine — wasmtime + cel_runtime.wasm parsed once.
const Engine& SharedEngine() {
  static const auto* engine = []() -> const Engine* {
    auto e = Engine::NewBuilder().Build();
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Compile + Plan + Eval helper.  One-shot: each test row gets its
// own Compiler/Program/Instance.
Value CompileAndEval(const Compiler& compiler, const std::string& expr,
                     const Activation& act) {
  auto program = compiler.Compile(expr);
  EXPECT_THAT(program.status(), IsOk()) << "Compile: " << expr;
  if (!program.ok()) return Value::Null();
  auto instance = SharedEngine().Plan(*program);
  EXPECT_THAT(instance.status(), IsOk()) << "Plan: " << expr;
  if (!instance.ok()) return Value::Null();
  auto value = instance->Eval(act);
  EXPECT_THAT(value.status(), IsOk()) << "Eval: " << expr;
  if (!value.ok()) return Value::Null();
  return *std::move(value);
}

Compiler BuildCompiler(
    std::vector<std::pair<std::string, CelType>> declarations) {
  Compiler::Builder b;
  for (auto& [name, type] : declarations) {
    b.DeclareVariable(name, type);
  }
  auto c = std::move(b).Build();
  EXPECT_THAT(c.status(), IsOk());
  return *std::move(c);
}

// ---------- list<int> ----------

TEST(ActivationMatrix, BoundListIntIndex) {
  Compiler c = BuildCompiler({{"xs", CelType::List(CelType::Int())}});
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
  EXPECT_EQ(*CompileAndEval(c, "xs[1]", a).AsInt(), 20);
}

TEST(ActivationMatrix, BoundListIntSize) {
  Compiler c = BuildCompiler({{"xs", CelType::List(CelType::Int())}});
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2), Value::Int(3),
                            Value::Int(4)}));
  EXPECT_EQ(*CompileAndEval(c, "size(xs)", a).AsInt(), 4);
}

// Slice 2 (host-list iter): comprehension over a bound list.
// `cel_list_arena_view` snapshots the host list into arena format
// at prologue entry; the existing inline arena walk then iterates
// uniformly.  See m5b §CCF-8.
TEST(ActivationMatrix, BoundListIntExists) {
  Compiler c = BuildCompiler({{"xs", CelType::List(CelType::Int())}});
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(3), Value::Int(5),
                            Value::Int(7)}));
  EXPECT_EQ(*CompileAndEval(c, "xs.exists(x, x > 5)", a).AsBool(), true);
  EXPECT_EQ(*CompileAndEval(c, "xs.exists(x, x > 100)", a).AsBool(), false);
}

TEST(ActivationMatrix, BoundListIntAll) {
  Compiler c = BuildCompiler({{"xs", CelType::List(CelType::Int())}});
  Activation a;
  a.Bind("xs", Value::List({Value::Int(2), Value::Int(4), Value::Int(6)}));
  EXPECT_EQ(*CompileAndEval(c, "xs.all(x, x % 2 == 0)", a).AsBool(), true);
}

TEST(ActivationMatrix, BoundListIntFilter) {
  Compiler c = BuildCompiler({{"xs", CelType::List(CelType::Int())}});
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2), Value::Int(3),
                            Value::Int(4)}));
  EXPECT_EQ(*CompileAndEval(c, "size(xs.filter(x, x > 2))", a).AsInt(), 2);
}

TEST(ActivationMatrix, BoundListStringTransform) {
  Compiler c = BuildCompiler({{"tags", CelType::List(CelType::String())}});
  Activation a;
  a.Bind("tags", Value::List({Value::String("alpha"), Value::String("beta"),
                              Value::String("gamma")}));
  EXPECT_EQ(
      *CompileAndEval(c, "tags.exists(t, t.startsWith('al'))", a).AsBool(),
      true);
  EXPECT_EQ(*CompileAndEval(c, "size(tags.filter(t, t == 'beta'))", a).AsInt(),
            1);
}

// ---------- list<string> ----------

TEST(ActivationMatrix, BoundListStringIn) {
  Compiler c = BuildCompiler({{"tags", CelType::List(CelType::String())}});
  Activation a;
  a.Bind("tags", Value::List({Value::String("admin"), Value::String("eng")}));
  EXPECT_EQ(*CompileAndEval(c, "'admin' in tags", a).AsBool(), true);
  EXPECT_EQ(*CompileAndEval(c, "'guest' in tags", a).AsBool(), false);
}

// ---------- map<string,int> ----------

TEST(ActivationMatrix, BoundMapStringIntLookup) {
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("us"), Value::Int(1)},
                          {Value::String("ca"), Value::Int(2)}}));
  EXPECT_EQ(*CompileAndEval(c, "m['us']", a).AsInt(), 1);
  EXPECT_EQ(*CompileAndEval(c, "m['ca']", a).AsInt(), 2);
}

TEST(ActivationMatrix, BoundMapSize) {
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("a"), Value::Int(1)},
                          {Value::String("b"), Value::Int(2)},
                          {Value::String("c"), Value::Int(3)}}));
  EXPECT_EQ(*CompileAndEval(c, "size(m)", a).AsInt(), 3);
}

TEST(ActivationMatrix, BoundMapInOperator) {
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("us"), Value::Int(1)}}));
  EXPECT_EQ(*CompileAndEval(c, "'us' in m", a).AsBool(), true);
  EXPECT_EQ(*CompileAndEval(c, "'xx' in m", a).AsBool(), false);
}

// Slice 1 (host-map iter): comprehension over a bound map iterates
// over keys.  The runtime snapshot's stride / cursor advance is
// asserted via .exists / .all / .filter on a multi-entry map.
TEST(ActivationMatrix, BoundMapExistsKey) {
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("us"), Value::Int(1)},
                          {Value::String("ca"), Value::Int(2)},
                          {Value::String("uk"), Value::Int(3)}}));
  EXPECT_EQ(*CompileAndEval(c, "m.exists(k, k == 'ca')", a).AsBool(), true);
  EXPECT_EQ(*CompileAndEval(c, "m.exists(k, k == 'fr')", a).AsBool(), false);
}

TEST(ActivationMatrix, BoundMapAllKeys) {
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("us"), Value::Int(1)},
                          {Value::String("ca"), Value::Int(2)}}));
  EXPECT_EQ(*CompileAndEval(c, "m.all(k, size(k) == 2)", a).AsBool(), true);
}

TEST(ActivationMatrix, BoundMapFilterByValue) {
  // Filter walks the keys but the predicate dereferences the value
  // via m[k] — exercises iter + map_lookup interleaving.
  Compiler c =
      BuildCompiler({{"m", CelType::Map(CelType::String(), CelType::Int())}});
  Activation a;
  a.Bind("m", Value::Map({{Value::String("us"), Value::Int(1)},
                          {Value::String("ca"), Value::Int(2)},
                          {Value::String("uk"), Value::Int(3)}}));
  EXPECT_EQ(*CompileAndEval(c, "size(m.filter(k, m[k] > 1))", a).AsInt(), 2);
}

TEST(ActivationMatrix, BoundMapIntKeysExists) {
  Compiler c = BuildCompiler(
      {{"codes", CelType::Map(CelType::Int(), CelType::String())}});
  Activation a;
  a.Bind("codes", Value::Map({{Value::Int(200), Value::String("OK")},
                              {Value::Int(404), Value::String("NotFound")}}));
  EXPECT_EQ(*CompileAndEval(c, "codes.exists(c, c >= 400)", a).AsBool(), true);
  EXPECT_EQ(*CompileAndEval(c, "codes.exists(c, c >= 500)", a).AsBool(), false);
}

// ---------- map<int,string> ----------

TEST(ActivationMatrix, BoundMapIntStringLookup) {
  Compiler c = BuildCompiler(
      {{"codes", CelType::Map(CelType::Int(), CelType::String())}});
  Activation a;
  a.Bind("codes", Value::Map({{Value::Int(200), Value::String("OK")},
                              {Value::Int(404), Value::String("NotFound")}}));
  EXPECT_EQ(*CompileAndEval(c, "codes[404]", a).AsString(), "NotFound");
}

// ---------- proto messages ----------

TEST(ActivationMatrix, BoundProtoFieldReads) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")}});
  Customer u;
  u.set_name("Ada");
  u.set_age(36);
  u.set_user_id(1001);
  u.set_balance_cents(500);
  u.set_credit_score(720.5);
  u.set_is_premium(true);
  Activation a;
  a.Bind("u", Value::Message(u));
  EXPECT_EQ(*CompileAndEval(c, "u.name", a).AsString(), "Ada");
  EXPECT_EQ(*CompileAndEval(c, "u.age", a).AsInt(), 36);
  EXPECT_EQ(*CompileAndEval(c, "u.user_id", a).AsInt(), 1001);
  EXPECT_EQ(*CompileAndEval(c, "u.balance_cents", a).AsUint(), 500u);
  EXPECT_DOUBLE_EQ(*CompileAndEval(c, "u.credit_score", a).AsDouble(), 720.5);
  EXPECT_EQ(*CompileAndEval(c, "u.is_premium", a).AsBool(), true);
}

TEST(ActivationMatrix, BoundProtoNestedMessage) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")}});
  Customer u;
  u.mutable_billing_address()->set_city("Berlin");
  u.mutable_billing_address()->set_country("DE");
  Activation a;
  a.Bind("u", Value::Message(u));
  EXPECT_EQ(*CompileAndEval(c, "u.billing_address.city", a).AsString(),
            "Berlin");
  EXPECT_EQ(*CompileAndEval(c, "u.billing_address.country", a).AsString(),
            "DE");
}

TEST(ActivationMatrix, BoundProtoMapField) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")}});
  Customer u;
  (*u.mutable_metadata())["region"] = "us-west-1";
  (*u.mutable_metadata())["tier"] = "gold";
  Activation a;
  a.Bind("u", Value::Message(u));
  EXPECT_EQ(*CompileAndEval(c, "u.metadata['region']", a).AsString(),
            "us-west-1");
  EXPECT_EQ(*CompileAndEval(c, "u.metadata['tier']", a).AsString(), "gold");
}

// Slice 1: comprehension over a proto map field.  ProtoMap (the
// kHost-backed accessor for a map<,> proto field) routes through
// the same `cel_host.cel_map_iter_open` trampoline as
// Value::Map activation bindings.
TEST(ActivationMatrix, BoundProtoMapFieldComprehension) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")}});
  Customer u;
  (*u.mutable_metadata())["region"] = "us-west-1";
  (*u.mutable_metadata())["tier"] = "gold";
  (*u.mutable_metadata())["env"] = "prod";
  Activation a;
  a.Bind("u", Value::Message(u));
  EXPECT_EQ(*CompileAndEval(c, "u.metadata.exists(k, k == 'tier')", a).AsBool(),
            true);
  EXPECT_EQ(*CompileAndEval(
                 c, "size(u.metadata.filter(k, u.metadata[k] != 'gold'))", a)
                 .AsInt(),
            2);
}

TEST(ActivationMatrix, BoundProtoRepeatedField) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")}});
  Customer u;
  u.add_tags("alpha");
  u.add_tags("beta");
  u.add_tags("gamma");
  Activation a;
  a.Bind("u", Value::Message(u));
  EXPECT_EQ(*CompileAndEval(c, "u.tags[1]", a).AsString(), "beta");
  EXPECT_EQ(*CompileAndEval(c, "size(u.tags)", a).AsInt(), 3);
  // Slice 2: comprehension over a proto repeated field
  // (host-origin list).  ProtoList is snapshotted into arena
  // format by `cel_list_arena_view` at prologue entry.
  EXPECT_EQ(*CompileAndEval(c, "u.tags.exists(t, t == 'beta')", a).AsBool(),
            true);
  EXPECT_EQ(*CompileAndEval(c, "u.tags.all(t, size(t) > 0)", a).AsBool(), true);
  EXPECT_EQ(
      *CompileAndEval(c, "size(u.tags.filter(t, t != 'beta'))", a).AsInt(), 2);
}

// ---------- multi-variable mixed activation ----------

TEST(ActivationMatrix, MultipleVariablesMixed) {
  Compiler c =
      BuildCompiler({{"u", CelType::Message("celwasm.testdata.Customer")},
                     {"min_age", CelType::Int()},
                     {"region", CelType::String()}});
  Customer u;
  u.set_name("Ada");
  u.set_age(36);
  (*u.mutable_metadata())["region"] = "us-west-1";
  Activation a;
  a.Bind("u", Value::Message(u));
  a.Bind("min_age", Value::Int(18));
  a.Bind("region", Value::String("us-west-1"));
  // Multi-binding eval covering: proto field read, scalar
  // comparison, and proto-map lookup compared to a bound string.
  EXPECT_EQ(*CompileAndEval(
                 c, "u.age >= min_age && u.metadata['region'] == region", a)
                 .AsBool(),
            true);
}

// ---------- HostMsg3 (richer wire-type matrix) ----------

TEST(ActivationMatrix, BoundProtoMapWithIntKey) {
  Compiler c =
      BuildCompiler({{"h", CelType::Message("celwasm.testdata.HostMsg3")}});
  HostMsg3 h;
  (*h.mutable_i64_to_str())[100] = "hundred";
  (*h.mutable_i64_to_str())[200] = "two-hundred";
  Activation a;
  a.Bind("h", Value::Message(h));
  EXPECT_EQ(*CompileAndEval(c, "h.i64_to_str[100]", a).AsString(), "hundred");
  EXPECT_EQ(*CompileAndEval(c, "h.i64_to_str[200]", a).AsString(),
            "two-hundred");
}

// Slice 2: list-of-messages binding with comprehension over it.
// The list backing is HostList; each element is a Value::Message
// (host backing).  The snapshot encodes each element as
// CEL_MESSAGE referencing the original externref slot, so the
// inner predicate sees the same proto field-read path.
TEST(ActivationMatrix, BoundListOfMessagesExists) {
  Compiler c = BuildCompiler({{"users", CelType::List(CelType::Message(
                                            "celwasm.testdata.Customer"))}});
  Customer u1;
  u1.set_name("Ada");
  u1.set_age(36);
  Customer u2;
  u2.set_name("Grace");
  u2.set_age(85);
  Activation a;
  a.Bind("users", Value::List({Value::Message(u1), Value::Message(u2)}));
  EXPECT_EQ(*CompileAndEval(c, "users.exists(u, u.age > 80)", a).AsBool(),
            true);
  EXPECT_EQ(*CompileAndEval(c, "users.all(u, u.age > 0)", a).AsBool(), true);
  EXPECT_EQ(
      *CompileAndEval(c, "size(users.filter(u, u.name == 'Ada'))", a).AsInt(),
      1);
}

TEST(ActivationMatrix, BoundProtoRepeatedMessageIndex) {
  Compiler c =
      BuildCompiler({{"h", CelType::Message("celwasm.testdata.HostMsg3")}});
  HostMsg3 h;
  HostMsg3* a1 = h.add_rep_msg();
  a1->set_s("first");
  a1->set_i32(1);
  HostMsg3* a2 = h.add_rep_msg();
  a2->set_s("second");
  a2->set_i32(2);
  Activation act;
  act.Bind("h", Value::Message(h));
  EXPECT_EQ(*CompileAndEval(c, "h.rep_msg[0].s", act).AsString(), "first");
  EXPECT_EQ(*CompileAndEval(c, "h.rep_msg[1].i32", act).AsInt(), 2);
}

}  // namespace
}  // namespace celwasm::tools::cel
