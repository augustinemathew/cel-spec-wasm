// M1 — Engine lifecycle + Engine::Plan basic wiring.
//
// Plan tests use a hand-written minimal WAT module instead of a
// real Compile()'d Program because the production codegen still
// emits expr-defines-memory; Plan §5.6 (revised) flips that in a
// follow-up commit, after which engine_test.cc can switch to using
// `Compiler::Compile`.  Synthetic WAT here ensures Plan's wasmtime
// wiring is tested in isolation and the test doesn't regress when
// the codegen flip lands.

#include "eval/engine.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/plugin.h"
#include "abi/runtime_catalogue.h"
#include "abi/wasm_binary.h"
#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "bazel/link_mode_test_helpers.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/host_call_context.h"
#include "eval/instance.h"
#include "eval/internal/abi_decode.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "tools/cpp/runfiles/runfiles.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;

// Returns `CompilerOptions` with `link_mode` set to the per-binary
// `kTestLinkMode` — picked at build time by `link_mode_cc_test`.
inline CompilerOptions LinkModeOpts() {
  CompilerOptions opts;
  opts.link_mode = kTestLinkMode;
  return opts;
}

// Mirror of the smoke test's expr WAT (see the experiment branch's
// two_phase_shared_memory_smoke_test.cc) — imports cel.memory +
// cel.arena_reset + cel.arena_alloc, exports `eval`.  Calls reset(64,
// 65536), alloc(24), writes a 24-byte payload.  Eval correctness
// is exercised in instance_test.cc once Eval lands; here we only
// need a Program whose imports match what Plan provides so
// instantiation succeeds.
// Phase C: the runtime exports a shared memory (wasm32-wasi-threads).
// The expr import must declare shared+max to match (Wasmtime rejects a
// non-shared import against a shared export).  Also note the
// `arena_reset` import is 0-arg post-runtime-refactor, not 2-arg.
constexpr char kSyntheticExprWat[] = R"WAT(
(module
  (import "cel" "memory" (memory 1 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (func (export "eval") (result i32)
    (local $off i32)
    (call $arena_reset)
    (local.set $off (call $arena_alloc (i32.const 24)))
    (i32.store (local.get $off) (i32.const 1))
    (i64.store offset=8 (local.get $off) (i64.const 42))
    (local.get $off)))
)WAT";

std::vector<uint8_t> Wat2Wasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  ABSL_CHECK(err == nullptr);
  std::vector<uint8_t> bytes(out.data, out.data + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

Program SyntheticProgram() {
  return Program(Wat2Wasm(kSyntheticExprWat));
}

TEST(EngineBuilderTest, BuildSucceedsWithDefaults) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
}

TEST(EngineBuilderTest, BuildIsCheapToCallTwiceFromIndependentBuilders) {
  auto a = Engine::NewBuilder().Build();
  auto b = Engine::NewBuilder().Build();
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
}

TEST(EngineLifetimeTest, MoveConstructionPreservesState) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine a = *std::move(engine_or);
  Engine b = std::move(a);
  (void)b;
}

TEST(EnginePlanTest, PlanSucceedsOnSyntheticProgram) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  Program program = SyntheticProgram();
  auto inst_or = engine.Plan(program);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
}

TEST(EnginePlanTest, PlanCalledTwiceProducesIndependentInstances) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  Program program = SyntheticProgram();
  auto a = engine.Plan(program);
  auto b = engine.Plan(program);
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
  // Both Instances are live independently; dropping one shouldn't
  // affect the other.  Just letting the scope close exercises that.
}

TEST(EnginePlanTest, PlanFailsOnMalformedBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  // Wasm magic + garbage past the version word.  Plan decodes the
  // cel.abi custom section from the raw bytes FIRST (before the
  // wasmtime module compile), so malformed framing fails in the
  // decoder and surfaces as `InvalidArgument` (the abi_decode
  // contract for a bad wasm header / section framing).
  Program garbage(std::vector<uint8_t>{0x00, 0x61, 0x73, 0x6d, 0xff, 0xff});
  auto inst_or = engine.Plan(garbage);
  EXPECT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kInvalidArgument);
}

// Instance::Eval invokes the `eval` export through
// `wasmtime_func_call_unchecked` assuming the fixed `[] -> [i32]`
// shape, so Plan must prove that shape once and reject anything
// else — a wrong-shaped export reaching the unchecked call would be
// undefined behaviour, not a graceful error.
TEST(EnginePlanTest, PlanRejectsWrongShapedEvalExport) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  const auto expect_rejected = [&](absl::string_view eval_func_wat) {
    const std::string wat = absl::StrCat(
        R"WAT((module
  (import "cel" "memory" (memory 1 1024 shared))
)WAT",
        eval_func_wat, ")");
    Program program{Wat2Wasm(wat)};
    auto inst_or = engine_or->Plan(program);
    EXPECT_FALSE(inst_or.ok()) << "Plan accepted: " << eval_func_wat;
    EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
        << inst_or.status();
    EXPECT_TRUE(
        absl::StrContains(inst_or.status().message(), "export signature"))
        << inst_or.status();
  };
  // Wrong result type ([] -> [i64]).
  expect_rejected(R"WAT((func (export "eval") (result i64) (i64.const 0)))WAT");
  // Wrong arity ([i32] -> [i32]).
  expect_rejected(
      R"WAT((func (export "eval") (param i32) (result i32) (i32.const 0)))WAT");
  // No results ([] -> []).
  expect_rejected(R"WAT((func (export "eval")))WAT");
}

// ── Plan-time ABI slot-extent validation ───────────────────────────
//
// `Instance::Eval(Activation)` writes a 24-byte CelValue at every
// cel.abi variable `slot_offset` in the SHARED linear memory the
// runtime's static data / heap also live in (above the
// `CELWASM_RESERVED_LOW_MEMORY_BYTES` = 262144 boundary).  The compiler
// validates its layout fits under the boundary before serializing, so
// a Program declaring a slot past it is corrupt / hand-crafted; Plan
// must reject it rather than let the marshal stomp runtime state.

