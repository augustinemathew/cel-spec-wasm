#include "compiler/codegen/expr_lower.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "common/ast.h"
#include "common/constant.h"
#include "common/expr.h"
#include "common/operators.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {
namespace {

namespace op = ::google::api::expr::common;

// Per-eval-function lowering state.  Holds the parameter layout (one
// WASM param per user-declared variable, in spec order) and any scratch
// locals the body accumulates — for example, the string-constant
// lowering allocates one i32 per literal to hold the `cel_alloc` result
// across the subsequent `i32.store8` chain.
//
// Local indexing in WebAssembly: parameters come first (0..num_params),
// then local variables (num_params..num_params+local_types.size()).
// `AddLocal` returns the absolute index in that single space.
//
// `idents` maps declared variable name → param index; `kIdentExpr`
// lowers to `local.get idents[name]`.  Misses surface as an
// `InvalidArgument` status, which is stronger than a lookup miss:
// the checker already rejects unknown identifiers, so a miss here
// indicates either a bug in the frontend (a var used in the
// expression wasn't declared) or a downstream caller that passed an
// `TypedAst` whose `variables()` list is out of sync with the AST.
//
// The context does NOT track which runtime imports have been
// declared: we always link eval modules fully against the runtime
// (see DeclareRuntimeImports), so there is nothing to gate on.
struct LoweringContext {
  WasmModule& mod;
  uint32_t num_params = 0;
  std::vector<BinaryenType> local_types;
  absl::flat_hash_map<std::string, BinaryenIndex> idents;

