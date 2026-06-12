// e2e: the caller-supplied descriptor pool is used across ALL eval
// codepaths, proven with a message that exists ONLY in that supplied
// pool — a `celwasm.test.Widget` hand-assembled into a
// `DescriptorPool` layered over the generated pool, never linked as a
// `cc_proto_library`.  The message instance is a `DynamicMessage`
// (built via `DynamicMessageFactory` + `TextFormat::ParseFromString`),
// not a generated C++ class, so every assertion here is load-bearing:
// a generated-pool-only eval would fail to resolve `Widget` at all.
//
// Three codepaths, each compiled with
// `Compiler::Builder::SetDescriptorPool(WidgetPool())` and evaluated
// with `Engine::Builder::SetDescriptorPool(WidgetPool())`:
//
//   (a) Bound variable — `w : celwasm.test.Widget`, `w.label`, bind
//       the dynamic Widget, expect "hello".  Exercises the field-read
//       trampolines (cel_host) against the supplied pool.
//   (b) Host function — `string @host.widget_label(...)`, the callback
//       reads `label` off the `const Message*` via reflection, expect
//       "hello".  Exercises message marshalling into a `@host` arg.
//   (c) Component function — a WAT component fixture takes the Widget
//       as `(list u8)` wire bytes and returns its first letter, expect
//       "h".  Exercises the `@component` proto Lift path.
//
// See `doc/implementation-plan/rewrite/m30-descriptor-pool.md`.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/host_call_context.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::celwasm::e2e::DefaultOpts;

// A `DescriptorPool` carrying `celwasm.test.Widget`, layered over the
// generated pool — the way a real caller builds one (schema resolves
// Widget; the generated underlay resolves the WKTs the checker/runtime
// need).  Widget lives ONLY in the schema layer, never the generated
// pool, so any compile/eval that resolves it proves `SetDescriptorPool`
// threaded the supplied pool through.  Function-local statics (process
// lifetime, no raw new).  Mirrors `WidgetPool()` in compiler_test.cc.
const google::protobuf::DescriptorPool* WidgetPool() {
  using google::protobuf::FieldDescriptorProto;
  static google::protobuf::SimpleDescriptorDatabase schema_db;
  static const bool kInitialized = [] {
    google::protobuf::FileDescriptorSet fds;
    google::protobuf::FileDescriptorProto* file = fds.add_file();
    file->set_name("widget.proto");
    file->set_package("celwasm.test");
    file->set_syntax("proto3");
    google::protobuf::DescriptorProto* widget = file->add_message_type();
    widget->set_name("Widget");
    FieldDescriptorProto* label = widget->add_field();
    label->set_name("label");
    label->set_number(1);
    label->set_type(FieldDescriptorProto::TYPE_STRING);
    label->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    FieldDescriptorProto* count = widget->add_field();
    count->set_name("count");
    count->set_number(2);
    count->set_type(FieldDescriptorProto::TYPE_INT64);
    count->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    for (const auto& f : fds.file()) {
      schema_db.Add(f);
    }
    return true;
  }();
  (void)kInitialized;
  static google::protobuf::DescriptorPoolDatabase generated_db(
      *google::protobuf::DescriptorPool::generated_pool());
  static google::protobuf::MergedDescriptorDatabase merged_db(&schema_db,
                                                              &generated_db);
  static google::protobuf::DescriptorPool pool(&merged_db);
  return &pool;
}

// A process-wide `DynamicMessageFactory` whose prototypes outlive every
// message it mints — the factory owns the prototype storage, so it must
// outlive the messages a builder returns.  Function-local static (process
// lifetime, no raw new).
google::protobuf::DynamicMessageFactory* WidgetFactory() {
  static google::protobuf::DynamicMessageFactory factory;
  return &factory;
}

// Builds a `celwasm.test.Widget` DynamicMessage with the given textproto
// body, resolved against the supplied pool.  Returns an owned message —
// the caller keeps it alive across the Eval window.
std::unique_ptr<google::protobuf::Message> MakeWidget(
    absl::string_view textproto) {
  const google::protobuf::Descriptor* desc =
      WidgetPool()->FindMessageTypeByName("celwasm.test.Widget");
  ABSL_CHECK(desc != nullptr) << "Widget must resolve in the supplied pool";
  const google::protobuf::Message* prototype =
      WidgetFactory()->GetPrototype(desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      std::string(textproto), msg.get()))
      << "textproto parse failed: " << textproto;
  return msg;
}

// Sanity guard shared by every test: the pool is genuinely load-bearing
// because `Widget` is absent from the generated pool.  If a future link
// edge pulls a `Widget` into the generated pool, this fails loudly and
// the tests stop proving anything.
TEST(DescriptorPoolDynamic, WidgetAbsentFromGeneratedPool) {
  EXPECT_EQ(
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          "celwasm.test.Widget"),
      nullptr);
  EXPECT_NE(WidgetPool()->FindMessageTypeByName("celwasm.test.Widget"),
            nullptr);
}

// ─────────────────────────────────────────────────────────────────────
// (a) Bound variable — `w.label` reads a field off the dynamic Widget.
// ─────────────────────────────────────────────────────────────────────

TEST(DescriptorPoolDynamic, BoundVariableFieldReadResolvesSuppliedType) {
  auto b = Compiler::NewBuilder();
  b.SetDescriptorPool(WidgetPool())
      .DeclareVariable("w", CelType::Message("celwasm.test.Widget"));
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  auto program = compiler->Compile("w.label", DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().SetDescriptorPool(WidgetPool()).Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::unique_ptr<google::protobuf::Message> widget =
      MakeWidget(R"pb(label: "hello" count: 7)pb");
  Activation act;
  act.Bind("w", Value::Message(*widget));

  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "hello");
}

