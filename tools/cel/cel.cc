// `cel` — command-line driver for the CEL→WASM AOT pipeline.
//
// Usage:
//   cel eval    <expr> [--var ...] [--proto ... | --descriptor_set ...] ...
//   cel check   <expr> [--var name:Type] [--proto ... | --descriptor_set ...]
//   cel compile <expr> --output <out.wasm> [--O 0..3] ...
//   cel generate --idl <fns.idl> --out_dir <dir> [--language cpp] ...
//
// See tools/cel/var_parser.h for the `--var` literal
// grammar and tools/cel/value_format.h for `--format`.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/internal/compile.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "tools/cel/run_generate.h"
#include "tools/cel/value_format.h"
#include "tools/cel/var_parser.h"

// Flag-parse helpers register external symbols; absl's parser
// machinery emits "use internal linkage" warnings on the generated
// helpers — same suppression pattern as run_conformance.cc.
// NOLINTBEGIN(misc-use-internal-linkage,bugprone-throwing-static-initialization)
//
// `--var` and `--format` are NOT absl flags: their values contain
// commas (`list<int>=[1, 2, 3]`, `map<string,int>=...`) and absl's
// default `std::vector<std::string>` parser comma-splits, then
// overwrites on each repeat.  They're handled by `ExtractRepeated`
// in `main()` before absl sees argv.

ABSL_FLAG(std::string, proto, "",
          "Path to a `.proto` source file providing message types "
          "referenced in --var declarations.  Mutually exclusive with "
          "--descriptor_set.");

ABSL_FLAG(std::string, descriptor_set, "",
          "Path to a binary FileDescriptorSet (the output of `protoc "
          "--descriptor_set_out=...`).  Use `--include_imports` to bundle "
          "multi-file schemas into one FDS.  Mutually exclusive with --proto.");

ABSL_FLAG(std::string, container, "",
          "Package container used for CEL name resolution (matches "
          "CEL-Go's `container` option).  Empty (default) means every "
          "ident must be fully qualified.");

ABSL_FLAG(int, O, 0,
          "Binaryen optimize level for the emitted expr module (0..3).  "
          "0 = no-op (default); 2 = balanced; recommended on a hot path.");

ABSL_FLAG(std::uint32_t, mem_size_bytes, 128u * 1024u,
          "Initial linear-memory size in bytes for the emitted module, "
          "rounded up to the next 64KiB wasm page.  Applies to dynamic "
          "link mode only — it has no effect on the default static "
          "output, and is not a way to enlarge the eval arena.");

ABSL_FLAG(std::string, output, "",
          "`cel compile` only: path to write the emitted wasm bytes.  "
          "If empty, bytes go to stdout.");

// `cel generate` flags.  Drive the emitter set from a single `.idl`
// file; outputs go to --out_dir.
ABSL_FLAG(std::string, idl, "",
          "`cel generate` only: path to the .idl input file.");
ABSL_FLAG(std::string, language, "cpp",
          "`cel generate` only: target language.  `cpp` (default); "
          "`go` is planned.");
ABSL_FLAG(std::string, out_dir, "",
          "`cel generate` only: directory to write generated files into "
          "(fns.wit, codec.h, generated_stub.cc, user_fns.h).");
ABSL_FLAG(std::string, package, "",
          "`cel generate` only: optional WIT package name override.  "
          "Default: `<module>:fns` derived from the IDL `Module` directive.");
ABSL_FLAG(std::vector<std::string>, include, {},
          "`cel generate` only: comma-separated #include paths to inject "
          "at the top of the generated user_fns.h + generated_stub.cc.  "
          "Typical use: `--include=acme/user.pb.h` for proto-typed fns.");
// NOLINTEND(misc-use-internal-linkage,bugprone-throwing-static-initialization)

