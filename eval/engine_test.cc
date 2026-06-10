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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "bazel/link_mode_test_helpers.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/instance.h"
#include "eval/internal/abi_decode.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

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
  // wasmtime_func_call against the cached eval_fn, which in turn
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

// ─── m24 — Engine::AddComponent failure-mode coverage ─────────────
//
// AddComponent runs at engine setup, before any Plan.  The four
// reachable failure modes are:
//   - empty `component_bytes` → InvalidArgument
//   - overload-id collides with an earlier `AddFunction` → AlreadyExists
//   - overload-id collides with an earlier `AddComponent` → AlreadyExists
//   - malformed component bytes → InvalidArgument (wasmtime error)
// The dispatch through to `wasmtime_component_func_call` only fires
// at Plan + Eval time — covered by the e2e foreign-fn matrix
// (task C.5) once we have a real component fixture.

// A tiny helper to build a kForeignComponent library declaring one
// nullary bool fn under the given overload-id.  Used as the
// conflict-detection surface.
celwasm::FunctionLibrary OneBoolFnLibrary(absl::string_view fn_name) {
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kBool;
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddForeignComponent(fn_name, ret, {})
                    .Build();
  ABSL_CHECK(lib_or.ok()) << lib_or.status();
  return *std::move(lib_or);
}

TEST(EngineAddComponentTest, RejectsEmptyComponentBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  std::vector<uint8_t> empty_bytes;
  auto s = engine_or->AddComponent(empty_bytes, OneBoolFnLibrary("f"));
  EXPECT_EQ(s.code(), absl::StatusCode::kInvalidArgument);
}

TEST(EngineAddComponentTest, RejectsMalformedComponentBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  // Anything that's not a real Component-Model component.  The
  // §13 probe confirmed wasmtime_component_new returns an error
  // shape we can surface as InvalidArgument.
  const std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef};
  auto s = engine_or->AddComponent(garbage, OneBoolFnLibrary("g"));
  EXPECT_FALSE(s.ok()) << "garbage component bytes should fail to parse";
}

TEST(EngineAddComponentTest,
     ConflictWithEarlierAddFunctionReportedAtRegistration) {
  // Conflict detection runs BEFORE the parse, so the test does not
  // need to provide a real component — the conflict trips first.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  HostCallback impl = [](HostCallContext& /*ctx*/) {
    return absl::OkStatus();
  };
  ASSERT_TRUE(engine_or->AddFunction("collide", /*num_args=*/1, impl).ok());
  const std::vector<uint8_t> any_bytes{0x00};  // never parsed
  auto s = engine_or->AddComponent(any_bytes, OneBoolFnLibrary("collide"));
  EXPECT_EQ(s.code(), absl::StatusCode::kAlreadyExists);
  EXPECT_NE(std::string(s.message()).find("collide"), std::string::npos);
  EXPECT_NE(std::string(s.message()).find("AddFunction"), std::string::npos);
}

TEST(EngineAddComponentTest,
     ConflictWithEarlierAddComponentReportedAtRegistration) {
  // Two AddComponent calls naming the same overload-id.  The second
  // is rejected.  We need the first call to actually land (i.e.
  // parse), so we use bytes that wasmtime accepts.  Real components
  // are non-trivial to inline here — use a 1-byte sentinel that the
  // conflict check would catch BEFORE the wasmtime_component_new
  // call on the second attempt.  To exercise the "earlier component
  // already registered" arm without needing a real component, we
  // construct an `Engine` whose state holds one already; this is
  // covered indirectly by the e2e foreign-fn matrix (C.5).  Here
  // we pin the AddFunction-first variant above and the
  // pre-parse-conflict-detection contract: the conflict check
  // executes for the SAME-overload-id case before any wasmtime
  // parse, so a garbage second component still surfaces
  // AlreadyExists rather than InvalidArgument.
  GTEST_SKIP() << "blocked on a real Component-Model component fixture "
                  "(needed to land the first AddComponent in this test) — "
                  "lands with C.5 / D.1";
}

