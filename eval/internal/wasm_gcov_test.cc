// Tests for eval/internal/wasm_gcov.{h,cc}.
//
// The sink half is asserted against the .gcda wire format compiler-rt's
// GCDAProfiling.c (llvmorg-19.1.5) produces: header (magic, version,
// stamp), function record tag 0x01000000, arcs record tag 0x01a10000
// with merge-by-positional-sum, object-summary tag 0xa1000000 for
// gcov >= 9, and an 8-zero-byte EOF marker.  The wasmtime half is
// exercised hermetically with a synthetic WAT module that imports the
// six env::llvm_* functions and exports a funcref table, mirroring
// what an instrumented cel_runtime.wasm looks like.

#include "eval/internal/wasm_gcov.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// "B02*" — decodes to gcov_version 102 (>= 4.7 checksummed function
// records, >= 9 object-summary form), the modern shape clang 19 emits.
constexpr uint32_t kVersionB02 = 0x4230322A;
// "402*" — gcov_version 42, the pre-4.7 form (no cfg_checksum).
constexpr uint32_t kVersion402 = 0x3430322A;

std::string TestDir(absl::string_view name) {
  std::string dir = absl::StrCat(::testing::TempDir(), "/wasm_gcov_", name);
  std::filesystem::remove_all(dir);
  return dir;
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

uint32_t U32At(const std::vector<uint8_t>& b, size_t word) {
  uint32_t v = 0;
  std::memcpy(&v, b.data() + word * 4, 4);
  return v;
}

uint64_t U64At(const std::vector<uint8_t>& b, size_t byte_off) {
  uint64_t v = 0;
  std::memcpy(&v, b.data() + byte_off, 8);
  return v;
}

// ——— WasmGcovSink ———

TEST(WasmGcovSinkTest, DisabledSinkWritesNothing) {
  WasmGcovSink sink("");
  EXPECT_FALSE(sink.enabled());
  sink.StartFile("x.gcda", kVersionB02, 1);
  sink.EmitFunction(1, 2, 3);
  const uint64_t counters[] = {4};
  sink.EmitArcs(absl::MakeSpan(counters));
  sink.SummaryInfo();
  EXPECT_TRUE(sink.EndFile().ok());
}

TEST(WasmGcovSinkTest, FreshFileExactBytes) {
  const std::string dir = TestDir("fresh");
  WasmGcovSink sink(dir);
  EXPECT_TRUE(sink.enabled());
  sink.StartFile("bazel-out/foo/_objs/bar/cel_fake.gcda", kVersionB02, 0x1234);
  sink.EmitFunction(7, 0xAA, 0xBB);
  const uint64_t counters[] = {5, 9};
  sink.EmitArcs(absl::MakeSpan(counters));
  sink.SummaryInfo();
  ASSERT_TRUE(sink.EndFile().ok());

  const auto bytes = ReadFileBytes(absl::StrCat(dir, "/cel_fake.gcda"));
  ASSERT_EQ(bytes.size(), 80u);
  EXPECT_EQ(U32At(bytes, 0), 0x67636461u);  // "gcda" magic
  EXPECT_EQ(U32At(bytes, 1), kVersionB02);
  EXPECT_EQ(U32At(bytes, 2), 0x1234u);
  EXPECT_EQ(U32At(bytes, 3), 0x01000000u);  // function tag
  EXPECT_EQ(U32At(bytes, 4), 3u);           // len (with cfg_checksum)
  EXPECT_EQ(U32At(bytes, 5), 7u);
  EXPECT_EQ(U32At(bytes, 6), 0xAAu);
  EXPECT_EQ(U32At(bytes, 7), 0xBBu);
  EXPECT_EQ(U32At(bytes, 8), 0x01a10000u);  // arcs tag
  EXPECT_EQ(U32At(bytes, 9), 4u);           // 2 counters * 2 words
  EXPECT_EQ(U64At(bytes, 40), 5u);
  EXPECT_EQ(U64At(bytes, 48), 9u);
  EXPECT_EQ(U32At(bytes, 14), 0xa1000000u);  // object summary tag
  EXPECT_EQ(U32At(bytes, 15), 2u);
  EXPECT_EQ(U32At(bytes, 16), 1u);  // runs
  EXPECT_EQ(U32At(bytes, 17), 0u);  // sum_max
  EXPECT_EQ(U64At(bytes, 72), 0u);  // 8-byte EOF marker
}

TEST(WasmGcovSinkTest, OldGcovVersionOmitsCfgChecksum) {
  const std::string dir = TestDir("oldver");
  WasmGcovSink sink(dir);
  sink.StartFile("f.gcda", kVersion402, 1);
  sink.EmitFunction(7, 0xAA, 0xBB);
  ASSERT_TRUE(sink.EndFile().ok());

  const auto bytes = ReadFileBytes(absl::StrCat(dir, "/f.gcda"));
  EXPECT_EQ(U32At(bytes, 3), 0x01000000u);
  EXPECT_EQ(U32At(bytes, 4), 2u);  // len WITHOUT cfg_checksum
  EXPECT_EQ(U32At(bytes, 5), 7u);
  EXPECT_EQ(U32At(bytes, 6), 0xAAu);
}

void WriteSession(const std::string& dir, uint64_t c0, uint64_t c1) {
  WasmGcovSink sink(dir);
  sink.StartFile("m.gcda", kVersionB02, 99);
  sink.EmitFunction(1, 2, 3);
  const uint64_t counters[] = {c0, c1};
  sink.EmitArcs(absl::MakeSpan(counters));
  sink.SummaryInfo();
  ASSERT_TRUE(sink.EndFile().ok());
}

TEST(WasmGcovSinkTest, MergeSumsCountersAndRuns) {
  const std::string dir = TestDir("merge");
  WriteSession(dir, 5, 9);
  WriteSession(dir, 10, 1);

  const auto bytes = ReadFileBytes(absl::StrCat(dir, "/m.gcda"));
  ASSERT_EQ(bytes.size(), 80u);
  EXPECT_EQ(U64At(bytes, 40), 15u);
  EXPECT_EQ(U64At(bytes, 48), 10u);
  EXPECT_EQ(U32At(bytes, 16), 2u);  // runs merged 1 -> 2
}

TEST(WasmGcovSinkTest, MergeShapeMismatchKeepsFreshCounters) {
  const std::string dir = TestDir("mismatch");
  WriteSession(dir, 5, 9);  // 2 counters on disk

  WasmGcovSink sink(dir);
  sink.StartFile("m.gcda", kVersionB02, 99);
  sink.EmitFunction(1, 2, 3);
  const uint64_t counters[] = {1, 2, 3};  // 3 counters now
  sink.EmitArcs(absl::MakeSpan(counters));
  ASSERT_TRUE(sink.EndFile().ok());

  const auto bytes = ReadFileBytes(absl::StrCat(dir, "/m.gcda"));
  EXPECT_EQ(U32At(bytes, 9), 6u);  // 3 counters, no old sum applied
  EXPECT_EQ(U64At(bytes, 40), 1u);
  EXPECT_EQ(U64At(bytes, 48), 2u);
  EXPECT_EQ(U64At(bytes, 56), 3u);
}

TEST(WasmGcovSinkTest, CreatesOutputDirAndFlattensPathToBasename) {
  const std::string dir = TestDir("mkdir");  // does not exist yet
  WasmGcovSink sink(dir);
  sink.StartFile("deep/nested/path/leaf.gcda", kVersionB02, 1);
  ASSERT_TRUE(sink.EndFile().ok());
  EXPECT_TRUE(std::filesystem::exists(absl::StrCat(dir, "/leaf.gcda")));
}

TEST(WasmGcovSinkTest, EndFileWithoutStartIsNoop) {
  WasmGcovSink sink(TestDir("nostart"));
  EXPECT_TRUE(sink.EndFile().ok());
}

TEST(WasmGcovSinkTest, TwoFilesInOneSession) {
  const std::string dir = TestDir("twofiles");
  WasmGcovSink sink(dir);
  sink.StartFile("a.gcda", kVersionB02, 1);
  ASSERT_TRUE(sink.EndFile().ok());
  sink.StartFile("b.gcda", kVersionB02, 2);
  ASSERT_TRUE(sink.EndFile().ok());
  EXPECT_TRUE(std::filesystem::exists(absl::StrCat(dir, "/a.gcda")));
  EXPECT_TRUE(std::filesystem::exists(absl::StrCat(dir, "/b.gcda")));
}

// ——— wasmtime glue ———

// Instrumented-module lookalike: imports the six env::llvm_* functions,
// exports a funcref table (as `--export-table` produces), and exposes
// `run_ctors` standing in for the static-ctor call to llvm_gcov_init.
// The writeout function reads the filename at guest offset 0 and two
// u64 counters at offset 64 — the test seeds those via env.mem_base.
constexpr char kInstrumentedWat[] = R"WAT(
(module
  (import "env" "llvm_gcda_start_file" (func $sf (param i32 i32 i32)))
  (import "env" "llvm_gcda_emit_function" (func $ef (param i32 i32 i32)))
  (import "env" "llvm_gcda_emit_arcs" (func $ea (param i32 i32)))
  (import "env" "llvm_gcda_summary_info" (func $si))
  (import "env" "llvm_gcda_end_file" (func $end))
  (import "env" "llvm_gcov_init" (func $init (param i32 i32)))
  (table (export "__indirect_function_table") 4 funcref)
  (elem (i32.const 1) $writeout $reset)
  (func $writeout
    i32.const 0 i32.const 1110716970 i32.const 42 call $sf
    i32.const 7 i32.const 170 i32.const 187 call $ef
    i32.const 2 i32.const 64 call $ea
    call $si
    call $end)
  (func $reset)
  (func (export "run_ctors") i32.const 1 i32.const 2 call $init))
)WAT";

