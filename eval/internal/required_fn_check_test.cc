// Unit matrix for the Plan-time required-function check
// (eval/internal/required_fn_check.h).  Registry states are built
// by hand (no wasmtime engine, no stores — `RegisteredPlugin::
// component` stays null; the check never touches it), and
// `RequiredFunction` rows are built directly, so every axis of the
// signature-agreement matrix from m35-plugin-ergonomics.md §12 is
// covered cheaply: missing fn, arity, param type, proto FQN, nested
// generic, return type, is_receiver — plus the host-side
// arity-only-vs-BindFunction-typed split and the legacy AddPlugin
// hash rendering.  The frozen §2/§5.3 message shapes are asserted
// EXACTLY here; the e2e twins (e2e/plugin_dispatch_test.cc) pin the
// two §2 headline shapes through the full pipeline.

#include "eval/internal/required_fn_check.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/celfn/function_library.h"
#include "eval/internal/wasmtime_engine_state.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::abi::RequiredFunction;
using ::celwasm::abi::Type;

// ── Wire-side builders ─────────────────────────────────────────────

Type Wire(Type::Kind kind) {
  Type t;
  t.set_kind(kind);
  return t;
}

Type WireProto(absl::string_view fqn) {
  Type t;
  t.set_kind(Type::KIND_PROTO);
  t.set_proto_fqn(std::string(fqn));
  return t;
}

Type WireList(Type elem) {
  Type t;
  t.set_kind(Type::KIND_LIST);
  *t.add_params() = std::move(elem);
  return t;
}

RequiredFunction Row(absl::string_view overload_id, absl::string_view fn_name,
                     RequiredFunction::Backend backend,
                     std::vector<Type> params, Type ret,
                     bool is_receiver = false) {
  RequiredFunction row;
  row.set_overload_id(std::string(overload_id));
  row.set_fn_name(std::string(fn_name));
  row.set_backend(backend);
  for (auto& p : params) {
    *row.add_param_types() = std::move(p);
  }
  *row.mutable_return_type() = std::move(ret);
  row.set_is_receiver(is_receiver);
  return row;
}

celwasm::abi::CelAbi AbiWith(std::vector<RequiredFunction> rows) {
  celwasm::abi::CelAbi abi;
  for (auto& row : rows) {
    *abi.add_required_functions() = std::move(row);
  }
  return abi;
}

// ── Decl-side builders ─────────────────────────────────────────────

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

CelfnType ProtoOf(absl::string_view fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::string(fqn);
  return t;
}

CelfnType ListOf(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(elem));
  return t;
}

FunctionLibrary PluginLib(absl::string_view fn_name, CelfnType return_type,
                          std::vector<CelfnParam> params) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddPlugin(fn_name, std::move(return_type), std::move(params))
          .Build();
  ABSL_CHECK(lib_or.ok()) << lib_or.status();
  return *std::move(lib_or);
}

// A registered plugin whose hash's leading bytes spell the §2
// example digest `3f9a2c1b04de` (the remaining 26 bytes stay zero —
// only the first 6 render).
std::array<uint8_t, 32> DocExampleHash() {
  std::array<uint8_t, 32> h{};
  h[0] = 0x3f;
  h[1] = 0x9a;
  h[2] = 0x2c;
  h[3] = 0x1b;
  h[4] = 0x04;
  h[5] = 0xde;
  return h;
}

RegisteredPlugin Plug(FunctionLibrary lib, std::array<uint8_t, 32> hash = {}) {
  RegisteredPlugin p;
  p.component = nullptr;  // never dereferenced by the check
  p.library = std::move(lib);
  p.hash = hash;
  return p;
}

using HostMap = std::map<std::string, RegisteredHostCallback>;
using PluginVec = std::vector<RegisteredPlugin>;

RegisteredHostCallback HostCb(uint8_t num_args) {
  RegisteredHostCallback cb;
  cb.num_args = num_args;
  cb.callback = [](HostCallContext&) {
    return absl::OkStatus();
  };
  return cb;
}

