// Tests for abi/celfn_wire.  Covers:
//   - TypeFromCelfn: every CelfnType kind (scalars, proto FQN,
//     list / map / optional element recursion, deep nesting), plus
//     CHECK-death on malformed generic shapes.
//   - TypeEquals: positive (reflexive over nested shapes, unknown
//     kinds numerically equal) and negative along every signature-
//     matrix axis expressible at the type level — kind, proto FQN,
//     params count, nested generic element (see
//     m35-plugin-ergonomics.md §12; arity / return / is_receiver
//     axes live on RequiredFunction and are compared by the Plan
//     check, not here).
//   - RenderSignature: the frozen message spellings from the m35
//     plan §2, receiver / zero-param / nested-generic forms, and
//     open-set rendering of unknown kinds.

#include "abi/celfn_wire.h"

#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::celwasm::abi::RequiredFunction;
using ::celwasm::abi::Type;

// --- CelfnType construction helpers --------------------------------

CelfnType Scalar(CelfnType::Kind kind) {
  CelfnType t;
  t.kind = kind;
  return t;
}

CelfnType Proto(std::string fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::move(fqn);
  return t;
}

CelfnType List(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(elem));
  return t;
}

CelfnType Map(CelfnType key, CelfnType value) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(key));
  t.map_kv.push_back(std::move(value));
  return t;
}

CelfnType Optional(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  t.optional_element.push_back(std::move(elem));
  return t;
}

// Wire-side helpers.

Type WireScalar(Type::Kind kind) {
  Type t;
  t.set_kind(kind);
  return t;
}

Type WireUnknownKind(int value) {
  Type t;
  t.set_kind(static_cast<Type::Kind>(value));
  return t;
}

// --- TypeFromCelfn: scalar kinds ---------------------------------

struct ScalarCase {
  CelfnType::Kind celfn = CelfnType::Kind::kBool;
  Type::Kind wire = Type::KIND_UNSPECIFIED;
};

class TypeFromCelfnScalarTest : public ::testing::TestWithParam<ScalarCase> {};

TEST_P(TypeFromCelfnScalarTest, MapsKindWithNoFqnAndNoParams) {
  const Type wire = TypeFromCelfn(Scalar(GetParam().celfn));
  EXPECT_EQ(wire.kind(), GetParam().wire);
  EXPECT_TRUE(wire.proto_fqn().empty());
  EXPECT_EQ(wire.params_size(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    AllScalarKinds, TypeFromCelfnScalarTest,
    ::testing::Values(
        ScalarCase{CelfnType::Kind::kBool, Type::KIND_BOOL},
        ScalarCase{CelfnType::Kind::kInt, Type::KIND_INT},
        ScalarCase{CelfnType::Kind::kUint, Type::KIND_UINT},
        ScalarCase{CelfnType::Kind::kDouble, Type::KIND_DOUBLE},
        ScalarCase{CelfnType::Kind::kString, Type::KIND_STRING},
        ScalarCase{CelfnType::Kind::kBytes, Type::KIND_BYTES},
        ScalarCase{CelfnType::Kind::kNull, Type::KIND_NULL},
        ScalarCase{CelfnType::Kind::kDuration, Type::KIND_DURATION},
        ScalarCase{CelfnType::Kind::kTimestamp, Type::KIND_TIMESTAMP},
        ScalarCase{CelfnType::Kind::kType, Type::KIND_TYPE}));

// --- TypeFromCelfn: composite kinds ------------------------------

TEST(TypeFromCelfnTest, ProtoCarriesFqn) {
  const Type wire = TypeFromCelfn(Proto("acme.User"));
  EXPECT_EQ(wire.kind(), Type::KIND_PROTO);
  EXPECT_EQ(wire.proto_fqn(), "acme.User");
  EXPECT_EQ(wire.params_size(), 0);
}

TEST(TypeFromCelfnTest, ListElementLandsInParams) {
  const Type wire = TypeFromCelfn(List(Scalar(CelfnType::Kind::kInt)));
  EXPECT_EQ(wire.kind(), Type::KIND_LIST);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_INT);
}

TEST(TypeFromCelfnTest, MapKeyValueLandInParamsInOrder) {
  const Type wire = TypeFromCelfn(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kUint)));
  EXPECT_EQ(wire.kind(), Type::KIND_MAP);
  ASSERT_EQ(wire.params_size(), 2);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_STRING);
  EXPECT_EQ(wire.params(1).kind(), Type::KIND_UINT);
}

