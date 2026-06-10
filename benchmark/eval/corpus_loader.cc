#include "benchmark/eval/corpus_loader.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "yaml-cpp/yaml.h"

namespace celbench {
namespace {

// ---------- filename helpers -------------------------------------------------

// Returns the file basename minus its extension.  `arithmetic.yaml`
// -> `arithmetic`; `/tmp/foo/arithmetic.yaml` -> `arithmetic`.
std::string FileStem(absl::string_view path) {
  size_t slash = path.find_last_of('/');
  absl::string_view base =
      (slash == absl::string_view::npos) ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot == absl::string_view::npos) return std::string(base);
  return std::string(base.substr(0, dot));
}

// ---------- value-literal parsing -------------------------------------------

// Maps the YAML `type: <name>` field to the typed-literal kind.
absl::StatusOr<CelValueLiteral::Kind> KindFromString(absl::string_view name) {
  if (name == "int") return CelValueLiteral::Kind::kInt;
  if (name == "uint") return CelValueLiteral::Kind::kUint;
  if (name == "double") return CelValueLiteral::Kind::kDouble;
  if (name == "bool") return CelValueLiteral::Kind::kBool;
  if (name == "string") return CelValueLiteral::Kind::kString;
  if (name == "bytes") return CelValueLiteral::Kind::kBytes;
  if (name == "null") return CelValueLiteral::Kind::kNull;
  if (name == "list") return CelValueLiteral::Kind::kList;
  if (name == "map") return CelValueLiteral::Kind::kMap;
  return absl::InvalidArgumentError(absl::StrCat("unknown type: ", name));
}

// Parses a YAML `{ type: int, value: 42 }`-shaped node into a
// CelValueLiteral.  Scalar-only; list/map are flagged for the
// caller to extend (per loader header doc).
absl::StatusOr<CelValueLiteral> ParseLiteral(const YAML::Node& n,
                                             absl::string_view ctx) {
  if (!n.IsMap() || !n["type"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(ctx, ": expected { type, value } mapping"));
  }
  auto kind_or = KindFromString(n["type"].as<std::string>());
  if (!kind_or.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat(ctx, ": ", kind_or.status().message()));
  }
  CelValueLiteral lit;
  lit.kind = *kind_or;
  // `null` carries no `value:` field; every other kind requires one.
  if (lit.kind == CelValueLiteral::Kind::kNull) return lit;

  if (!n["value"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(ctx, ": missing `value` field"));
  }
  const YAML::Node& v = n["value"];
  switch (lit.kind) {
    case CelValueLiteral::Kind::kInt:
      lit.int_value = v.as<std::int64_t>();
      break;
    case CelValueLiteral::Kind::kUint:
      lit.uint_value = v.as<std::uint64_t>();
      break;
    case CelValueLiteral::Kind::kDouble:
      lit.double_value = v.as<double>();
      break;
    case CelValueLiteral::Kind::kBool:
      lit.bool_value = v.as<bool>();
      break;
    case CelValueLiteral::Kind::kString:
    case CelValueLiteral::Kind::kBytes:
      lit.string_value = v.as<std::string>();
      break;
    case CelValueLiteral::Kind::kList:
    case CelValueLiteral::Kind::kMap:
      // Header doc calls this out as a deferred extension; refuse
      // loudly rather than silently producing an empty list/map.
      return absl::UnimplementedError(absl::StrCat(
          ctx, ": list/map activation literals not yet supported"));
    case CelValueLiteral::Kind::kNull:
      break;  // unreachable; handled above.
  }
  return lit;
}

// ---------- source-variable scan --------------------------------------------

// Returns true iff `name` is a CEL reserved word or built-in literal
// the source-variable heuristic should ignore.  The scan flags
// anything else as a candidate variable; the validator then
// cross-checks against the activation list.  See header doc for the
// known limitation (function/macro names like `size`, `has` collide).
bool IsReservedIdent(absl::string_view name) {
  static const std::set<std::string>* const kReserved =
      new std::set<std::string>{
          "true",
          "false",
          "null",
          "in",
      };
  return kReserved->count(std::string(name)) > 0;
}