// ── No-op / open-set arms ──────────────────────────────────────────

TEST(RequiredFnCheckTest, EmptyRequiredListNoOpsEntirely) {
  // Pre-field-8 Programs (and programs with no custom-fn call
  // sites) carry an empty list: the check must pass even against a
  // completely empty registry.
  EXPECT_THAT(
      CheckRequiredFunctions(celwasm::abi::CelAbi(), HostMap(), PluginVec()),
      IsOk());
}

TEST(RequiredFnCheckTest, UnknownBackendRowsAreSkipped) {
  // Open-set wire data: BACKEND_UNSPECIFIED and a future value the
  // engine doesn't know must not fail the check — an unbound import
  // still fails loudly at wasmtime link time.
  auto abi = AbiWith(
      {Row("mystery_int", "mystery", RequiredFunction::BACKEND_UNSPECIFIED,
           {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT)),
       Row("future_int", "future", static_cast<RequiredFunction::Backend>(99),
           {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT))});
  EXPECT_THAT(CheckRequiredFunctions(abi, HostMap(), PluginVec()), IsOk());
}

// ── PLUGIN rows ────────────────────────────────────────────────────

TEST(RequiredFnCheckTest, MissingPluginFnExactFrozenMessage) {
  // The §2 missing-plugin shape, verbatim (single line).
  auto abi = AbiWith(
      {Row("is_adult_message_acme_User", "is_adult", RequiredFunction::PLUGIN,
           {WireProto("acme.User")}, Wire(Type::KIND_BOOL))});
  auto s = CheckRequiredFunctions(abi, HostMap(), PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires plugin function "
            "`is_adult_message_acme_User` (`bool is_adult(proto(acme.User))`) "
            "but no registered plugin declares it; register the providing "
            "plugin with Engine::Use before Plan");
}

TEST(RequiredFnCheckTest, PluginRowBackedByHostCallbackStillMissing) {
  // Backend must match: an overload-id registered as a HOST callback
  // does not satisfy a PLUGIN row.
  HostMap hosts;
  hosts.emplace("mul_int_int", HostCb(3));
  auto abi = AbiWith({Row("mul_int_int", "mul", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts, PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("no registered plugin declares it"));
}

TEST(RequiredFnCheckTest, MatchingPluginDeclPasses) {
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("add", Prim(CelfnType::Kind::kInt),
                     {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                      CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})));
  auto abi = AbiWith({Row("add_int_int", "add", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_INT))});
  EXPECT_THAT(CheckRequiredFunctions(abi, HostMap(), plugins), IsOk());
}

TEST(RequiredFnCheckTest, SecondRegisteredPluginCanOwnTheDecl) {
  // Registry scan crosses plugins: the owning decl living in the
  // SECOND registered plugin still satisfies the row.
  PluginVec plugins;
  plugins.push_back(Plug(PluginLib("other", Prim(CelfnType::Kind::kBool), {})));
  plugins.push_back(
      Plug(PluginLib("add", Prim(CelfnType::Kind::kInt),
                     {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                      CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})));
  auto abi = AbiWith({Row("add_int_int", "add", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_INT))});
  EXPECT_THAT(CheckRequiredFunctions(abi, HostMap(), plugins), IsOk());
}

TEST(RequiredFnCheckTest, ProtoFqnMismatchExactFrozenMessage) {
  // The §2 signature-mismatch shape, verbatim, including the hash
  // rendered as the first 12 lowercase hex chars.
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("is_adult", Prim(CelfnType::Kind::kBool),
                     {CelfnParam{false, ProtoOf("acme.Person"), "u"}}),
           DocExampleHash()));
  // The registered decl synthesises overload-id
  // `is_adult_message_acme_Person`; the Program's row carries the
  // SAME id (the drift scenario: the plugin was rebuilt against a
  // renamed proto out from under the compiled program).
  auto abi = AbiWith({Row(plugins[0].library.decls()[0].overload_id, "is_adult",
                          RequiredFunction::PLUGIN, {WireProto("acme.User")},
                          Wire(Type::KIND_BOOL))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires plugin function "
            "`is_adult_message_acme_Person` with signature "
            "`bool is_adult(proto(acme.User))` but the registered plugin "
            "(hash 3f9a2c1b04de) declares `bool is_adult(proto(acme.Person))`; "
            "signatures must match exactly — recompile the program or rebuild "
            "the plugin");
}