// Appends a `cel.abi` custom section carrying `abi` to `wasm`.
std::vector<uint8_t> AppendCelAbiSection(std::vector<uint8_t> wasm,
                                         const celwasm::abi::CelAbi& abi) {
  std::string payload;
  ABSL_CHECK(abi.SerializeToString(&payload));
  constexpr absl::string_view kName = "cel.abi";
  const auto push_leb = [](std::vector<uint8_t>& out, uint64_t v) {
    do {
      uint8_t byte = v & 0x7f;
      v >>= 7;
      if (v != 0) byte |= 0x80;
      out.push_back(byte);
    } while (v != 0);
  };
  std::vector<uint8_t> body;
  push_leb(body, kName.size());
  body.insert(body.end(), kName.begin(), kName.end());
  body.insert(body.end(), payload.begin(), payload.end());
  wasm.push_back(0x00);  // custom-section id
  push_leb(wasm, body.size());
  wasm.insert(wasm.end(), body.begin(), body.end());
  return wasm;
}

// Synthetic Program (dynamic shape — imports `cel.*`) with a cel.abi
// section declaring one int variable at `slot_offset`.
Program SyntheticProgramWithVariableSlot(uint32_t slot_offset) {
  celwasm::abi::CelAbi abi;
  abi.set_runtime_abi_version(celwasm::abi::kRuntimeAbiVersion);
  abi.set_link_mode(celwasm::abi::LINK_MODE_DYNAMIC);
  auto* variable = abi.add_variables();
  variable->set_name("x");
  variable->set_local_index(0);
  variable->set_slot_offset(slot_offset);
  return Program(AppendCelAbiSection(Wat2Wasm(kSyntheticExprWat), abi));
}

TEST(EnginePlanTest, PlanAcceptsVariableSlotAtWindowBoundary) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  // Slot ends exactly AT the 262144-byte reserved window (m31 §10):
  // the largest offset the compiler could legally emit.
  Program program = SyntheticProgramWithVariableSlot(262144 - 24);
  auto inst_or = engine_or->Plan(program);
  EXPECT_TRUE(inst_or.ok()) << inst_or.status();
}

TEST(EnginePlanTest, PlanRejectsVariableSlotPastReservedWindow) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  // Slot crosses the 262144-byte boundary by 8 bytes; honoring it
  // would write over the runtime's static data.
  Program program = SyntheticProgramWithVariableSlot(262144 - 16);
  auto inst_or = engine_or->Plan(program);
  EXPECT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kInvalidArgument)
      << inst_or.status();
}

TEST(EnginePlanTest, PlanRejectsVariableSlotFarPastMemory) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  // A slot way past any plausible memory size — the raw-write-past-
  // shared-memory shape that SIGSEGVs the host inside wasmtime's
  // registered region if it ever reaches the marshal.
  Program program = SyntheticProgramWithVariableSlot(0x7fffffff);
  auto inst_or = engine_or->Plan(program);
  EXPECT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kInvalidArgument)
      << inst_or.status();
}

TEST(EnginePlanThreadingTest, ConcurrentPlanCallsAllSucceed) {
  // Threading invariant from the plan + cel-host-surface design:
  // Engine + parsed runtime module are wasmtime-thread-safe;
  // Plan() creates a fresh store / linker / memory per call,
  // sharing only the engine + runtime module.  Multiple threads
  // can call Plan concurrently against the same Engine.
  //
  // Test: N threads each call Plan; collect their resulting
  // Instances; verify all are valid via memory_size_bytes().  If
  // wasmtime races during concurrent module instantiation, this
  // either crashes (TSan would see it) or produces a Plan that
  // returns an unhealthy Instance.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);
  Program program = SyntheticProgram();

  constexpr int kNumThreads = 8;
  constexpr int kPlansPerThread = 4;
  std::vector<std::thread> threads;
  std::vector<std::vector<Instance>> per_thread(kNumThreads);
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&, t]() {
      per_thread[t].reserve(kPlansPerThread);
      for (int i = 0; i < kPlansPerThread; ++i) {
        auto inst_or = engine.Plan(program);
        ABSL_CHECK_OK(inst_or)
            << "thread " << t << " Plan iter " << i << ": " << inst_or.status();
        per_thread[t].push_back(*std::move(inst_or));
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  // Every Instance is healthy: store + memory still alive,
  // memory_size_bytes() returns the expected 2-page count.
  for (int t = 0; t < kNumThreads; ++t) {
    for (int i = 0; i < kPlansPerThread; ++i) {
      EXPECT_GT(per_thread[t][i].memory_size_bytes(), 0u)
          << "thread " << t << " plan " << i;
    }
  }
}

TEST(EnginePlanTest, InstanceOutlivesEngineAndCompilerWithEvalProof) {
  // Build everything inside an inner scope so Compiler + Engine +
  // Program all destruct before the assertions run.  Instance
  // holds a shared_ptr to WasmtimeEngineState (the engine + parsed
  // runtime module), so the wasm execution machinery stays alive;
  // its own store / linker / instance were always Instance-owned.
  //
  // Proof of life via Eval: the Eval call goes through
  // wasmtime_func_call_unchecked against the cached eval_fn, which in turn
  // dispatches into the runtime instance (which holds an internal
  // ref to the runtime module via the shared_ptr).  If any of
  // {engine, runtime module, store, linker, helpers_instance,
  // expr_instance, eval_fn} were freed during Engine's destruction
  // this would either trap or UB.  A clean Int(42) is the only
  // way to pass.
  Instance inst = ([]() {
    auto compiler_or = Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(compiler_or);
    auto engine_or = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(engine_or);
    auto prog_or = compiler_or->Compile("42", LinkModeOpts());
    ABSL_CHECK_OK(prog_or);
    auto inst_or = engine_or->Plan(*prog_or);
    ABSL_CHECK_OK(inst_or);
    return *std::move(inst_or);
  })();

  auto v_or = inst.Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kInt);
  auto i = v_or->AsInt();
  ASSERT_TRUE(i.ok());
  EXPECT_EQ(*i, 42);

  // Eval is also re-entrant after the originating Engine drop.
  auto v2_or = inst.Eval();
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();
  EXPECT_EQ(*v2_or->AsInt(), 42);
}

