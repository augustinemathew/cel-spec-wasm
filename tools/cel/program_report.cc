#include "tools/cel/program_report.h"

#include <string>
#include <vector>

#include "abi/program_facts.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

namespace celwasm::tools::cel {

std::string FormatProgramFacts(const ::celwasm::abi::ProgramFacts& facts) {
  if (!facts.has_abi_section) {
    return "vars:       <none — this module carries no cel.abi section>\n";
  }
  std::vector<std::string> rendered;
  rendered.reserve(facts.vars.size());
  for (const ::celwasm::abi::DeclaredVar& v : facts.vars) {
    rendered.push_back(absl::StrCat(v.name, ":", v.type_name));
  }
  // Split by what the caller can actually do about each: a plugin
  // function is satisfiable with a wasm artifact, an @host function
  // only from C++ in the embedder's process.
  std::vector<std::string> host_fns;
  std::vector<std::string> plugin_fns;
  for (const ::celwasm::abi::RequiredFn& fn : facts.required_fns) {
    (fn.is_host ? host_fns : plugin_fns).push_back(fn.signature);
  }
  std::string out = absl::StrCat(
      "vars:       ", rendered.empty() ? "none" : absl::StrJoin(rendered, ", "),
      "\n");
  absl::StrAppend(&out, "plugin fns: ",
                  plugin_fns.empty() ? "none" : absl::StrJoin(plugin_fns, ", "),
                  "\n");
  absl::StrAppend(&out, "host fns:   ",
                  host_fns.empty() ? "none" : absl::StrJoin(host_fns, ", "),
                  "\n");
  if (!host_fns.empty()) {
    absl::StrAppend(&out,
                    "            (@host impls are C++ in your process — this "
                    "program is not runnable by `cel run`)\n");
  }
  absl::StrAppend(&out,
                  "link:       ", facts.static_linked ? "static" : "dynamic",
                  " (cel.abi v", facts.abi_version, ", runtime abi v",
                  facts.runtime_abi_version, ")\n");
  return out;
}

}  // namespace celwasm::tools::cel