  BinaryenIndex AddLocal(BinaryenType type) {
    local_types.push_back(type);
    return static_cast<BinaryenIndex>(
        num_params + local_types.size() - 1);
  }
};

absl::StatusOr<BinaryenExpressionRef> LowerExpr(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr);

// Declares every import the eval module may reference, up front.  Each
// eval module is always linked against the runtime at instantiation
// time, so declaring imports the current AST happens not to use is
// harmless — wasmtime accepts unused imports as long as they resolve.
// Keeping the set fixed here avoids per-subtree "is this declared yet?"
// bookkeeping and mirrors the design doc's two-module layout.
absl::Status DeclareRuntimeImports(WasmModule& mod) {
  // Shared linear memory — every CelValue offset the codegen emits
  // is interpreted against the runtime's arena.  One page minimum.
  auto s = mod.AddMemoryImport(/*external_module=*/"cel",
                               /*external_base=*/"memory",
                               /*initial_pages=*/1,
                               /*max_pages=*/std::nullopt);
  if (!s.ok()) return s;

  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();

  auto import1 = [&](absl::string_view name, BinaryenType p0,
                     BinaryenType result) {
    mod.AddFunctionImport(name, /*external_module=*/"cel",
                          /*external_base=*/name,
                          absl::Span<const BinaryenType>(&p0, 1), result);
  };
  auto import2 = [&](absl::string_view name, BinaryenType p0,
                     BinaryenType p1, BinaryenType result) {
    BinaryenType params[2] = {p0, p1};
    mod.AddFunctionImport(name, /*external_module=*/"cel",
                          /*external_base=*/name,
                          absl::Span<const BinaryenType>(params, 2), result);
  };

  // Allocation + string/bytes construction (M3 slice A).
  import1("cel_alloc", i32, i32);
  import2("cel_make_string_view", i32, i32, i32);
  import2("cel_make_bytes_view", i32, i32, i32);
  // String/bytes helpers expected to land in M3 — pre-declaring them
  // keeps the import list stable once those lowerings arrive.
  import2("cel_string_eq", i32, i32, i32);
  import2("cel_bytes_eq", i32, i32, i32);
  import2("cel_string_concat", i32, i32, i32);
  import1("cel_string_size", i32, i64);
  import1("cel_bool_from_value", i32, i32);
  return absl::OkStatus();
}

absl::Status UnimplementedKind(absl::string_view kind, int64_t id) {
  return absl::UnimplementedError(
      absl::StrCat("expr_lower: ", kind,
                   " is not yet supported (expr id ", id, ")"));
}

absl::Status UnimplementedRepr(absl::string_view op_name, Repr r, int64_t id) {
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: operator `", op_name, "` does not support Repr `",
      ReprName(r), "` (expr id ", id, ")"));
}

// The checker populates annotations() for every typed node.  Failing to
// find one means the AST was not run through PopulateAnnotations (which
// our frontend does) or the node is DYN (which static_subset rejects).
absl::StatusOr<Repr> ReprOf(const TypedAst& ast, const cel::Expr& e) {
  const NodeAnnotation* a = ast.annotations().Find(e.id());
  if (a == nullptr || a->repr == Repr::kUnknown) {
    return absl::FailedPreconditionError(absl::StrCat(
        "expr_lower: missing Repr annotation for expr id ", e.id(),
        " — was PopulateAnnotations run? did RejectDyn pass?"));
  }
  return a->repr;
}

// Lowers a string literal as:
//   (block (result i32)
//     (local.set $scratch (call $cel_alloc (i32.const len)))
//     (i32.store8 offset=0 (local.get $scratch) (i32.const b0))
//     ... one i32.store8 per byte ...
//     (call $cel_make_string_view (local.get $scratch) (i32.const len)))
//
// Rationale: we deliberately do NOT use a wasm data segment.  The eval
// module and the runtime module share the runtime's linear memory, and
// a data segment in the eval module would need to agree on a byte range
// that doesn't alias the runtime's own static data / bump arena.  That
// coordination is brittle; using `cel_alloc` at instantiation time
// simply rents a fresh, never-aliasing region from the runtime itself.
// The store-per-byte expansion is verbose but is only emitted per
// literal (once), and the resulting code is trivial for wasmtime's
// baseline compiler.
absl::StatusOr<BinaryenExpressionRef> LowerStringLiteral(
    LoweringContext& ctx, absl::string_view bytes) {
  BinaryenModuleRef m = ctx.mod.raw();
  const uint32_t len = static_cast<uint32_t>(bytes.size());
  const BinaryenIndex scratch = ctx.AddLocal(BinaryenTypeInt32());

  std::vector<BinaryenExpressionRef> children;
  children.reserve(2 + bytes.size());

  // local.set $scratch (call $cel_alloc len)
  {
    BinaryenExpressionRef alloc_arg =
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(len)));
    BinaryenExpressionRef alloc_call = BinaryenCall(
        m, "cel_alloc", &alloc_arg, 1, BinaryenTypeInt32());
    children.push_back(BinaryenLocalSet(m, scratch, alloc_call));
  }

  // One i32.store8 per byte, using `offset=i` to avoid emitting a
  // separate add for each position.
  for (uint32_t i = 0; i < len; ++i) {
    const uint8_t byte = static_cast<uint8_t>(bytes[i]);
    BinaryenExpressionRef ptr =
        BinaryenLocalGet(m, scratch, BinaryenTypeInt32());
    BinaryenExpressionRef value =
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(byte)));
    children.push_back(BinaryenStore(m,
                                     /*bytes=*/1,
                                     /*offset=*/i,
                                     /*align=*/1,
                                     ptr,
                                     value,
                                     /*type=*/BinaryenTypeInt32(),
                                     /*memoryName=*/"memory"));
  }

  // Trailing call that produces the block's i32 result.
  {
    BinaryenExpressionRef args[2] = {
        BinaryenLocalGet(m, scratch, BinaryenTypeInt32()),
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(len))),
    };
    children.push_back(BinaryenCall(m, "cel_make_string_view", args, 2,
                                    BinaryenTypeInt32()));
  }

  return BinaryenBlock(m,
                       /*name=*/nullptr,
                       children.data(),
                       static_cast<BinaryenIndex>(children.size()),
                       /*type=*/BinaryenTypeInt32());
}

