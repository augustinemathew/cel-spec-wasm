#include "compiler_v2/codegen/resolve_pass.h"

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/expr.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

namespace {

// Walks every `kConst` node in the AST and asserts its annotation was
// populated with a non-unknown `Repr`.  A kConst missing a type_map entry
// — or carrying a type that `ReprOf` can't map — would cause codegen to
// emit garbage rodata bytes (every literal is bound straight into
// `.rodata` via its Repr).  Per CLAUDE.md we never `DCHECK` an invariant
// violation: a silently miscompiled release build is strictly worse than
// a crash that names the offending node.
class KConstReprAudit : public cel::AstVisitorBase {
 public:
  explicit KConstReprAudit(const WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitConst(const cel::Expr& expr,
                      const cel::Constant& /*constant*/) override {
    const NodeAnnotation* ann = annotations_.Find(expr.id());
    ABSL_CHECK(ann != nullptr)
        << "ResolvePass: kConst node id=" << expr.id()
        << " has no NodeAnnotation (type_map missing this id)";
    ABSL_CHECK(ann->repr != Repr::kUnknown)
        << "ResolvePass: kConst node id=" << expr.id()
        << " has Repr::kUnknown (type_map type not mappable by ReprOf)";
  }

 private:
  const WasmAnnotations& annotations_;
};

// Walks every `kIdentExpr` in the AST, interns the name into a dense
// table (`local_index` 0, 1, 2, ... in first-seen order), and writes
// the index onto the node's `NodeAnnotation::local_index`.  Maintains
// a scope stack so comprehension iter / accu names shadow outer
// bindings (M5.B Slice A).
//
// `variables_` accumulates one entry per distinct *binding* — free
// variables AND comprehension-scope iter / accu vars.  Each entry's
// `kind` tells the downstream pipeline whether it's an
// Activation-bound free variable (set in the function prelude) or a
// comprehension-scope local (set by the comprehension's loop
// prologue).
//
// Per the cel-cpp probe (2026-05-17): cel-cpp's comprehension macros
// emit `accu_var = "@result"` and `iter_var` per the user-written
// name (`v`, `k`, etc., or `"#unused"` for `cel.bind`).  Nested
// comprehensions in cel-cpp may use suffixed names (`@result0`,
// `@result1`, …); the scope stack accommodates either by storing a
// per-frame name→binding map and walking innermost-first.
//
// Scope discipline per ast_traverse.cc:250-269 (subexpression order
// ITER_RANGE → ACCU_INIT → LOOP_CONDITION → LOOP_STEP → RESULT):
//   - `iter_range`, `accu_init` evaluated in the OUTER scope (no
//     iter / accu binding yet).
//   - `loop_condition`, `loop_step` evaluated in the inner scope
//     with iter_var (+ iter_var2 if non-empty) + accu_var bound.
//   - `result` evaluated in an inner scope with ONLY accu_var
//     bound — iter binding has been popped.
//
// Comprehension-scope idents get `ann.scope_id = depth` where depth
// is the 1-based comprehension nesting count.  Free-variable idents
// keep `scope_id = 0`.  Downstream visitors that semantically apply
// only to free vars (MapOriginVisitor / ListOriginVisitor /
// AttributePathResolver) use `scope_id == 0` as the discriminator.
class ScopedIdentResolver : public cel::AstVisitorBase {
 public:
  ScopedIdentResolver(WasmAnnotations& annotations,
                      std::vector<ResolvedVariable>& variables,
                      uint32_t& max_scope_id)
      : annotations_(annotations),
        variables_(variables),
        max_scope_id_(max_scope_id) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    NodeAnnotation& ann = annotations_[expr.id()];
    // Repr should be populated from the checker type_map by the
    // first pass.  If it isn't, the checker accepted an ident whose
    // type we can't map — invariant violation, not a recoverable
    // runtime error.
    ABSL_CHECK(ann.repr != Repr::kUnknown)
        << "ResolvePass: kIdent node id=" << expr.id() << " name=`"
        << ident.name() << "` has Repr::kUnknown "
        << "(checker left the type_map entry absent or non-mappable)";