TEST(EngineBuilderJitPerfMapTest, EnabledEngineBuildsPlansAndEvals) {
  // The perfmap strategy only changes wasmtime's JIT bookkeeping (it
  // writes a /tmp/perf-<pid>.map symbol file); the observable contract
  // here is that an Engine built with it on still compiles the runtime
  // module and evaluates correctly.  Exercises the chained-rvalue
  // builder form `benchmark/eval/celwasm_bench` uses.
  auto engine_or = Engine::NewBuilder().EnableJitPerfMap(true).Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok());
  auto prog_or = compiler_or->Compile("40 + 2", LinkModeOpts());
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
  auto v_or = inst_or->Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

TEST(EngineBuilderJitPerfMapTest, DisabledExplicitlyMatchesDefault) {
  // `EnableJitPerfMap(false)` is the default; both spellings build a
  // working Engine (lvalue-builder form covers the `&` overload).
  Engine::Builder b;
  b.EnableJitPerfMap(false);
  auto engine_or = std::move(b).Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
}

// ─── M13 Slice C.1 — Engine::AddModule + Engine::AddFunction ───
//
// Mirrors Probe 4 + Probe 5's coverage, but against the production
// `celwasm::Engine` API rather than the probe-stage `ProbeEngine` /
// inline wasmtime harness.

// A minimal self-contained custom-module wasm: defines + exports
// memory, exports one function.  Bytes precomputed via
// `wasmtime_wat2wasm` to keep the test self-contained (no probe
// data files).
std::vector<uint8_t> MakeMinimalCustomModuleBytes() {
  constexpr absl::string_view kWat = R"(
    (module
      (memory (export "memory") 2)
      (func (export "allow_string_string") (param i32 i32 i32))))";
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(kWat.data(), kWat.size(), &out);
  ABSL_CHECK_EQ(err, nullptr);
  std::vector<uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(out.data),
      reinterpret_cast<const uint8_t*>(out.data) + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

TEST(EngineAddModuleTest, AddModuleAcceptsValidAlias) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  const auto bytes = MakeMinimalCustomModuleBytes();
  EXPECT_TRUE(engine_or->AddModule("rules", bytes).ok());
}

TEST(EngineAddModuleTest, AddModuleRejectsEmptyAlias) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  const auto bytes = MakeMinimalCustomModuleBytes();
  auto s = engine_or->AddModule("", bytes);
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument);
}

TEST(EngineAddModuleTest, AddModuleRejectsReservedAliases) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  const auto bytes = MakeMinimalCustomModuleBytes();
  for (absl::string_view reserved :
       {"cel", "cel_host", "cel_env", "cel_fn", "host"}) {
    auto s = engine_or->AddModule(reserved, bytes);
    EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument)
        << "alias `" << reserved << "` should be rejected";
  }
}

TEST(EngineAddModuleTest, AddModuleRejectsDuplicateAlias) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  const auto bytes = MakeMinimalCustomModuleBytes();
  ASSERT_TRUE(engine_or->AddModule("rules", bytes).ok());
  auto s = engine_or->AddModule("rules", bytes);
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
}

TEST(EngineAddModuleTest, AddModuleRejectsMalformedBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  // Not a valid wasm module.
  const std::vector<uint8_t> bad_bytes{0x00, 0x01, 0x02, 0x03, 0x04};
  auto s = engine_or->AddModule("bad", bad_bytes);
  EXPECT_FALSE(s.ok()) << "malformed wasm bytes should fail to parse";
}

TEST(EngineAddFunctionTest, AddFunctionAcceptsValidImpl) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  EXPECT_TRUE(engine_or->AddFunction("upper_string", 2, impl).ok());
}

TEST(EngineAddFunctionTest, AddFunctionRejectsZeroArity) {
  // num_args must be ≥ 1 (out_slot is always present).
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  auto s = engine_or->AddFunction("zero", 0, impl);
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument);
}

TEST(EngineAddFunctionTest, AddFunctionRejectsEmptyImpl) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback empty;
  auto s = engine_or->AddFunction("upper_string", 2, std::move(empty));
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument);
}

TEST(EngineAddFunctionTest, AddFunctionRejectsDuplicateOverloadId) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  ASSERT_TRUE(engine_or->AddFunction("upper_string", 2, impl).ok());
  auto s = engine_or->AddFunction("upper_string", 2, impl);
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
}

// ─── Engine::BindFunction — declaration-first registration ─────────
//
// Layer 3 sugar over AddTypedFunction: the `.celfn` declaration
// string drives parsing, overload-id synthesis, and registration-time
// signature validation.  The matrix here is registration-level; the
// end-to-end dispatch proof through wasmtime lives in
// e2e/host_fn_test.cc (HostFnTest.BindFunctionDeclFirstRoundTrip).

// Registers `fn` against `celfn_decl` on a fresh engine and returns
// the status — most cases need nothing else from the engine.
template <typename Fn>
absl::Status BindOnFreshEngine(absl::string_view celfn_decl, Fn fn) {
  auto engine_or = Engine::NewBuilder().Build();
  ABSL_CHECK(engine_or.ok()) << engine_or.status();
  return engine_or->BindFunction(celfn_decl, std::move(fn));
}

// Asserts the registration fails InvalidArgument with `expect_substr`
// somewhere in the message (the declared-CEL-type / provided-C++-type
// naming contract).
template <typename Fn>
void ExpectBindMismatch(absl::string_view celfn_decl, Fn fn,
                        absl::string_view expect_substr) {
  auto s = BindOnFreshEngine(celfn_decl, std::move(fn));
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), expect_substr))
      << "expected `" << expect_substr << "` in: " << s.message();
}

// — happy path: every canonical param pairing registers —

