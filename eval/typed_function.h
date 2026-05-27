// typed_function — Layer 2 of the host-call adapter (m21).  Adapts a
// plain typed C++ lambda into a `HostCallback`, mapping each parameter
// type to its `HostCallContext::ArgXxx` accessor and the return to its
// `ReturnXxx`, so the embedder writes zero marshalling and the
// kind-checks happen for them:
//
//   engine.AddTypedFunction("double_it_int",
//       [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });
//
//   engine.AddTypedFunction("headline_message_acme_User",
//       [](const acme::User& u) -> absl::StatusOr<std::string> {
//         return absl::AsciiStrToUpper(u.first());
//       });
//
// Canonical types ONLY — no implicit conversions (m21 §5.1).  The
// declared parameter / return spellings must be EXACTLY one of:
//
//   param:  bool, int64_t, uint64_t, double, absl::string_view,
//           absl::Duration, absl::Time, HostListView, HostMapView,
//           Value, `const M&` (M : google::protobuf::Message),
//           `const google::protobuf::Message*` (polymorphic, no cast).
//   return: absl::StatusOr<R> where R is one of bool, int64_t,
//           uint64_t, double, std::string, absl::Duration, absl::Time,
//           std::unique_ptr<M> (M : Message), std::vector<Value>,
//           std::vector<std::pair<Value,Value>>, or Value.
//
// Anything else — `int`, `long`, `float`, `char*`, a by-value proto, a
// bare-`R` (non-StatusOr) return — is a COMPILE error naming the
// offending type, never a silent narrowing or mis-dispatch.  CEL
// `string` and `bytes` share `absl::string_view` (a string_view param
// accepts either; a `std::string` return encodes as `string`); a
// `bytes`-specific result uses the raw `HostCallContext::ReturnBytes`.
//
// Unknown / error arguments are absorbed by the trampoline before the
// closure runs (eval/host_call_context.h), so a typed lambda only ever
// sees all-known args.  To *emit* an unknown, return `Value::Unknown()`
// (the function-origin sentinel form) from a `StatusOr<Value>` lambda.

#ifndef CELWASM_EVAL_TYPED_FUNCTION_H_
#define CELWASM_EVAL_TYPED_FUNCTION_H_

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "eval/host_call_context.h"
#include "eval/host_callback.h"
#include "eval/value.h"
#include "google/protobuf/message.h"