// Walks `source` left-to-right; emits every identifier-shaped token
// that isn't reserved.  An identifier is `[A-Za-z_][A-Za-z0-9_]*`
// and must NOT start immediately after an alphanumeric character —
// a letter glued to a digit is a literal suffix (`1u`, `0x2A`,
// `2.5e3`), not a variable.  Skips quoted strings (single/double).
// Treats anything else as punctuation.  Good enough for the current
// corpus shape — see header doc.
std::set<std::string> ScanIdents(absl::string_view source) {
  std::set<std::string> out;
  size_t i = 0;
  const size_t n = source.size();
  while (i < n) {
    char c = source[i];
    const bool after_alnum =
        i > 0 && (std::isalnum(static_cast<unsigned char>(source[i - 1])) != 0);
    if (c == '\'' || c == '"') {
      // Skip over the string literal — anything up to the next
      // matching, non-escaped quote.  CEL string escapes are
      // backslash-based; treat \\X as one unit.
      char quote = c;
      ++i;
      while (i < n && source[i] != quote) {
        if (source[i] == '\\' && i + 1 < n) {
          i += 2;
        } else {
          ++i;
        }
      }
      if (i < n) ++i;  // consume closing quote
      continue;
    }
    if (((std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_') &&
        !after_alnum) {
      size_t start = i;
      while (i < n) {
        char k = source[i];
        if ((std::isalnum(static_cast<unsigned char>(k)) != 0) || k == '_') {
          ++i;
        } else {
          break;
        }
      }
      std::string tok(source.substr(start, i - start));
      if (!IsReservedIdent(tok)) {
        out.insert(std::move(tok));
      }
      continue;
    }
    ++i;
  }
  return out;
}

// ---------- per-file parse + validate ---------------------------------------

// Reads `path` into memory and runs it through the YAML parser.
absl::StatusOr<YAML::Node> ParseYamlFile(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return absl::NotFoundError(absl::StrCat("cannot open corpus file: ", path));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  try {
    return YAML::Load(buf.str());
  } catch (const YAML::Exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": YAML parse error: ", e.what()));
  }
}

// Checks top-level shape (surface name, file-basename agreement,
// presence of cells sequence) and returns the declared surface name.
absl::StatusOr<std::string> ValidateTopLevel(const std::string& path,
                                             const YAML::Node& doc) {
  if (!doc.IsMap()) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": top-level must be a map"));
  }
  if (!doc["surface"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": missing `surface` field"));
  }
  auto surface = doc["surface"].as<std::string>();
  std::string stem = FileStem(path);
  if (stem != surface) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": surface `", surface,
                     "` disagrees with file basename `", stem, "`"));
  }
  if (!doc["cells"] || !doc["cells"].IsSequence()) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": missing or non-sequence `cells` field"));
  }
  return surface;
}

// Validates the cell id per DESIGN.md §6.3 (non-empty, no whitespace,
// no `/`).
absl::Status ValidateCellId(const std::string& path, const std::string& id) {
  if (id.empty()) {
    return absl::InvalidArgumentError(absl::StrCat(path, ": empty `id`"));
  }
  for (char c : id) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      return absl::InvalidArgumentError(
          absl::StrCat(path, ": cell id `", id, "` contains whitespace"));
    }
    if (c == '/') {
      return absl::InvalidArgumentError(
          absl::StrCat(path, ": cell id `", id, "` contains `/`"));
    }
  }
  return absl::OkStatus();
}

// Cross-checks the source-identifier scan against the activation
// list, unless the cell opts out via `skip-source-check`.
absl::Status ValidateSourceActivationParity(const std::string& path,
                                            const Cell& cell) {
  for (const auto& t : cell.tags) {
    if (t == "skip-source-check") return absl::OkStatus();
  }
  std::set<std::string> src_idents = ScanIdents(cell.source);
  std::set<std::string> act_names;
  for (const auto& ae : cell.activation) {
    act_names.insert(ae.name);
  }
  for (const auto& s : src_idents) {
    if (act_names.count(s) == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat(path, " cell `", cell.id,
                       "`: source references unbound variable `", s, "`"));
    }
  }
  for (const auto& a : act_names) {
    if (src_idents.count(a) == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat(path, " cell `", cell.id,
                       "`: activation has unused variable `", a, "`"));
    }
  }
  return absl::OkStatus();
}