TEST(EngineBindFunctionTest, BindsBoolIntUintDoubleParams) {
  auto s = BindOnFreshEngine(
      "bool @host.f(bool b, int i, uint u, double d);",
      [](bool b, int64_t, uint64_t, double) -> absl::StatusOr<bool> {
        return b;
      });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, StringViewParamAcceptsStringDecl) {
  auto s = BindOnFreshEngine("int @host.f(string s);",
                             [](absl::string_view) -> absl::StatusOr<int64_t> {
                               return 0;
                             });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, StringViewParamAcceptsBytesDecl) {
  auto s = BindOnFreshEngine("int @host.f(bytes b);",
                             [](absl::string_view) -> absl::StatusOr<int64_t> {
                               return 0;
                             });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, BindsDurationAndTimestampParams) {
  auto s = BindOnFreshEngine(
      "int @host.f(Duration d, Timestamp t);",
      [](absl::Duration, absl::Time) -> absl::StatusOr<int64_t> {
        return 0;
      });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, BindsListAndMapParams) {
  auto s = BindOnFreshEngine(
      "int @host.f(list<int> xs, map<string, int> m);",
      [](HostListView, HostMapView) -> absl::StatusOr<int64_t> {
        return 0;
      });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, BindsConcreteProtoParam) {
  auto s = BindOnFreshEngine(
      "bool @host.f(proto(celwasm.abi.CelAbi) a);",
      [](const celwasm::abi::CelAbi&) -> absl::StatusOr<bool> {
        return true;
      });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, BindsPolymorphicProtoParam) {
  auto s = BindOnFreshEngine(
      "bool @host.f(proto(celwasm.abi.CelAbi) a);",
      [](const google::protobuf::Message*) -> absl::StatusOr<bool> {
        return true;
      });
  EXPECT_TRUE(s.ok()) << s;
}

TEST(EngineBindFunctionTest, ValueParamMatchesAnyDeclaredType) {
  // typed_function.h's ArgTrait specializations are keyed on the EXACT
  // parameter type; `const Value&` has no specialization (only `Value`),
  // so a Value param at the typed surface must be by-value — the
  // performance warning is a structural constraint of the typed API.
  auto scalar =
      BindOnFreshEngine("int @host.f(int x);",
                        // NOLINTNEXTLINE(performance-unnecessary-value-param)
                        [](Value) -> absl::StatusOr<int64_t> {
                          return 0;
                        });
  EXPECT_TRUE(scalar.ok()) << scalar;
  auto aggregate =
      BindOnFreshEngine("int @host.f(list<int> xs);",
                        // NOLINTNEXTLINE(performance-unnecessary-value-param)
                        [](Value) -> absl::StatusOr<int64_t> {
                          return 0;
                        });
  EXPECT_TRUE(aggregate.ok()) << aggregate;
  // `null` has no canonical C++ spelling; Value is the only match.
  auto null_decl =
      BindOnFreshEngine("int @host.f(null n);",
                        // NOLINTNEXTLINE(performance-unnecessary-value-param)
                        [](Value) -> absl::StatusOr<int64_t> {
                          return 0;
                        });
  EXPECT_TRUE(null_decl.ok()) << null_decl;
}

TEST(EngineBindFunctionTest, BindsNullaryDecl) {
  auto s =
      BindOnFreshEngine("int @host.now_ms();", []() -> absl::StatusOr<int64_t> {
        return 0;
      });
  EXPECT_TRUE(s.ok()) << s;
}

// — overload-id synthesis matches the hand-spelled forms —

TEST(EngineBindFunctionTest, OverloadIdMatchesHandSpelledForms) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  ASSERT_TRUE(
      engine_or
          ->BindFunction("int @host.discount_pct(string tier);",
                         [](absl::string_view) -> absl::StatusOr<int64_t> {
                           return 20;
                         })
          .ok());
  // The Layer-2 hand-spelled overload-id now collides — proof the
  // synthesised id is exactly `discount_pct_string`.
  auto typed = engine_or->AddTypedFunction(
      "discount_pct_string", [](absl::string_view) -> absl::StatusOr<int64_t> {
        return 20;
      });
  EXPECT_EQ(typed.code(), absl::StatusCode::kAlreadyExists);
  // ... and so does the raw Layer-0 form (num_args = params + out_slot).
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  auto raw = engine_or->AddFunction("discount_pct_string", 2, impl);
  EXPECT_EQ(raw.code(), absl::StatusCode::kAlreadyExists);
}

TEST(EngineBindFunctionTest, ReceiverDeclSynthesisesSameIdAsPlainParam) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  ASSERT_TRUE(engine_or
                  ->BindFunction(
                      "string @host.upper(this string s);",
                      [](absl::string_view s) -> absl::StatusOr<std::string> {
                        return std::string(s);
                      })
                  .ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  auto raw = engine_or->AddFunction("upper_string", 2, impl);
  EXPECT_EQ(raw.code(), absl::StatusCode::kAlreadyExists);
}

TEST(EngineBindFunctionTest, DuplicateRegistrationAlreadyExists) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  constexpr absl::string_view kDecl = "int @host.f(int x);";
  ASSERT_TRUE(engine_or
                  ->BindFunction(kDecl,
                                 [](int64_t x) -> absl::StatusOr<int64_t> {
                                   return x;
                                 })
                  .ok());
  auto s =
      engine_or->BindFunction(kDecl, [](int64_t x) -> absl::StatusOr<int64_t> {
        return x;
      });
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
}

// — declaration-shape rejections —

TEST(EngineBindFunctionTest, ParseErrorRejected) {
  auto s = BindOnFreshEngine("this is not a celfn decl",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "Engine::BindFunction"))
      << s.message();
}

TEST(EngineBindFunctionTest, MultiDeclStringRejected) {
  auto s = BindOnFreshEngine("int @host.a(int x); int @host.b(int x);",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(
      absl::StrContains(s.message(), "exactly one declaration, found 2"))
      << s.message();
}

TEST(EngineBindFunctionTest, PluginBackendRejected) {
  auto s = BindOnFreshEngine("int @plugin.f(int x);",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "@plugin")) << s.message();
}

TEST(EngineBindFunctionTest, NativeBackendRejected) {
  auto s = BindOnFreshEngine("Module m;\nint @native.f(int x) = x + 1;",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "@native")) << s.message();
}

// — signature-vs-declaration rejections —

TEST(EngineBindFunctionTest, ArityMismatchFewerCppParams) {
  auto s = BindOnFreshEngine("int @host.f(int x, int y);",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "declares 2 parameter(s)"))
      << s.message();
}