// Lowers an `IdentExpr` to a `local.get` against the param slot the
// variable was assigned in `LowerToEvalFunction`.  The node's Repr
// annotation drives the result type — it must match the param's
// declared BinaryenType (both are derived from the same user-supplied
// type string upstream, so agreement is by construction).
absl::StatusOr<BinaryenExpressionRef> LowerIdent(LoweringContext& ctx,
                                                 const TypedAst& ast,
                                                 const cel::Expr& expr) {
  const std::string& name = expr.ident_expr().name();
  auto it = ctx.idents.find(name);
  if (it == ctx.idents.end()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expr_lower: identifier `", name, "` was not declared in "
        "CheckOptions::variable_specs (expr id ", expr.id(), ")"));
  }
  auto repr = ReprOf(ast, expr);
  if (!repr.ok()) return repr.status();
  BinaryenType t = WasmTypeFor(*repr);
  if (t == BinaryenTypeNone()) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: identifier `", name, "` has Repr `", ReprName(*repr),
        "` which has no scalar ABI lowering (expr id ", expr.id(), ")"));
  }
  return BinaryenLocalGet(ctx.mod.raw(), it->second, t);
}

absl::StatusOr<BinaryenExpressionRef> LowerConstant(LoweringContext& ctx,
                                                    const TypedAst& ast,
                                                    const cel::Expr& expr) {
  const cel::Constant& c = expr.const_expr();
  BinaryenModuleRef m = ctx.mod.raw();
  switch (c.kind_case()) {
    case cel::ConstantKindCase::kBool:
      return BinaryenConst(m, BinaryenLiteralInt32(c.bool_value() ? 1 : 0));
    case cel::ConstantKindCase::kInt:
      return BinaryenConst(m, BinaryenLiteralInt64(c.int_value()));
    case cel::ConstantKindCase::kUint:
      return BinaryenConst(m, BinaryenLiteralInt64(
                                  static_cast<int64_t>(c.uint_value())));
    case cel::ConstantKindCase::kDouble:
      return BinaryenConst(m, BinaryenLiteralFloat64(c.double_value()));
    case cel::ConstantKindCase::kString:
      return LowerStringLiteral(ctx, c.string_value());
    default:
      return absl::UnimplementedError(absl::StrCat(
          "expr_lower: constant kind ", static_cast<int>(c.kind_case()),
          " not yet supported (expr id ", expr.id(), ")"));
  }
  (void)ast;
}

// Binary arithmetic.  Dispatch on the Repr of the first operand (the
// checker guarantees both operands share a type for these overloads).
absl::StatusOr<BinaryenExpressionRef> LowerArithmetic(
    absl::string_view name, Repr r, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, WasmModule& mod) {
  BinaryenModuleRef m = mod.raw();
  BinaryenOp bop;
  if (name == op::CelOperator::ADD) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenAddInt64();
    else if (r == Repr::kDouble) bop = BinaryenAddFloat64();
    else return UnimplementedRepr(name, r, /*id=*/0);
  } else if (name == op::CelOperator::SUBTRACT) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenSubInt64();
    else if (r == Repr::kDouble) bop = BinaryenSubFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::MULTIPLY) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenMulInt64();
    else if (r == Repr::kDouble) bop = BinaryenMulFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::DIVIDE) {
    if (r == Repr::kInt) bop = BinaryenDivSInt64();
    else if (r == Repr::kUint) bop = BinaryenDivUInt64();
    else if (r == Repr::kDouble) bop = BinaryenDivFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::MODULO) {
    // CEL has no % for double; checker rejects it.
    if (r == Repr::kInt) bop = BinaryenRemSInt64();
    else if (r == Repr::kUint) bop = BinaryenRemUInt64();
    else return UnimplementedRepr(name, r, 0);
  } else {
    return absl::InternalError(
        absl::StrCat("LowerArithmetic: unhandled op `", name, "`"));
  }
  return BinaryenBinary(m, bop, lhs, rhs);
}

