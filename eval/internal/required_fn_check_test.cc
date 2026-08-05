// Unit matrix for the Plan-time required-function check
// (eval/internal/required_fn_check.h).  Registry states are built
// by hand (no wasmtime engine, no stores), and `RequiredFunction`
// rows are built directly, so every axis of the signature-agreement
// matrix is covered cheaply: missing fn, arity, param type, proto
// FQN, nested generic, return type, is_receiver — split across the
// host-side arity-only (`AddFunction`) and BindFunction-typed
// (`decl_signature`) registration shapes.  The frozen §5.3 message
// shapes (m35-plugin-ergonomics.md) are asserted EXACTLY here.

#include "eval/internal/required_fn_check.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
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

using HostMap = std::map<std::string, RegisteredHostCallback>;

RegisteredHostCallback HostCb(uint8_t num_args) {
  RegisteredHostCallback cb;
  cb.num_args = num_args;
  cb.callback = [](HostCallContext&) {
    return absl::OkStatus();
  };
  return cb;
}

// A BindFunction-captured registration: arity + full decl signature.
RegisteredHostCallback TypedHostCb(const RequiredFunction& decl_signature) {
  RegisteredHostCallback cb =
      HostCb(static_cast<uint8_t>(decl_signature.param_types_size() + 1));
  cb.decl_signature = decl_signature;
  return cb;
}

// ── No-op / open-set arms ──────────────────────────────────────────

TEST(RequiredFnCheckTest, EmptyRequiredListNoOpsEntirely) {
  // Pre-field-8 Programs (and programs with no custom-fn call
  // sites) carry an empty list: the check must pass even against a
  // completely empty registry.
  EXPECT_THAT(CheckRequiredFunctions(celwasm::abi::CelAbi(), HostMap()),
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
  EXPECT_THAT(CheckRequiredFunctions(abi, HostMap()), IsOk());
}

// ── Retired plugin backend ─────────────────────────────────────────

TEST(RequiredFnCheckTest, RetiredPluginBackendRowRejectedLoudly) {
  // Wire value 2 of `abi.RequiredFunction.backend` was the removed
  // wasm-component plugin backend.  A Program carrying such a row
  // can never run on this engine, so — unlike genuinely unknown
  // future backends — it is rejected loudly, naming the row, not
  // skipped.  Spelled numerically so this pin outlives the deletion
  // of the generated PLUGIN enum member (proto3 open-enum decode
  // preserves the value on stale Programs).
  auto abi = AbiWith({Row("is_adult_message_acme_User", "is_adult",
                          static_cast<RequiredFunction::Backend>(2),
                          {WireProto("acme.User")}, Wire(Type::KIND_BOOL))});
  auto s = CheckRequiredFunctions(abi, HostMap());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires function "
            "`is_adult_message_acme_User` (`bool is_adult(proto(acme.User))`) "
            "via the removed wasm-component plugin backend; this engine "
            "supports only host-backed custom functions — recompile the "
            "program declaring the function with the `@host.` backend");
}

TEST(RequiredFnCheckTest, RetiredPluginBackendRejectsEvenWithMatchingHostCb) {
  // A host callback registered under the same overload-id does NOT
  // satisfy a plugin-backend row: the Program's call sites were
  // compiled for a backend that no longer exists.
  HostMap hosts;
  hosts.emplace("mul_int_int", HostCb(3));
  auto abi = AbiWith({Row("mul_int_int", "mul",
                          static_cast<RequiredFunction::Backend>(2),
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("removed wasm-component plugin backend"));
}

// ── HOST rows ──────────────────────────────────────────────────────

TEST(RequiredFnCheckTest, MissingHostFnExactFrozenMessage) {
  // The §5.3 missing-host shape, verbatim (single line).
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` (`int discount_pct(string)`) but none is "
            "registered; call Engine::BindFunction (or AddFunction) before "
            "Plan");
}

TEST(RequiredFnCheckTest, HostArityMismatchExactFrozenMessage) {
  // The §5.3 arity shape, verbatim: wasm arity = param_types + 1.
  HostMap hosts;
  hosts.emplace("discount_pct_string", HostCb(3));
  auto abi = AbiWith(
      {Row("discount_pct_string", "discount_pct", RequiredFunction::HOST,
           {Wire(Type::KIND_STRING)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts);
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
  EXPECT_THAT(CheckRequiredFunctions(abi, hosts), IsOk());
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
  EXPECT_THAT(CheckRequiredFunctions(abi, hosts), IsOk());
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
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_EQ(s.message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` with signature "
            "`int discount_pct(string)` but Engine::BindFunction registered "
            "`int discount_pct(bytes)`; signatures must match exactly — "
            "recompile the program or fix the registration");
}

TEST(RequiredFnCheckTest, TypedProtoFqnMismatchFails) {
  // proto(...) params compare by FQN — the drift scenario where the
  // registration was rebuilt against a renamed proto out from under
  // the compiled program.
  HostMap hosts;
  hosts.emplace("is_adult_message_acme_Person",
                TypedHostCb(Row("is_adult_message_acme_Person", "is_adult",
                                RequiredFunction::HOST,
                                {WireProto("acme.Person")},
                                Wire(Type::KIND_BOOL))));
  auto abi = AbiWith({Row("is_adult_message_acme_Person", "is_adult",
                          RequiredFunction::HOST, {WireProto("acme.User")},
                          Wire(Type::KIND_BOOL))});
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`bool is_adult(proto(acme.User))`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("registered "
                                   "`bool is_adult(proto(acme.Person))`"));
}