struct WasmtimeHarness {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* module = nullptr;
  wasmtime_instance_t instance{};

  ~WasmtimeHarness() {
    if (module != nullptr) wasmtime_module_delete(module);
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }

  // Builds engine/store/linker, registers the gcov imports for `env`,
  // compiles `wat`, and instantiates it.
  void InitOrDie(absl::string_view wat, WasmGcovEnv* env) {
    engine = wasm_engine_new();
    ASSERT_NE(engine, nullptr);
    store = wasmtime_store_new(engine, nullptr, nullptr);
    ASSERT_NE(store, nullptr);
    linker = wasmtime_linker_new(engine);
    ASSERT_NE(linker, nullptr);
    ASSERT_TRUE(RegisterWasmGcovImports(linker, env).ok());

    wasm_byte_vec_t wasm;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &wasm);
    ASSERT_EQ(err, nullptr);
    err = wasmtime_module_new(engine, reinterpret_cast<uint8_t*>(wasm.data),
                              wasm.size, &module);
    wasm_byte_vec_delete(&wasm);
    ASSERT_EQ(err, nullptr);

    wasm_trap_t* trap = nullptr;
    err = wasmtime_linker_instantiate(linker, Ctx(), module, &instance, &trap);
    ASSERT_EQ(err, nullptr);
    ASSERT_EQ(trap, nullptr);
  }

  wasmtime_context_t* Ctx() {
    return wasmtime_store_context(store);
  }

  void CallExportOrDie(const char* name) {
    wasmtime_extern_t ext;
    ASSERT_TRUE(wasmtime_instance_export_get(Ctx(), &instance, name,
                                             std::strlen(name), &ext));
    ASSERT_EQ(ext.kind, WASMTIME_EXTERN_FUNC);
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(Ctx(), &ext.of.func, nullptr, 0, nullptr, 0, &trap);
    ASSERT_EQ(err, nullptr);
    ASSERT_EQ(trap, nullptr);
  }
};