TEST(EngineBindFunctionTest, ArityMismatchMoreCppParams) {
  auto s = BindOnFreshEngine("int @host.f();",
                             [](int64_t x) -> absl::StatusOr<int64_t> {
                               return x;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "callable takes 1"))
      << s.message();
}

TEST(EngineBindFunctionTest, MismatchNamesIndexDeclTypeAndCppType) {
  // Param 0 matches; param 1 is declared `string` but provided
  // int64_t — the message names all three facts.
  auto s = BindOnFreshEngine("int @host.f(int a, string b);",
                             [](int64_t, int64_t) -> absl::StatusOr<int64_t> {
                               return 0;
                             });
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "parameter 1")) << s.message();
  EXPECT_TRUE(absl::StrContains(s.message(), "`string`")) << s.message();
  EXPECT_TRUE(absl::StrContains(s.message(), "`int64_t`")) << s.message();
}

TEST(EngineBindFunctionTest, IntDeclRejectsUintParam) {
  ExpectBindMismatch(
      "int @host.f(int x);",
      [](uint64_t) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "uint64_t");
}

TEST(EngineBindFunctionTest, UintDeclRejectsIntParam) {
  ExpectBindMismatch(
      "int @host.f(uint x);",
      [](int64_t) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "int64_t");
}

TEST(EngineBindFunctionTest, DoubleDeclRejectsIntParam) {
  ExpectBindMismatch(
      "int @host.f(double x);",
      [](int64_t) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "int64_t");
}

TEST(EngineBindFunctionTest, BoolDeclRejectsDoubleParam) {
  ExpectBindMismatch(
      "int @host.f(bool x);",
      [](double) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "double");
}

TEST(EngineBindFunctionTest, StringDeclRejectsIntParam) {
  ExpectBindMismatch(
      "int @host.f(string x);",
      [](int64_t) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "int64_t");
}

TEST(EngineBindFunctionTest, BytesDeclRejectsBoolParam) {
  ExpectBindMismatch(
      "int @host.f(bytes x);",
      [](bool) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "bool");
}

TEST(EngineBindFunctionTest, DurationDeclRejectsTimestampParam) {
  ExpectBindMismatch(
      "int @host.f(Duration x);",
      [](absl::Time) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "absl::Time");
}

TEST(EngineBindFunctionTest, TimestampDeclRejectsDurationParam) {
  ExpectBindMismatch(
      "int @host.f(Timestamp x);",
      [](absl::Duration) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "absl::Duration");
}

TEST(EngineBindFunctionTest, ListDeclRejectsMapParam) {
  ExpectBindMismatch(
      "int @host.f(list<int> x);",
      [](HostMapView) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "HostMapView");
}

TEST(EngineBindFunctionTest, MapDeclRejectsListParam) {
  ExpectBindMismatch(
      "int @host.f(map<string, int> x);",
      [](HostListView) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "HostListView");
}

TEST(EngineBindFunctionTest, ProtoDeclRejectsIntParam) {
  ExpectBindMismatch(
      "int @host.f(proto(celwasm.abi.CelAbi) x);",
      [](int64_t) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "int64_t");
}

TEST(EngineBindFunctionTest, NullDeclRejectsBoolParam) {
  // `null` has no canonical C++ spelling — only `Value` matches.
  ExpectBindMismatch(
      "int @host.f(null x);",
      [](bool) -> absl::StatusOr<int64_t> {
        return 0;
      },
      "`null`");
}

TEST(EnginePlanWithCustomsTest, PlanStillWorksWithRegisteredModuleAndCallback) {
  // Smoke test: register a module + a host callback, then Plan the
  // standard "42" program (which uses neither).  Plan should
  // succeed; the registered module instantiates cleanly but its
  // exports are never imported by this program — that's fine.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  ASSERT_TRUE(
      engine_or->AddModule("rules", MakeMinimalCustomModuleBytes()).ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  ASSERT_TRUE(engine_or->AddFunction("never_called", 2, impl).ok());

  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok());
  auto prog_or = compiler_or->Compile("42", LinkModeOpts());
  ASSERT_TRUE(prog_or.ok());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();

  auto v_or = inst_or->Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

// ─── m24 — Engine::AddPlugin failure-mode coverage ─────────────
//
// AddPlugin runs at engine setup, before any Plan.  The four
// reachable failure modes are:
//   - empty `plugin_bytes` → InvalidArgument
//   - overload-id collides with an earlier `AddFunction` → AlreadyExists
//   - overload-id collides with an earlier `AddPlugin` → AlreadyExists
//   - malformed plugin bytes → InvalidArgument (wasmtime error)
// The dispatch through to `wasmtime_component_func_call` only fires
// at Plan + Eval time — covered by the e2e foreign-fn matrix
// (task C.5) once we have a real plugin fixture.

// A tiny helper to build a kPlugin library declaring one
// nullary bool fn under the given overload-id.  Used as the
// conflict-detection surface.
celwasm::FunctionLibrary OneBoolFnLibrary(absl::string_view fn_name) {
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddPlugin(fn_name, celwasm::CelType::Bool(), {})
                    .Build();
  ABSL_CHECK(lib_or.ok()) << lib_or.status();
  return *std::move(lib_or);
}

TEST(EngineAddPluginTest, RejectsEmptyPluginBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  std::vector<uint8_t> empty_bytes;
  auto s = engine_or->AddPlugin(empty_bytes, OneBoolFnLibrary("f"));
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument);
}

TEST(EngineAddPluginTest, RejectsMalformedPluginBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  // Anything that's not a real Component-Model component.  The
  // §13 probe confirmed wasmtime_component_new returns an error
  // shape we can surface as InvalidArgument.
  const std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef};
  auto s = engine_or->AddPlugin(garbage, OneBoolFnLibrary("g"));
  EXPECT_FALSE(s.ok()) << "garbage plugin bytes should fail to parse";
}

TEST(EngineAddPluginTest,
     ConflictWithEarlierAddFunctionReportedAtRegistration) {
  // Conflict detection runs BEFORE the parse, so the test does not
  // need to provide a real plugin — the conflict trips first.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  ASSERT_TRUE(engine_or->AddFunction("collide", /*num_args=*/1, impl).ok());
  const std::vector<uint8_t> any_bytes{0x00};  // never parsed
  auto s = engine_or->AddPlugin(any_bytes, OneBoolFnLibrary("collide"));
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
  EXPECT_NE(std::string(s.message()).find("collide"), std::string::npos);
  EXPECT_NE(std::string(s.message()).find("AddFunction"), std::string::npos);
}

