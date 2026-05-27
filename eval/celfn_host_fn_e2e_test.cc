// End-to-end coverage for the `@host.` custom-function ("native
// function") backend.  Drives the WHOLE flow through the real
// wasmtime runtime, for every argument / return scalar kind, in both
// receiver (`this`) and global call forms:
//
//   parse `.celfn` IDL  →  declare host fns on the Compiler  →
//   Compile a CEL expression that CALLS them  →  Engine::Plan with
//   real HostCallback impls  →  Instance::Eval  →  decode + assert.
//
// This complements the focused acceptance test in instance_test.cc
// (`InstanceCustomFnEvalTest`, `is_number(string)→bool`).  Here we
// fill the type matrix, composition, multi-decl libraries, and the
// negative / boundary edges.
//
// ABI being exercised (frozen; runtime/cel_data.h + m13-custom-fns.md
// §"ABI"): each `@host.<name>` lowers to a `cel_fn.<overload_id>`
// wasm import with signature `(out_slot, arg0_slot, …) → void`.  Every
// slot is the byte offset of a 24-byte CelValue { kind:u32@0, pad@4,
// payload@8 }.  The HostCallback reads its args from those offsets and
// writes its result to `out_slot`.
//
// Known ABI limitation (m13-custom-fns.md §"Host complex-type gap",
// row "Host — string/bytes return (newly constructed)"): the raw
// HostCallback receives only `(mem, mem_size, out_slot, arg_slots)` —
// it has NO `arena_alloc`, so it can only return a string/bytes span
// pointing at bytes that already live in linear memory.  The
// string-return tests below therefore transform in place (the
// uppercase transform is length-preserving) and point the result span
// at the same bytes — this is the supported shape today.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

// ─── 24-byte CelValue wire helpers ────────────────────────────────
//
// Mirror runtime/cel_data.h's frozen layout without dragging the C
// header in.  kind is u32@0; the scalar payload begins at byte 8.
// Strings/bytes store a CelSpan { ptr:u32@8, len:u32@12 }.  All
// little-endian (the runtime asserts a LE host).

// CelKind values (runtime/cel_data.h) used here.
constexpr uint32_t kCelBool = 1;
constexpr uint32_t kCelInt = 2;
constexpr uint32_t kCelUint = 3;
constexpr uint32_t kCelDouble = 4;
constexpr uint32_t kCelString = 5;
constexpr uint32_t kCelBytes = 6;
constexpr uint32_t kCelError = 16;

constexpr uint32_t kKindOffset = 0;
constexpr uint32_t kPayloadOffset = 8;
constexpr uint32_t kCelValueSize = 24;

uint32_t ReadKind(const uint8_t* mem, uint32_t slot) {
  uint32_t k = 0;
  std::memcpy(&k, mem + slot + kKindOffset, sizeof(k));
  return k;
}

template <typename T>
T ReadPayload(const uint8_t* mem, uint32_t slot) {
  T v{};
  std::memcpy(&v, mem + slot + kPayloadOffset, sizeof(T));
  return v;
}

void WriteKind(uint8_t* mem, uint32_t slot, uint32_t kind) {
  std::memcpy(mem + slot + kKindOffset, &kind, sizeof(kind));
}

template <typename T>
void WritePayload(uint8_t* mem, uint32_t slot, T v) {
  std::memcpy(mem + slot + kPayloadOffset, &v, sizeof(T));
}

// A CelSpan is two u32s at payload offset: ptr@8, len@12.
struct WireSpan {
  uint32_t ptr;
  uint32_t len;
};
WireSpan ReadSpan(const uint8_t* mem, uint32_t slot) {
  WireSpan s{};
  std::memcpy(&s.ptr, mem + slot + kPayloadOffset, sizeof(s.ptr));
  std::memcpy(&s.len, mem + slot + kPayloadOffset + 4, sizeof(s.len));
  return s;
}
void WriteSpan(uint8_t* mem, uint32_t slot, WireSpan s) {
  std::memcpy(mem + slot + kPayloadOffset, &s.ptr, sizeof(s.ptr));
  std::memcpy(mem + slot + kPayloadOffset + 4, &s.len, sizeof(s.len));
}

