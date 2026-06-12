#include "e2e/fuzz/grammar_aggregates.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_scalars.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Tag suffix for production names, e.g. "int" / "list_int" /
// "map_string_int".  Derived from the canonical TypeKey but with
// `<`, `>`, `,` replaced by `_` so the strings are bare
// identifiers — safer in failure messages and easier to grep.
std::string Tag(const CelType& t) {
  std::string out = TypeKey(t);
  for (char& c : out) {
    if (c == '<' || c == '>' || c == ',') c = '_';
  }
  // Strip trailing underscores (from a closing `>`).
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

// The six scalar element types we admit inside aggregate
// targets.  Matches the scalar leaf type vocab.
const std::vector<CelType>& ScalarVocab() {
  static const auto* kV = new std::vector<CelType>{
      CelType::Bool(),   CelType::Int(),    CelType::Uint(),
      CelType::Double(), CelType::String(), CelType::Bytes(),
  };
  return *kV;
}

// Realistic K×V map combinations.  CEL admits int / uint / bool /
// string as map keys; we sample a representative subset rather
// than the full Cartesian product (24 entries) — every spec-
// admitted key kind appears at least once.  V can be any scalar.
struct MapKV {
  CelType k;
  CelType v;
};
const std::vector<MapKV>& MapVocab() {
  static const auto* kV = new std::vector<MapKV>{
      {CelType::String(), CelType::Int()},
      {CelType::String(), CelType::String()},
      {CelType::String(), CelType::Bool()},
      {CelType::Int(), CelType::Int()},
      {CelType::Bool(), CelType::Int()},
  };
  return *kV;
}

// Literal-only list leaves (depth-0 eligible) so L1's leaf
// coverage check passes.  Several leaf shapes per element type so
// depth-0 picks among varied list sizes; sources are fixed
// literals (no `%i`) that cel-cpp type-infers to the right
// element type.
void RegisterListLeaves(GrammarBuilder& b) {
  struct LeafSrc {
    CelType elt;
    const char* name_suffix = nullptr;
    const char* source = nullptr;
  };
  const LeafSrc leaves[] = {
      // Bool
      {CelType::Bool(), "1", "[true]"},
      {CelType::Bool(), "3", "[true, false, true]"},
      {CelType::Bool(), "5", "[true, false, true, false, true]"},
      // Int — variety of sizes and value ranges.
      {CelType::Int(), "1", "[0]"},
      {CelType::Int(), "3", "[1, 2, 3]"},
      {CelType::Int(), "5", "[1, 2, 3, 4, 5]"},
      {CelType::Int(), "primes", "[2, 3, 5, 7, 11, 13, 17, 19]"},
      {CelType::Int(), "10", "[10, 20, 30, 40, 50, 60, 70, 80, 90, 100]"},
      {CelType::Int(), "negs", "[-3, -2, -1, 0, 1, 2, 3]"},
      // Uint
      {CelType::Uint(), "1", "[0u]"},
      {CelType::Uint(), "3", "[1u, 2u, 3u]"},
      {CelType::Uint(), "5", "[1u, 2u, 3u, 4u, 5u]"},
      // Double
      {CelType::Double(), "1", "[0.0]"},
      {CelType::Double(), "3", "[1.5, 2.5, 3.5]"},
      {CelType::Double(), "5", "[0.0, 0.5, 1.0, 1.5, 2.0]"},
      // String — short fixed alphabet.
      {CelType::String(), "1", R"([""])"},
      {CelType::String(), "3", R"(["a", "b", "c"])"},
      {CelType::String(), "5",
       R"(["alpha", "beta", "gamma", "delta", "epsilon"])"},
      // Bytes
      {CelType::Bytes(), "1", R"([b""])"},
      {CelType::Bytes(), "3", R"([b"x", b"y", b"z"])"},
  };
  for (const LeafSrc& l : leaves) {
    b.Leaf(CelType::List(l.elt),
           absl::StrCat("list_", Tag(l.elt), "_leaf_", l.name_suffix),
           l.source);
  }
}

// Recursive list constructors of size 1, 2, 3, 5, 7, 10.  Each
// `%i` is filled by the type-T sub-walk so depth-N recursion can
// nest arbitrary expressions inside.  Sized literals (vs.
// variadic) keep format strings explicit, so L1 placeholder-
// consistency catches catalog typos; past size 3 the generic
// Repeated helper is used (typed builders cap at 3 args).
void RegisterListConstructors(GrammarBuilder& b) {
  for (const CelType& elt : ScalarVocab()) {
    const std::string tag = Tag(elt);
    const CelType list_t = CelType::List(elt);
    b.Unary(list_t, absl::StrCat("list_", tag, "_lit_1"), "[%0]", elt);
    b.Binary(list_t, absl::StrCat("list_", tag, "_lit_2"), "[%0, %1]", elt,
             elt);
    b.Ternary(list_t, absl::StrCat("list_", tag, "_lit_3"), "[%0, %1, %2]", elt,
              elt, elt);
    b.Repeated(list_t, absl::StrCat("list_", tag, "_lit_5"),
               "[%0, %1, %2, %3, %4]", elt, /*arity=*/5);
    b.Repeated(list_t, absl::StrCat("list_", tag, "_lit_7"),
               "[%0, %1, %2, %3, %4, %5, %6]", elt, 7);
    b.Repeated(list_t, absl::StrCat("list_", tag, "_lit_10"),
               "[%0, %1, %2, %3, %4, %5, %6, %7, %8, %9]", elt, 10);
  }
}

void RegisterMapConstructors(GrammarBuilder& b) {
  // Per-map leaf so depth-0 recursion has something to pick.
  // Source is a 1-entry literal whose K, V are leaf scalars
  // appropriate to the type.
  struct LeafSrc {
    CelType k;
    CelType v;
    const char* source = nullptr;
  };
  const LeafSrc leaves[] = {
      {CelType::String(), CelType::Int(), R"({"k": 0})"},
      {CelType::String(), CelType::String(), R"({"k": "v"})"},
      {CelType::String(), CelType::Bool(), R"({"k": true})"},
      {CelType::Int(), CelType::Int(), "{0: 0}"},
      {CelType::Bool(), CelType::Int(), "{true: 0}"},
  };
  for (const LeafSrc& l : leaves) {
    const std::string tag = absl::StrCat(Tag(l.k), "_", Tag(l.v));
    b.Leaf(CelType::Map(l.k, l.v), absl::StrCat("map_", tag, "_leaf"),
           l.source);
  }
  // Recursive 1-entry constructor.
  for (const MapKV& kv : MapVocab()) {
    const std::string tag = absl::StrCat(Tag(kv.k), "_", Tag(kv.v));
    b.Binary(CelType::Map(kv.k, kv.v), absl::StrCat("map_", tag, "_lit_1"),
             "{%0: %1}", kv.k, kv.v);
  }
}

// String functions that bridge string ↔ list<string>: `split`
// (string → list<string>) and `join` (list<string> → string).
// Total over their typed inputs (an empty separator splits into
// characters; join concatenates).  The scalar-returning string
// functions (contains/indexOf/matches/…) live in the scalar
// catalog.
void RegisterStringAggregateFunctions(GrammarBuilder& b) {
  const CelType ls = CelType::List(CelType::String());
  b.Binary(ls, "string_split", "(%0).split(%1)", CelType::String(),
           CelType::String());
  b.Unary(CelType::String(), "list_join", "(%0).join()", ls);
  b.Binary(CelType::String(), "list_join_sep", "(%0).join(%1)", ls,
           CelType::String());
}

void RegisterSizeProductions(GrammarBuilder& b) {
  for (const CelType& elt : ScalarVocab()) {
    b.Unary(CelType::Int(), absl::StrCat("size_list_", Tag(elt)), "size(%0)",
            CelType::List(elt));
  }
  for (const MapKV& kv : MapVocab()) {
    const std::string tag = absl::StrCat(Tag(kv.k), "_", Tag(kv.v));
    b.Unary(CelType::Int(), absl::StrCat("size_map_", tag), "size(%0)",
            CelType::Map(kv.k, kv.v));
  }
}

void RegisterInProductions(GrammarBuilder& b) {
  // _in_ over a list: element type must match the list's elt.
  for (const CelType& elt : ScalarVocab()) {
    b.Binary(CelType::Bool(), absl::StrCat("in_list_", Tag(elt)), "(%0 in %1)",
             elt, CelType::List(elt));
  }
  // _in_ over a map: probe must match the key type.
  for (const MapKV& kv : MapVocab()) {
    const std::string tag = absl::StrCat(Tag(kv.k), "_", Tag(kv.v));
    b.Binary(CelType::Bool(), absl::StrCat("in_map_", tag), "(%0 in %1)", kv.k,
             CelType::Map(kv.k, kv.v));
  }
}

void RegisterListComprehensions(GrammarBuilder& b) {
  // Each macro binds `v: <elt>` while generating the body.
  for (const CelType& elt : ScalarVocab()) {
    const std::string tag = Tag(elt);
    const CelType list_t = CelType::List(elt);

    // Predicate-style — result is Bool.
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_exists_list_", tag),
                    "(%0).exists(v, %1)", list_t, /*iter=*/{"v", elt},
                    /*body_type=*/CelType::Bool());
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_all_list_", tag),
                    "(%0).all(v, %1)", list_t, {"v", elt}, CelType::Bool());
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_exists_one_list_", tag),
                    "(%0).exists_one(v, %1)", list_t, {"v", elt},
                    CelType::Bool());

    // Filter — same shape as exists, but yields a list<T>.
    b.Comprehension(list_t, absl::StrCat("comp_filter_list_", tag),
                    "(%0).filter(v, %1)", list_t, {"v", elt}, CelType::Bool());

    // Map(v, f) — body type matches the result element type.
    // Same-type mapping only (T → T) to keep the catalog
    // bounded; cross-type maps are an easy follow-up (m30.C).
    b.Comprehension(list_t, absl::StrCat("comp_map_list_", tag),
                    "(%0).map(v, %1)", list_t, {"v", elt}, /*body_type=*/elt);
  }
}