TEST(RequiredFnCheckTest, LegacyAddPluginRendersHashUnavailable) {
  // A plugin registered through the legacy `AddPlugin(bytes, lib)`
  // escape has an all-zero hash: the mismatch message renders
  // `hash unavailable; registered via AddPlugin` instead of 12 hex
  // chars of a hash that does not exist.
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("add", Prim(CelfnType::Kind::kInt),
                     {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                      CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})));
  auto abi = AbiWith({Row("add_int_int", "add", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_STRING))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(
      std::string(s.message()),
      ::testing::HasSubstr("(hash unavailable; registered via AddPlugin)"));
}

TEST(RequiredFnCheckTest, ParamCountMismatchFails) {
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("add", Prim(CelfnType::Kind::kInt),
                     {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"}})));
  // Row reuses the registered id but declares two params.
  auto abi = AbiWith({Row("add_int", "add", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`int add(int, int)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("declares `int add(int)`"));
}

TEST(RequiredFnCheckTest, ParamTypeMismatchFails) {
  PluginVec plugins;
  plugins.push_back(Plug(
      PluginLib("echo", Prim(CelfnType::Kind::kString),
                {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}})));
  auto abi = AbiWith({Row("echo_string", "echo", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_STRING))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`string echo(int)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("declares `string echo(string)`"));
}

TEST(RequiredFnCheckTest, NestedGenericMismatchFails) {
  // list<int> vs list<string> differ only inside the generic — the
  // recursive TypeEquals arm must catch it.
  PluginVec plugins;
  plugins.push_back(Plug(PluginLib(
      "sum", Prim(CelfnType::Kind::kInt),
      {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}})));
  auto abi =
      AbiWith({Row("sum_list_int", "sum", RequiredFunction::PLUGIN,
                   {WireList(Wire(Type::KIND_STRING))}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`int sum(list<string>)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("declares `int sum(list<int>)`"));
}

TEST(RequiredFnCheckTest, ReturnTypeMismatchFails) {
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("add", Prim(CelfnType::Kind::kInt),
                     {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                      CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})));
  auto abi = AbiWith({Row("add_int_int", "add", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_STRING))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`string add(int, int)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("declares `int add(int, int)`"));
}

TEST(RequiredFnCheckTest, IsReceiverMismatchFails) {
  // Receiver-style and plain declarations synthesise the SAME
  // overload-id (pinned by EngineBindFunctionTest.
  // ReceiverDeclSynthesisesSameIdAsPlainParam), so is_receiver drift
  // is exactly the case the dedicated flag compare exists for.
  PluginVec plugins;
  plugins.push_back(
      Plug(PluginLib("upper", Prim(CelfnType::Kind::kString),
                     {CelfnParam{true, Prim(CelfnType::Kind::kString), "s"}})));
  ASSERT_TRUE(plugins[0].library.decls()[0].is_receiver);
  auto abi = AbiWith({Row("upper_string", "upper", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_STRING)}, Wire(Type::KIND_STRING),
                          /*is_receiver=*/false)});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`string upper(string)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("declares `string upper(this string)`"));
}

// ── HOST rows ──────────────────────────────────────────────────────

TEST(RequiredFnCheckTest, MissingHostFnExactFrozenMessage) {
  // The §5.3 missing-host shape, verbatim (single line).
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap(), PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` (`int discount_pct(string)`) but none is "
            "registered; call Engine::BindFunction (or AddFunction) before "
            "Plan");
}

TEST(RequiredFnCheckTest, HostRowBackedByPluginDeclStillMissing) {
  // Backend must match in this direction too.
  PluginVec plugins;
  plugins.push_back(Plug(
      PluginLib("discount_pct", Prim(CelfnType::Kind::kInt),
                {CelfnParam{false, Prim(CelfnType::Kind::kString), "tier"}})));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap(), plugins);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("but none is registered"));
}