TEST(TypeFromCelfnTest, OptionalElementLandsInParams) {
  const Type wire = TypeFromCelfn(Optional(Scalar(CelfnType::Kind::kBool)));
  EXPECT_EQ(wire.kind(), Type::KIND_OPTIONAL);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_BOOL);
}

TEST(TypeFromCelfnTest, DeepNestingRecursesListMapProto) {
  // list<map<string, proto(acme.User)>>
  const Type wire = TypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  ASSERT_EQ(wire.kind(), Type::KIND_LIST);
  ASSERT_EQ(wire.params_size(), 1);
  const Type& map = wire.params(0);
  ASSERT_EQ(map.kind(), Type::KIND_MAP);
  ASSERT_EQ(map.params_size(), 2);
  EXPECT_EQ(map.params(0).kind(), Type::KIND_STRING);
  EXPECT_EQ(map.params(1).kind(), Type::KIND_PROTO);
  EXPECT_EQ(map.params(1).proto_fqn(), "acme.User");
}

// Malformed generic shapes are builder-invariant violations, not
// admissible inputs — they must fail loudly, never emit a
// plausible-looking half-empty wire type.
TEST(TypeFromCelfnDeathTest, ListWithoutElementChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  EXPECT_DEATH(TypeFromCelfn(t), "kList");
}

TEST(TypeFromCelfnDeathTest, MapWithoutKeyValueChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  EXPECT_DEATH(TypeFromCelfn(t), "kMap");
}

TEST(TypeFromCelfnDeathTest, OptionalWithoutElementChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  EXPECT_DEATH(TypeFromCelfn(t), "kOptional");
}

// --- TypeFromCelType: the unified-vocabulary mapping -------------
//
// Pins EVERY representable CelType::Kind → abi::Type.Kind pair,
// asserting the frozen wire numerics (cel_abi.proto Type.Kind:
// BOOL=1 INT=2 UINT=3 DOUBLE=4 STRING=5 BYTES=6 DURATION=7
// TIMESTAMP=8 PROTO=9 LIST=10 MAP=11 TYPE=12 OPTIONAL=13 NULL=14).
// A drifted mapping is a silent wire break — the numeric literals
// here are deliberate.

struct CelTypeScalarCase {
  CelType cel;
  Type::Kind wire = Type::KIND_UNSPECIFIED;
  int wire_number = 0;
};

class TypeFromCelTypeScalarTest
    : public ::testing::TestWithParam<CelTypeScalarCase> {};

TEST_P(TypeFromCelTypeScalarTest, MapsKindWithNoFqnAndNoParams) {
  const Type wire = TypeFromCelType(GetParam().cel);
  EXPECT_EQ(wire.kind(), GetParam().wire);
  EXPECT_EQ(static_cast<int>(wire.kind()), GetParam().wire_number);
  EXPECT_TRUE(wire.proto_fqn().empty());
  EXPECT_EQ(wire.params_size(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    AllScalarKinds, TypeFromCelTypeScalarTest,
    ::testing::Values(
        CelTypeScalarCase{CelType::Bool(), Type::KIND_BOOL, 1},
        CelTypeScalarCase{CelType::Int(), Type::KIND_INT, 2},
        CelTypeScalarCase{CelType::Uint(), Type::KIND_UINT, 3},
        CelTypeScalarCase{CelType::Double(), Type::KIND_DOUBLE, 4},
        CelTypeScalarCase{CelType::String(), Type::KIND_STRING, 5},
        CelTypeScalarCase{CelType::Bytes(), Type::KIND_BYTES, 6},
        CelTypeScalarCase{CelType::Duration(), Type::KIND_DURATION, 7},
        CelTypeScalarCase{CelType::Timestamp(), Type::KIND_TIMESTAMP, 8},
        CelTypeScalarCase{CelType::Type(), Type::KIND_TYPE, 12},
        CelTypeScalarCase{CelType::Null(), Type::KIND_NULL, 14}));

TEST(TypeFromCelTypeTest, MessageCarriesFqnAsProtoKind) {
  const Type wire = TypeFromCelType(CelType::Message("acme.User"));
  EXPECT_EQ(wire.kind(), Type::KIND_PROTO);
  EXPECT_EQ(static_cast<int>(wire.kind()), 9);
  EXPECT_EQ(wire.proto_fqn(), "acme.User");
  EXPECT_EQ(wire.params_size(), 0);
}

TEST(TypeFromCelTypeTest, ListElementLandsInParams) {
  const Type wire = TypeFromCelType(CelType::List(CelType::Int()));
  EXPECT_EQ(wire.kind(), Type::KIND_LIST);
  EXPECT_EQ(static_cast<int>(wire.kind()), 10);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_INT);
}

TEST(TypeFromCelTypeTest, MapKeyValueLandInParamsInOrder) {
  const Type wire =
      TypeFromCelType(CelType::Map(CelType::String(), CelType::Uint()));
  EXPECT_EQ(wire.kind(), Type::KIND_MAP);
  EXPECT_EQ(static_cast<int>(wire.kind()), 11);
  ASSERT_EQ(wire.params_size(), 2);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_STRING);
  EXPECT_EQ(wire.params(1).kind(), Type::KIND_UINT);
}