TEST(WasmGcovGlueTest, NonInstrumentedModuleInstantiatesWithImportsDefined) {
  WasmGcovEnv env(std::string(""));
  WasmtimeHarness h;
  h.InitOrDie("(module (func (export \"f\")))", &env);
  EXPECT_THAT(env.writeout_fns, IsEmpty());
}

TEST(WasmGcovGlueTest, GcovInitRegistersWriteoutIndex) {
  WasmGcovEnv env(TestDir("glue_init"));
  WasmtimeHarness h;
  h.InitOrDie(kInstrumentedWat, &env);
  h.CallExportOrDie("run_ctors");
  EXPECT_THAT(env.writeout_fns, ElementsAre(1u));
}

TEST(WasmGcovGlueTest, DumpInvokesWriteoutAndWritesGcda) {
  const std::string dir = TestDir("glue_dump");
  WasmGcovEnv env(dir);
  WasmtimeHarness h;
  h.InitOrDie(kInstrumentedWat, &env);
  h.CallExportOrDie("run_ctors");

  // Seed the fake guest memory: filename at 0, two counters at 64.
  std::vector<uint8_t> guest_mem(128, 0);
  const char kName[] = "cel_glue.gcda";
  std::memcpy(guest_mem.data(), kName, sizeof(kName));
  const uint64_t c0 = 3, c1 = 4;
  std::memcpy(guest_mem.data() + 64, &c0, 8);
  std::memcpy(guest_mem.data() + 72, &c1, 8);
  env.mem_base = guest_mem.data();
  env.mem_size = guest_mem.size();

  ASSERT_TRUE(DumpWasmGcov(h.Ctx(), h.instance, &env).ok());

  const auto bytes = ReadFileBytes(absl::StrCat(dir, "/cel_glue.gcda"));
  ASSERT_EQ(bytes.size(), 80u);
  EXPECT_EQ(U32At(bytes, 0), 0x67636461u);
  EXPECT_EQ(U32At(bytes, 1), 1110716970u);  // the WAT's version const
  EXPECT_EQ(U32At(bytes, 5), 7u);           // function ident
  EXPECT_EQ(U64At(bytes, 40), 3u);
  EXPECT_EQ(U64At(bytes, 48), 4u);
}