bool SlotInBounds(uint32_t slot, size_t mem_size) {
  return static_cast<size_t>(slot) + kCelValueSize <= mem_size;
}

// ─── Test fixture plumbing ────────────────────────────────────────

// Build a Compiler from one or more `.celfn` declarations plus any
// variable declarations the expression needs.
struct CompilerSpec {
  std::vector<std::string> celfn_decls;
  std::vector<std::pair<std::string, CelType>> variables;
};

absl::StatusOr<Compiler> MakeCompiler(const CompilerSpec& spec) {
  auto b = Compiler::NewBuilder();
  for (const auto& [name, type] : spec.variables) {
    b.DeclareVariable(name, type);
  }
  for (const auto& decl : spec.celfn_decls) {
    b.AddFunction(decl);
  }
  return std::move(b).Build();
}

// Compile + Plan + Eval in one shot, registering `funcs` on the
// engine.  Returns the decoded result Value.
struct RegFn {
  std::string overload_id;
  uint8_t num_args;
  HostCallback impl;
};

absl::StatusOr<Value> RunExpr(const CompilerSpec& spec,
                              absl::string_view source,
                              std::vector<RegFn> funcs, const Activation& act) {
  auto compiler_or = MakeCompiler(spec);
  if (!compiler_or.ok()) return compiler_or.status();
  auto prog_or = compiler_or->Compile(source);
  if (!prog_or.ok()) return prog_or.status();

  auto engine_or = Engine::NewBuilder().Build();
  if (!engine_or.ok()) return engine_or.status();
  for (auto& f : funcs) {
    auto s =
        engine_or->AddFunction(f.overload_id, f.num_args, std::move(f.impl));
    if (!s.ok()) return s;
  }
  auto inst_or = engine_or->Plan(*prog_or);
  if (!inst_or.ok()) return inst_or.status();
  return inst_or->Eval(act);
}

// ════════════════════════════════════════════════════════════════
// Argument + return scalar-type matrix
// ════════════════════════════════════════════════════════════════

