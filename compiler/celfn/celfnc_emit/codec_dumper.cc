// Tool main(): construct a representative FunctionLibrary covering
// every m24 §6 row that the codec emitter handles, render the
// generated codec.h, print to stdout.  Pairs with a `genrule` in
// BUILD.bazel that captures the output and feeds it to a cc_library
// — if the codec emitter ever produces text the compiler rejects,
// `bazel build` fails at codec_compile_check.  This is the
// regression gate the user asked for during m26 §3.5 + §4 design:
// "when the code changes we can validate it is compilable."
//
// NOT a permanent reference fixture; the inputs here are tuned to
// exercise the full type matrix the emitter implements TODAY.  Add
// rows here as the emitter grows.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "compiler/celfn/celfnc_emit/cpp_codec_emitter.h"
#include "compiler/celfn/function_library.h"

namespace {

using celwasm::CelType;

}  // namespace

int main() {  // NOLINT(misc-use-internal-linkage)
  celwasm::FunctionLibrary::Builder b;

  // Every type the emitter handles, in one decl each.
  b.AddPlugin("f_string", CelType::String(),
              {celwasm::CelfnParam{false, CelType::String(), "x"}});
  b.AddPlugin("f_bytes", CelType::Bytes(),
              {celwasm::CelfnParam{false, CelType::Bytes(), "x"}});
  b.AddPlugin("f_duration", CelType::Duration(),
              {celwasm::CelfnParam{false, CelType::Duration(), "x"}});
  b.AddPlugin("f_timestamp", CelType::Timestamp(),
              {celwasm::CelfnParam{false, CelType::Timestamp(), "x"}});
  b.AddPlugin("f_list_int", CelType::List(CelType::Int()),
              {celwasm::CelfnParam{false, CelType::List(CelType::Int()), "x"}});
  b.AddPlugin(
      "f_list_string", CelType::List(CelType::String()),
      {celwasm::CelfnParam{false, CelType::List(CelType::String()), "x"}});
  b.AddPlugin("f_list_list_int", CelType::List(CelType::List(CelType::Int())),
              {celwasm::CelfnParam{
                  false, CelType::List(CelType::List(CelType::Int())), "x"}});
  b.AddPlugin(
      "f_map_string_int", CelType::Int(),
      {celwasm::CelfnParam{
          false, CelType::Map(CelType::String(), CelType::Int()), "x"}});
  b.AddPlugin("f_proto", CelType::Bool(),
              {celwasm::CelfnParam{false, CelType::Message("acme.User"), "u"}});

  auto lib_or = std::move(b).Build();
  ABSL_CHECK_OK(lib_or) << lib_or.status();
  auto text_or =
      celwasm::celfnc_emit::EmitCodecH(*lib_or, "rules", "cel:customfn");
  ABSL_CHECK_OK(text_or) << text_or.status();
  std::fwrite(text_or->data(), 1, text_or->size(), stdout);
  return 0;
}