namespace celwasm::tools::cel {
namespace {

using ::celwasm::Activation;
using ::celwasm::Engine;
using ::celwasm::Program;
using ::celwasm::Value;

// Process exit codes.  The contract is documented in tools/cel/README.md
// and pinned by cel_smoke_test.sh; keep the three in sync.
//
//   0  success
//   1  the expression or program failed — check/compile diagnostics, a
//      non-OK Eval status, or a result that is a CEL error or unknown
//   2  usage — bad subcommand, flag, --var syntax, or unreadable input
//
// The split matters for scripting: `1` means "the CEL said no", `2`
// means "the invocation was wrong".  Both are distinguishable from `0`,
// which is the bug this replaced — a CEL error value used to print on
// stdout and exit 0.
inline constexpr int kExitOk = 0;
inline constexpr int kExitExprFailure = 1;
inline constexpr int kExitUsage = 2;

// Same schema-loading shape as parse_and_check.cc::LoadDescriptorPool,
// re-implemented here so the CLI can construct DynamicMessage
// instances for --var bindings.  Returns a pool that contains the
// schema-supplied messages plus the process-wide generated pool,
// merged.
struct PoolBundle {
  std::unique_ptr<google::protobuf::SimpleDescriptorDatabase> schema_db;
  std::unique_ptr<google::protobuf::DescriptorPoolDatabase> generated_db;
  std::unique_ptr<google::protobuf::MergedDescriptorDatabase> merged_db;
  std::unique_ptr<google::protobuf::DescriptorPool> owned_pool;
  const google::protobuf::DescriptorPool* pool = nullptr;
};

class StringErrorCollector : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, int column, absl::string_view message) override {
    absl::StrAppend(&text_, "  line ", line + 1, ":", column + 1, " ", message,
                    "\n");
  }
  void RecordWarning(int /*line*/, int /*column*/,
                     absl::string_view /*message*/) override {}
  const std::string& text() const {
    return text_;
  }

 private:
  std::string text_;
};

absl::StatusOr<google::protobuf::FileDescriptorProto> LoadProtoSource(
    absl::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open --proto file: ", path));
  }
  std::string buf((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  google::protobuf::io::ArrayInputStream input(buf.data(),
                                               static_cast<int>(buf.size()));
  StringErrorCollector collector;
  google::protobuf::io::Tokenizer tokenizer(&input, &collector);
  google::protobuf::compiler::Parser parser;
  parser.RecordErrorsTo(&collector);
  google::protobuf::FileDescriptorProto file;
  if (!parser.Parse(&tokenizer, &file)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "failed to parse --proto ", path, ":\n", collector.text()));
  }
  file.set_name(std::string(path));
  return file;
}

absl::Status LoadDescriptorSet(absl::string_view path,
                               google::protobuf::SimpleDescriptorDatabase& db) {
  std::ifstream in{std::string(path), std::ios::binary};
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open --descriptor_set file: ", path));
  }
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(bytes)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "--descriptor_set ", path, " is not a valid FileDescriptorSet"));
  }
  for (const auto& f : fds.file()) {
    if (!db.Add(f)) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate file `", f.name(), "` in --descriptor_set"));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<PoolBundle> BuildPool() {
  const std::string proto_path = absl::GetFlag(FLAGS_proto);
  const std::string fds_path = absl::GetFlag(FLAGS_descriptor_set);
  if (!proto_path.empty() && !fds_path.empty()) {
    return absl::InvalidArgumentError(
        "--proto and --descriptor_set are mutually exclusive");
  }
  PoolBundle out;
  if (proto_path.empty() && fds_path.empty()) {
    out.pool = google::protobuf::DescriptorPool::generated_pool();
    return out;
  }
  out.schema_db =
      std::make_unique<google::protobuf::SimpleDescriptorDatabase>();
  if (!proto_path.empty()) {
    auto file = LoadProtoSource(proto_path);
    if (!file.ok()) return file.status();
    if (!out.schema_db->Add(*file)) {
      return absl::InvalidArgumentError(
          absl::StrCat("could not register --proto ", proto_path));
    }
  } else {
    if (auto s = LoadDescriptorSet(fds_path, *out.schema_db); !s.ok()) {
      return s;
    }
  }
  out.generated_db = std::make_unique<google::protobuf::DescriptorPoolDatabase>(
      *google::protobuf::DescriptorPool::generated_pool());
  out.merged_db = std::make_unique<google::protobuf::MergedDescriptorDatabase>(
      out.schema_db.get(), out.generated_db.get());
  out.owned_pool =
      std::make_unique<google::protobuf::DescriptorPool>(out.merged_db.get());
  out.pool = out.owned_pool.get();
  return out;
}

