// `cel generate` — drive the four cpp emitters from one IDL.

#include "tools/cel/run_generate.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "compiler/celfn/celfnc_emit/cpp_codec_emitter.h"
#include "compiler/celfn/celfnc_emit/cpp_skeleton_emitter.h"
#include "compiler/celfn/celfnc_emit/cpp_stub_emitter.h"
#include "compiler/celfn/celfnc_emit/wit_emitter.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::tools::cel {
namespace {

absl::StatusOr<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot open --idl file: ", path));
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

absl::Status WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path);
  if (!out) {
    return absl::PermissionDeniedError(
        absl::StrCat("cannot open output file: ", path));
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    return absl::DataLossError(absl::StrCat("write failed: ", path));
  }
  return absl::OkStatus();
}

absl::Status RunCppPath(const GenerateOptions& opts,
                        const FunctionLibrary& lib) {
  // The package is always `cel:<module>` (with fallback `cel:customfn`
  // when the IDL has no `Module foo;` directive), via the shared
  // derivation helper — the same one `Plugin::Load` uses, so the
  // generated WIT and the engine's interface lookup cannot drift
  // (m35-plugin-ergonomics.md §4: no override exists).  This pairs
  // with the hardcoded `world customfn` in wit_emitter.cc so exports
  // come out as `exports_cel_<module>_fns_*` (m26 §7.5.1).
  const std::string pkg = DeriveWitPackageName(lib.module_name());
  const std::string ver = std::string(kWitPackageVersion);
  const std::string ns = lib.module_name();  // empty → global scope

  auto wit = celfnc_emit::EmitWit(lib, pkg, ver);
  if (!wit.ok()) return wit.status();
  auto codec = celfnc_emit::EmitCodecH(lib, ns, pkg);
  if (!codec.ok()) return codec.status();
  auto stub = celfnc_emit::EmitStubCc(lib, ns, pkg, opts.extra_includes);
  if (!stub.ok()) return stub.status();
  auto skel = celfnc_emit::EmitUserFnsH(lib, ns, opts.extra_includes);
  if (!skel.ok()) return skel.status();

  namespace fs = std::filesystem;
  fs::create_directories(opts.out_dir);
  if (auto s = WriteFile(opts.out_dir + "/fns.wit", *wit); !s.ok()) return s;
  if (auto s = WriteFile(opts.out_dir + "/codec.h", *codec); !s.ok()) return s;
  if (auto s = WriteFile(opts.out_dir + "/generated_stub.cc", *stub); !s.ok()) {
    return s;
  }
  if (auto s = WriteFile(opts.out_dir + "/user_fns.h", *skel); !s.ok()) {
    return s;
  }
  return absl::OkStatus();
}

}  // namespace

int RunGenerate(const GenerateOptions& opts) {
  if (opts.idl_path.empty()) {
    std::cerr << "ERROR: cel generate: --idl is required\n";
    return 2;
  }
  if (opts.out_dir.empty()) {
    std::cerr << "ERROR: cel generate: --out_dir is required\n";
    return 2;
  }
  const std::string lang = opts.language.empty() ? "cpp" : opts.language;
  if (lang != "cpp") {
    std::cerr << "ERROR: cel generate: --language=" << lang
              << " not supported in v1 (only `cpp`; `go` arrives with H.4)\n";
    return 2;
  }

  auto src = ReadFile(opts.idl_path);
  if (!src.ok()) {
    std::cerr << "ERROR: " << src.status().message() << "\n";
    return 1;
  }
  auto lib_or = ParseCelfnSource(*src);
  if (!lib_or.ok()) {
    std::cerr << "ERROR: parse IDL: " << lib_or.status().message() << "\n";
    return 1;
  }

  if (auto s = RunCppPath(opts, *lib_or); !s.ok()) {
    std::cerr << "ERROR: emit: " << s.message() << "\n";
    return 1;
  }
  std::cerr << "wrote fns.wit, codec.h, generated_stub.cc, user_fns.h to "
            << opts.out_dir << "\n";
  return 0;
}

}  // namespace celwasm::tools::cel
