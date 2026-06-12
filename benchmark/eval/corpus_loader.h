// Corpus loader for the comparative benchmark system.  Reads YAML
// files under benchmark/eval/corpus/, validates them per
// benchmark/DESIGN.md §6.3, and returns an in-memory `Cell` vector
// the bench mains iterate to register Google Benchmark cases.
//
// The loader is **comparator-neutral**: it parses each YAML literal
// into a `CelValueLiteral` (the typed-but-backend-agnostic value
// form) so the celwasm wrapper and the cel-cpp wrapper can each
// translate it to their own `Value` type at use time.  Keeping the
// loader free of either backend's `Value` is what lets the same
// corpus drive both binaries (the cel-cpp binary cannot link any
// first-party `//eval/...` target — see DESIGN.md §5.1, §8).
//
// Activation entry forms the loader accepts:
//
//   * Scalar:    `a: { type: int, value: 42 }` (int|uint|double|bool|
//     string|bytes|null).
//   * List (explicit):  `xs: { type: list, elem: int, values: [1, 2, 3] }`
//     — `elem` is any scalar kind; `values` items parse per the elem
//     kind with the same rules as a scalar `value`.
//   * List (generated): `xs: { type: list, elem: int, gen: { range: N } }`
//     expands to `[0 .. N-1]`;
//     `perms: { type: list, elem: string,
//               gen: { template: "perm%07d", count: N } }`
//     expands the single %d-family directive (`%d` or `%0<width>d`,
//     zero-padded) with the index `[0 .. N-1]`.  Generated lists are
//     expanded EAGERLY at load time — a `range: 1000000` cell
//     materialises one million literals; bench startup memory is an
//     accepted cost (the bound-list benches need lists that big).
//   * Message:   `c: { type: message,
//                      message_type: "celwasm.testdata.Customer",
//                      textproto: 'name: "Ada"' }`
//     — the loader stores `message_type` + the raw textproto TEXT and
//     validates only that both are non-empty.  It does NOT parse the
//     textproto (that would need a protobuf dep and break comparator
//     neutrality); each bench main parses it at registration time.
//
// Current limitations (documented up front so callers don't have to
// rediscover them):
//
//   * **Map activations are not supported.**  `CelValueLiteral` models
//     maps, but `LoadCorpus` rejects `type: map` activation entries.
//     List elements are scalar-only (no list-of-list / list-of-message).
//
//   * **Source-variable check is a heuristic, not a CEL parser.**
//     The validator that asserts every variable in `source` is bound
//     in `activation` (and vice versa) does a simple identifier scan,
//     skipping a small allowlist of CEL literals and reserved words
//     (`true`, `false`, `null`, `in`) plus any identifier immediately
//     preceded by `.` (a field selection like `c.name` must not
//     demand a binding for `name`).  It is good enough for the
//     short arithmetic / comparison expressions in the phase-1
//     corpus, but it will misclassify identifiers that double as
//     function or macro names (e.g. `size`, `has`, `matches`).
//     Affected cells can opt out with the `skip-source-check` tag.

#ifndef CELWASM_BENCHMARK_EVAL_CORPUS_LOADER_H_
#define CELWASM_BENCHMARK_EVAL_CORPUS_LOADER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celbench {

// A typed value literal — the comparator-neutral form of any value
// the corpus can carry (activation entry or `expected`).  Each
// instance is a discriminated union over the CEL primitive types
// plus the two aggregate kinds; the populated payload field is
// determined by `kind`.
struct CelValueLiteral {
  enum class Kind : std::uint8_t {
    kInt,
    kUint,
    kDouble,
    kBool,
    kString,
    kBytes,
    kNull,
    kList,
    kMap,
    kMessage,
  };

  Kind kind = Kind::kNull;