TEST(EngineAddComponentTest, EmptyLibraryNoDeclsParsesOnly) {
  // An AddComponent call with a library that declares NO
  // kForeignComponent decls is degenerate but well-defined: the
  // parse still has to succeed.  Garbage bytes therefore still
  // fail — we're confirming the empty library doesn't bypass the
  // parse step.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  auto lib_or = celwasm::FunctionLibrary::Builder().Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  const std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef};
  auto s = engine_or->AddComponent(garbage, *lib_or);
  EXPECT_FALSE(s.ok()) << "garbage bytes should fail even with empty library";
}

TEST(EngineAddComponentTest, PlanSucceedsWhenNoComponentsRegistered) {
  // Regression: the new InstantiateAndBindComponents step in Plan
  // must be a no-op when no components are registered.  This pins
  // that Plan stays green for code paths that don't use the
  // component backend at all.
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

// Reads an unsigned LEB128 u32 at `*pos`, advancing it.  Test-local
// minimal reader — fixture wasm is well-formed by construction.
uint32_t ReadLebU32(const std::vector<uint8_t>& bytes, size_t* pos) {
  uint32_t result = 0;
  uint32_t shift = 0;
  while (true) {
    ABSL_CHECK_LT(*pos, bytes.size());
    const uint8_t b = bytes[(*pos)++];
    result |= static_cast<uint32_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return result;
    shift += 7;
  }
}

// Appends `v` as unsigned LEB128.
void AppendLebU32(std::vector<uint8_t>& out, uint32_t v) {
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v != 0) b |= 0x80;
    out.push_back(b);
  } while (v != 0);
}

// Locates the `cel.abi` custom section's proto payload (the bytes
// AFTER the section name) within a wasm byte stream.  Returns
// [begin, end) indices into `bytes`.  CHECK-fails if absent — the
// fixtures that call this always carry the section.
struct PayloadRange {
  size_t begin;
  size_t end;
};
PayloadRange FindCelAbiPayload(const std::vector<uint8_t>& bytes) {
  size_t pos = 8;  // skip magic + version
  while (pos < bytes.size()) {
    const uint8_t section_id = bytes[pos++];
    const uint32_t section_size = ReadLebU32(bytes, &pos);
    const size_t section_end = pos + section_size;
    if (section_id == 0) {
      size_t p = pos;
      const uint32_t name_len = ReadLebU32(bytes, &p);
      const absl::string_view name(
          reinterpret_cast<const char*>(bytes.data() + p), name_len);
      if (name == "cel.abi") return {p + name_len, section_end};
    }
    pos = section_end;
  }
  ABSL_CHECK(false) << "no cel.abi custom section in fixture wasm";
  return {0, 0};
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
  // LINK_MODE_STATIC.  Built by appending a hand-framed cel.abi
  // custom section — custom sections may appear anywhere, and the
  // fixture has none of its own.  `runtime_abi_version` stays 0 with
  // an otherwise-empty abi, which `CheckRuntimeAbiVersion` admits.
  celwasm::abi::CelAbi abi;
  abi.set_link_mode(celwasm::abi::LINK_MODE_STATIC);
  const std::string payload = abi.SerializeAsString();

  std::vector<uint8_t> body;
  constexpr absl::string_view kName = "cel.abi";
  AppendLebU32(body, static_cast<uint32_t>(kName.size()));
  body.insert(body.end(), kName.begin(), kName.end());
  body.insert(body.end(), payload.begin(), payload.end());

  std::vector<uint8_t> bytes = Wat2Wasm(kSyntheticExprWat);
  bytes.push_back(0x00);  // custom section id
  AppendLebU32(bytes, static_cast<uint32_t>(body.size()));
  bytes.insert(bytes.end(), body.begin(), body.end());

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