TEST(EngineAddPluginTest, ConflictWithEarlierAddPluginReportedAtRegistration) {
  // Two AddPlugin calls naming the same overload-id; the second
  // is rejected.  The first registration needs bytes that
  // wasmtime_component_new accepts — an empty `(component)` is
  // enough, because the export ↔ decl lookup happens at per-Plan
  // plugin instantiation, not at registration.  The second call
  // reuses the overload-id with garbage bytes: the conflict check
  // executes BEFORE any wasmtime parse, so it must surface
  // AlreadyExists (naming the prior plugin), not the
  // InvalidArgument a parse attempt would produce.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  const std::vector<uint8_t> plugin_bytes = Wat2Wasm("(component)");
  ASSERT_TRUE(engine_or->AddPlugin(plugin_bytes, OneBoolFnLibrary("dup")).ok());
  const std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef};
  auto s = engine_or->AddPlugin(garbage, OneBoolFnLibrary("dup"));
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
  EXPECT_NE(std::string(s.message()).find("dup"), std::string::npos);
  EXPECT_NE(std::string(s.message()).find("previously-registered plugin"),
            std::string::npos);
}

TEST(EngineAddPluginTest, EmptyLibraryNoDeclsParsesOnly) {
  // An AddPlugin call with a library that declares NO
  // kPlugin decls is degenerate but well-defined: the
  // parse still has to succeed.  Garbage bytes therefore still
  // fail — we're confirming the empty library doesn't bypass the
  // parse step.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  auto lib_or = celwasm::FunctionLibrary::Builder().Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  const std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef};
  auto s = engine_or->AddPlugin(garbage, *lib_or);
  EXPECT_FALSE(s.ok()) << "garbage bytes should fail even with empty library";
}

TEST(EngineAddPluginTest, PlanSucceedsWhenNoPluginsRegistered) {
  // Regression: the new InstantiateAndBindPlugins step in Plan
  // must be a no-op when no plugins are registered.  This pins
  // that Plan stays green for code paths that don't use the
  // plugin backend at all.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok());
  auto prog_or = compiler_or->Compile("42");
  ASSERT_TRUE(prog_or.ok());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
  auto v_or = inst_or->Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

// ─── m35 B1 — Engine::Use (one-noun plugin registration) ─────────
//
// `Use(plugin)` wraps the AddPlugin internals and adds (a) a STATIC
// export check against the parsed component (interface + every
// decl's kebab-case export, via wasmtime_component_get_export_index
// — no store, no instantiation) and (b) content-hash retention on
// the registry entry.  Failure matrix per m35-plugin-ergonomics.md
// §3.3/§3.4:
//   - overload-id collision (AddFunction / prior plugin) →
//     AlreadyExists, checked BEFORE the parse + export check
//   - structurally-corrupt component body → InvalidArgument
//   - missing WIT interface / missing per-decl export →
//     FailedPrecondition naming the missing thing, at registration
//     (BEFORE any Plan)

// Loads the macro-built `demo_plugin.wasm` (carries its `cel.fns`
// section: Module customfn; @plugin.{greet,add,len}) from runfiles.
std::vector<uint8_t> LoadDemoPluginBytes() {
  std::string error;
  auto runfiles = absl::WrapUnique(Runfiles::CreateForTest(&error));
  ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path = runfiles->Rlocation(
      "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/demo_plugin.wasm");
  ABSL_CHECK(!path.empty()) << "demo_plugin.wasm not in runfiles";
  std::ifstream f(path, std::ios::binary);
  ABSL_CHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

// A valid-but-empty component carrying a `cel.fns` section that
// declares `fns` the component does not export: the fixture the
// static-export-check negatives are built from (component WAT has
// no cel.fns of its own; the section is appended with the
// production framing writer).
std::vector<uint8_t> ComponentWithCelFns(absl::string_view component_wat,
                                         absl::string_view celfn_text) {
  const std::vector<uint8_t> component = Wat2Wasm(component_wat);
  auto with_fns = AppendCustomSection(
      component, "cel.fns",
      {reinterpret_cast<const uint8_t*>(celfn_text.data()), celfn_text.size()});
  ABSL_CHECK_OK(with_fns.status());
  return *std::move(with_fns);
}

TEST(EngineUseTest, HappyPathRegistersMacroBuiltDemoPlugin) {
  // The macro-built artifact self-describes (cel.fns embedded);
  // Load → Use resolves interface `cel:customfn/fns@0.1.0` and the
  // kebab exports greet-string-int / add-int-int / len-string
  // statically.  Plan/Eval through the registered plugin is the
  // demo e2e's job (demo_plugin_e2e_test.cc).
  auto plugin_or = Plugin::Load(LoadDemoPluginBytes());
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  EXPECT_TRUE(engine_or->Use(*plugin_or).ok());
}

TEST(EngineUseTest, MissingInterfaceFailsAtUseNamingInterface) {
  // `(component)` exports nothing; the appended cel.fns declares a
  // phantom fn, deriving interface `cel:customfn/fns@0.1.0` (no
  // Module directive → fallback module `customfn`).  Use must fail
  // FailedPrecondition at registration — BEFORE any Plan — naming
  // the interface it could not resolve.
  auto plugin_or = Plugin::Load(
      ComponentWithCelFns("(component)", "int @plugin.phantom(int x);\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  ASSERT_EQ(plugin_or->wit_interface(), "cel:customfn/fns@0.1.0");
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto s = engine_or->Use(*plugin_or);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_TRUE(absl::StrContains(s.message(),
                                "does not export interface "
                                "`cel:customfn/fns@0.1.0`"))
      << s;
}

// Component exporting interface `cel:customfn/fns@0.1.0` with ONE
// nested export (`add-int-int`) — the fixture for the
// export-missing-under-interface arm.
constexpr absl::string_view kAddOnlyIfaceComponentWat = R"WAT(
(component
  (core module $m
    (func (export "add") (param i64 i64) (result i64)
      (i64.add (local.get 0) (local.get 1))))
  (core instance $ci (instantiate $m))
  (func $add (param "a" s64) (param "b" s64) (result s64)
    (canon lift (core func $ci "add")))
  (instance $fns (export "add-int-int" (func $add)))
  (export "cel:customfn/fns@0.1.0" (instance $fns)))
)WAT";

TEST(EngineUseTest, MissingExportUnderInterfaceFailsAtUseNamingExport) {
  // The interface resolves; the second decl's kebab export
  // (`phantom-int`) does not exist under it.  Use fails
  // FailedPrecondition naming the kebab export, the interface, and
  // the CEL overload-id — before any Plan.
  auto plugin_or = Plugin::Load(ComponentWithCelFns(
      kAddOnlyIfaceComponentWat,
      "int @plugin.add(int a, int b);\nint @plugin.phantom(int x);\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto s = engine_or->Use(*plugin_or);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "`phantom-int`")) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "cel:customfn/fns@0.1.0")) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "`phantom_int`")) << s;
}

TEST(EngineUseTest, AllDeclaredExportsPresentUnderInterfacePasses) {
  // Positive twin of the negative above: decls exactly matching the
  // interface's nested exports register cleanly.
  auto plugin_or = Plugin::Load(ComponentWithCelFns(
      kAddOnlyIfaceComponentWat, "int @plugin.add(int a, int b);\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  EXPECT_TRUE(engine_or->Use(*plugin_or).ok());
}

TEST(EngineUseTest, CollisionWithEarlierAddFunctionAlreadyExists) {
  // Collision detection runs FIRST — before the parse and the
  // static export check — so even a plugin that would fail the
  // export check reports the AlreadyExists collision.
  auto plugin_or = Plugin::Load(
      ComponentWithCelFns("(component)", "int @plugin.phantom(int x);\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  ASSERT_TRUE(engine_or->AddFunction("phantom_int", /*num_args=*/2, impl).ok());
  auto s = engine_or->Use(*plugin_or);
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "Engine::Use")) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "phantom_int")) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "AddFunction")) << s;
}

TEST(EngineUseTest, CollisionWithPriorPluginAlreadyExists) {
  // Same Plugin registered twice: the second Use collides on every
  // overload-id with the first registration.
  auto plugin_or = Plugin::Load(ComponentWithCelFns(
      kAddOnlyIfaceComponentWat, "int @plugin.add(int a, int b);\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  ASSERT_TRUE(engine_or->Use(*plugin_or).ok());
  auto s = engine_or->Use(*plugin_or);
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "add_int_int")) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "previously-registered plugin"))
      << s;
}

TEST(EngineUseTest, CollisionAcrossUseAndLegacyAddPluginAlreadyExists) {
  // The registry is shared between Use and the legacy AddPlugin
  // escape: an overload-id registered via AddPlugin blocks a later
  // Use of a plugin declaring the same id.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  ASSERT_TRUE(
      engine_or->AddPlugin(Wat2Wasm("(component)"), OneBoolFnLibrary("phantom"))
          .ok());
  auto plugin_or = Plugin::Load(
      ComponentWithCelFns("(component)", "bool @plugin.phantom();\n"));
  ASSERT_TRUE(plugin_or.ok()) << plugin_or.status();
  auto s = engine_or->Use(*plugin_or);
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists) << s;
  EXPECT_TRUE(absl::StrContains(s.message(), "previously-registered plugin"))
      << s;
}