// Populate `celwasm::CompileOptions` from the global flag state +
// the parsed --var declarations.  Variable specs flow through as
// `name:TypeSpec` strings (the form parse_and_check.cc consumes).
absl::StatusOr<celwasm::CompileOptions> BuildCompileOptions(
    absl::string_view source_desc, const std::vector<ParsedVar>& vars) {
  celwasm::CompileOptions opts;
  opts.check.description = std::string(source_desc);
  opts.check.container = absl::GetFlag(FLAGS_container);
  opts.mem_size_bytes = absl::GetFlag(FLAGS_mem_size_bytes);
  opts.optimize_level = absl::GetFlag(FLAGS_O);
  const std::string proto_path = absl::GetFlag(FLAGS_proto);
  const std::string fds_path = absl::GetFlag(FLAGS_descriptor_set);
  if (!proto_path.empty() && !fds_path.empty()) {
    return absl::InvalidArgumentError(
        "--proto and --descriptor_set are mutually exclusive");
  }
  if (!proto_path.empty()) {
    opts.check.schema = celwasm::SchemaProtoSource{proto_path};
  } else if (!fds_path.empty()) {
    opts.check.schema = celwasm::SchemaDescriptorSet{fds_path};
  }
  for (const auto& v : vars) {
    // CelType → spec-string round-trip.  We could thread the
    // original spec from the --var flag, but reusing the CelType-
    // formatter keeps a single source of truth and the cost is
    // dust (a few dozen StrCats per Compile).
    auto FormatType = [](const auto& self,
                         const ::celwasm::CelType& t) -> std::string {
      switch (t.kind()) {
        case ::celwasm::CelType::Kind::kBool:
          return "bool";
        case ::celwasm::CelType::Kind::kInt:
          return "int";
        case ::celwasm::CelType::Kind::kUint:
          return "uint";
        case ::celwasm::CelType::Kind::kDouble:
          return "double";
        case ::celwasm::CelType::Kind::kString:
          return "string";
        case ::celwasm::CelType::Kind::kBytes:
          return "bytes";
        case ::celwasm::CelType::Kind::kDuration:
          return "duration";
        case ::celwasm::CelType::Kind::kTimestamp:
          return "timestamp";
        case ::celwasm::CelType::Kind::kType:
          return "type";
        case ::celwasm::CelType::Kind::kMessage:
          return std::string(t.message_fully_qualified_name());
        case ::celwasm::CelType::Kind::kList:
          return absl::StrCat("list<", self(self, t.list_element()), ">");
        case ::celwasm::CelType::Kind::kMap:
          return absl::StrCat("map<", self(self, t.map_key()), ",",
                              self(self, t.map_value()), ">");
        case ::celwasm::CelType::Kind::kUnknown:
          break;
      }
      ABSL_CHECK(false) << "unhandled CelType in --var spec";
    };
    opts.check.variable_specs.push_back(
        absl::StrCat(v.name, ":", FormatType(FormatType, v.type)));
  }
  return opts;
}

absl::Status BindActivation(const std::vector<ParsedVar>& vars,
                            Activation& act) {
  for (const auto& v : vars) {
    if (!v.has_value) continue;  // `--var name:Type` declaration-only form.
    act.Bind(v.name, v.value);
  }
  return absl::OkStatus();
}

// Hand-collected `--var` / `--format` values, populated by
// `ExtractRepeated` in `main()` before absl flag parsing.
std::vector<std::string>& VarFlags() {
  static auto* v = new std::vector<std::string>{};
  return *v;
}
std::vector<std::string>& FormatFlags() {
  static auto* v = new std::vector<std::string>{};
  return *v;
}

// Parse every --var flag against the supplied descriptor pool.
absl::StatusOr<std::vector<ParsedVar>> ParseAllVars(
    const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
  std::vector<ParsedVar> out;
  for (const auto& flag : VarFlags()) {
    auto pv = ParseVarFlag(flag, pool, factory);
    if (!pv.ok()) return pv.status();
    out.push_back(*std::move(pv));
  }
  return out;
}

// --- Shared subcommand front end --------------------------------------------