TEST(RequiredFnCheckTest, HostArityMismatchExactFrozenMessage) {
  // The §5.3 arity shape, verbatim: wasm arity = param_types + 1.
  HostMap hosts;
  hosts.emplace("discount_pct_string", HostCb(3));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts, PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` with wasm arity 2 but it was registered "
            "with arity 3");
}

TEST(RequiredFnCheckTest, AddFunctionRegistrationIsArityOnly) {
  // Raw AddFunction / AddTypedFunction capture no decl: matching
  // arity passes even though the engine cannot compare types (the
  // documented arity-only contract on both methods).
  HostMap hosts;
  hosts.emplace("discount_pct_string", HostCb(2));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  EXPECT_THAT(CheckRequiredFunctions(abi, hosts, PluginVec()), IsOk());
}

// A BindFunction-captured registration: arity + full decl signature.
RegisteredHostCallback TypedHostCb(const RequiredFunction& decl_signature) {
  RegisteredHostCallback cb =
      HostCb(static_cast<uint8_t>(decl_signature.param_types_size() + 1));
  cb.decl_signature = decl_signature;
  return cb;
}

TEST(RequiredFnCheckTest, BindFunctionTypedMatchPasses) {
  HostMap hosts;
  hosts.emplace(
      "discount_pct_string",
      TypedHostCb(Row("discount_pct_string", "discount_pct",
                      RequiredFunction::HOST, {Wire(Type::KIND_STRING)},
                      Wire(Type::KIND_INT))));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  EXPECT_THAT(CheckRequiredFunctions(abi, hosts, PluginVec()), IsOk());
}

TEST(RequiredFnCheckTest, BindFunctionTypedMismatchFails) {
  // Same arity, different param type: only the BindFunction-typed
  // compare can catch this, and its message names both spellings.
  HostMap hosts;
  hosts.emplace(
      "discount_pct_string",
      TypedHostCb(Row("discount_pct_string", "discount_pct",
                      RequiredFunction::HOST, {Wire(Type::KIND_BYTES)},
                      Wire(Type::KIND_INT))));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts, PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` with signature "
            "`int discount_pct(string)` but Engine::BindFunction registered "
            "`int discount_pct(bytes)`; signatures must match exactly — "
            "recompile the program or fix the registration");
}

TEST(RequiredFnCheckTest, ArityFailureWinsOverTypedCompare) {
  // Both the arity and the captured types are wrong: the arity
  // message fires (checked first — first failure wins per row).
  HostMap hosts;
  RegisteredHostCallback cb = TypedHostCb(Row(
      "f_int", "f", RequiredFunction::HOST,
      {Wire(Type::KIND_BYTES), Wire(Type::KIND_BYTES)}, Wire(Type::KIND_INT)));
  hosts.emplace("f_int", std::move(cb));
  auto abi = AbiWith({Row("f_int", "f", RequiredFunction::HOST,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts, PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("with wasm arity 2 but it was registered "
                                   "with arity 3"));
}

// ── Determinism ────────────────────────────────────────────────────

TEST(RequiredFnCheckTest, FirstFailureWinsInWireOrder) {
  // Two unsatisfied rows: the error names the FIRST row in wire
  // order, deterministically.
  auto abi = AbiWith({Row("first_int", "first", RequiredFunction::PLUGIN,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT)),
                      Row("second_int", "second", RequiredFunction::HOST,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap(), PluginVec());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()), ::testing::HasSubstr("`first_int`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::Not(::testing::HasSubstr("`second_int`")));
}

}  // namespace
}  // namespace celwasm