// Parses the optional `activation` block (a YAML map of var-name to
// literal) into the Cell's activation vector.  Absent or empty block
// is a no-op.
absl::Status ParseActivationInto(const std::string& path, Cell* cell,
                                 const YAML::Node& cn) {
  if (!cn["activation"] || !cn["activation"].IsMap()) return absl::OkStatus();
  for (const auto& kv : cn["activation"]) {
    ActivationEntry ae;
    ae.name = kv.first.as<std::string>();
    auto lit_or =
        ParseLiteral(kv.second, absl::StrCat(path, " cell `", cell->id,
                                             "` activation `", ae.name, "`"));
    if (!lit_or.ok()) return lit_or.status();
    ae.value = std::move(*lit_or);
    cell->activation.push_back(std::move(ae));
  }
  return absl::OkStatus();
}

// Pulls the optional `tags` sequence into the Cell.
void ParseTagsInto(Cell* cell, const YAML::Node& cn) {
  if (!cn["tags"] || !cn["tags"].IsSequence()) return;
  for (const auto& t : cn["tags"]) {
    cell->tags.push_back(t.as<std::string>());
  }
}

// Parses one cell's YAML node into a Cell.  All field-level
// validation lives here; cross-cell duplicate detection lives in
// the caller.
absl::StatusOr<Cell> ParseCellNode(const std::string& path,
                                   const std::string& surface,
                                   const YAML::Node& cn) {
  Cell cell;
  cell.surface = surface;
  if (!cn["id"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": cell missing `id`"));
  }
  cell.id = cn["id"].as<std::string>();
  if (auto s = ValidateCellId(path, cell.id); !s.ok()) return s;
  if (!cn["source"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": cell `", cell.id, "` missing `source`"));
  }
  cell.source = cn["source"].as<std::string>();
  if (auto s = ParseActivationInto(path, &cell, cn); !s.ok()) return s;
  if (!cn["expected"]) {
    return absl::InvalidArgumentError(
        absl::StrCat(path, ": cell `", cell.id, "` missing `expected`"));
  }
  auto exp_or = ParseLiteral(
      cn["expected"], absl::StrCat(path, " cell `", cell.id, "` expected"));
  if (!exp_or.ok()) return exp_or.status();
  cell.expected = std::move(*exp_or);
  ParseTagsInto(&cell, cn);
  if (auto s = ValidateSourceActivationParity(path, cell); !s.ok()) return s;
  return cell;
}

absl::Status ParseOneFile(const std::string& path, std::vector<Cell>* out) {
  auto doc_or = ParseYamlFile(path);
  if (!doc_or.ok()) return doc_or.status();
  auto surface_or = ValidateTopLevel(path, *doc_or);
  if (!surface_or.ok()) return surface_or.status();
  std::set<std::string> seen_ids_in_file;
  for (const auto& cn : (*doc_or)["cells"]) {
    auto cell_or = ParseCellNode(path, *surface_or, cn);
    if (!cell_or.ok()) return cell_or.status();
    if (!seen_ids_in_file.insert(cell_or->id).second) {
      return absl::InvalidArgumentError(absl::StrCat(
          path, ": duplicate cell id `", cell_or->id, "` within file"));
    }
    out->push_back(std::move(*cell_or));
  }
  return absl::OkStatus();
}

// ---------- double formatting (mirrors cel_runtime) -------------------------

// `std::to_chars(double, general)` shortest-round-trip — same as
// `runtime/cel_convert_double_format.cc:117`.  32 bytes is ample for
// any finite double's shortest form (worst case ~25 chars).
std::string FormatDouble(double v) {
  constexpr int kBufSize = 32;
  char buf[kBufSize];
  std::to_chars_result r =
      std::to_chars(buf, buf + kBufSize, v, std::chars_format::general);
  if (r.ec != std::errc()) {
    return "<double-format-error>";
  }
  return {buf, static_cast<size_t>(r.ptr - buf)};
}

}  // namespace