    // Walk scope stack innermost-first.  Inner binding wins (shadow).
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->frame.find(ident.name());
      if (found != it->frame.end()) {
        const uint32_t local_index = found->second;
        ResolvedVariable& v = variables_[local_index];
        if (v.repr == Repr::kUnknown) {
          // First reference inside the comprehension body resolves
          // the binding's Repr from the checker's annotation on
          // this kIdent.  Subsequent references CHECK match.
          v.repr = ann.repr;
        } else {
          ABSL_CHECK(v.repr == ann.repr)
              << "ResolvePass: kIdent name=`" << ident.name()
              << "` (scope-bound) appears with mismatched Repr ("
              << ReprName(v.repr) << " vs " << ReprName(ann.repr) << ")";
        }
        ann.local_index = local_index;
        ann.scope_id = it->depth;
        return;
      }
    }
    // Free-variable fall-through — flat intern table.
    auto it = free_name_to_index_.find(ident.name());
    uint32_t local_index;
    if (it == free_name_to_index_.end()) {
      local_index = static_cast<uint32_t>(variables_.size());
      variables_.push_back(
          ResolvedVariable{ident.name(), local_index, ann.repr,
                           ResolvedVariableKind::kFreeVariable});
      free_name_to_index_.emplace(ident.name(), local_index);
    } else {
      local_index = it->second;
      ABSL_CHECK(variables_[local_index].repr == ann.repr)
          << "ResolvePass: kIdent name=`" << ident.name()
          << "` appears with mismatched Repr ("
          << ReprName(variables_[local_index].repr) << " vs "
          << ReprName(ann.repr) << ")";
    }
    ann.local_index = local_index;
    ann.scope_id = 0;
  }

  // M5.B Slice E: for list-source comprehensions, iter_var's wasm
  // local holds a moving pointer into the list payload (no
  // workspace slot needed — `kComprehensionIter` tag).  For
  // map-source comprehensions, iter_var binds to the CURRENT KEY,
  // which the runtime `cel_map_iter_key_at` helper writes into a
  // dedicated workspace slot each iteration.  Peek at the
  // iter_range's repr (annotated by the checker via
  // PopulateAnnotations before ResolvePass runs) to pick the right
  // allocation lifecycle.  Map-source iter_vars share the
  // `kComprehensionAccu` lifecycle in LayoutPass — they get a
  // workspace slot and are excluded from EmitVariablePrelude /
  // cel.abi.variables[].
  ResolvedVariableKind IterKindForRange(const cel::ComprehensionExpr& comp) {
    const NodeAnnotation* range_pre = annotations_.Find(comp.iter_range().id());
    const bool map_source =
        range_pre != nullptr && range_pre->repr == Repr::kMap;
    return map_source ? ResolvedVariableKind::kComprehensionAccu
                      : ResolvedVariableKind::kComprehensionIter;
  }

  void PreVisitComprehension(const cel::Expr& expr,
                             const cel::ComprehensionExpr& comp) override {
    // Allocate iter / iter2 / accu bindings up front.  Their Reprs
    // start at kUnknown and resolve lazily on first kIdent
    // reference in the loop body (PostVisitIdent above).
    CompFrame f;
    f.iter_var = std::string(comp.iter_var());
    f.iter_var2 = std::string(comp.iter_var2());
    f.accu_var = std::string(comp.accu_var());
    f.iter_local_index = static_cast<uint32_t>(variables_.size());
    variables_.push_back(ResolvedVariable{f.iter_var, f.iter_local_index,
                                          Repr::kUnknown,
                                          IterKindForRange(comp)});
    if (!f.iter_var2.empty()) {
      // Two-iter-var (Slice F): codegen owns the index counter /
      // value workspace; LayoutPass treats both as
      // `kComprehensionIter` (loop-prologue-set).
      f.iter_local_index2 = static_cast<uint32_t>(variables_.size());
      variables_.push_back(
          ResolvedVariable{f.iter_var2, f.iter_local_index2, Repr::kUnknown,
                           ResolvedVariableKind::kComprehensionIter});
    }
    f.accu_local_index = static_cast<uint32_t>(variables_.size());
    variables_.push_back(
        ResolvedVariable{f.accu_var, f.accu_local_index, Repr::kUnknown,
                         ResolvedVariableKind::kComprehensionAccu});
    // Stamp the per-comp binding indices on the comp node's
    // NodeAnnotation so codegen looks them up by expr_id, not by
    // name.  Name-based lookup conflates nested same-name accu_vars
    // — cel-cpp uses "@result" at every depth in the standard
    // macros, so the inner's name would resolve to the outer's
    // LaidOutVariable entry (caught by the 2026-05-17 nested probe).
    NodeAnnotation& ann = annotations_[expr.id()];
    ann.comp_iter_local_index = f.iter_local_index;
    ann.comp_accu_local_index = f.accu_local_index;
    comp_frames_.push_back(std::move(f));
  }

