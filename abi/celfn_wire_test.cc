// Tests for abi/celfn_wire.  Covers:
//   - FnTypeFromCelfn: every CelfnType kind (scalars, proto FQN,
//     list / map / optional element recursion, deep nesting), plus
//     CHECK-death on malformed generic shapes.
//   - FnTypeEquals: positive (reflexive over nested shapes, unknown
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

namespace celwasm {
namespace {

using ::celwasm::abi::FnType;
using ::celwasm::abi::RequiredFunction;

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

FnType WireScalar(FnType::Kind kind) {
  FnType t;
  t.set_kind(kind);
  return t;
}

FnType WireUnknownKind(int value) {
  FnType t;
  t.set_kind(static_cast<FnType::Kind>(value));
  return t;
}

// --- FnTypeFromCelfn: scalar kinds ---------------------------------

struct ScalarCase {
  CelfnType::Kind celfn = CelfnType::Kind::kBool;
  FnType::Kind wire = FnType::FN_KIND_UNSPECIFIED;
};

class FnTypeFromCelfnScalarTest : public ::testing::TestWithParam<ScalarCase> {
};

TEST_P(FnTypeFromCelfnScalarTest, MapsKindWithNoFqnAndNoParams) {
  const FnType wire = FnTypeFromCelfn(Scalar(GetParam().celfn));
  EXPECT_EQ(wire.kind(), GetParam().wire);
  EXPECT_TRUE(wire.proto_fqn().empty());
  EXPECT_EQ(wire.params_size(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    AllScalarKinds, FnTypeFromCelfnScalarTest,
    ::testing::Values(
        ScalarCase{CelfnType::Kind::kBool, FnType::FN_KIND_BOOL},
        ScalarCase{CelfnType::Kind::kInt, FnType::FN_KIND_INT},
        ScalarCase{CelfnType::Kind::kUint, FnType::FN_KIND_UINT},
        ScalarCase{CelfnType::Kind::kDouble, FnType::FN_KIND_DOUBLE},
        ScalarCase{CelfnType::Kind::kString, FnType::FN_KIND_STRING},
        ScalarCase{CelfnType::Kind::kBytes, FnType::FN_KIND_BYTES},
        ScalarCase{CelfnType::Kind::kNull, FnType::FN_KIND_NULL},
        ScalarCase{CelfnType::Kind::kDuration, FnType::FN_KIND_DURATION},
        ScalarCase{CelfnType::Kind::kTimestamp, FnType::FN_KIND_TIMESTAMP},
        ScalarCase{CelfnType::Kind::kType, FnType::FN_KIND_TYPE}));

// --- FnTypeFromCelfn: composite kinds ------------------------------

TEST(FnTypeFromCelfnTest, ProtoCarriesFqn) {
  const FnType wire = FnTypeFromCelfn(Proto("acme.User"));
  EXPECT_EQ(wire.kind(), FnType::FN_KIND_PROTO);
  EXPECT_EQ(wire.proto_fqn(), "acme.User");
  EXPECT_EQ(wire.params_size(), 0);
}

TEST(FnTypeFromCelfnTest, ListElementLandsInParams) {
  const FnType wire = FnTypeFromCelfn(List(Scalar(CelfnType::Kind::kInt)));
  EXPECT_EQ(wire.kind(), FnType::FN_KIND_LIST);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), FnType::FN_KIND_INT);
}

TEST(FnTypeFromCelfnTest, MapKeyValueLandInParamsInOrder) {
  const FnType wire = FnTypeFromCelfn(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kUint)));
  EXPECT_EQ(wire.kind(), FnType::FN_KIND_MAP);
  ASSERT_EQ(wire.params_size(), 2);
  EXPECT_EQ(wire.params(0).kind(), FnType::FN_KIND_STRING);
  EXPECT_EQ(wire.params(1).kind(), FnType::FN_KIND_UINT);
}

TEST(FnTypeFromCelfnTest, OptionalElementLandsInParams) {
  const FnType wire = FnTypeFromCelfn(Optional(Scalar(CelfnType::Kind::kBool)));
  EXPECT_EQ(wire.kind(), FnType::FN_KIND_OPTIONAL);
  ASSERT_EQ(wire.params_size(), 1);
  EXPECT_EQ(wire.params(0).kind(), FnType::FN_KIND_BOOL);
}

TEST(FnTypeFromCelfnTest, DeepNestingRecursesListMapProto) {
  // list<map<string, proto(acme.User)>>
  const FnType wire = FnTypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  ASSERT_EQ(wire.kind(), FnType::FN_KIND_LIST);
  ASSERT_EQ(wire.params_size(), 1);
  const FnType& map = wire.params(0);
  ASSERT_EQ(map.kind(), FnType::FN_KIND_MAP);
  ASSERT_EQ(map.params_size(), 2);
  EXPECT_EQ(map.params(0).kind(), FnType::FN_KIND_STRING);
  EXPECT_EQ(map.params(1).kind(), FnType::FN_KIND_PROTO);
  EXPECT_EQ(map.params(1).proto_fqn(), "acme.User");
}