// ---------- public API ------------------------------------------------------

// Public declaration lives in corpus_loader.h; clang-tidy's include
// path for the header is incomplete in compile_commands.json and it
// mistakes this for a static candidate.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::StatusOr<std::vector<Cell>> LoadCorpus(
    absl::Span<const std::string> yaml_paths) {
  std::vector<Cell> all;
  for (const auto& p : yaml_paths) {
    if (auto s = ParseOneFile(p, &all); !s.ok()) return s;
  }

  // Cross-file unique-(surface, id) check.  A per-file uniqueness
  // check ran in ParseOneFile; this catches the same (surface, id)
  // appearing in two different paths (e.g. a surface split across
  // files).
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& c : all) {
    auto key = std::make_pair(c.surface, c.id);
    if (!seen.insert(key).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate (surface, id) = (`", c.surface, "`, `", c.id,
                       "`) across files"));
    }
  }

  // Deterministic order — (surface, id) ascending — so registration
  // order is reproducible run-to-run.
  std::sort(all.begin(), all.end(), [](const Cell& a, const Cell& b) {
    if (a.surface != b.surface) return a.surface < b.surface;
    return a.id < b.id;
  });
  return all;
}

namespace {

// Renders a list literal as `[e1, e2, …]` using the same canonical
// form recursively for each element.
std::string FormatList(const std::vector<CelValueLiteral>& list);

// Renders a map literal as `{k1: v1, k2: v2, …}` with entries sorted
// by their canonical key string for run-to-run stability.
std::string FormatMap(
    const std::vector<std::pair<CelValueLiteral, CelValueLiteral>>& map);

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string CanonicalForm(const CelValueLiteral& v) {
  switch (v.kind) {
    case CelValueLiteral::Kind::kInt:
      return std::to_string(v.int_value);
    case CelValueLiteral::Kind::kUint:
      return absl::StrCat(v.uint_value, "u");
    case CelValueLiteral::Kind::kDouble:
      return FormatDouble(v.double_value);
    case CelValueLiteral::Kind::kBool:
      return v.bool_value ? "true" : "false";
    case CelValueLiteral::Kind::kString:
      // Quoted to disambiguate from `null`, `true`, etc.  Embedded
      // quotes/backslashes pass through unescaped — parity is
      // value-byte-equal so the byte-identical string trivially
      // matches; this isn't a JSON encoder.
      return absl::StrCat("\"", v.string_value, "\"");
    case CelValueLiteral::Kind::kBytes:
      return absl::StrCat("b\"", v.string_value, "\"");
    case CelValueLiteral::Kind::kNull:
      return "null";
    case CelValueLiteral::Kind::kList:
      return FormatList(v.list_value);
    case CelValueLiteral::Kind::kMap:
      return FormatMap(v.map_value);
  }
  return "<unknown-kind>";  // unreachable; switch covers all enum values.
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string AbbreviateForLabel(absl::string_view s) {
  constexpr size_t kMaxLabelPayload = 64;
  if (s.size() <= kMaxLabelPayload) return std::string(s);
  return absl::StrCat(s.substr(0, kMaxLabelPayload), "...(len=", s.size(), ")");
}

namespace {

std::string FormatList(const std::vector<CelValueLiteral>& list) {
  std::string s = "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i != 0) s += ", ";
    s += CanonicalForm(list[i]);
  }
  s += "]";
  return s;
}

std::string FormatMap(
    const std::vector<std::pair<CelValueLiteral, CelValueLiteral>>& map) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(map.size());
  for (const auto& kv : map) {
    entries.emplace_back(CanonicalForm(kv.first), CanonicalForm(kv.second));
  }
  std::sort(entries.begin(), entries.end());
  std::string s = "{";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) s += ", ";
    s += entries[i].first;
    s += ": ";
    s += entries[i].second;
  }
  s += "}";
  return s;
}

}  // namespace

}  // namespace celbench