  void PreVisitComprehensionSubexpression(
      const cel::Expr& /*expr*/, const cel::ComprehensionExpr& /*comp*/,
      cel::ComprehensionArg arg) override {
    CompFrame& f = comp_frames_.back();
    if (arg == cel::ComprehensionArg::LOOP_CONDITION) {
      // Enter the inner (iter+accu) scope for cond + step.
      const uint32_t depth = static_cast<uint32_t>(scopes_.size()) + 1;
      max_scope_id_ = std::max(max_scope_id_, depth);
      ScopeFrame frame;
      frame.depth = depth;
      frame.frame.emplace(f.iter_var, f.iter_local_index);
      if (!f.iter_var2.empty()) {
        frame.frame.emplace(f.iter_var2, f.iter_local_index2);
      }
      frame.frame.emplace(f.accu_var, f.accu_local_index);
      scopes_.push_back(std::move(frame));
    } else if (arg == cel::ComprehensionArg::RESULT) {
      // Enter the inner (accu-only) scope for `result`.
      const uint32_t depth = static_cast<uint32_t>(scopes_.size()) + 1;
      max_scope_id_ = std::max(max_scope_id_, depth);
      ScopeFrame frame;
      frame.depth = depth;
      frame.frame.emplace(f.accu_var, f.accu_local_index);
      scopes_.push_back(std::move(frame));
    }
  }

  void PostVisitComprehensionSubexpression(
      const cel::Expr& /*expr*/, const cel::ComprehensionExpr& /*comp*/,
      cel::ComprehensionArg arg) override {
    // Both LOOP_STEP and RESULT close their pushed scope.  The
    // LOOP_CONDITION pre-push is balanced by LOOP_STEP's pop because
    // the same frame covers both subexpressions per spec; we
    // therefore do nothing on PostVisit(LOOP_CONDITION).
    if (arg == cel::ComprehensionArg::LOOP_STEP ||
        arg == cel::ComprehensionArg::RESULT) {
      ABSL_CHECK(!scopes_.empty())
          << "ResolvePass: scope underflow at comprehension subexpression "
          << static_cast<int>(arg);
      scopes_.pop_back();
    }
  }

  void PostVisitComprehension(const cel::Expr& /*expr*/,
                              const cel::ComprehensionExpr& /*comp*/) override {
    ABSL_CHECK(!comp_frames_.empty())
        << "ResolvePass: comprehension frame underflow";
    comp_frames_.pop_back();
  }

 private:
  struct ScopeFrame {
    absl::flat_hash_map<std::string, uint32_t> frame;
    uint32_t depth = 0;
  };
  struct CompFrame {
    std::string iter_var;
    std::string iter_var2;
    std::string accu_var;
    uint32_t iter_local_index = 0;
    uint32_t iter_local_index2 = 0;
    uint32_t accu_local_index = 0;
  };

  WasmAnnotations& annotations_;
  std::vector<ResolvedVariable>& variables_;
  uint32_t& max_scope_id_;
  absl::flat_hash_map<std::string, uint32_t> free_name_to_index_;
  std::vector<ScopeFrame> scopes_;
  std::vector<CompFrame> comp_frames_;
};