TEST(EngineUseTest, HashRetainedOnRegistryEntry) {
  GTEST_SKIP()
      << "RegisteredPlugin::hash is populated by Engine::Use (all-zero on "
         "the legacy AddPlugin path) but has no observation seam: the "
         "registry lives on the private WasmtimeEngineState and the "
         "Plan-time diagnostics that would surface the hash are future "
         "work (m35-plugin-ergonomics.md §9/§11 — hash enforcement is an "
         "embedder conversation).  Un-skip by asserting on the Plan-time "
         "hash diagnostic once that surface lands.";
  // Intended assertion: after engine.Use(plugin), the registry entry
  // for `plugin` carries plugin.hash() (non-zero); after the legacy
  // engine.AddPlugin(bytes, lib), the entry's hash is all-zero.
}

// ─── m28 — link-mode label tripwire (`ValidateLinkModeLabel`) ──────
//
// `Engine::Plan` decodes the cel.abi section up front and, when the
// section is present, cross-checks its `link_mode` label against the
// import-derived routing (`ModuleImportsCelNamespace`).  A label that
// contradicts the module's actual shape — a mislabeled / corrupted
// artifact from a Program cache — fails Plan with FailedPrecondition
// naming both signals.  Absent section → no validation (synthetic WAT
// fixtures, legacy Programs).  Unknown future enum values → no
// validation (open-set wire data).

// Locates the `cel.abi` custom section's proto payload (the bytes
// AFTER the section name) within a wasm byte stream, via
// `FindCustomSection` (abi/wasm_binary.h).  Returns [begin, end)
// indices into `bytes` — offsets rather than the returned span,
// because the caller patches the payload in place.  CHECK-fails if
// absent — the fixtures that call this always carry the section.
struct PayloadRange {
  size_t begin;
  size_t end;
};
PayloadRange FindCelAbiPayload(const std::vector<uint8_t>& bytes) {
  const absl::StatusOr<absl::Span<const uint8_t>> payload =
      FindCustomSection(bytes, "cel.abi");
  ABSL_CHECK_OK(payload.status())
      << "no cel.abi custom section in fixture wasm";
  const auto begin = static_cast<size_t>(payload->data() - bytes.data());
  return {begin, begin + payload->size()};
}

// Compiles `42` in the given mode and returns a mutable copy of the
// Program's wasm bytes.
std::vector<uint8_t> CompileToBytes(CompilerOptions::LinkMode mode) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ABSL_CHECK_OK(compiler_or);
  CompilerOptions opts;
  opts.link_mode = mode;
  auto prog_or = compiler_or->Compile("42", opts);
  ABSL_CHECK_OK(prog_or);
  return {prog_or->wasm_bytes().begin(), prog_or->wasm_bytes().end()};
}