// The descriptor pool, the dynamic-message factory that must outlive
// every message `Value` parsed against it, the parsed `--var` list, and
// the compile options derived from them.  Every expression-taking
// subcommand needs exactly this bundle, so it is assembled once here
// rather than repeated per subcommand.
struct CliSetup {
  PoolBundle pool;
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory;
  std::vector<ParsedVar> vars;
  celwasm::CompileOptions opts;
};

absl::StatusOr<CliSetup> PrepareCli(absl::string_view source_desc) {
  CliSetup s;
  auto pool = BuildPool();
  if (!pool.ok()) return pool.status();
  s.pool = *std::move(pool);
  s.factory =
      std::make_unique<google::protobuf::DynamicMessageFactory>(s.pool.pool);
  auto vars = ParseAllVars(*s.pool.pool, *s.factory);
  if (!vars.ok()) return vars.status();
  s.vars = *std::move(vars);
  auto opts = BuildCompileOptions(source_desc, s.vars);
  if (!opts.ok()) return opts.status();
  s.opts = *std::move(opts);
  return s;
}

// Parse every `--format` value.  Done before any compilation so a typo
// fails as usage rather than after the expensive work.
absl::StatusOr<std::vector<Format>> ResolveFormats() {
  std::vector<Format> formats;
  for (const auto& name : FormatFlags()) {
    auto f = ParseFormatName(name);
    if (!f.ok()) return f.status();
    formats.push_back(*f);
  }
  return formats;
}

// Render a successful evaluation result and return the process exit
// code.  A CEL error or unknown is a legitimate library result but not
// a usable process result: it goes to stderr and fails, so a caller can
// branch on `$?` instead of scraping stdout.
int ReportResult(const Value& value, const std::vector<Format>& formats) {
  if (value.kind() == Value::Kind::kError ||
      value.kind() == Value::Kind::kUnknown) {
    auto rendered = FormatScalar(value);
    std::cerr << "ERROR: " << (rendered.ok() ? *rendered : "<unrenderable>")
              << "\n";
    return kExitExprFailure;
  }
  if (value.kind() == Value::Kind::kMessage) {
    auto out = FormatMessage(value, formats);
    if (!out.ok()) {
      std::cerr << "ERROR: format: " << out.status().message() << "\n";
      return kExitExprFailure;
    }
    std::cout << *out;
    if (!out->empty() && out->back() != '\n') std::cout << "\n";
    return kExitOk;
  }
  auto out = FormatScalar(value);
  if (!out.ok()) {
    std::cerr << "ERROR: format: " << out.status().message() << "\n";
    return kExitExprFailure;
  }
  std::cout << *out << "\n";
  return kExitOk;
}

// --- Subcommands ------------------------------------------------------------

int RunEval(absl::string_view expr) {
  auto formats = ResolveFormats();
  if (!formats.ok()) {
    std::cerr << "ERROR: " << formats.status().message() << "\n";
    return kExitUsage;
  }
  auto setup = PrepareCli("<cli>");
  if (!setup.ok()) {
    std::cerr << "ERROR: " << setup.status().message() << "\n";
    return kExitUsage;
  }
  auto artifact = celwasm::Compile(expr, setup->opts);
  if (!artifact.ok()) {
    std::cerr << "ERROR: " << artifact.status().message() << "\n";
    return kExitExprFailure;
  }
  Program program(std::move(artifact->wasm_bytes));
  auto engine = Engine::NewBuilder().Build();
  if (!engine.ok()) {
    std::cerr << "ERROR: engine: " << engine.status().message() << "\n";
    return kExitExprFailure;
  }
  auto instance = engine->Plan(program);
  if (!instance.ok()) {
    std::cerr << "ERROR: plan: " << instance.status().message() << "\n";
    return kExitExprFailure;
  }
  Activation act;
  if (auto s = BindActivation(setup->vars, act); !s.ok()) {
    std::cerr << "ERROR: " << s.message() << "\n";
    return kExitUsage;
  }
  auto value = setup->vars.empty() ? instance->Eval() : instance->Eval(act);
  if (!value.ok()) {
    std::cerr << "ERROR: eval: " << value.status().message() << "\n";
    return kExitExprFailure;
  }
  return ReportResult(*value, *formats);
}