// Walks kIdent + kSelect post-order.  Each node gets its attribute
// path computed as `(root_variable, qualifiers)` and interned into
// `attributes_` — duplicate paths share an id.  The node's
// `NodeAnnotation::attribute_id` is stamped with the interned id.
//
// Invariant: id 0 is the sentinel ("no attribute"); callers push it
// at index 0 before running the traversal.  Real entries start at 1.
//
// At `cel_get_field` time, the trampoline reads the OPERAND'S
// attribute_id (via `ctx.layout.annotations.Find(sel.operand().id())
// ->attribute_id`) and appends the select's field name — see
// `BuildAttributeForSelect` in `cel_host.cc`.  So this visitor's
// job is: stamp each path-bearing node with its OWN path id; the
// codegen uses operand's id at emission.
class AttributePathResolver : public cel::AstVisitorBase {
 public:
  AttributePathResolver(WasmAnnotations& annotations,
                        std::vector<AttributeEntryRow>& attributes)
      : annotations_(annotations), attributes_(attributes) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    NodeAnnotation& ann = annotations_[expr.id()];
    // Comprehension-scope idents are not attribute roots — they are
    // bound per-iteration by the comprehension's loop prologue, not
    // by `Activation::Bind`.  Unknown-pattern matching applies only
    // to free-variable attribute paths.
    if (ann.scope_id != 0) {
      ann.attribute_id = 0;
      return;
    }
    ann.attribute_id = Intern(ident.name(), {});
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& sel) override {
    const NodeAnnotation* op = annotations_.Find(sel.operand().id());
    if (op == nullptr || op->attribute_id == 0) {
      // Non-path-bearing operand (e.g. a literal, kCall, map) —
      // leave attribute_id at 0; unknown-pattern match is a no-op.
      return;
    }
    const AttributeEntryRow& base = attributes_[op->attribute_id];
    std::vector<std::string> qualifiers = base.qualifiers;
    qualifiers.push_back(sel.field());
    annotations_[expr.id()].attribute_id =
        Intern(base.root_variable, std::move(qualifiers));
  }

 private:
  uint32_t Intern(absl::string_view root_variable,
                  std::vector<std::string> qualifiers) {
    std::string key =
        absl::StrCat(root_variable, "|", absl::StrJoin(qualifiers, "."));
    auto it = key_to_id_.find(key);
    if (it != key_to_id_.end()) return it->second;
    const auto id = static_cast<uint32_t>(attributes_.size());
    attributes_.push_back(
        AttributeEntryRow{std::string(root_variable), std::move(qualifiers)});
    key_to_id_.emplace(std::move(key), id);
    return id;
  }

  WasmAnnotations& annotations_;
  std::vector<AttributeEntryRow>& attributes_;
  absl::flat_hash_map<std::string, uint32_t> key_to_id_;
};

// M3.F: stamps `map_origin` on every map-typed node per the
// `map-list-dispatch.md` §2.6 inference table:
//   kMapExpr        → kArena  (literal in the wasm bump arena)
//   kIdent[map<>]   → kHost   (Activation::Bind hands us a backing)
//   kSelect[map<>]  → kHost   (proto map field via ProtoBacking)
// Codegen reads this annotation at the kCallExpr(_[_]) emission
// site to choose between cel_map_lookup_arena (fast path),
// cel_host.cel_map_lookup (host trampoline), and the kDynamic
// dispatcher.  Branch-coalescing rules (?: / && / ||) over map
// operands stay deferred to M5.
class MapOriginVisitor : public cel::AstVisitorBase {
 public:
  explicit MapOriginVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitMap(const cel::Expr& expr, const cel::MapExpr& /*m*/) override {
    annotations_[expr.id()].map_origin = Origin::kArena;
  }

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& /*ident*/) override {
    StampHostIfMapTyped(expr);
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& /*sel*/) override {
    StampHostIfMapTyped(expr);
  }

 private:
  void StampHostIfMapTyped(const cel::Expr& expr) {
    NodeAnnotation* ann = &annotations_[expr.id()];
    if (ann->repr != Repr::kMap) return;
    // M5.B Slice A: a comprehension-scope ident binding to a
    // map-typed value (e.g. `[[m1, m2]].exists(m, ...)` where `m`
    // is a map) reads its bytes out of the OUTER list's arena
    // payload — not a host backing.  Leaving origin at the default
    // `kDynamic` lets codegen fall through to the runtime
    // dispatcher, which inspects the CelValue kind tag at runtime.
    // (For Slice A's tested matrix — scalar iter_vars over list
    // literals — this branch is unreachable.)
    if (ann->scope_id != 0) return;
    ann->map_origin = Origin::kHost;
  }
  WasmAnnotations& annotations_;
};

// M4.F: stamps `list_origin` on every list-typed node per the
// `map-list-dispatch.md` §2.6 inference table (mirror of
// MapOriginVisitor):
//   kListExpr       → kArena  (literal in the wasm bump arena)
//   kIdent[list<>]  → kHost   (Activation::Bind hands us a backing)
//   kSelect[list<>] → kHost   (proto repeated field via ProtoList)
// Codegen reads this annotation at the kCallExpr(_[_]) emission
// site to choose between cel_list_at_arena (fast path),
// cel_host.cel_list_at (host trampoline), and the kDynamic
// dispatcher.  Branch-coalescing (?: / && / ||) over list operands
// stays deferred to M5.
class ListOriginVisitor : public cel::AstVisitorBase {
 public:
  explicit ListOriginVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitList(const cel::Expr& expr,
                     const cel::ListExpr& /*l*/) override {
    annotations_[expr.id()].list_origin = Origin::kArena;
  }

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& /*ident*/) override {
    StampHostIfListTyped(expr);
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& /*sel*/) override {
    StampHostIfListTyped(expr);
  }

 private:
  void StampHostIfListTyped(const cel::Expr& expr) {
    NodeAnnotation* ann = &annotations_[expr.id()];
    if (ann->repr != Repr::kList) return;
    // Mirror of MapOriginVisitor: comp-scope idents binding a
    // list-typed value live in arena, not a host backing.
    if (ann->scope_id != 0) return;
    ann->list_origin = Origin::kHost;
  }
  WasmAnnotations& annotations_;
};