  // Exactly one of the payload fields below is meaningful per `kind`:
  //   kInt     -> int_value
  //   kUint    -> uint_value
  //   kDouble  -> double_value
  //   kBool    -> bool_value
  //   kString  -> string_value
  //   kBytes   -> string_value  (raw bytes; UTF-8 not assumed)
  //   kNull    -> (none)
  //   kList    -> list_value (+ list_elem_kind)
  //   kMap     -> map_value  (insertion-ordered key/value pairs)
  //   kMessage -> string_value (raw textproto text) + message_type
  std::int64_t int_value = 0;
  std::uint64_t uint_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  std::string string_value;
  std::vector<CelValueLiteral> list_value;
  std::vector<std::pair<CelValueLiteral, CelValueLiteral>> map_value;

  // kMessage only: the fully-qualified proto message type name the
  // textproto in `string_value` parses as (e.g.
  // "celwasm.testdata.Customer").
  std::string message_type;

  // kList only: the scalar kind of every element, stamped by the
  // loader from the YAML `elem` field.  Lets a backend derive the
  // declared `list<T>` type without inspecting elements (which an
  // explicit empty `values` list doesn't have).
  Kind list_elem_kind = Kind::kNull;
};

// A single named activation entry: the variable name as it appears
// in the CEL source, paired with the literal value to bind to it.
struct ActivationEntry {
  std::string name;
  CelValueLiteral value;
};

// One row of the corpus — a single benchmark cell.  `(surface, id)`
// is the join key the reporter uses to align celwasm and cel-cpp
// timings (see DESIGN.md §12).
struct Cell {
  std::string surface;
  std::string id;
  std::string source;
  std::vector<ActivationEntry> activation;
  CelValueLiteral expected;
  std::vector<std::string> tags;
};

// Loads every YAML in `yaml_paths`, validates them per DESIGN.md §6.3,
// and returns a flat vector of Cells sorted by `(surface, id)` for
// deterministic registration order.
//
// Returns `absl::InvalidArgumentError` (with a message naming the
// file + cell id + rule violated) on any of:
//   * YAML parse failure or missing required field.
//   * `surface` inside the YAML disagrees with the file basename.
//   * Empty `id`, `id` containing whitespace, or `id` containing '/'
//     (these break `--benchmark_filter` regex usage).
//   * Duplicate `(surface, id)` pair across all files.
//   * Unknown `expected.type` / activation entry type.
//   * Malformed list entry: missing or non-scalar `elem`; both or
//     neither of `values` / `gen`; `gen` with both or neither of
//     `range` / (`template` + `count`); `range` / `count` < 1; a
//     `values` item that fails the elem-kind parse; a `template`
//     without exactly one %d-family directive.
//   * Malformed message entry: missing or empty `message_type` /
//     `textproto`.  (Textproto VALIDITY is checked by the bench
//     mains at registration — the loader has no protobuf dep.)
//   * Variable mentioned in `source` is not in `activation`
//     (unless the cell carries the `skip-source-check` tag).
//   * Variable mentioned in `activation` is not in `source`.
//
// Returns `absl::NotFoundError` if a path in `yaml_paths` cannot be
// opened.
absl::StatusOr<std::vector<Cell>> LoadCorpus(
    absl::Span<const std::string> yaml_paths);

// Stable, run-to-run-identical string form of a literal.  Used by
// the parity check (DESIGN.md §11) to compare celwasm's output
// against cel-cpp's against the YAML-declared `expected` without
// host-specific formatting drift.
//
// Doubles are serialised via `std::to_chars` shortest-round-trip
// (`chars_format::general`) — the same convention `cel_runtime`'s
// `cel_convert.{h,cc}` adopted (see `runtime/cel_convert_double_format.cc`
// for rationale).  That keeps every comparator's canonical form
// byte-identical for the same `double`.
std::string CanonicalForm(const CelValueLiteral& v);

// Bounded rendering of a (possibly long) string/bytes payload for a
// Google Benchmark result label: payloads up to 64 bytes pass through
// verbatim; longer ones render as the first 64 bytes +
// "...(len=<N>)".  Both bench mains use this for their string/bytes
// `result=` labels, so the truncation is byte-identical across
// comparators and the labels stay mechanically diffable.
std::string AbbreviateForLabel(absl::string_view s);

}  // namespace celbench

#endif  // CELWASM_BENCHMARK_EVAL_CORPUS_LOADER_H_