// Rewrites the wire value of the trailing `link_mode` field in a
// STATIC Program's cel.abi payload.  The emitter serializes fields
// in number order and `link_mode` (field 7, the highest) is the only
// non-default trailing field a static compile adds, so the payload
// ends with the 2-byte record `0x38 0x01` (tag fld7/varint, value 1).
// Patching the value byte in place flips the label without resizing
// any section, keeping all wasm framing intact.
std::vector<uint8_t> PatchStaticLinkModeByte(std::vector<uint8_t> bytes,
                                             uint8_t new_value) {
  const PayloadRange r = FindCelAbiPayload(bytes);
  ABSL_CHECK_GE(r.end - r.begin, 2u);
  ABSL_CHECK_EQ(bytes[r.end - 2], 0x38) << "payload does not end with the "
                                           "link_mode field tag";
  ABSL_CHECK_EQ(bytes[r.end - 1], 0x01) << "link_mode wire value is not "
                                           "LINK_MODE_STATIC";
  bytes[r.end - 1] = new_value;
  return bytes;
}

TEST(EnginePlanLinkModeTripwireTest, CorrectlyLabeledProgramsPlanInBothModes) {
  // Both routing paths with truthful labels: the tripwire must stay
  // silent and Plan + Eval succeed.  (The rest of this file's
  // Compile()-based tests cover the per-binary kTestLinkMode variant;
  // this one pins BOTH modes inside a single binary.)
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  for (auto mode : {CompilerOptions::LinkMode::kDynamic,
                    CompilerOptions::LinkMode::kStatic}) {
    Program program(CompileToBytes(mode));
    auto inst_or = engine_or->Plan(program);
    ASSERT_TRUE(inst_or.ok()) << inst_or.status();
    auto v_or = inst_or->Eval();
    ASSERT_TRUE(v_or.ok()) << v_or.status();
    EXPECT_EQ(*v_or->AsInt(), 42);
  }
}

TEST(EnginePlanLinkModeTripwireTest, MislabeledStaticProgramRejectedAtPlan) {
  // A static-shaped Program (no cel.* imports) whose cel.abi claims
  // LINK_MODE_DYNAMIC — the cache-corruption shape the tripwire
  // exists for.  0x00 is the explicit LINK_MODE_DYNAMIC wire value.
  auto bytes = PatchStaticLinkModeByte(
      CompileToBytes(CompilerOptions::LinkMode::kStatic),
      /*new_value=*/0x00);
  // Sanity: the patch decodes as intended.
  auto abi_or = DecodeCelAbiFromWasm(bytes);
  ASSERT_TRUE(abi_or.ok()) << abi_or.status();
  ASSERT_EQ(abi_or->link_mode(), celwasm::abi::LINK_MODE_DYNAMIC);

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto inst_or = engine_or->Plan(Program(std::move(bytes)));
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition);
  // The diagnostic names both signals: the label and the import shape.
  EXPECT_NE(std::string(inst_or.status().message()).find("LINK_MODE_DYNAMIC"),
            std::string::npos)
      << inst_or.status();
  EXPECT_NE(std::string(inst_or.status().message()).find("cel"),
            std::string::npos)
      << inst_or.status();
}

TEST(EnginePlanLinkModeTripwireTest,
     MislabeledDynamicShapedModuleRejectedAtPlan) {
  // The opposite arm: a module that DOES import cel.* (the synthetic
  // WAT fixture) carrying a cel.abi section that claims
  // LINK_MODE_STATIC.  Built by appending a cel.abi custom section
  // framed by `BuildCustomSection` (abi/wasm_binary.h) — custom
  // sections may appear anywhere, and the fixture has none of its
  // own.  `runtime_abi_version` stays 0 with an otherwise-empty abi,
  // which `CheckRuntimeAbiVersion` admits.
  celwasm::abi::CelAbi abi;
  abi.set_link_mode(celwasm::abi::LINK_MODE_STATIC);
  const std::string payload = abi.SerializeAsString();

  const std::vector<uint8_t> section = BuildCustomSection(
      "cel.abi",
      absl::Span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

  std::vector<uint8_t> bytes = Wat2Wasm(kSyntheticExprWat);
  bytes.insert(bytes.end(), section.begin(), section.end());

  // Sanity: the appended section decodes as intended.
  auto abi_or = DecodeCelAbiFromWasm(bytes);
  ASSERT_TRUE(abi_or.ok()) << abi_or.status();
  ASSERT_EQ(abi_or->link_mode(), celwasm::abi::LINK_MODE_STATIC);

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto inst_or = engine_or->Plan(Program(std::move(bytes)));
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_NE(std::string(inst_or.status().message()).find("LINK_MODE_STATIC"),
            std::string::npos)
      << inst_or.status();
}

TEST(EnginePlanLinkModeTripwireTest, UnknownFutureLinkModeValueNotValidated) {
  // Open-set wire data: a link_mode value this engine doesn't know
  // (e.g. 2, a future hybrid mode) must NOT fail validation — the
  // engine routes on the import shape alone.  Plan + Eval succeed.
  auto bytes = PatchStaticLinkModeByte(
      CompileToBytes(CompilerOptions::LinkMode::kStatic),
      /*new_value=*/0x02);
  auto abi_or = DecodeCelAbiFromWasm(bytes);
  ASSERT_TRUE(abi_or.ok()) << abi_or.status();
  ASSERT_EQ(static_cast<int>(abi_or->link_mode()), 2);

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto inst_or = engine_or->Plan(Program(std::move(bytes)));
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
  auto v_or = inst_or->Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

TEST(EnginePlanLinkModeTripwireTest, AbiLessModulePlansWithoutValidation) {
  // No cel.abi section at all (synthetic WAT fixture / legacy
  // Program) → decode is NotFound → no label to validate → Plan
  // succeeds on the import-derived routing alone.  Same fixture as
  // PlanSucceedsOnSyntheticProgram; restated here so the tripwire's
  // absent-section contract is pinned by name.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  auto inst_or = engine_or->Plan(SyntheticProgram());
  EXPECT_TRUE(inst_or.ok()) << inst_or.status();
}

}  // namespace
}  // namespace celwasm