absl::StatusOr<BinaryenExpressionRef> LowerComparison(
    absl::string_view name, Repr arg_r, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, WasmModule& mod) {
  BinaryenModuleRef m = mod.raw();
  BinaryenOp bop;
  const bool eq = (name == op::CelOperator::EQUALS);
  const bool ne = (name == op::CelOperator::NOT_EQUALS);
  if (eq || ne) {
    switch (arg_r) {
      case Repr::kBool:
        bop = eq ? BinaryenEqInt32() : BinaryenNeInt32();
        break;
      case Repr::kInt:
      case Repr::kUint:
        bop = eq ? BinaryenEqInt64() : BinaryenNeInt64();
        break;
      case Repr::kDouble:
        bop = eq ? BinaryenEqFloat64() : BinaryenNeFloat64();
        break;
      default:
        return UnimplementedRepr(name, arg_r, 0);
    }
    return BinaryenBinary(m, bop, lhs, rhs);
  }
  // Ordered comparisons.  Bool is not ordered in CEL.
  const bool lt = (name == op::CelOperator::LESS);
  const bool le = (name == op::CelOperator::LESS_EQUALS);
  const bool gt = (name == op::CelOperator::GREATER);
  const bool ge = (name == op::CelOperator::GREATER_EQUALS);
  switch (arg_r) {
    case Repr::kInt:
      bop = lt ? BinaryenLtSInt64()
          : le ? BinaryenLeSInt64()
          : gt ? BinaryenGtSInt64()
          : ge ? BinaryenGeSInt64()
               : BinaryenEqInt64();
      break;
    case Repr::kUint:
      bop = lt ? BinaryenLtUInt64()
          : le ? BinaryenLeUInt64()
          : gt ? BinaryenGtUInt64()
          : ge ? BinaryenGeUInt64()
               : BinaryenEqInt64();
      break;
    case Repr::kDouble:
      bop = lt ? BinaryenLtFloat64()
          : le ? BinaryenLeFloat64()
          : gt ? BinaryenGtFloat64()
          : ge ? BinaryenGeFloat64()
               : BinaryenEqFloat64();
      break;
    default:
      return UnimplementedRepr(name, arg_r, 0);
  }
  return BinaryenBinary(m, bop, lhs, rhs);
}