// Slice 1.5 (dyn-passthrough-plan.md, Option A): for every `dyn(scalar)`
// call admitted by the static-subset gate, copy the argument's
// non-storage annotation fields onto the call node so downstream
// consumers (operand reads in `==`, comprehension scope walks, the
// attribute-pattern matcher) see the underlying scalar type — the
// call site's checker-assigned `dyn` type would otherwise leave the
// annotation at `Repr::kUnknown` with empty `attribute_id` /
// `overload_id`.  Storage forwarding lives in LayoutPass: ResolvePass
// runs before slots are assigned, so we cannot copy `storage` here.
class DynPassthroughVisitor : public cel::AstVisitorBase {
 public:
  explicit DynPassthroughVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& call) override {
    if (call.function() != "dyn" || call.args().size() != 1 ||
        call.has_target()) {
      return;
    }
    const cel::Expr& arg = call.args()[0];
    const NodeAnnotation* arg_ann = annotations_.Find(arg.id());
    if (arg_ann == nullptr) return;
    NodeAnnotation& self = annotations_[expr.id()];
    self.repr = arg_ann->repr;
    self.field_number = arg_ann->field_number;
    self.overload_id = arg_ann->overload_id;
    self.local_index = arg_ann->local_index;
    self.attribute_id = arg_ann->attribute_id;
    self.map_origin = arg_ann->map_origin;
    self.list_origin = arg_ann->list_origin;
  }

 private:
  WasmAnnotations& annotations_;
};

// M5.F: stamps `overload_id` on every kCallExpr from cel-cpp's
// `Ast::reference_map`.  cel-cpp's checker writes a Reference for
// each call node listing the resolved standard-library overload
// (e.g. "add_int64" for `1+2`); we copy the first entry as a
// string_view pointing into cel-cpp's owned storage (lifetime
// tied to the surrounding TypedAst).  Codegen reads this in
// `EmitGeneralCall` to pick the wasm helper from `OverloadTable`.
//
// Empty `overload_id` is the legitimate default for
// special-cased calls (`_[_]`, `_&&_`, `_||_`, `_?_:_`) which
// don't go through the table — `expr_lower.cc` dispatches on
// `call.function()` for those before consulting the annotation.
class OverloadIdResolver : public cel::AstVisitorBase {
 public:
  OverloadIdResolver(const cel::Ast::ReferenceMap& reference_map,
                     WasmAnnotations& annotations)
      : reference_map_(reference_map), annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& /*call*/) override {
    auto it = reference_map_.find(expr.id());
    if (it == reference_map_.end()) return;
    const auto& overloads = it->second.overload_id();
    if (overloads.empty()) return;
    // string_view points into cel-cpp's `Reference::overload_id_`
    // (a `std::vector<std::string>`).  The Reference is owned by
    // `Ast::reference_map_`; lifetime is the surrounding TypedAst,
    // which lives through codegen.
    annotations_[expr.id()].overload_id = absl::string_view(overloads.front());
  }

 private:
  const cel::Ast::ReferenceMap& reference_map_;
  WasmAnnotations& annotations_;
};