TEST(RequiredFnCheckTest, TypedNestedGenericMismatchFails) {
  // list<int> vs list<string> differ only inside the generic — the
  // recursive TypeEquals arm must catch it.
  HostMap hosts;
  hosts.emplace("sum_list_int",
                TypedHostCb(Row("sum_list_int", "sum", RequiredFunction::HOST,
                                {WireList(Wire(Type::KIND_INT))},
                                Wire(Type::KIND_INT))));
  auto abi =
      AbiWith({Row("sum_list_int", "sum", RequiredFunction::HOST,
                   {WireList(Wire(Type::KIND_STRING))}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`int sum(list<string>)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("registered `int sum(list<int>)`"));
}

TEST(RequiredFnCheckTest, TypedReturnTypeMismatchFails) {
  HostMap hosts;
  hosts.emplace("add_int_int",
                TypedHostCb(Row("add_int_int", "add", RequiredFunction::HOST,
                                {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                                Wire(Type::KIND_INT))));
  auto abi = AbiWith({Row("add_int_int", "add", RequiredFunction::HOST,
                          {Wire(Type::KIND_INT), Wire(Type::KIND_INT)},
                          Wire(Type::KIND_STRING))});
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`string add(int, int)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("registered `int add(int, int)`"));
}

TEST(RequiredFnCheckTest, TypedIsReceiverMismatchFails) {
  // Receiver-style and plain declarations synthesise the SAME
  // overload-id (pinned by EngineBindFunctionTest.
  // ReceiverDeclSynthesisesSameIdAsPlainParam), so is_receiver drift
  // is exactly the case the dedicated flag compare exists for.
  HostMap hosts;
  hosts.emplace("upper_string",
                TypedHostCb(Row("upper_string", "upper", RequiredFunction::HOST,
                                {Wire(Type::KIND_STRING)},
                                Wire(Type::KIND_STRING),
                                /*is_receiver=*/true)));
  auto abi = AbiWith({Row("upper_string", "upper", RequiredFunction::HOST,
                          {Wire(Type::KIND_STRING)}, Wire(Type::KIND_STRING),
                          /*is_receiver=*/false)});
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("`string upper(string)`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("registered `string upper(this string)`"));
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
  auto s = CheckRequiredFunctions(abi, hosts);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()),
              ::testing::HasSubstr("with wasm arity 2 but it was registered "
                                   "with arity 3"));
}

// ── Determinism ────────────────────────────────────────────────────

TEST(RequiredFnCheckTest, FirstFailureWinsInWireOrder) {
  // Two unsatisfied rows: the error names the FIRST row in wire
  // order, deterministically.
  auto abi = AbiWith({Row("first_int", "first", RequiredFunction::HOST,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT)),
                      Row("second_int", "second", RequiredFunction::HOST,
                          {Wire(Type::KIND_INT)}, Wire(Type::KIND_INT))});
  auto s = CheckRequiredFunctions(abi, HostMap());
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_THAT(std::string(s.message()), ::testing::HasSubstr("`first_int`"));
  EXPECT_THAT(std::string(s.message()),
              ::testing::Not(::testing::HasSubstr("`second_int`")));
}

}  // namespace
}  // namespace celwasm