int RunCheck(absl::string_view expr) {
  auto setup = PrepareCli("<cli>");
  if (!setup.ok()) {
    std::cerr << "ERROR: " << setup.status().message() << "\n";
    return kExitUsage;
  }
  auto ast = celwasm::ParseAndCheck(expr, setup->opts.check);
  if (!ast.ok()) {
    std::cerr << "ERROR: " << ast.status().message() << "\n";
    return kExitExprFailure;
  }
  std::cout << "OK\n";
  return kExitOk;
}

int RunCompile(absl::string_view expr) {
  auto setup = PrepareCli("<cli>");
  if (!setup.ok()) {
    std::cerr << "ERROR: " << setup.status().message() << "\n";
    return kExitUsage;
  }
  auto artifact = celwasm::Compile(expr, setup->opts);
  if (!artifact.ok()) {
    std::cerr << "ERROR: " << artifact.status().message() << "\n";
    return kExitExprFailure;
  }
  const auto& bytes = artifact->wasm_bytes;
  const std::string out_path = absl::GetFlag(FLAGS_output);
  if (out_path.empty()) {
    std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    return kExitOk;
  }
  std::ofstream out(out_path, std::ios::binary);
  if (!out) {
    std::cerr << "ERROR: cannot open --output " << out_path << "\n";
    return kExitUsage;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  std::cerr << "wrote " << bytes.size() << " bytes to " << out_path << "\n";
  return kExitOk;
}

// Pull every `--<name>=VALUE` and `--<name> VALUE` occurrence out of
// `argv`, appending to `sink` in order.  Returns the surviving argv
// (caller passes that to absl::ParseCommandLine).  The original
// `argv[0]` is preserved at the head of the result.
std::vector<char*> ExtractRepeated(absl::Span<char* const> argv,
                                   absl::string_view name,
                                   std::vector<std::string>& sink) {
  std::vector<char*> out;
  out.reserve(argv.size());
  if (!argv.empty()) out.push_back(argv[0]);
  const std::string eq_form = absl::StrCat("--", name, "=");
  const std::string bare = absl::StrCat("--", name);
  for (std::size_t i = 1; i < argv.size(); ++i) {
    absl::string_view a = argv[i];
    if (absl::StartsWith(a, eq_form)) {
      sink.emplace_back(a.substr(eq_form.size()));
      continue;
    }
    if (a == bare && i + 1 < argv.size()) {
      sink.emplace_back(argv[i + 1]);
      ++i;
      continue;
    }
    out.push_back(argv[i]);
  }
  return out;
}

void PrintUsage(std::ostream& os, absl::string_view argv0) {
  os << "usage: " << argv0 << " <subcommand> <expr> [flags...]\n"
     << "subcommands:\n"
     << "  eval     compile + evaluate <expr>; print the result\n"
     << "  check    parse + type-check <expr>; print OK / errors\n"
     << "  compile  compile <expr> to wasm bytes (--output PATH)\n"
     << "  generate emit custom-function bindings (fns.wit, codec.h,\n"
     << "           generated_stub.cc, user_fns.h) from a .idl file\n"
     << "common flags:\n"
     << "  --var name:Type=value    (repeatable) declare + bind\n"
     << "  --var name:Type          (repeatable) declare only\n"
     << "  --proto PATH             .proto source for message types\n"
     << "  --descriptor_set PATH    FileDescriptorSet for message types\n"
     << "  --container PKG          name-resolution container\n"
     << "  --format FMT             (eval, repeatable) textproto|json|cel\n"
     << "  --O LEVEL                Binaryen optimize level (0..3)\n"
     << "  --mem_size_bytes N       linear-memory size in bytes\n"
     << "                           (rounded up to a 64KiB wasm page)\n"
     << "  --output PATH            (compile) wasm output path\n"
     << "generate flags:\n"
     << "  --idl PATH               required: .idl input\n"
     << "  --out_dir PATH           required: output dir\n"
     << "  --language LANG          cpp (default); go (planned)\n"
     << "  --package PKG            WIT package name override\n";
}

// True when `--help` / `-h` / `help` appears anywhere in argv, so that
// `cel eval --help` prints this tool's usage rather than absl's full
// flag dump.
bool WantsHelp(absl::Span<char* const> argv) {
  for (std::size_t i = 1; i < argv.size(); ++i) {
    const absl::string_view a = argv[i];
    if (a == "-h" || a == "--help" || a == "help") return true;
  }
  return false;
}

bool IsKnownSubcommand(absl::string_view s) {
  return s == "eval" || s == "check" || s == "compile" || s == "generate";
}

// Peel the subcommand out of argv, pull the repeatable `--var` /
// `--format` values (whose values may contain commas or `=`, which
// absl's parser would mangle), then parse the remaining flags.
//
// `ParseAbseilFlagsOnly` rather than `ParseCommandLine`: the latter
// calls exit(1) on an unrecognized flag, colliding with the
// "expression failed" code.  Reporting them here keeps flag errors
// classified as usage.  Returns nullopt when a flag was unrecognized.
std::optional<std::vector<char*>> ParseFlagsAfterSubcommand(
    absl::Span<char* const> argv) {
  std::vector<char*> rest;
  rest.reserve(argv.size() - 1);
  rest.push_back(argv[0]);
  for (std::size_t i = 2; i < argv.size(); ++i) {
    rest.push_back(argv[i]);
  }
  rest = ExtractRepeated(rest, "var", VarFlags());
  rest = ExtractRepeated(rest, "format", FormatFlags());

  std::vector<char*> positional;
  std::vector<absl::UnrecognizedFlag> unrecognized;
  absl::ParseAbseilFlagsOnly(static_cast<int>(rest.size()), rest.data(),
                             positional, unrecognized);
  if (!unrecognized.empty()) {
    absl::ReportUnrecognizedFlags(unrecognized);
    return std::nullopt;
  }
  return positional;
}

int RunGenerateSubcommand() {
  GenerateOptions opts;
  opts.idl_path = absl::GetFlag(FLAGS_idl);
  opts.language = absl::GetFlag(FLAGS_language);
  opts.out_dir = absl::GetFlag(FLAGS_out_dir);
  opts.package_name = absl::GetFlag(FLAGS_package);
  opts.extra_includes = absl::GetFlag(FLAGS_include);
  return RunGenerate(opts);
}

// Validate the positional count for `subcommand` and dispatch.
// `positional[0]` is argv[0]; expression-taking subcommands need
// exactly one more.
int Dispatch(absl::string_view subcommand, absl::Span<char* const> positional,
             absl::string_view argv0) {
  if (subcommand == "generate") {
    if (positional.size() != 1) {
      std::cerr << "ERROR: `generate` takes no positional argument; "
                   "use --idl PATH instead.  Got "
                << (positional.size() - 1) << " unexpected.\n";
      PrintUsage(std::cerr, argv0);
      return kExitUsage;
    }
    return RunGenerateSubcommand();
  }
  if (positional.size() != 2) {
    std::cerr << "ERROR: expected exactly one positional <expr>, got "
              << (positional.size() - 1) << "\n";
    PrintUsage(std::cerr, argv0);
    return kExitUsage;
  }
  const absl::string_view expr = positional[1];
  if (subcommand == "eval") return RunEval(expr);
  if (subcommand == "check") return RunCheck(expr);
  if (subcommand == "compile") return RunCompile(expr);
  ABSL_CHECK(false) << "subcommand `" << subcommand << "` slipped the gate";
}

}  // namespace
}  // namespace celwasm::tools::cel

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  namespace cli = celwasm::tools::cel;
  if (argc < 2) {
    cli::PrintUsage(std::cerr, argv[0]);
    return cli::kExitUsage;
  }
  const absl::Span<char* const> args(argv, static_cast<std::size_t>(argc));
  if (cli::WantsHelp(args)) {
    cli::PrintUsage(std::cout, argv[0]);
    return cli::kExitOk;
  }
  const std::string subcommand = argv[1];
  if (!cli::IsKnownSubcommand(subcommand)) {
    std::cerr << "ERROR: unknown subcommand `" << subcommand << "`\n";
    cli::PrintUsage(std::cerr, argv[0]);
    return cli::kExitUsage;
  }

  auto positional = cli::ParseFlagsAfterSubcommand(args);
  if (!positional.has_value()) {
    cli::PrintUsage(std::cerr, argv[0]);
    return cli::kExitUsage;
  }
  return cli::Dispatch(subcommand, *positional, argv[0]);
}