void RegisterMapComprehensions(GrammarBuilder& b) {
  // For map<K,V>, the macro's iter_var is bound to the KEY (this
  // is the v1 single-iter form; the v2 two-iter form binds
  // key + value but introduces partiality — deferred).
  for (const MapKV& kv : MapVocab()) {
    const std::string tag = absl::StrCat(Tag(kv.k), "_", Tag(kv.v));
    const CelType map_t = CelType::Map(kv.k, kv.v);
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_exists_map_", tag),
                    "(%0).exists(k, %1)", map_t, /*iter=*/{"k", kv.k},
                    /*body_type=*/CelType::Bool());
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_all_map_", tag),
                    "(%0).all(k, %1)", map_t, {"k", kv.k}, CelType::Bool());
    b.Comprehension(CelType::Bool(), absl::StrCat("comp_exists_one_map_", tag),
                    "(%0).exists_one(k, %1)", map_t, {"k", kv.k},
                    CelType::Bool());
  }
}

// One-level-nested aggregate targets — `list<list<T>>`,
// `list<map<K,V>>`, `map<K, list<V>>`.  These exercise codegen
// paths flat aggregates never reach: a `HostListBacking` whose
// elements are themselves containers, a map value that is a list,
// a comprehension whose iter_var is a container.  The recursive
// `compare.cc::Compare` already verifies them element-by-element,
// so no harness change is needed — only the grammar productions.
//
// Kept to a hand-picked vocab (not the full nested Cartesian
// product) so the catalog stays bounded; every inner type is
// already a registered target, so L1's arg-type-reachability
// check passes.
void RegisterNestedAggregates(GrammarBuilder& b) {
  struct Nest {
    CelType type;
    const char* tag = nullptr;
    const char* leaf = nullptr;
    CelType inner;  // the element (list) or value (map) type
  };
  const CelType li = CelType::List(CelType::Int());
  const CelType ls = CelType::List(CelType::String());
  const CelType lb = CelType::List(CelType::Bool());
  const CelType msi = CelType::Map(CelType::String(), CelType::Int());
  const Nest nests[] = {
      {CelType::List(li), "list_list_int", "[[1, 2, 3]]", li},
      {CelType::List(ls), "list_list_string", R"([["a", "b"]])", ls},
      {CelType::List(lb), "list_list_bool", "[[true, false]]", lb},
      {CelType::List(msi), "list_map_string_int", R"([{"k": 0}])", msi},
      {CelType::Map(CelType::String(), li), "map_string_list_int",
       R"({"k": [1, 2, 3]})", li},
  };
  for (const Nest& n : nests) {
    // Leaf (depth-0) + 1- and 2-element constructors whose slots
    // are filled by the inner-aggregate sub-walk.
    b.Leaf(n.type, absl::StrCat(n.tag, "_leaf"), n.leaf);
    if (n.type.kind() == CelType::Kind::kList) {
      b.Unary(n.type, absl::StrCat(n.tag, "_lit_1"), "[%0]", n.inner);
      b.Binary(n.type, absl::StrCat(n.tag, "_lit_2"), "[%0, %1]", n.inner,
               n.inner);
    } else {
      // Map with a fixed string key and one value slot — a single
      // placeholder, so Unary.
      b.Unary(n.type, absl::StrCat(n.tag, "_lit_1"), R"({"k": %0})", n.inner);
    }
    // size() over the outer container.
    b.Unary(CelType::Int(), absl::StrCat("size_", n.tag), "size(%0)", n.type);
  }
  // Comprehensions whose iter_var is itself a container: the body
  // can call size()/_in_ on the inner aggregate.  Just the list-of-
  // list shapes — enough to exercise the container-iter_var path.
  b.Comprehension(CelType::Bool(), "comp_exists_list_list_int",
                  "(%0).exists(v, %1)", CelType::List(li), /*iter=*/{"v", li},
                  /*body_type=*/CelType::Bool());
  b.Comprehension(CelType::Bool(), "comp_all_list_list_int", "(%0).all(v, %1)",
                  CelType::List(li), {"v", li}, CelType::Bool());
}

}  // namespace

Grammar BuildGrammar() {
  GrammarBuilder b;

  // Layer the scalar productions first so the full grammar is a
  // strict superset — any scalar-catalog regression still lights up
  // through the same name.
  RegisterScalarProductions(b);

  // Then the aggregate additions.
  RegisterListLeaves(b);
  RegisterListConstructors(b);
  RegisterMapConstructors(b);
  RegisterSizeProductions(b);
  RegisterInProductions(b);
  RegisterListComprehensions(b);
  RegisterMapComprehensions(b);
  RegisterNestedAggregates(b);
  RegisterStringAggregateFunctions(b);

  Grammar g = std::move(b).Build();
  ABSL_CHECK_OK(g.Validate())
      << "full grammar failed L1 validation; the catalog in "
         "grammar_aggregates.cc is malformed";
  return g;
}

}  // namespace celwasm::fuzz
