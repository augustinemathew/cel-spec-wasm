# M18 `network_ext` — Slice-0 AST-shape probe findings

Status: **probe complete 2026-05-24.**  Probe:
`compiler/probes/network/ast_shape_probe_test.cc` (manual-tagged,
9 tests, all green).  Run:

```
bazel test //compiler/probes/network:ast_shape_probe_test --test_output=all
```

## VERDICT

**GREEN — every shape in `network_ext.textproto` type-checks against
self-declared decls.  No cel-cpp library is needed, and no shape
fails to type-check.**  The two new types and all ~16 functions
(including the overloaded receiver methods and the bare type
literals) were declared on a plain `TypeCheckerBuilder` using only
the `OpaqueType` + `MakeFunctionDecl`/`MakeOverloadDecl`/
`MakeMemberOverloadDecl` + `AddFunction`/`MergeFunction` +
`AddVariable` API — the exact pattern `checker/optional.cc` uses.
The risk flagged in plan §7 ("self-declared decls are new ground …
else fall back to string-typed") is **retired**: we get full
`type()`-distinctness and the abstract types fall out cleanly.

The plan's open §3.1 / §8 questions are all answered below.  Two
plan assumptions turned out **more favorable** than written (see
"Assumptions that changed").

## The decl recipe that worked (verbatim from the probe)

Types are **named abstract (opaque) types with zero parameters** —
identical to how `OptionalType` is an `OpaqueType` under the hood.
A `cel::Type` wraps the `OpaqueType` via its templated alternative
ctor.  The arena must outlive the checker.

```cpp
cel::Type NetIpType(google::protobuf::Arena* arena) {
  return cel::Type(cel::OpaqueType(arena, "net.IP", /*parameters=*/{}));
}
cel::Type NetCidrType(google::protobuf::Arena* arena) {
  return cel::Type(cel::OpaqueType(arena, "net.CIDR", /*parameters=*/{}));
}
```

Global constructor + validator (one `MakeFunctionDecl` per name,
`AddFunction`):

```cpp
auto d = cel::MakeFunctionDecl(
    "ip", cel::MakeOverloadDecl("net_ip_string", ip, str));
builder.AddFunction(*d);

auto isip = cel::MakeFunctionDecl(
    "isIP", cel::MakeOverloadDecl("net_isIP_string", bln, str),
    cel::MakeOverloadDecl("net_isIP_string_int", bln, str, i64));
builder.AddFunction(*isip);
```

Namespace function `ip.isCanonical` — declare the **dotted name as a
single global function name**, NOT a receiver:

```cpp
auto d = cel::MakeFunctionDecl(
    "ip.isCanonical",
    cel::MakeOverloadDecl("net_ip_isCanonical_string", bln, str));
builder.AddFunction(*d);
```

Receiver methods — `MakeMemberOverloadDecl`, first arg is the
receiver type:

```cpp
auto fam = cel::MakeFunctionDecl(
    "family", cel::MakeMemberOverloadDecl("net_ip_family", i64, ip));
auto contains = cel::MakeFunctionDecl(
    "containsIP",
    cel::MakeMemberOverloadDecl("net_cidr_containsIP_ip", bln, cidr, ip),
    cel::MakeMemberOverloadDecl("net_cidr_containsIP_string", bln, cidr, str));
```

`string()` over the new kinds — extend the standard-lib `string`
function with **`MergeFunction`** (not `AddFunction`):

```cpp
auto d = cel::MakeFunctionDecl(
    "string", cel::MakeOverloadDecl("net_string_ip", str, ip),
    cel::MakeOverloadDecl("net_string_cidr", str, cidr));
builder.MergeFunction(*d);
```

`<cidr>.ip()` member method shares the name `"ip"` with the global
constructor — register via **`MergeFunction`** so the member overload
joins the existing global decl (member-vs-global flag disambiguates):

```cpp
auto d = cel::MakeFunctionDecl(
    "ip", cel::MakeMemberOverloadDecl("net_cidr_ip", ip, cidr));
builder.MergeFunction(*d);
```

Bare type literals `net.IP` / `net.CIDR` — register as **variable
decls of `TypeType`**, exactly how `optional.cc` registers
`optional_type`:

```cpp
builder.AddVariable(cel::MakeVariableDecl(
    "net.IP", cel::Type(cel::TypeType(arena, ip))));
builder.AddVariable(cel::MakeVariableDecl(
    "net.CIDR", cel::Type(cel::TypeType(arena, cidr))));
```

`==` needs **no decl** — it falls out of the standard `equals`
overload (`_==_` resolves to overload id `equals` on the abstract
types).  `type()` likewise resolves via the standard `type` overload.

## Per-shape evidence table

`ip(...)` → global `kCallExpr`, result `net.IP` (abstract_type).
`cidr(...)` → global `kCallExpr`, result `net.CIDR`.
Receiver methods → `kCallExpr` with `has_target=true`, args = method
args only (receiver is the `target`, not an arg).

| Expr | AST shape | overload id | result type |
|---|---|---|---|
| `string(ip('…'))` | global call(global call) | `net_string_ip` / `net_ip_string` | string |
| `ip('…bad…')` | global call | `net_ip_string` | net.IP (eval-error at runtime) |
| `isIP('…')` | global call | `net_isIP_string` | bool |
| `ip.isCanonical('…')` | **global call, fn="ip.isCanonical", no target** | `net_ip_isCanonical_string` | bool |
| `type(ip('…')) == net.IP` | `_==_`(type(call), **ident net.IP**) | `equals` / `type` / `net_ip_string` | bool |
| `isIP(cidr('…'))` | **TYPE-CHECK ERROR** | — | — |
| `ip('…').family()` | receiver call, target=ip call | `net_ip_family` | int |
| `ip('…').isUnspecified()` | receiver call | `net_ip_isUnspecified` | bool |
| `ip('…').isLoopback()` | receiver call | `net_ip_isLoopback` | bool |
| `ip('…').isGlobalUnicast()` | receiver call | `net_ip_isGlobalUnicast` | bool |
| `ip('…').isLinkLocalUnicast()` | receiver call | `net_ip_isLinkLocalUnicast` | bool |
| `ip('…').isLinkLocalMulticast()` | receiver call | `net_ip_isLinkLocalMulticast` | bool |
| `ip('…') == ip('…')` | `_==_`(call, call) | `equals` | bool |
| `type(cidr('…')) == net.CIDR` | `_==_`(type(call), ident net.CIDR) | `equals` / `type` | bool |
| `cidr('…') == cidr('…')` | `_==_`(call, call) | `equals` | bool |
| `cidr('…').containsIP(ip('…'))` | receiver call, 1 arg | `net_cidr_containsIP_ip` | bool |
| `cidr('…').containsIP('…')` | receiver call, 1 arg | `net_cidr_containsIP_string` | bool |
| `cidr('…').containsCIDR(cidr('…'))` | receiver call, 1 arg | `net_cidr_containsCIDR_cidr` | bool |
| `cidr('…').containsCIDR('…')` | receiver call, 1 arg | `net_cidr_containsCIDR_string` | bool |
| `cidr('…').ip()` | receiver call, 0 args, fn="ip" | `net_cidr_ip` | net.IP |
| `cidr('…').masked()` | receiver call, 0 args | `net_cidr_masked` | net.CIDR |
| `cidr('…').prefixLength()` | receiver call, 0 args | `net_cidr_prefixLength` | int |
| `string(cidr('…'))` | global call | `net_string_cidr` | string |
| `net.IP` | **single `kIdentExpr` name="net.IP"** | (ref.name=net.IP) | type(net.IP) |
| `net.CIDR` | **single `kIdentExpr` name="net.CIDR"** | (ref.name=net.CIDR) | type(net.CIDR) |

## Answers to the plan's specific probe questions (§3.1, §8)

1. **Can `TypeCheckerBuilder` register abstract type decls + the ~16
   overloads with no library?**  YES.  No `AddTypeDecl` call is even
   needed — the abstract types exist purely as the result/arg types of
   the function overloads + the `TypeType` of the bare-literal
   variables.  API is `AddFunction` (new name) / `MergeFunction`
   (extend an existing name, e.g. `string`, `ip`) / `AddVariable`
   (bare type literals).

2. **Do receiver overloads reach codegen as `kCallExpr` with target?**
   YES — `MakeMemberOverloadDecl` produces exactly the string_ext
   receiver shape: `has_target=true`, the receiver is the `target`,
   and the method's own args are `args`.  Same handling M12 already
   wired.

3. **Does `type(ip(...))` need `net.IP` registered as an ident of type
   `type(net.IP)`?**  YES — `AddVariable(MakeVariableDecl("net.IP",
   TypeType(arena, net.IP)))`.  With it, `net.IP` resolves as a single
   `kIdentExpr` (the checker's qualified-name resolution swallows the
   dot into one ident because the variable name contains it), and
   `type(ip(...)) == net.IP` type-checks to bool.

4. **`ip.isCanonical` — namespace function or receiver?**  Namespace
   function.  The parser folds the dotted name into the **function
   name** of a global `kCallExpr` (`fn="ip.isCanonical"`,
   `has_target=false`).  It is NOT a select on the `ip` constructor
   and NOT a receiver.  Disambiguation from the `ip()` constructor is
   free: `ip(s)` is a 1-arg call to the function named `"ip"`;
   `ip.isCanonical(s)` is a 1-arg call to the distinctly-named
   function `"ip.isCanonical"`.  Codegen routes on the function name.

5. **Does `isIP(s, int)` 2-arg version appear in the corpus?**  NO.
   The corpus only exercises `isIP(string)`.  I declared the 2-arg
   `isIP(string,int)` overload defensively and it type-checks, but it
   is **not required for conformance** and can be dropped from M18
   scope unless we want spec parity.  (No corpus row supplies a
   version int.)

6. **Does `string(net.IP)` / `type(net.IP)` / `==` need special
   handling?**  NO for `type`/`==` (standard overloads `type` /
   `equals` resolve on the abstract types).  `string` needs the two
   extra overloads merged in via `MergeFunction` — but that is a decl
   addition, not special checker logic.

## Full enumerated function / overload surface (as declared)

| fn name | form | overload id | sig |
|---|---|---|---|
| `ip` | global | `net_ip_string` | (string) → net.IP |
| `ip` | member | `net_cidr_ip` | net.CIDR.() → net.IP |
| `cidr` | global | `net_cidr_string` | (string) → net.CIDR |
| `isIP` | global | `net_isIP_string` | (string) → bool |
| `isIP` | global | `net_isIP_string_int` | (string,int) → bool *(not in corpus)* |
| `ip.isCanonical` | global | `net_ip_isCanonical_string` | (string) → bool |
| `string` | global | `net_string_ip` | (net.IP) → string |
| `string` | global | `net_string_cidr` | (net.CIDR) → string |
| `family` | member | `net_ip_family` | net.IP.() → int |
| `isLoopback` | member | `net_ip_isLoopback` | net.IP.() → bool |
| `isUnspecified` | member | `net_ip_isUnspecified` | net.IP.() → bool |
| `isGlobalUnicast` | member | `net_ip_isGlobalUnicast` | net.IP.() → bool |
| `isLinkLocalUnicast` | member | `net_ip_isLinkLocalUnicast` | net.IP.() → bool |
| `isLinkLocalMulticast` | member | `net_ip_isLinkLocalMulticast` | net.IP.() → bool |
| `containsIP` | member | `net_cidr_containsIP_ip` | net.CIDR.(net.IP) → bool |
| `containsIP` | member | `net_cidr_containsIP_string` | net.CIDR.(string) → bool |
| `containsCIDR` | member | `net_cidr_containsCIDR_cidr` | net.CIDR.(net.CIDR) → bool |
| `containsCIDR` | member | `net_cidr_containsCIDR_string` | net.CIDR.(string) → bool |
| `masked` | member | `net_cidr_masked` | net.CIDR.() → net.CIDR |
| `prefixLength` | member | `net_cidr_prefixLength` | net.CIDR.() → int |
| `_==_` | (standard) | `equals` | (T,T) → bool — works on net.IP / net.CIDR free |
| `type` | (standard) | `type` | (T) → type(T) — works free |

(Overload-id strings above are the probe's chosen names — they become
the seeds for `overload_table.cc` in Slice D.  Adjust to match the
repo's id convention before wiring.)

## Eval-error message strings the corpus expects (runtime kernel, not checker)

These are NOT checker errors — every one of these rows type-checks
fine; the kernel must produce these exact strings at eval time:

| row | expr | expected eval-error message |
|---|---|---|
| parse_invalid_ipv4 | `ip('192.168.0.1.0')` | `IP Address '192.168.0.1.0' parse error during conversion from string` |
| ip_is_canonical_invalid_ipv4 | `ip.isCanonical('127.0.0.1.0')` | `IP Address '127.0.0.1.0' parse error during conversion from string` |
| parse_invalid_ipv6 | `ip('2001:db8:::68')` | `IP Address '2001:db8:::68' parse error` |
| parse_invalid_ipv6_with_zone | `ip('fe80::1%en0')` | `IP Address with zone value is not allowed` |
| parse_invalid_ipv4_in_ipv6 | `ip('::ffff:192.168.0.1')` | `IPv4-mapped IPv6 address is not allowed` |
| parse_invalid_cidr_ipv4 | `cidr('192.168.0.0/')` | `network address parse error during conversion from string` |
| parse_invalid_cidr_with_zone | `cidr('fe80::1%en0/24')` | `CIDR with zone value is not allowed` |
| parse_invalid_cidr_ipv4_in_ipv6 | `cidr('::ffff:192.168.0.1/24')` | `IPv4-mapped IPv6 address is not allowed` |

The single **checker** error in the corpus (`is_ip_cidr_compile_error`,
`disable_check: false`) expects:
`found no matching overload for 'isIP' applied to '(net.CIDR)'` — the
probe confirms our self-declared `isIP` decl produces **byte-identical**
text: `ERROR: :1:5: found no matching overload for 'isIP' applied to
'(net.CIDR)'`.

> Note on `ip('::ffff:192.168.0.1')` vs `ip('::ffff:c0a8:1')`: the
> corpus REJECTS the dotted-decimal v4-mapped form
> (`::ffff:192.168.0.1` → "IPv4-mapped IPv6 address is not allowed")
> but ACCEPTS the hex v4-mapped form `::ffff:c0a8:1` and treats it as
> EQUAL to `ip('192.168.0.1')` (row `ipv4_equals_ipv6`).  The kernel's
> v4-mapped handling is therefore form-sensitive — pin both cases.

## Assumptions that changed (plan-vs-probe deltas)

- **§3.1 assumed an `AddTypeDecl`-style call might be required to
  register the abstract types.**  FALSE — there is no such call and
  none is needed.  The types exist solely as overload arg/result types
  + the `TypeType` of the bare-literal variables.  This is strictly
  simpler than the plan anticipated.

- **§7 risk "fall back to string-typed, losing `type()` rows" is
  retired.**  The abstract-type path works end-to-end at the checker
  level for all 69 rows' shapes, including the `type(...) == net.IP`
  / `net.CIDR` rows.  No fallback needed.

- **`==` was listed in the plan §2.2 function table as something to
  declare.**  FALSE — it needs no decl; the standard `equals` overload
  resolves on the abstract types automatically.  (The runtime still
  needs new-kind arms in `cel_equals`, per §7 — that part stands.)

- **`isIP(string, int)` 2-arg overload (§8 open question): does NOT
  appear in the corpus.**  Declaring it is optional for conformance.

## Remaining Slice-0 work (not part of this probe)

The §3.2 runtime-representation decisions (new `CelKind`s vs opaque
tag; 16-byte payload layout; parse/contains kernel ABI) are a
separate WAT-first exercise (`wat/m18_ip_parse_string.wat`,
`wat/m18_cidr_contains.wat`) and are NOT addressed here — this probe
covers only the checker/AST surface (§3.1).