namespace celwasm {
namespace typed_internal {

// ─────────────────────── argument traits ───────────────────────────
//
// `ArgTrait<T>::Decode(ctx, j)` returns `absl::StatusOr<Storage>`;
// `ArgTrait<T>::Arg(Storage&)` converts the decoded storage into the
// exact value the lambda parameter expects.  The primary template is an
// always-false static_assert — only canonical types specialize it, so a
// non-canonical parameter is a clear compile error, never a coercion.

template <typename T, typename Enable = void>
struct ArgTrait {
  static_assert(sizeof(T) == 0,
                "host-fn parameter is not a canonical CEL type. Use exactly: "
                "int64_t, uint64_t, double, bool, absl::string_view, "
                "absl::Duration, absl::Time, const M& (M:Message), "
                "const google::protobuf::Message*, HostListView, HostMapView, "
                "or Value. (e.g. int64_t, not int/long; double, not float.)");
};

template <>
struct ArgTrait<bool> {
  using Storage = bool;
  static absl::StatusOr<bool> Decode(const HostCallContext& c, int j) {
    return c.ArgBool(j);
  }
  static bool Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<int64_t> {
  using Storage = int64_t;
  static absl::StatusOr<int64_t> Decode(const HostCallContext& c, int j) {
    return c.ArgInt(j);
  }
  static int64_t Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<uint64_t> {
  using Storage = uint64_t;
  static absl::StatusOr<uint64_t> Decode(const HostCallContext& c, int j) {
    return c.ArgUint(j);
  }
  static uint64_t Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<double> {
  using Storage = double;
  static absl::StatusOr<double> Decode(const HostCallContext& c, int j) {
    return c.ArgDouble(j);
  }
  static double Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<absl::string_view> {
  using Storage = absl::string_view;
  // CEL string and bytes share the wire representation; a string_view
  // param accepts either (string first, then bytes).
  static absl::StatusOr<absl::string_view> Decode(const HostCallContext& c,
                                                  int j) {
    auto s = c.ArgString(j);
    if (s.ok()) return s;
    auto b = c.ArgBytes(j);
    if (b.ok()) return b;
    return s.status();
  }
  static absl::string_view Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<absl::Duration> {
  using Storage = absl::Duration;
  static absl::StatusOr<absl::Duration> Decode(const HostCallContext& c,
                                               int j) {
    return c.ArgDuration(j);
  }
  static absl::Duration Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<absl::Time> {
  using Storage = absl::Time;
  static absl::StatusOr<absl::Time> Decode(const HostCallContext& c, int j) {
    return c.ArgTimestamp(j);
  }
  static absl::Time Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<HostListView> {
  using Storage = HostListView;
  static absl::StatusOr<HostListView> Decode(const HostCallContext& c, int j) {
    return c.ArgList(j);
  }
  static HostListView Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<HostMapView> {
  using Storage = HostMapView;
  static absl::StatusOr<HostMapView> Decode(const HostCallContext& c, int j) {
    return c.ArgMap(j);
  }
  static HostMapView Arg(Storage& s) {
    return s;
  }
};
template <>
struct ArgTrait<Value> {
  using Storage = Value;
  static absl::StatusOr<Value> Decode(const HostCallContext& c, int j) {
    return c.ArgValue(j);
  }
  static Value Arg(Storage& s) {
    return s;
  }
};

// proto, concrete: `const M&` (M : Message).  ArgProto + dynamic_cast;
// wrong message type → InvalidArgument.
template <typename M>
struct ArgTrait<
    const M&,
    std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>> {
  using Storage = const M*;
  static absl::StatusOr<const M*> Decode(const HostCallContext& c, int j) {
    auto m = c.ArgProto(j);
    if (!m.ok()) return m.status();
    const auto* d = dynamic_cast<const M*>(*m);
    if (d == nullptr) {
      return absl::InvalidArgumentError(
          "host-fn proto arg: wrong message type");
    }
    return d;
  }
  static const M& Arg(Storage& s) {
    return *s;
  }
};

// proto, polymorphic: `const google::protobuf::Message*`.  ArgProto
// straight through — no cast, accepts any message.
template <typename M>
struct ArgTrait<
    const M*,
    std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>> {
  using Storage = const M*;
  static absl::StatusOr<const M*> Decode(const HostCallContext& c, int j) {
    return c.ArgProto(j);
  }
  static const M* Arg(Storage& s) {
    return s;
  }
};

// ─────────────────────── return traits ─────────────────────────────

template <typename R, typename Enable = void>
struct ReturnTrait {
  static_assert(sizeof(R) == 0,
                "host-fn return is not a canonical CEL type. The lambda must "
                "return absl::StatusOr<R> where R is one of: bool, int64_t, "
                "uint64_t, double, std::string, absl::Duration, absl::Time, "
                "std::unique_ptr<M> (M:Message), std::vector<Value>, "
                "std::vector<std::pair<Value,Value>>, or Value.");
};

template <>
struct ReturnTrait<bool> {
  static absl::Status Encode(HostCallContext& c, bool v) {
    return c.ReturnBool(v);
  }
};
template <>
struct ReturnTrait<int64_t> {
  static absl::Status Encode(HostCallContext& c, int64_t v) {
    return c.ReturnInt(v);
  }
};
template <>
struct ReturnTrait<uint64_t> {
  static absl::Status Encode(HostCallContext& c, uint64_t v) {
    return c.ReturnUint(v);
  }
};
template <>
struct ReturnTrait<double> {
  static absl::Status Encode(HostCallContext& c, double v) {
    return c.ReturnDouble(v);
  }
};
template <>
struct ReturnTrait<std::string> {
  static absl::Status Encode(HostCallContext& c, const std::string& v) {
    return c.ReturnString(v);
  }
};
template <>
struct ReturnTrait<absl::Duration> {
  static absl::Status Encode(HostCallContext& c, absl::Duration v) {
    return c.ReturnDuration(v);
  }
};
template <>
struct ReturnTrait<absl::Time> {
  static absl::Status Encode(HostCallContext& c, absl::Time v) {
    return c.ReturnTimestamp(v);
  }
};
template <>
struct ReturnTrait<std::vector<Value>> {
  static absl::Status Encode(HostCallContext& c, const std::vector<Value>& v) {
    return c.ReturnList(v);
  }
};
template <>
struct ReturnTrait<std::vector<std::pair<Value, Value>>> {
  static absl::Status Encode(HostCallContext& c,
                             const std::vector<std::pair<Value, Value>>& v) {
    return c.ReturnMap(v);
  }
};
template <>
struct ReturnTrait<Value> {
  static absl::Status Encode(HostCallContext& c, const Value& v) {
    return c.ReturnValue(v);
  }
};
// proto: std::unique_ptr<M> (M : Message) — OWNING, interned.
template <typename M>
struct ReturnTrait<
    std::unique_ptr<M>,
    std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>> {
  static absl::Status Encode(HostCallContext& c, std::unique_ptr<M> v) {
    return c.ReturnProto(std::move(v));
  }
};

// ─────────────────────── canonical-type detection ──────────────────
//
// Mirrors the specialization set above as a testable boolean (the
// must-not-compile coverage: `static_assert(!kIsCanonicalHostArg<int>)`).
// Kept in sync with ArgTrait / ReturnTrait by construction.

template <typename T, typename = void>
struct IsCanonicalArg : std::false_type {};
template <>
struct IsCanonicalArg<bool> : std::true_type {};
template <>
struct IsCanonicalArg<int64_t> : std::true_type {};
template <>
struct IsCanonicalArg<uint64_t> : std::true_type {};
template <>
struct IsCanonicalArg<double> : std::true_type {};
template <>
struct IsCanonicalArg<absl::string_view> : std::true_type {};
template <>
struct IsCanonicalArg<absl::Duration> : std::true_type {};
template <>
struct IsCanonicalArg<absl::Time> : std::true_type {};
template <>
struct IsCanonicalArg<HostListView> : std::true_type {};
template <>
struct IsCanonicalArg<HostMapView> : std::true_type {};
template <>
struct IsCanonicalArg<Value> : std::true_type {};
template <typename M>
struct IsCanonicalArg<
    const M&, std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>>
    : std::true_type {};
template <typename M>
struct IsCanonicalArg<
    const M*, std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>>
    : std::true_type {};

template <typename R, typename = void>
struct IsCanonicalReturn : std::false_type {};
template <>
struct IsCanonicalReturn<bool> : std::true_type {};
template <>
struct IsCanonicalReturn<int64_t> : std::true_type {};
template <>
struct IsCanonicalReturn<uint64_t> : std::true_type {};
template <>
struct IsCanonicalReturn<double> : std::true_type {};
template <>
struct IsCanonicalReturn<std::string> : std::true_type {};
template <>
struct IsCanonicalReturn<absl::Duration> : std::true_type {};
template <>
struct IsCanonicalReturn<absl::Time> : std::true_type {};
template <>
struct IsCanonicalReturn<std::vector<Value>> : std::true_type {};
template <>
struct IsCanonicalReturn<std::vector<std::pair<Value, Value>>>
    : std::true_type {};
template <>
struct IsCanonicalReturn<Value> : std::true_type {};
template <typename M>
struct IsCanonicalReturn<
    std::unique_ptr<M>,
    std::enable_if_t<std::is_base_of_v<google::protobuf::Message, M>>>
    : std::true_type {};

template <typename T>
inline constexpr bool kIsCanonicalHostArg = IsCanonicalArg<T>::value;
template <typename R>
inline constexpr bool kIsCanonicalHostReturn = IsCanonicalReturn<R>::value;

// ─────────────────────── signature decomposition ───────────────────
//
// Extract the return type R and the parameter pack from a callable.
// The declared parameter types are preserved verbatim (so a proto `const
// M&` keeps its reference) and fed to ArgTrait — there is no
// call-and-convert site, so no implicit conversion can sneak in.

template <typename T>
struct FnSignature {
  static_assert(sizeof(T) == 0,
                "host-fn must be a callable returning absl::StatusOr<R>");
};
// const-qualified operator() (the common non-mutable lambda).
template <typename C, typename R, typename... A>
struct FnSignature<absl::StatusOr<R> (C::*)(A...) const> {
  using Ret = R;
  using ArgsTuple = std::tuple<A...>;
  static constexpr std::size_t kArity = sizeof...(A);
};
// mutable lambda.
template <typename C, typename R, typename... A>
struct FnSignature<absl::StatusOr<R> (C::*)(A...)> {
  using Ret = R;
  using ArgsTuple = std::tuple<A...>;
  static constexpr std::size_t kArity = sizeof...(A);
};
// plain function pointer.
template <typename R, typename... A>
struct FnSignature<absl::StatusOr<R> (*)(A...)> {
  using Ret = R;
  using ArgsTuple = std::tuple<A...>;
  static constexpr std::size_t kArity = sizeof...(A);
};

// Pick the right FnSignature for lambdas (via &C::operator()) vs plain
// function pointers.
template <typename C>
struct CallableSignature
    : FnSignature<decltype(&std::remove_reference_t<C>::operator())> {};
template <typename R, typename... A>
struct CallableSignature<absl::StatusOr<R> (*)(A...)>
    : FnSignature<absl::StatusOr<R> (*)(A...)> {};

// ─────────────────────── the closure builder ───────────────────────

template <typename Fn, typename R, typename ArgsTuple, std::size_t... Is>
absl::Status InvokeTyped(HostCallContext& ctx, const Fn& fn,
                         std::index_sequence<Is...>) {
  // Decode each argument to its exact declared type.  (Decode reads
  // independent arg slots, so evaluation order is irrelevant.)
  auto storage =
      std::make_tuple(ArgTrait<std::tuple_element_t<Is, ArgsTuple>>::Decode(
          ctx, static_cast<int>(Is))...);
  // First non-OK decode wins.
  absl::Status err;
  ((err.ok() && !std::get<Is>(storage).ok()
        ? (err = std::get<Is>(storage).status(), void())
        : void()),
   ...);
  if (!err.ok()) return err;

  absl::StatusOr<R> result =
      fn(ArgTrait<std::tuple_element_t<Is, ArgsTuple>>::Arg(
          *std::get<Is>(storage))...);
  if (!result.ok()) return result.status();
  return ReturnTrait<R>::Encode(ctx, *std::move(result));
}

template <typename Fn>
HostCallback MakeCallback(Fn fn) {
  using Sig = CallableSignature<Fn>;
  using R = typename Sig::Ret;
  using ArgsTuple = typename Sig::ArgsTuple;
  static_assert(kIsCanonicalHostReturn<R>,
                "host-fn return type is not a canonical CEL return type");
  return [fn = std::move(fn)](HostCallContext& ctx) -> absl::Status {
    return InvokeTyped<Fn, R, ArgsTuple>(
        ctx, fn, std::make_index_sequence<Sig::kArity>{});
  };
}

}  // namespace typed_internal

// Adapt a typed lambda / function pointer into a `{HostCallback,
// num_args}` pair.  `num_args` is the total wasm arity the engine
// registers — the lambda's parameter count plus 1 for the out slot
// (matching `Engine::AddFunction`'s `num_args` contract).
struct TypedFunction {
  HostCallback callback;
  uint8_t num_args = 0;
};

template <typename Fn>
TypedFunction BindTypedFunction(Fn fn) {
  using Sig = typed_internal::CallableSignature<std::decay_t<Fn>>;
  // `MakeCallback` returns a `HostCallback` (a `std::function`) that owns a
  // copy of the move-captured closure — the callable outlives this frame and
  // refers to no stack object.  clang-analyzer-core.StackAddressEscape does
  // not model std::function's ownership and reports a spurious escape here.
  // NOLINTNEXTLINE(clang-analyzer-core.StackAddressEscape)
  return TypedFunction{
      typed_internal::MakeCallback(std::move(fn)),
      static_cast<uint8_t>(Sig::kArity + 1),
  };
}

}  // namespace celwasm

#endif  // CELWASM_EVAL_TYPED_FUNCTION_H_
