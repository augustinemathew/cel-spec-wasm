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

celwasm::CelfnType Prim(celwasm::CelfnType::Kind k) {
  celwasm::CelfnType t;
  t.kind = k;
  return t;
}

celwasm::CelfnType ListOf(celwasm::CelfnType e) {
  celwasm::CelfnType t;
  t.kind = celwasm::CelfnType::Kind::kList;
  t.list_element.push_back(std::move(e));
  return t;
}

celwasm::CelfnType MapOf(celwasm::CelfnType k, celwasm::CelfnType v) {
  celwasm::CelfnType t;
  t.kind = celwasm::CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(k));
  t.map_kv.push_back(std::move(v));
  return t;
}

celwasm::CelfnType ProtoOf(std::string fqn) {
  celwasm::CelfnType t;
  t.kind = celwasm::CelfnType::Kind::kProto;
  t.proto_fqn = std::move(fqn);
  return t;
}

}  // namespace

int main() {  // NOLINT(misc-use-internal-linkage)
  using K = celwasm::CelfnType::Kind;
  celwasm::FunctionLibrary::Builder b;

  // Every type the emitter handles, in one decl each.
  b.AddPlugin("f_string", Prim(K::kString),
                        {celwasm::CelfnParam{false, Prim(K::kString), "x"}});
  b.AddPlugin("f_bytes", Prim(K::kBytes),
                        {celwasm::CelfnParam{false, Prim(K::kBytes), "x"}});
  b.AddPlugin("f_duration", Prim(K::kDuration),
                        {celwasm::CelfnParam{false, Prim(K::kDuration), "x"}});
  b.AddPlugin("f_timestamp", Prim(K::kTimestamp),
                        {celwasm::CelfnParam{false, Prim(K::kTimestamp), "x"}});
  b.AddPlugin(
      "f_list_int", ListOf(Prim(K::kInt)),
      {celwasm::CelfnParam{false, ListOf(Prim(K::kInt)), "x"}});
  b.AddPlugin(
      "f_list_string", ListOf(Prim(K::kString)),
      {celwasm::CelfnParam{false, ListOf(Prim(K::kString)), "x"}});
  b.AddPlugin(
      "f_list_list_int", ListOf(ListOf(Prim(K::kInt))),
      {celwasm::CelfnParam{false, ListOf(ListOf(Prim(K::kInt))), "x"}});
  b.AddPlugin(
      "f_map_string_int", Prim(K::kInt),
      {celwasm::CelfnParam{false, MapOf(Prim(K::kString), Prim(K::kInt)),
                           "x"}});
  b.AddPlugin(
      "f_proto", Prim(K::kBool),
      {celwasm::CelfnParam{false, ProtoOf("acme.User"), "u"}});

  auto lib_or = std::move(b).Build();
  ABSL_CHECK_OK(lib_or) << lib_or.status();
  auto text_or =
      celwasm::celfnc_emit::EmitCodecH(*lib_or, "rules", "cel:customfn");
  ABSL_CHECK_OK(text_or) << text_or.status();
  std::fwrite(text_or->data(), 1, text_or->size(), stdout);
  return 0;
}