TEST(TypeFromCelTypeTest, OptionalElementLandsInParams) {
  const Type wire = TypeFromCelType(CelType::Optional(CelType::Bool()));
  EXPECT_EQ(wire.kind(), Type::KIND_OPTIONAL);
  EXPECT_EQ(static_cast<int>(wire.kind()), 13);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), Type::KIND_BOOL);
}

TEST(TypeFromCelTypeTest, DeepNestingRecursesListMapMessage) {
  // list<map<string, acme.User>>
  const Type wire = TypeFromCelType(CelType::List(
      CelType::Map(CelType::String(), CelType::Message("acme.User"))));
  ASSERT_EQ(wire.kind(), Type::KIND_LIST);
  ASSERT_EQ(wire.params_size(), 1);
  const Type& map = wire.params(0);
  ASSERT_EQ(map.kind(), Type::KIND_MAP);
  ASSERT_EQ(map.params_size(), 2);
  EXPECT_EQ(map.params(0).kind(), Type::KIND_STRING);
  EXPECT_EQ(map.params(1).kind(), Type::KIND_PROTO);
  EXPECT_EQ(map.params(1).proto_fqn(), "acme.User");
}

// kUnknown (the default-constructed sentinel) has no wire spelling —
// a builder-invariant violation must fail loudly, never emit a
// plausible-looking KIND_UNSPECIFIED wire type.
TEST(TypeFromCelTypeDeathTest, UnknownKindChecks) {
  EXPECT_DEATH(TypeFromCelType(CelType()), "kUnknown");
}

// --- TypeEquals: positive ----------------------------------------

TEST(TypeEqualsTest, ScalarReflexive) {
  EXPECT_TRUE(
      TypeEquals(WireScalar(Type::KIND_INT), WireScalar(Type::KIND_INT)));
}

TEST(TypeEqualsTest, NestedShapeReflexive) {
  const Type a = TypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  const Type b = TypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  EXPECT_TRUE(TypeEquals(a, b));
}

TEST(TypeEqualsTest, SameProtoFqnEqual) {
  EXPECT_TRUE(TypeEquals(TypeFromCelfn(Proto("acme.User")),
                         TypeFromCelfn(Proto("acme.User"))));
}

// Open-set wire contract: kinds this binary doesn't know compare
// numerically — same number equal, different numbers unequal.
TEST(TypeEqualsTest, UnknownKindsEqualWhenSameNumber) {
  EXPECT_TRUE(TypeEquals(WireUnknownKind(99), WireUnknownKind(99)));
}

TEST(TypeEqualsTest, UnknownKindsUnequalWhenDifferentNumbers) {
  EXPECT_FALSE(TypeEquals(WireUnknownKind(99), WireUnknownKind(100)));
}

TEST(TypeEqualsTest, UnspecifiedEqualsUnspecified) {
  EXPECT_TRUE(TypeEquals(Type(), Type()));
}

// --- TypeEquals: negative, one per matrix axis -------------------

TEST(TypeEqualsTest, DistinguishesKind) {
  EXPECT_FALSE(
      TypeEquals(WireScalar(Type::KIND_INT), WireScalar(Type::KIND_UINT)));
}

TEST(TypeEqualsTest, DistinguishesProtoFqn) {
  EXPECT_FALSE(TypeEquals(TypeFromCelfn(Proto("acme.User")),
                          TypeFromCelfn(Proto("acme.Person"))));
}