// Malformed generic shapes are builder-invariant violations, not
// admissible inputs — they must fail loudly, never emit a
// plausible-looking half-empty wire type.
TEST(FnTypeFromCelfnDeathTest, ListWithoutElementChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  EXPECT_DEATH(FnTypeFromCelfn(t), "kList");
}

TEST(FnTypeFromCelfnDeathTest, MapWithoutKeyValueChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  EXPECT_DEATH(FnTypeFromCelfn(t), "kMap");
}

TEST(FnTypeFromCelfnDeathTest, OptionalWithoutElementChecks) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  EXPECT_DEATH(FnTypeFromCelfn(t), "kOptional");
}

// --- FnTypeEquals: positive ----------------------------------------

TEST(FnTypeEqualsTest, ScalarReflexive) {
  EXPECT_TRUE(FnTypeEquals(WireScalar(FnType::FN_KIND_INT),
                           WireScalar(FnType::FN_KIND_INT)));
}

TEST(FnTypeEqualsTest, NestedShapeReflexive) {
  const FnType a = FnTypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  const FnType b = FnTypeFromCelfn(
      List(Map(Scalar(CelfnType::Kind::kString), Proto("acme.User"))));
  EXPECT_TRUE(FnTypeEquals(a, b));
}

TEST(FnTypeEqualsTest, SameProtoFqnEqual) {
  EXPECT_TRUE(FnTypeEquals(FnTypeFromCelfn(Proto("acme.User")),
                           FnTypeFromCelfn(Proto("acme.User"))));
}

// Open-set wire contract: kinds this binary doesn't know compare
// numerically — same number equal, different numbers unequal.
TEST(FnTypeEqualsTest, UnknownKindsEqualWhenSameNumber) {
  EXPECT_TRUE(FnTypeEquals(WireUnknownKind(99), WireUnknownKind(99)));
}

TEST(FnTypeEqualsTest, UnknownKindsUnequalWhenDifferentNumbers) {
  EXPECT_FALSE(FnTypeEquals(WireUnknownKind(99), WireUnknownKind(100)));
}

TEST(FnTypeEqualsTest, UnspecifiedEqualsUnspecified) {
  EXPECT_TRUE(FnTypeEquals(FnType(), FnType()));
}

// --- FnTypeEquals: negative, one per matrix axis -------------------

TEST(FnTypeEqualsTest, DistinguishesKind) {
  EXPECT_FALSE(FnTypeEquals(WireScalar(FnType::FN_KIND_INT),
                            WireScalar(FnType::FN_KIND_UINT)));
}

TEST(FnTypeEqualsTest, DistinguishesProtoFqn) {
  EXPECT_FALSE(FnTypeEquals(FnTypeFromCelfn(Proto("acme.User")),
                            FnTypeFromCelfn(Proto("acme.Person"))));
}

TEST(FnTypeEqualsTest, DistinguishesParamsCount) {
  FnType one = WireScalar(FnType::FN_KIND_LIST);
  *one.add_params() = WireScalar(FnType::FN_KIND_INT);
  FnType two = one;
  *two.add_params() = WireScalar(FnType::FN_KIND_INT);
  EXPECT_FALSE(FnTypeEquals(one, two));
}

TEST(FnTypeEqualsTest, DistinguishesNestedGenericElement) {
  // list<map<string, int>> vs list<map<string, uint>>.
  const FnType a = FnTypeFromCelfn(List(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kInt))));
  const FnType b = FnTypeFromCelfn(List(
      Map(Scalar(CelfnType::Kind::kString), Scalar(CelfnType::Kind::kUint))));
  EXPECT_FALSE(FnTypeEquals(a, b));
}

TEST(FnTypeEqualsTest, DistinguishesNestedProtoFqn) {
  const FnType a = FnTypeFromCelfn(List(Proto("acme.User")));
  const FnType b = FnTypeFromCelfn(List(Proto("acme.Person")));
  EXPECT_FALSE(FnTypeEquals(a, b));
}

// --- RenderSignature -----------------------------------------------

RequiredFunction MakeFn(std::string fn_name, FnType return_type,
                        std::vector<FnType> param_types,
                        bool is_receiver = false) {
  RequiredFunction fn;
  fn.set_fn_name(std::move(fn_name));
  *fn.mutable_return_type() = std::move(return_type);
  for (FnType& p : param_types) {
    *fn.add_param_types() = std::move(p);
  }
  fn.set_is_receiver(is_receiver);
  return fn;
}