// int → int: double_it(x) returns 2*x.  Asserts the callback decoded
// the int arg and the eval returns the transformed int.
TEST(CelfnHostFnE2ETest, IntArgIntReturnTransforms) {
  CompilerSpec spec{{"int @host.double_it(int x);"}, {{"x", CelType::Int()}}};
  RegFn fn{"double_it_int", 2,
           [](uint8_t* mem, size_t mem_size, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (args.size() != 1 || !SlotInBounds(args[0], mem_size) ||
                 !SlotInBounds(out, mem_size)) {
               return absl::InvalidArgumentError("double_it: bad slots");
             }
             if (ReadKind(mem, args[0]) != kCelInt) {
               return absl::InvalidArgumentError("double_it: arg not int");
             }
             auto v = ReadPayload<int64_t>(mem, args[0]);
             WriteKind(mem, out, kCelInt);
             WritePayload<int64_t>(mem, out, v * 2);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("x", Value::Int(21));
  auto v = RunExpr(spec, "double_it(x)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 42);
}

// uint → uint: inc(x) returns x+1.
TEST(CelfnHostFnE2ETest, UintArgUintReturnTransforms) {
  CompilerSpec spec{{"uint @host.inc(uint x);"}, {{"x", CelType::Uint()}}};
  RegFn fn{"inc_uint", 2,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelUint) {
               return absl::InvalidArgumentError("inc: arg not uint");
             }
             auto v = ReadPayload<uint64_t>(mem, args[0]);
             WriteKind(mem, out, kCelUint);
             WritePayload<uint64_t>(mem, out, v + 1);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("x", Value::Uint(99u));
  auto v = RunExpr(spec, "inc(x)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kUint);
  EXPECT_EQ(*v->AsUint(), 100u);
}

// double → double: half(x) returns x/2.
TEST(CelfnHostFnE2ETest, DoubleArgDoubleReturnTransforms) {
  CompilerSpec spec{{"double @host.half(double x);"},
                    {{"x", CelType::Double()}}};
  RegFn fn{"half_double", 2,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelDouble) {
               return absl::InvalidArgumentError("half: arg not double");
             }
             auto v = ReadPayload<double>(mem, args[0]);
             WriteKind(mem, out, kCelDouble);
             WritePayload<double>(mem, out, v / 2.0);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("x", Value::Double(7.0));
  auto v = RunExpr(spec, "half(x)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v->AsDouble(), 3.5);
}

// bool → bool: negate(b) returns !b.
TEST(CelfnHostFnE2ETest, BoolArgBoolReturnTransforms) {
  CompilerSpec spec{{"bool @host.negate(bool b);"}, {{"b", CelType::Bool()}}};
  RegFn fn{"negate_bool", 2,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelBool) {
               return absl::InvalidArgumentError("negate: arg not bool");
             }
             auto v = ReadPayload<int32_t>(mem, args[0]);
             WriteKind(mem, out, kCelBool);
             WritePayload<int32_t>(mem, out, v ? 0 : 1);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("b", Value::Bool(true));
  auto v = RunExpr(spec, "negate(b)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v->AsBool());
}

// string → int: length(s) returns the byte length.  Arg is a string,
// return is an int — exercises decoding a CelSpan arg.
TEST(CelfnHostFnE2ETest, StringArgIntReturnComputesLength) {
  CompilerSpec spec{{"int @host.length(string s);"},
                    {{"s", CelType::String()}}};
  RegFn fn{"length_string", 2,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelString) {
               return absl::InvalidArgumentError("length: arg not string");
             }
             WireSpan sp = ReadSpan(mem, args[0]);
             WriteKind(mem, out, kCelInt);
             WritePayload<int64_t>(mem, out, static_cast<int64_t>(sp.len));
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("s", Value::String("hello"));
  auto v = RunExpr(spec, "length(s)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 5);
}

// string → string: upper(s) uppercases IN PLACE (length-preserving),
// then returns a span at the same bytes.  This is the only string-
// RETURN shape the current ABI supports (callback has no arena_alloc —
// see file header + m13-custom-fns.md §"Host complex-type gap").
TEST(CelfnHostFnE2ETest, StringArgStringReturnUppercasesInPlace) {
  CompilerSpec spec{{"string @host.upper(this string s);"},
                    {{"s", CelType::String()}}};
  RegFn fn{"upper_string", 2,
           [](uint8_t* mem, size_t mem_size, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelString) {
               return absl::InvalidArgumentError("upper: arg not string");
             }
             WireSpan sp = ReadSpan(mem, args[0]);
             if (static_cast<size_t>(sp.ptr) + sp.len > mem_size) {
               return absl::OutOfRangeError("upper: span oob");
             }
             for (uint32_t i = 0; i < sp.len; ++i) {
               uint8_t c = mem[sp.ptr + i];
               if (c >= 'a' && c <= 'z') mem[sp.ptr + i] = c - 32;
             }
             WriteKind(mem, out, kCelString);
             WriteSpan(mem, out, sp);  // point at the now-uppercased bytes
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("s", Value::String("Hello"));
  auto v = RunExpr(spec, "s.upper()", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString);
  EXPECT_EQ(*v->AsString(), "HELLO");
}

// bytes → int: byte_sum(b) returns the sum of the byte values.
// Exercises a CEL_BYTES arg (same CelSpan shape as string, distinct
// kind tag).
TEST(CelfnHostFnE2ETest, BytesArgIntReturnSumsBytes) {
  CompilerSpec spec{{"int @host.byte_sum(bytes b);"},
                    {{"b", CelType::Bytes()}}};
  RegFn fn{"byte_sum_bytes", 2,
           [](uint8_t* mem, size_t mem_size, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (ReadKind(mem, args[0]) != kCelBytes) {
               return absl::InvalidArgumentError("byte_sum: arg not bytes");
             }
             WireSpan sp = ReadSpan(mem, args[0]);
             if (static_cast<size_t>(sp.ptr) + sp.len > mem_size) {
               return absl::OutOfRangeError("byte_sum: span oob");
             }
             int64_t sum = 0;
             for (uint32_t i = 0; i < sp.len; ++i) {
               sum += mem[sp.ptr + i];
             }
             WriteKind(mem, out, kCelInt);
             WritePayload<int64_t>(mem, out, sum);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("b", Value::Bytes(std::string("\x01\x02\x03", 3)));
  auto v = RunExpr(spec, "byte_sum(b)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 6);
}

// ════════════════════════════════════════════════════════════════
// Receiver (`this`) vs global call forms
// ════════════════════════════════════════════════════════════════

// Global form: add(a, b) — two positional int args, called as
// `add(a, b)`.  Asserts both args decode and arg order is preserved.
TEST(CelfnHostFnE2ETest, GlobalTwoArgFnAddsInOrder) {
  CompilerSpec spec{{"int @host.add(int a, int b);"},
                    {{"a", CelType::Int()}, {"b", CelType::Int()}}};
  RegFn fn{"add_int_int", 3,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (args.size() != 2) {
               return absl::InvalidArgumentError("add: want 2 args");
             }
             auto a = ReadPayload<int64_t>(mem, args[0]);
             auto b = ReadPayload<int64_t>(mem, args[1]);
             WriteKind(mem, out, kCelInt);
             // Non-commutative combine so arg ORDER is asserted, not
             // just the sum: a - b.
             WritePayload<int64_t>(mem, out, a - b);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("a", Value::Int(10));
  act.Bind("b", Value::Int(3));
  auto v = RunExpr(spec, "add(a, b)", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 7);  // 10 - 3, NOT 3 - 10
}

// Receiver form: s.is_number() — the `this` receiver becomes arg 0.
TEST(CelfnHostFnE2ETest, ReceiverFnReceivesThisAsFirstArg) {
  CompilerSpec spec{{"bool @host.is_number(this string s);"},
                    {{"s", CelType::String()}}};
  RegFn fn{"is_number_string", 2,
           [](uint8_t* mem, size_t /*mem_size*/, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             if (args.size() != 1 || ReadKind(mem, args[0]) != kCelString) {
               return absl::InvalidArgumentError("is_number: bad arg");
             }
             WireSpan sp = ReadSpan(mem, args[0]);
             bool all_digits = sp.len > 0;
             for (uint32_t i = 0; i < sp.len; ++i) {
               uint8_t c = mem[sp.ptr + i];
               if (c < '0' || c > '9') {
                 all_digits = false;
                 break;
               }
             }
             WriteKind(mem, out, kCelBool);
             WritePayload<int32_t>(mem, out, all_digits ? 1 : 0);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("s", Value::String("12345"));
  auto v = RunExpr(spec, "s.is_number()", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v->AsBool());
}

// ════════════════════════════════════════════════════════════════
// Multiple functions in one library + ParseCelfnSource (multi-decl)
// ════════════════════════════════════════════════════════════════

// A single `.celfn` source string declaring two host fns, parsed via
// ParseCelfnSource and attached with AddLibrary, both called in one
// expression.
TEST(CelfnHostFnE2ETest, MultiDeclLibraryViaParseCelfnSourceBothFire) {
  constexpr absl::string_view kSrc =
      "int @host.double_it(int x);\n"
      "int @host.triple_it(int x);\n";
  auto lib_or = ParseCelfnSource(kSrc);
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  ASSERT_EQ(lib_or->decls().size(), 2u);

  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddLibrary(*std::move(lib_or));
  auto compiler_or = std::move(b).Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  // double_it(x) + triple_it(x) == 5*x.
  auto prog_or = compiler_or->Compile("double_it(x) + triple_it(x)");
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();

  auto mul = [](int64_t k) {
    return [k](uint8_t* mem, size_t, uint32_t out,
               absl::Span<const uint32_t> args) -> absl::Status {
      auto v = ReadPayload<int64_t>(mem, args[0]);
      WriteKind(mem, out, kCelInt);
      WritePayload<int64_t>(mem, out, v * k);
      return absl::OkStatus();
    };
  };
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  ASSERT_TRUE(engine_or->AddFunction("double_it_int", 2, mul(2)).ok());
  ASSERT_TRUE(engine_or->AddFunction("triple_it_int", 2, mul(3)).ok());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();

  Activation act;
  act.Bind("x", Value::Int(4));
  auto v = inst_or->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 20);  // 8 + 12
}

// ════════════════════════════════════════════════════════════════
// Composition
// ════════════════════════════════════════════════════════════════

// A host fn's result feeds a CEL operator: double_it(x) > 40.
TEST(CelfnHostFnE2ETest, HostFnResultFeedsComparisonOperator) {
  CompilerSpec spec{{"int @host.double_it(int x);"}, {{"x", CelType::Int()}}};
  RegFn fn{"double_it_int", 2,
           [](uint8_t* mem, size_t, uint32_t out,
              absl::Span<const uint32_t> args) -> absl::Status {
             auto v = ReadPayload<int64_t>(mem, args[0]);
             WriteKind(mem, out, kCelInt);
             WritePayload<int64_t>(mem, out, v * 2);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("x", Value::Int(21));
  auto v = RunExpr(spec, "double_it(x) > 40", {std::move(fn)}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v->AsBool());  // 42 > 40
}

// A host fn's result feeds another host fn: inc(double_it(x)).
TEST(CelfnHostFnE2ETest, HostFnResultFeedsAnotherHostFn) {
  CompilerSpec spec{{"int @host.double_it(int x);", "int @host.inc(int x);"},
                    {{"x", CelType::Int()}}};
  std::vector<RegFn> funcs;
  funcs.push_back({"double_it_int", 2,
                   [](uint8_t* mem, size_t, uint32_t out,
                      absl::Span<const uint32_t> args) -> absl::Status {
                     auto v = ReadPayload<int64_t>(mem, args[0]);
                     WriteKind(mem, out, kCelInt);
                     WritePayload<int64_t>(mem, out, v * 2);
                     return absl::OkStatus();
                   }});
  funcs.push_back({"inc_int", 2,
                   [](uint8_t* mem, size_t, uint32_t out,
                      absl::Span<const uint32_t> args) -> absl::Status {
                     auto v = ReadPayload<int64_t>(mem, args[0]);
                     WriteKind(mem, out, kCelInt);
                     WritePayload<int64_t>(mem, out, v + 1);
                     return absl::OkStatus();
                   }});
  Activation act;
  act.Bind("x", Value::Int(20));
  auto v = RunExpr(spec, "inc(double_it(x))", std::move(funcs), act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 41);  // (20*2)+1
}

// ════════════════════════════════════════════════════════════════
// Negative / error-propagation paths
// ════════════════════════════════════════════════════════════════

// Malformed IDL → the deferred parse error surfaces at Compiler::Build.
TEST(CelfnHostFnE2ETest, MalformedIdlFailsAtCompilerBuild) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("string @host.upper(this string s)");  // missing `;`
  auto compiler_or = std::move(b).Build();
  EXPECT_FALSE(compiler_or.ok());
  EXPECT_EQ(compiler_or.status().code(), absl::StatusCode::kInvalidArgument);
}

// ParseCelfnSource on garbage → InvalidArgument, no library.
TEST(CelfnHostFnE2ETest, ParseCelfnSourceRejectsGarbage) {
  auto lib_or = ParseCelfnSource("this is not celfn");
  EXPECT_FALSE(lib_or.ok());
  EXPECT_EQ(lib_or.status().code(), absl::StatusCode::kInvalidArgument);
}

// A declared host fn with NO registered callback → Plan fails cleanly
// (the `cel_fn.<overload_id>` import can't be resolved by the linker),
// NOT a crash.
TEST(CelfnHostFnE2ETest, MissingCallbackFailsAtPlanNoCrash) {
  CompilerSpec spec{{"int @host.double_it(int x);"}, {{"x", CelType::Int()}}};
  auto compiler_or = MakeCompiler(spec);
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  auto prog_or = compiler_or->Compile("double_it(x)");
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok());
  // Deliberately register NOTHING.
  auto inst_or = engine_or->Plan(*prog_or);
  EXPECT_FALSE(inst_or.ok())
      << "Plan must reject a program importing an unbound cel_fn.*";
}

// A callback that returns a CEL error value → the error kind decodes
// through to the result Value (Instance maps CEL_ERROR → Value::Error).
TEST(CelfnHostFnE2ETest, CallbackReturningCelErrorPropagates) {
  CompilerSpec spec{{"int @host.always_err(int x);"}, {{"x", CelType::Int()}}};
  RegFn fn{"always_err_int", 2,
           [](uint8_t* mem, size_t, uint32_t out,
              absl::Span<const uint32_t> /*args*/) -> absl::Status {
             // Write a CEL_ERROR CelValue (payload carries an error
             // code; the kind tag is what the decoder dispatches on).
             WriteKind(mem, out, kCelError);
             WritePayload<int32_t>(mem, out, /*CEL_ERR_DIVIDE_BY_ZERO*/ 11);
             return absl::OkStatus();
           }};
  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = RunExpr(spec, "always_err(x)", {std::move(fn)}, act);
  // Either the result decodes as a Value::Error, or Eval surfaces a
  // non-OK status carrying the error — both are valid "the error
  // propagated, did not crash, did not return a bogus int" outcomes.
  if (v.ok()) {
    EXPECT_EQ(v->kind(), Value::Kind::kError) << "expected an error Value";
  } else {
    SUCCEED() << "error surfaced as status: " << v.status();
  }
}

// A callback that returns a non-OK absl::Status → traps the wasm and
// surfaces as a non-OK Eval status (not a crash, not a wrong value).
TEST(CelfnHostFnE2ETest, CallbackReturningNonOkStatusTraps) {
  CompilerSpec spec{{"int @host.boom(int x);"}, {{"x", CelType::Int()}}};
  RegFn fn{"boom_int", 2,
           [](uint8_t*, size_t, uint32_t,
              absl::Span<const uint32_t>) -> absl::Status {
             return absl::InternalError("callback exploded");
           }};
  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = RunExpr(spec, "boom(x)", {std::move(fn)}, act);
  EXPECT_FALSE(v.ok()) << "a trapping callback must surface a non-OK status";
}

// ════════════════════════════════════════════════════════════════
// Boundary values — round-trip through the callback
// ════════════════════════════════════════════════════════════════

// echo_int(x) returns x verbatim — asserts boundary int64 values
// round-trip through the wire encoding undamaged.
absl::Status EchoIntCallback(uint8_t* mem, size_t, uint32_t out,
                             absl::Span<const uint32_t> args) {
  if (ReadKind(mem, args[0]) != kCelInt) {
    return absl::InvalidArgumentError("echo_int: not int");
  }
  auto v = ReadPayload<int64_t>(mem, args[0]);
  WriteKind(mem, out, kCelInt);
  WritePayload<int64_t>(mem, out, v);
  return absl::OkStatus();
}

class IntBoundaryTest : public testing::TestWithParam<int64_t> {};

TEST_P(IntBoundaryTest, RoundTripsThroughCallback) {
  CompilerSpec spec{{"int @host.echo_int(int x);"}, {{"x", CelType::Int()}}};
  Activation act;
  act.Bind("x", Value::Int(GetParam()));
  auto v =
      RunExpr(spec, "echo_int(x)", {{"echo_int_int", 2, EchoIntCallback}}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, IntBoundaryTest,
                         testing::Values(int64_t{0}, int64_t{-1}, int64_t{1},
                                         INT64_MIN, INT64_MAX));

// uint64 boundaries (0, 1, UINT64_MAX).
absl::Status EchoUintCallback(uint8_t* mem, size_t, uint32_t out,
                              absl::Span<const uint32_t> args) {
  if (ReadKind(mem, args[0]) != kCelUint) {
    return absl::InvalidArgumentError("echo_uint: not uint");
  }
  auto v = ReadPayload<uint64_t>(mem, args[0]);
  WriteKind(mem, out, kCelUint);
  WritePayload<uint64_t>(mem, out, v);
  return absl::OkStatus();
}

class UintBoundaryTest : public testing::TestWithParam<uint64_t> {};

TEST_P(UintBoundaryTest, RoundTripsThroughCallback) {
  CompilerSpec spec{{"uint @host.echo_uint(uint x);"},
                    {{"x", CelType::Uint()}}};
  Activation act;
  act.Bind("x", Value::Uint(GetParam()));
  auto v = RunExpr(spec, "echo_uint(x)",
                   {{"echo_uint_uint", 2, EchoUintCallback}}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kUint);
  EXPECT_EQ(*v->AsUint(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, UintBoundaryTest,
                         testing::Values(uint64_t{0}, uint64_t{1}, UINT64_MAX));

// Echo a string arg back (point the result span at the input bytes).
// Covers empty string, multi-byte UTF-8 — that the CelSpan {ptr,len}
// survives the round trip and the decoder reads the exact bytes.
absl::Status EchoStringCallback(uint8_t* mem, size_t mem_size, uint32_t out,
                                absl::Span<const uint32_t> args) {
  if (ReadKind(mem, args[0]) != kCelString) {
    return absl::InvalidArgumentError("echo_string: not string");
  }
  WireSpan sp = ReadSpan(mem, args[0]);
  if (static_cast<size_t>(sp.ptr) + sp.len > mem_size) {
    return absl::OutOfRangeError("echo_string: span oob");
  }
  WriteKind(mem, out, kCelString);
  WriteSpan(mem, out, sp);
  return absl::OkStatus();
}

class StringBoundaryTest : public testing::TestWithParam<std::string> {};

TEST_P(StringBoundaryTest, RoundTripsThroughCallback) {
  CompilerSpec spec{{"string @host.echo_string(string s);"},
                    {{"s", CelType::String()}}};
  Activation act;
  act.Bind("s", Value::String(GetParam()));
  auto v = RunExpr(spec, "echo_string(s)",
                   {{"echo_string_string", 2, EchoStringCallback}}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString);
  EXPECT_EQ(std::string(*v->AsString()), GetParam());
}

INSTANTIATE_TEST_SUITE_P(Boundaries, StringBoundaryTest,
                         testing::Values(std::string(""),       // empty
                                         std::string("a"),      // single
                                         std::string("héllo"),  // multi-byte
                                         std::string("日本語")));

// Echo a bytes arg, including an embedded NUL — proves the {ptr,len}
// span carries length explicitly (not NUL-terminated) so interior
// NULs survive.
absl::Status EchoBytesCallback(uint8_t* mem, size_t mem_size, uint32_t out,
                               absl::Span<const uint32_t> args) {
  if (ReadKind(mem, args[0]) != kCelBytes) {
    return absl::InvalidArgumentError("echo_bytes: not bytes");
  }
  WireSpan sp = ReadSpan(mem, args[0]);
  if (static_cast<size_t>(sp.ptr) + sp.len > mem_size) {
    return absl::OutOfRangeError("echo_bytes: span oob");
  }
  WriteKind(mem, out, kCelBytes);
  WriteSpan(mem, out, sp);
  return absl::OkStatus();
}

TEST(CelfnHostFnE2ETest, BytesArgWithEmbeddedNulRoundTrips) {
  CompilerSpec spec{{"bytes @host.echo_bytes(bytes b);"},
                    {{"b", CelType::Bytes()}}};
  // Embedded NULs: build via push_back so the bytes are unambiguous
  // (a string literal would clang-tidy-flag the length/NUL mismatch).
  std::string payload;
  payload.push_back('a');
  payload.push_back('\0');
  payload.push_back('b');
  payload.push_back('\0');
  payload.push_back('c');
  Activation act;
  act.Bind("b", Value::Bytes(payload));
  auto v = RunExpr(spec, "echo_bytes(b)",
                   {{"echo_bytes_bytes", 2, EchoBytesCallback}}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes);
  EXPECT_EQ(std::string(*v->AsBytes()), payload);
}

}  // namespace
}  // namespace celwasm