TEST(TypeEqualsTest, DistinguishesParamsCount) {
  Type one = WireScalar(Type::KIND_LIST);
  *one.add_params() = WireScalar(Type::KIND_INT);
  Type two = one;
  *two.add_params() = WireScalar(Type::KIND_INT);
  EXPECT_FALSE(TypeEquals(one, two));
}

TEST(TypeEqualsTest, DistinguishesNestedGenericElement) {
  // list<map<string, int>> vs list<map<string, uint>>.
  const Type a = TypeFromCelfn(List(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kInt))));
  const Type b = TypeFromCelfn(List(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kUint))));
  EXPECT_FALSE(TypeEquals(a, b));
}

TEST(TypeEqualsTest, DistinguishesNestedProtoFqn) {
  const Type a = TypeFromCelfn(List(Proto("acme.User")));
  const Type b = TypeFromCelfn(List(Proto("acme.Person")));
  EXPECT_FALSE(TypeEquals(a, b));
}

// --- RenderSignature -----------------------------------------------

RequiredFunction MakeFn(std::string fn_name, Type return_type,
                        std::vector<Type> param_types,
                        bool is_receiver = false) {
  RequiredFunction fn;
  fn.set_fn_name(std::move(fn_name));
  *fn.mutable_return_type() = std::move(return_type);
  for (Type& p : param_types) {
    *fn.add_param_types() = std::move(p);
  }
  fn.set_is_receiver(is_receiver);
  return fn;
}

// The exact spelling frozen by the m35 plan §2's error messages.
TEST(RenderSignatureTest, ProtoParamMatchesPlanSpelling) {
  const RequiredFunction fn = MakeFn("is_adult", WireScalar(Type::KIND_BOOL),
                                     {TypeFromCelfn(Proto("acme.User"))});
  EXPECT_EQ(RenderSignature(fn), "bool is_adult(proto(acme.User))");
}

TEST(RenderSignatureTest, ReceiverRendersThisOnFirstParam) {
  const RequiredFunction fn =
      MakeFn("upper", WireScalar(Type::KIND_STRING),
             {WireScalar(Type::KIND_STRING)}, /*is_receiver=*/true);
  EXPECT_EQ(RenderSignature(fn), "string upper(this string)");
}

TEST(RenderSignatureTest, ZeroParams) {
  const RequiredFunction fn =
      MakeFn("invocation_id", WireScalar(Type::KIND_INT), {});
  EXPECT_EQ(RenderSignature(fn), "int invocation_id()");
}

TEST(RenderSignatureTest, MultipleParamsCommaSeparated) {
  const RequiredFunction fn =
      MakeFn("discount_pct", WireScalar(Type::KIND_INT),
             {WireScalar(Type::KIND_STRING), WireScalar(Type::KIND_DOUBLE)});
  EXPECT_EQ(RenderSignature(fn), "int discount_pct(string, double)");
}

TEST(RenderSignatureTest, NestedGenericsAndGrammarSpellings) {
  // list<int> f(map<string, list<double>>, optional<bytes>, Duration,
  //             Timestamp, null, type)
  const RequiredFunction fn =
      MakeFn("f", TypeFromCelfn(List(Scalar(CelfnType::Kind::kInt))),
             {TypeFromCelfn(Map(Scalar(CelfnType::Kind::kString),
                                List(Scalar(CelfnType::Kind::kDouble)))),
              TypeFromCelfn(Optional(Scalar(CelfnType::Kind::kBytes))),
              WireScalar(Type::KIND_DURATION), WireScalar(Type::KIND_TIMESTAMP),
              WireScalar(Type::KIND_NULL), WireScalar(Type::KIND_TYPE)});
  EXPECT_EQ(RenderSignature(fn),
            "list<int> f(map<string, list<double>>, optional<bytes>, "
            "Duration, Timestamp, null, type)");
}

// Open-set wire data renders, never rejects.
TEST(RenderSignatureTest, UnknownKindRendersNumerically) {
  const RequiredFunction fn =
      MakeFn("mystery", WireUnknownKind(99), {WireScalar(Type::KIND_INT)});
  EXPECT_EQ(RenderSignature(fn), "<kind 99> mystery(int)");
}

TEST(RenderSignatureTest, UnspecifiedKindRendersNumerically) {
  const RequiredFunction fn = MakeFn("f", WireScalar(Type::KIND_BOOL),
                                     {WireScalar(Type::KIND_UNSPECIFIED)});
  EXPECT_EQ(RenderSignature(fn), "bool f(<kind 0>)");
}

