// `cel generate` subcommand — drive the four cpp / go emitters
// from a single `.idl` file.  See m26 §3 / §7 for the contract.
//
// Sibling of RunEval / RunCheck / RunCompile in `cel.cc`.

#ifndef CELWASM_TOOLS_CEL_RUN_GENERATE_H_
#define CELWASM_TOOLS_CEL_RUN_GENERATE_H_

#include <string>
#include <vector>

namespace celwasm::tools::cel {

struct GenerateOptions {
  std::string idl_path;
  std::string language;  // "cpp" only in v1; "go" arrives with H.4.
  std::string out_dir;
  std::vector<std::string> extra_includes;
  // The WIT package name/version are NOT configurable: they are
  // always derived from the IDL's `Module foo;` directive via
  // DeriveWitPackageName/kWitPackageVersion (fallback module
  // `customfn`), so the generated WIT and the engine's interface
  // lookup cannot drift (m35-plugin-ergonomics.md §4).
};

// 0 on success, non-zero on error.  Diagnostics go to stderr.
int RunGenerate(const GenerateOptions& opts);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_TOOLS_CEL_RUN_GENERATE_H_