// M7.A: walks every `kStructExpr` post-order, interns its `name()`
// FQN into `output.message_types`, and stamps the dense id onto the
// node's `NodeAnnotation::message_type_id`.  Index 0 is the
// reserved sentinel; the first entry pushed by `RunAnnotationVisitors`.
// Codegen reads the stamped id in the kStructExpr lowering arm to
// emit `cel_host.cel_make_message(type_id, out_slot)`; the host
// resolves `id → Descriptor*` against the descriptor pool at Plan
// time.  See `m7-proto-literals.md` §4.2.
//
// `name()` is the message FQN cel-cpp's checker stamped on the
// kStructExpr (e.g. `"celwasm.testdata.HostMsg3"`).  Empty `name()`
// means "struct literal lowered as a map" (`design.md` §4.7.4) —
// those nodes lower through `kMapExpr` at parse time and never
// reach codegen as a kStructExpr.  We CHECK rather than tolerate
// the empty case here so a parser regression that lets one
// through fails loudly at intern time, not at trampoline-call
// time with an opaque id-out-of-range.
class MessageTypeIdVisitor : public cel::AstVisitorBase {
 public:
  MessageTypeIdVisitor(WasmAnnotations& annotations,
                       std::vector<MessageTypeRow>& types)
      : annotations_(annotations), types_(types) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitStruct(const cel::Expr& expr,
                       const cel::StructExpr& s) override {
    ABSL_CHECK(!s.name().empty())
        << "ResolvePass: kStructExpr id=" << expr.id()
        << " has empty name() — should have lowered as kMapExpr at parse "
           "time per design.md §4.7.4";
    auto it = fqn_to_id_.find(s.name());
    uint32_t id = 0;
    if (it == fqn_to_id_.end()) {
      id = static_cast<uint32_t>(types_.size());
      types_.push_back(MessageTypeRow{s.name()});
      fqn_to_id_[s.name()] = id;
    } else {
      id = it->second;
    }
    annotations_[expr.id()].message_type_id = id;
  }

 private:
  WasmAnnotations& annotations_;
  std::vector<MessageTypeRow>& types_;
  // FQN → dense id (excluding the sentinel at index 0; sentinel is
  // pushed by `RunAnnotationVisitors` before this visitor runs).
  absl::flat_hash_map<std::string, uint32_t> fqn_to_id_;
};

// Runs the per-kind annotation-stamping visitors on `output` over
// `root`.  Each visitor is independent — sequencing matters only for
// the dyn-passthrough forwarder, which runs last so every other
// visitor's writes on the argument are visible to copy.
void RunAnnotationVisitors(const cel::Ast& checked, const cel::Expr& root,
                           ResolveOutput& output) {
  KConstReprAudit audit(output.annotations);
  cel::AstTraverse(root, audit);

  ScopedIdentResolver ident_resolver(output.annotations, output.variables,
                                     output.max_scope_id);
  // Comprehension subexpression callbacks are OFF by default in
  // cel-cpp's traversal; enable them so the scope stack push/pop
  // hooks (PreVisitComprehensionSubexpression / PostVisit…) fire
  // and the iter / accu bindings are visible during the body walk.
  // Without this, every comp-scoped kIdent falls through to the
  // free-variable path and the scope is invisible to codegen.
  cel::AstTraverse(root, ident_resolver,
                   cel::TraversalOptions{.use_comprehension_callbacks = true});

  output.attributes.emplace_back();
  AttributePathResolver attr_resolver(output.annotations, output.attributes);
  cel::AstTraverse(root, attr_resolver);

  // M7.A: message-type intern table.  Sentinel at id 0; visitor
  // populates ids 1..N for distinct kStructExpr FQNs.
  output.message_types.emplace_back();
  MessageTypeIdVisitor type_visitor(output.annotations, output.message_types);
  cel::AstTraverse(root, type_visitor);

  MapOriginVisitor map_origin_visitor(output.annotations);
  cel::AstTraverse(root, map_origin_visitor);

  ListOriginVisitor list_origin_visitor(output.annotations);
  cel::AstTraverse(root, list_origin_visitor);

  OverloadIdResolver overload_resolver(checked.reference_map(),
                                       output.annotations);
  cel::AstTraverse(root, overload_resolver);

  DynPassthroughVisitor dyn_visitor(output.annotations);
  cel::AstTraverse(root, dyn_visitor);
}

}  // namespace

absl::StatusOr<ResolveOutput> ResolvePass(const TypedAst& ast) {
  ABSL_CHECK(ast.has_ast()) << "ResolvePass: TypedAst has no checked cel::Ast";

  ResolveOutput output;

  // Inherit the annotations ParseAndCheck already populated:
  // `repr` from the type_map and `field_number` from descriptor
  // resolution on every kSelect (see frontend/parse_and_check.cc →
  // ir/typed_ast.cc::PopulateAnnotations).  ResolvePass then adds
  // `local_index` for kIdent nodes and other per-kind fields.
  for (const auto& [expr_id, ann] : ast.annotations().nodes()) {
    output.annotations[expr_id] = ann;
  }

  RunAnnotationVisitors(ast.ast(), ast.ast().root_expr(), output);
  return output;
}

}  // namespace celwasm