// --- RenderType (the public single-type renderer) ----------------

TEST(RenderTypeTest, GrammarSpellingsIncludingNestedComposites) {
  // The grammar spellings (`Duration` / `Timestamp` capitalised,
  // `map<K, V>` with a space) — the contract Plan messages and emit
  // tests pin; compiler.cc diagnostics compose TypeFromCelfn with
  // this renderer so they can't diverge from it.
  EXPECT_EQ(RenderType(WireScalar(Type::KIND_DURATION)), "Duration");
  EXPECT_EQ(RenderType(WireScalar(Type::KIND_TIMESTAMP)), "Timestamp");
  EXPECT_EQ(RenderType(TypeFromCelfn(Map(Scalar(CelfnType::Kind::kString),
                                         Scalar(CelfnType::Kind::kDuration)))),
            "map<string, Duration>");
  EXPECT_EQ(
      RenderType(TypeFromCelfn(Optional(Map(Scalar(CelfnType::Kind::kString),
                                            Scalar(CelfnType::Kind::kInt))))),
      "optional<map<string, int>>");
}

TEST(RenderTypeTest, UnknownKindRendersNumerically) {
  EXPECT_EQ(RenderType(WireUnknownKind(99)), "<kind 99>");
}

// --- RequiredFunctionFromDecl --------------------------------------

CelfnDecl MakeDecl(CelfnDecl::Backend backend, std::string fn_name,
                   std::string overload_id, CelfnType return_type,
                   std::vector<CelfnParam> params) {
  CelfnDecl d;
  d.backend = backend;
  d.fn_name = std::move(fn_name);
  d.overload_id = std::move(overload_id);
  d.params = std::move(params);
  d.num_args = static_cast<uint8_t>(d.params.size() + 1);
  d.is_receiver = !d.params.empty() && d.params[0].is_receiver;
  d.return_type = std::move(return_type);
  return d;
}

TEST(RequiredFunctionFromDeclTest, PluginDeclMapsAllFields) {
  const CelfnDecl decl =
      MakeDecl(CelfnDecl::Backend::kPlugin, "is_adult",
               "is_adult_message_acme_User", Scalar(CelfnType::Kind::kBool),
               {CelfnParam{false, Proto("acme.User"), "u"}});
  const RequiredFunction row = RequiredFunctionFromDecl(decl);
  EXPECT_EQ(row.overload_id(), "is_adult_message_acme_User");
  EXPECT_EQ(row.fn_name(), "is_adult");
  EXPECT_EQ(row.backend(), RequiredFunction::PLUGIN);
  ASSERT_EQ(row.param_types_size(), 1);
  EXPECT_EQ(row.param_types(0).kind(), Type::KIND_PROTO);
  EXPECT_EQ(row.param_types(0).proto_fqn(), "acme.User");
  EXPECT_EQ(row.return_type().kind(), Type::KIND_BOOL);
  EXPECT_FALSE(row.is_receiver());
  EXPECT_EQ(RenderSignature(row), "bool is_adult(proto(acme.User))");
}

TEST(RequiredFunctionFromDeclTest, HostReceiverDeclMapsBackendAndReceiver) {
  const CelfnDecl decl =
      MakeDecl(CelfnDecl::Backend::kHost, "upper", "upper_string",
               Scalar(CelfnType::Kind::kString),
               {CelfnParam{true, Scalar(CelfnType::Kind::kString), "s"}});
  const RequiredFunction row = RequiredFunctionFromDecl(decl);
  EXPECT_EQ(row.backend(), RequiredFunction::HOST);
  EXPECT_TRUE(row.is_receiver());
  EXPECT_EQ(RenderSignature(row), "string upper(this string)");
}

TEST(RequiredFunctionFromDeclDeathTest, CelDefinedDeclChecks) {
  const CelfnDecl decl =
      MakeDecl(CelfnDecl::Backend::kCelDefined, "twice", "twice_int",
               Scalar(CelfnType::Kind::kInt),
               {CelfnParam{false, Scalar(CelfnType::Kind::kInt), "x"}});
  EXPECT_DEATH(RequiredFunctionFromDecl(decl), "has no cel_fn wire backend");
}

}  // namespace
}  // namespace celwasm