// The exact spelling frozen by the m35 plan §2's error messages.
TEST(RenderSignatureTest, ProtoParamMatchesPlanSpelling) {
  const RequiredFunction fn =
      MakeFn("is_adult", WireScalar(FnType::FN_KIND_BOOL),
             {FnTypeFromCelfn(Proto("acme.User"))});
  EXPECT_EQ(RenderSignature(fn), "bool is_adult(proto(acme.User))");
}

TEST(RenderSignatureTest, ReceiverRendersThisOnFirstParam) {
  const RequiredFunction fn =
      MakeFn("upper", WireScalar(FnType::FN_KIND_STRING),
             {WireScalar(FnType::FN_KIND_STRING)}, /*is_receiver=*/true);
  EXPECT_EQ(RenderSignature(fn), "string upper(this string)");
}

TEST(RenderSignatureTest, ZeroParams) {
  const RequiredFunction fn =
      MakeFn("invocation_id", WireScalar(FnType::FN_KIND_INT), {});
  EXPECT_EQ(RenderSignature(fn), "int invocation_id()");
}

TEST(RenderSignatureTest, MultipleParamsCommaSeparated) {
  const RequiredFunction fn = MakeFn("discount_pct",
                                     WireScalar(FnType::FN_KIND_INT),
                                     {WireScalar(FnType::FN_KIND_STRING),
                                      WireScalar(FnType::FN_KIND_DOUBLE)});
  EXPECT_EQ(RenderSignature(fn), "int discount_pct(string, double)");
}

TEST(RenderSignatureTest, NestedGenericsAndGrammarSpellings) {
  // list<int> f(map<string, list<double>>, optional<bytes>, Duration,
  //             Timestamp, null, type)
  const RequiredFunction fn = MakeFn(
      "f", FnTypeFromCelfn(List(Scalar(CelfnType::Kind::kInt))),
      {FnTypeFromCelfn(Map(Scalar(CelfnType::Kind::kString),
                           List(Scalar(CelfnType::Kind::kDouble)))),
       FnTypeFromCelfn(Optional(Scalar(CelfnType::Kind::kBytes))),
       WireScalar(FnType::FN_KIND_DURATION),
       WireScalar(FnType::FN_KIND_TIMESTAMP), WireScalar(FnType::FN_KIND_NULL),
       WireScalar(FnType::FN_KIND_TYPE)});
  EXPECT_EQ(RenderSignature(fn),
            "list<int> f(map<string, list<double>>, optional<bytes>, "
            "Duration, Timestamp, null, type)");
}

// Open-set wire data renders, never rejects.
TEST(RenderSignatureTest, UnknownKindRendersNumerically) {
  const RequiredFunction fn =
      MakeFn("mystery", WireUnknownKind(99), {WireScalar(FnType::FN_KIND_INT)});
  EXPECT_EQ(RenderSignature(fn), "<kind 99> mystery(int)");
}

TEST(RenderSignatureTest, UnspecifiedKindRendersNumerically) {
  const RequiredFunction fn = MakeFn("f", WireScalar(FnType::FN_KIND_BOOL),
                                     {WireScalar(FnType::FN_KIND_UNSPECIFIED)});
  EXPECT_EQ(RenderSignature(fn), "bool f(<kind 0>)");
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
  const CelfnDecl decl = MakeDecl(
      CelfnDecl::Backend::kPlugin, "is_adult", "is_adult_message_acme_User",
      Scalar(CelfnType::Kind::kBool),
      {CelfnParam{false, Proto("acme.User"), "u"}});
  const RequiredFunction row = RequiredFunctionFromDecl(decl);
  EXPECT_EQ(row.overload_id(), "is_adult_message_acme_User");
  EXPECT_EQ(row.fn_name(), "is_adult");
  EXPECT_EQ(row.backend(), RequiredFunction::PLUGIN);
  ASSERT_EQ(row.param_types_size(), 1);
  EXPECT_EQ(row.param_types(0).kind(), FnType::FN_KIND_PROTO);
  EXPECT_EQ(row.param_types(0).proto_fqn(), "acme.User");
  EXPECT_EQ(row.return_type().kind(), FnType::FN_KIND_BOOL);
  EXPECT_FALSE(row.is_receiver());
  EXPECT_EQ(RenderSignature(row), "bool is_adult(proto(acme.User))");
}

TEST(RequiredFunctionFromDeclTest, HostReceiverDeclMapsBackendAndReceiver) {
  const CelfnDecl decl = MakeDecl(
      CelfnDecl::Backend::kHost, "upper", "upper_string",
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
  EXPECT_DEATH(RequiredFunctionFromDecl(decl),
               "has no cel_fn wire backend");
}

}  // namespace
}  // namespace celwasm