TEST(DescriptorPoolDynamic, BoundVariableInt64FieldReadResolvesSuppliedType) {
  auto b = Compiler::NewBuilder();
  b.SetDescriptorPool(WidgetPool())
      .DeclareVariable("w", CelType::Message("celwasm.test.Widget"));
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  auto program = compiler->Compile("w.count", DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().SetDescriptorPool(WidgetPool()).Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::unique_ptr<google::protobuf::Message> widget =
      MakeWidget(R"pb(label: "hello" count: 7)pb");
  Activation act;
  act.Bind("w", Value::Message(*widget));

  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 7);
}

// ─────────────────────────────────────────────────────────────────────
// (b) Host function — `@host.widget_label(w)` reads `label` off the
//     marshalled `const Message*` via reflection (the message is a
//     DynamicMessage, so there is no generated class to cast to).
// ─────────────────────────────────────────────────────────────────────

TEST(DescriptorPoolDynamic, HostFunctionMessageArgResolvesSuppliedType) {
  auto b = Compiler::NewBuilder();
  b.SetDescriptorPool(WidgetPool())
      .DeclareVariable("w", CelType::Message("celwasm.test.Widget"))
      .AddFunction("string @host.widget_label(proto(celwasm.test.Widget) w);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  auto program = compiler->Compile("widget_label(w)", DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().SetDescriptorPool(WidgetPool()).Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto st = engine->AddFunction(
      "widget_label_message_celwasm_test_Widget", 2,
      [](HostCallContext& ctx) -> absl::Status {
        auto m = ctx.ArgProto(0);
        if (!m.ok()) return m.status();
        const google::protobuf::Message* msg = *m;
        const google::protobuf::Descriptor* desc = msg->GetDescriptor();
        const google::protobuf::FieldDescriptor* label =
            desc->FindFieldByName("label");
        if (label == nullptr) {
          return absl::InvalidArgumentError("Widget has no `label` field");
        }
        return ctx.ReturnString(msg->GetReflection()->GetString(*msg, label));
      });
  ASSERT_TRUE(st.ok()) << st;

  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::unique_ptr<google::protobuf::Message> widget =
      MakeWidget(R"pb(label: "hello" count: 7)pb");
  Activation act;
  act.Bind("w", Value::Message(*widget));

  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "hello");
}

// ─────────────────────────────────────────────────────────────────────
// (c) Component function — a WAT component takes the Widget as
//     `(list u8)` wire bytes (the `@component` proto Lift path
//     serializes the message) and returns the first letter of `label`.
// ─────────────────────────────────────────────────────────────────────

std::vector<uint8_t> WatToWasm(absl::string_view wat) {
  wasm_byte_vec_t wasm;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &wasm);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string err_str(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    ABSL_CHECK(false) << "wat2wasm failed: " << err_str;
  }
  std::vector<uint8_t> out(
      reinterpret_cast<const uint8_t*>(wasm.data),
      reinterpret_cast<const uint8_t*>(wasm.data) + wasm.size);
  wasm_byte_vec_delete(&wasm);
  return out;
}

// `widget-first-letter-... : (list u8) -> string`.  The arg is the
// proto wire encoding of `Widget{label:<s>, count:<n>}`.  proto3
// serializes fields in field-number order and `label` is field 1, so
// the bytes start 0x0A <len> 'h' 'e' … — the first letter of `label`
// is at arg offset 2 (valid while label is field 1 and shorter than
// 128 bytes, i.e. a one-byte length varint).  Mirrors the Customer
// `kFirstLetterComponentWat` fixture in foreign_fn_type_matrix_test.cc.
constexpr absl::string_view kWidgetFirstLetterComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "f") (param $ptr i32) (param $len i32) (result i32)
      i32.const 8
      local.get $ptr
      i32.const 2
      i32.add
      i32.store
      i32.const 12
      i32.const 1
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "f" (core func $f))
  (func (export "widget-first-letter-message-celwasm-test-widget")
      (param "w" (list u8)) (result string)
    (canon lift (core func $f) (memory $mem) (realloc $realloc))))
)WAT";

TEST(DescriptorPoolDynamic, ComponentFunctionMessageArgResolvesSuppliedType) {
  CelfnType string_ret;
  string_ret.kind = CelfnType::Kind::kString;
  CelfnType widget_arg;
  widget_arg.kind = CelfnType::Kind::kProto;
  widget_arg.proto_fqn = "celwasm.test.Widget";
  auto lib = FunctionLibrary::Builder()
                 .AddForeignComponent(
                     "widget_first_letter", string_ret,
                     {CelfnParam{/*is_receiver=*/false, widget_arg, "w"}})
                 .Build();
  ASSERT_TRUE(lib.ok()) << lib.status();

  auto b = Compiler::NewBuilder();
  b.SetDescriptorPool(WidgetPool())
      .DeclareVariable("w", CelType::Message("celwasm.test.Widget"))
      .AddFunction(
          "string @component.widget_first_letter(proto(celwasm.test.Widget) "
          "w);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();

  auto program = compiler->Compile("widget_first_letter(w)", DefaultOpts());
  ASSERT_TRUE(program.ok()) << program.status();

  auto engine = Engine::NewBuilder().SetDescriptorPool(WidgetPool()).Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto st =
      engine->AddComponent(WatToWasm(kWidgetFirstLetterComponentWat), *lib);
  ASSERT_TRUE(st.ok()) << st;

  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();

  std::unique_ptr<google::protobuf::Message> widget =
      MakeWidget(R"pb(label: "hello" count: 7)pb");
  Activation act;
  act.Bind("w", Value::Message(*widget));

  auto v = instance->Eval(act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsString(), "h");
}

}  // namespace
}  // namespace celwasm