absl::StatusOr<BinaryenExpressionRef> LowerCall(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr) {
  const cel::CallExpr& call = expr.call_expr();
  const std::string& fn = call.function();
  WasmModule& mod = ctx.mod;
  BinaryenModuleRef m = mod.raw();

  // Member form (x.f(...)) is only relevant for things like method
  // calls, which the MVP doesn't support.
  if (call.has_target()) {
    return UnimplementedKind(absl::StrCat("method call `", fn, "`"),
                             expr.id());
  }

  // Unary: logical not.
  if (fn == op::CelOperator::LOGICAL_NOT) {
    if (call.args().size() != 1) {
      return absl::InvalidArgumentError(
          absl::StrCat("`!_` takes 1 argument, got ", call.args().size()));
    }
    auto v = LowerExpr(ctx, ast, call.args()[0]);
    if (!v.ok()) return v.status();
    return BinaryenUnary(m, BinaryenEqZInt32(), *v);
  }

  // Unary: negate.
  if (fn == op::CelOperator::NEGATE) {
    if (call.args().size() != 1) {
      return absl::InvalidArgumentError(
          absl::StrCat("`-_` takes 1 argument, got ", call.args().size()));
    }
    auto v = LowerExpr(ctx, ast, call.args()[0]);
    if (!v.ok()) return v.status();
    auto arg_r = ReprOf(ast, call.args()[0]);
    if (!arg_r.ok()) return arg_r.status();
    switch (*arg_r) {
      case Repr::kInt:
        // 0 - x preserves i64 signedness; CEL also rejects negating uint.
        return BinaryenBinary(
            m, BinaryenSubInt64(),
            BinaryenConst(m, BinaryenLiteralInt64(0)), *v);
      case Repr::kDouble:
        return BinaryenUnary(m, BinaryenNegFloat64(), *v);
      default:
        return UnimplementedRepr(fn, *arg_r, expr.id());
    }
  }

  // Ternary: _?_:_
  if (fn == op::CelOperator::CONDITIONAL) {
    if (call.args().size() != 3) {
      return absl::InvalidArgumentError(
          absl::StrCat("`_?_:_` takes 3 arguments, got ", call.args().size()));
    }
    auto cond = LowerExpr(ctx, ast, call.args()[0]);
    if (!cond.ok()) return cond.status();
    auto t = LowerExpr(ctx, ast, call.args()[1]);
    if (!t.ok()) return t.status();
    auto f = LowerExpr(ctx, ast, call.args()[2]);
    if (!f.ok()) return f.status();
    return BinaryenIf(m, *cond, *t, *f);
  }

  // Short-circuit logical: _&&_ and _||_.
  if (fn == op::CelOperator::LOGICAL_AND ||
      fn == op::CelOperator::LOGICAL_OR) {
    if (call.args().size() != 2) {
      return absl::InvalidArgumentError(
          absl::StrCat("`", fn, "` takes 2 arguments, got ",
                       call.args().size()));
    }
    auto l = LowerExpr(ctx, ast, call.args()[0]);
    if (!l.ok()) return l.status();
    auto r = LowerExpr(ctx, ast, call.args()[1]);
    if (!r.ok()) return r.status();
    // a && b  ->  if (a) b else 0
    // a || b  ->  if (a) 1 else b
    const bool is_and = (fn == op::CelOperator::LOGICAL_AND);
    BinaryenExpressionRef if_true = is_and ? *r
                                           : BinaryenConst(m,
                                                 BinaryenLiteralInt32(1));
    BinaryenExpressionRef if_false = is_and ? BinaryenConst(m,
                                                  BinaryenLiteralInt32(0))
                                            : *r;
    return BinaryenIf(m, *l, if_true, if_false);
  }

  // Binary arithmetic and comparisons: both operands share a Repr;
  // dispatch on the first.
  if (call.args().size() == 2) {
    auto arg_r = ReprOf(ast, call.args()[0]);
    if (!arg_r.ok()) return arg_r.status();
    auto l = LowerExpr(ctx, ast, call.args()[0]);
    if (!l.ok()) return l.status();
    auto r = LowerExpr(ctx, ast, call.args()[1]);
    if (!r.ok()) return r.status();
    if (fn == op::CelOperator::ADD || fn == op::CelOperator::SUBTRACT ||
        fn == op::CelOperator::MULTIPLY || fn == op::CelOperator::DIVIDE ||
        fn == op::CelOperator::MODULO) {
      return LowerArithmetic(fn, *arg_r, *l, *r, mod);
    }
    if (fn == op::CelOperator::EQUALS || fn == op::CelOperator::NOT_EQUALS ||
        fn == op::CelOperator::LESS || fn == op::CelOperator::LESS_EQUALS ||
        fn == op::CelOperator::GREATER ||
        fn == op::CelOperator::GREATER_EQUALS) {
      return LowerComparison(fn, *arg_r, *l, *r, mod);
    }
  }

  return UnimplementedKind(absl::StrCat("call `", fn, "`"), expr.id());
}

absl::StatusOr<BinaryenExpressionRef> LowerExpr(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr) {
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kConstant:
      return LowerConstant(ctx, ast, expr);
    case cel::ExprKindCase::kCallExpr:
      return LowerCall(ctx, ast, expr);
    case cel::ExprKindCase::kIdentExpr:
      return LowerIdent(ctx, ast, expr);
    case cel::ExprKindCase::kSelectExpr:
      return UnimplementedKind("SelectExpr", expr.id());
    case cel::ExprKindCase::kListExpr:
      return UnimplementedKind("ListExpr", expr.id());
    case cel::ExprKindCase::kStructExpr:
      return UnimplementedKind("StructExpr", expr.id());
    case cel::ExprKindCase::kMapExpr:
      return UnimplementedKind("MapExpr", expr.id());
    case cel::ExprKindCase::kComprehensionExpr:
      return UnimplementedKind("ComprehensionExpr", expr.id());
    case cel::ExprKindCase::kUnspecifiedExpr:
      return absl::InvalidArgumentError(
          absl::StrCat("expr_lower: unspecified expr (id ", expr.id(), ")"));
  }
  return absl::InternalError("expr_lower: unreachable kind_case");
}

}  // namespace