TEST(WasmGcovGlueTest, DumpWithNoRegistrationsIsNoop) {
  WasmGcovEnv env(TestDir("glue_noreg"));
  WasmtimeHarness h;
  h.InitOrDie("(module (func (export \"f\")))", &env);
  EXPECT_TRUE(DumpWasmGcov(h.Ctx(), h.instance, &env).ok());
}

TEST(WasmGcovGlueTest, DumpDisabledSinkIsNoopEvenWithRegistrations) {
  WasmGcovEnv env(std::string(""));
  WasmtimeHarness h;
  h.InitOrDie(kInstrumentedWat, &env);
  h.CallExportOrDie("run_ctors");
  ASSERT_THAT(env.writeout_fns, ElementsAre(1u));
  EXPECT_TRUE(DumpWasmGcov(h.Ctx(), h.instance, &env).ok());
}

TEST(WasmGcovGlueTest, DumpWithoutExportedTableErrors) {
  WasmGcovEnv env(TestDir("glue_notable"));
  WasmtimeHarness h;
  h.InitOrDie("(module (func (export \"f\")))", &env);
  env.writeout_fns.push_back(1);  // simulate a registration
  const absl::Status s = DumpWasmGcov(h.Ctx(), h.instance, &env);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(WasmGcovEnvTest, OutputDirFromEnvReadsVariable) {
  ASSERT_EQ(::setenv("CELWASM_WASM_GCOV_DIR", "/tmp/x", 1), 0);
  EXPECT_EQ(WasmGcovEnv::OutputDirFromEnv(), "/tmp/x");
  ASSERT_EQ(::unsetenv("CELWASM_WASM_GCOV_DIR"), 0);
  EXPECT_EQ(WasmGcovEnv::OutputDirFromEnv(), "");
}

// The default ctor is the env-fallback path (used when the engine was
// built without an explicit CollectWasmCoverage dir).
TEST(WasmGcovEnvTest, DefaultCtorEnablesSinkFromEnv) {
  ASSERT_EQ(::setenv("CELWASM_WASM_GCOV_DIR", "/tmp/x", 1), 0);
  EXPECT_TRUE(WasmGcovEnv().sink.enabled());
  ASSERT_EQ(::unsetenv("CELWASM_WASM_GCOV_DIR"), 0);
  EXPECT_FALSE(WasmGcovEnv().sink.enabled());
}

}  // namespace
}  // namespace celwasm