BinaryenType WasmTypeFor(Repr r) {
  switch (r) {
    case Repr::kBool:
      return BinaryenTypeInt32();
    case Repr::kInt:
    case Repr::kUint:
    case Repr::kEnum:
    case Repr::kDuration:
    case Repr::kTimestamp:
      return BinaryenTypeInt64();
    case Repr::kDouble:
      return BinaryenTypeFloat64();
    case Repr::kType:
      return BinaryenTypeInt32();
    // Strings and bytes travel as i32 offsets into the shared linear
    // memory — each is a pointer to a `CelValue` owned by the runtime's
    // arena.  The codegen materialises the offset via `cel_alloc` +
    // `cel_make_string_view` (see LowerStringLiteral).
    case Repr::kString:
    case Repr::kBytes:
      return BinaryenTypeInt32();
    default:
      return BinaryenTypeNone();
  }
}

absl::StatusOr<LoweredFunction> LowerToEvalFunction(const TypedAst& ast,
                                                   absl::string_view func_name,
                                                   WasmModule& mod) {
  if (!ast.has_ast()) {
    return absl::InvalidArgumentError(
        "LowerToEvalFunction: TypedAst has no underlying cel::Ast");
  }
  const cel::Expr& root = ast.ast().root_expr();
  auto root_r = ReprOf(ast, root);
  if (!root_r.ok()) return root_r.status();
  BinaryenType result_type = WasmTypeFor(*root_r);
  if (result_type == BinaryenTypeNone()) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: root Repr `", ReprName(*root_r),
        "` has no scalar ABI lowering in M2"));
  }
  if (auto s = DeclareRuntimeImports(mod); !s.ok()) return s;

  // Build the parameter list from the declared variables.  Each user
  // variable becomes one WASM param whose type is the ABI encoding of
  // its Repr.  Variables whose Repr has no scalar encoding (list, map,
  // message, dyn) fail cleanly here — their support lives in later
  // milestones.  Duplicate names would be a checker bug (the decl-
  // builder would have refused to add the second one) but are worth
  // flagging here too since ctx.idents silently overwrites on collision.
  std::vector<BinaryenType> params;
  params.reserve(ast.variables().size());
  LoweringContext ctx{mod};
  for (const Variable& v : ast.variables()) {
    BinaryenType pt = WasmTypeFor(v.repr);
    if (pt == BinaryenTypeNone()) {
      return absl::UnimplementedError(absl::StrCat(
          "expr_lower: variable `", v.name, "` has Repr `",
          ReprName(v.repr), "` which has no scalar ABI lowering in M3"));
    }
    BinaryenIndex idx = static_cast<BinaryenIndex>(params.size());
    params.push_back(pt);
    auto [it, inserted] = ctx.idents.emplace(v.name, idx);
    if (!inserted) {
      return absl::InvalidArgumentError(absl::StrCat(
          "expr_lower: duplicate variable name `", v.name, "` in specs"));
    }
  }
  ctx.num_params = static_cast<uint32_t>(params.size());

  auto body = LowerExpr(ctx, ast, root);
  if (!body.ok()) return body.status();
  mod.AddFunction(func_name, params, result_type,
                  /*local_types=*/ctx.local_types, *body);
  BinaryenFunctionRef fn =
      BinaryenGetFunction(mod.raw(), std::string(func_name).c_str());
  if (fn == nullptr) {
    return absl::InternalError(absl::StrCat(
        "expr_lower: BinaryenGetFunction returned null for `", func_name,
        "` immediately after AddFunction"));
  }
  return LoweredFunction{fn, result_type, *root_r};
}

}  // namespace celwasm
