# M18 — `network_ext` extension (IP / CIDR)

Status: **plan — drafted 2026-05-24; checker assumptions probe-validated
2026-05-24 (GREEN).**  Target:
`tests/simple/testdata/network_ext.textproto` (69 rows, currently
0 PASS — all SKIP under `ext_unimpl`).  Conformance is the definition
of done.

> **Slice-0 checker probe: GREEN** (`m18-ast-probe-findings.md`;
> `compiler_v2/probes/network/ast_shape_probe_test.cc`, 9 tests).
> The load-bearing risk — "can we self-declare the two types + ~16
> overloads with no cel-cpp library?" — is **retired**: all 69 corpus
> shapes type-check against a plain `TypeCheckerBuilder`, including the
> `type(...) == net.IP/net.CIDR` rows.  The working decl recipe is in
> the findings doc.  §3.1 / §8 below are reconciled to it; the only
> remaining Slice-0 work is the §3.2 runtime-representation WAT (the
> probe covered the checker surface only).

> **The big architectural delta from M12/M16.**  `string_ext`, `math`,
> `optionals`, and `encoders` all came with a cel-cpp checker library
> (`StringsCheckerLibrary()`, `MathCheckerLibrary()`, …) that supplied
> the parser macros + type decls for free.  **cel-cpp ships NO network
> extension** — only a `codelab/network_functions.*` tutorial, not a
> real `extensions/` library.  So M18 must **declare the functions and
> the two new types ourselves** in the checker.  That self-declaration
> is the load-bearing new work; the runtime kernels are conventional
> self-hosted C (like math/string).

> **Two new CEL types.**  The corpus uses `type(ip(...)) == net.IP` and
> `type(cidr(...)) == net.CIDR`, with `string()` round-trips and `==`.
> So `net.IP` and `net.CIDR` are first-class abstract types, not
> string aliases — closer to M14's `optional<T>` (new kind +
> representation) than to M16's pure functions over existing kinds.

## 1. Why M18

`network_ext` is the **largest single unimplemented bucket** (69 rows).
It's self-hostable (pure operate-on-bytes/strings, no descriptor pool,
no host round-trip), but it's a genuine milestone: two new types, IPv4
+ IPv6 parsing/canonicalization, and ~16 functions including
overloaded receiver methods.

## 2. Scope

### 2.1 The two types

| Type | Spec name | Constructed by | Runtime payload (proposed) |
|---|---|---|---|
| IP address | `net.IP` | `ip(string)` | 16 bytes (v4 mapped into v6 or a version tag) + family flag |
| CIDR prefix | `net.CIDR` | `cidr(string)` | IP bytes + prefix length (u8) |

Both round-trip to canonical text via `string(...)` and compare with
`==` (structural / normalized equality).  `type(...)` yields `net.IP`
/ `net.CIDR`.

### 2.2 Functions (≈16)

Enumerated from the corpus (`grep` of `network_ext.textproto`):

| Function | Form | Signature | Result |
|---|---|---|---|
| `ip(s)` | global | `(string) → net.IP` | parse; **eval-error on invalid** |
| `cidr(s)` | global | `(string) → net.CIDR` | parse; eval-error on bad mask / zone |
| `isIP(s)` / `isIP(s, ver)` | global | `(string[, int]) → bool` | validate without constructing |
| `ip.isCanonical(s)` | namespace | `(string) → bool` | canonical-form check; errors on invalid |
| `string(x)` | global | `(net.IP\|net.CIDR) → string` | canonical text |
| `type(x)` | global | `(net.IP\|net.CIDR) → type` | `net.IP` / `net.CIDR` |
| `<ip>.family()` | receiver | `() → int` | 4 or 6 |
| `<ip>.isLoopback()` | receiver | `() → bool` | classification |
| `<ip>.isUnspecified()` | receiver | `() → bool` | classification |
| `<ip>.isGlobalUnicast()` | receiver | `() → bool` | classification |
| `<ip>.isLinkLocalUnicast()` | receiver | `() → bool` | classification |
| `<ip>.isLinkLocalMulticast()` | receiver | `() → bool` | classification |
| `<cidr>.containsIP(net.IP)` / `(string)` | receiver | `→ bool` | membership (overloaded) |
| `<cidr>.containsCIDR(net.CIDR)` / `(string)` | receiver | `→ bool` | subnet containment (overloaded) |
| `<cidr>.ip()` | receiver | `() → net.IP` | network address |
| `<cidr>.masked()` | receiver | `() → net.CIDR` | zero the host bits |
| `<cidr>.prefixLength()` | receiver | `() → int` | mask length |
| `==` on `net.IP` / `net.CIDR` | operator | structural equality | bool — **no decl needed** (standard `equals` resolves on the abstract types; probe-confirmed). Runtime still needs new-kind arms in `cel_equals`. |

Both IPv4 and IPv6 are in scope (the corpus exercises `2001:db8::68`,
`::ffff:192.168.0.1`, zone-id rejection `fe80::1%en0`, non-canonical
uppercase, etc.).

### 2.3 Out of scope

  - Any `disable_check: true` rows (none expected here — network_ext
    is eval-driven).
  - Hostname / DNS resolution (not in the extension).

## 3. The two architectural decisions (freeze in Slice 0)

These are the M18-specific unknowns; resolve them WAT-first + with a
checker probe before any production code, mirroring M16's probe.

### 3.1 Declaring the functions + types without a cel-cpp library — **RESOLVED (probe GREEN)**

Confirmed working recipe (full code in `m18-ast-probe-findings.md`):

  - **Types**: `cel::Type(cel::OpaqueType(arena, "net.IP", /*params=*/{}))`
    and `"net.CIDR"` — named zero-param abstract types, identical to how
    `OptionalType` is an `OpaqueType`.  **No `AddTypeDecl` call exists
    or is needed** — the types live only as overload arg/result types +
    the `TypeType` of the bare-literal variables.
  - **Functions**: `MakeFunctionDecl(name, MakeOverloadDecl(id, result,
    args…) / MakeMemberOverloadDecl(id, result, receiver, args…))` then
    `builder.AddFunction(decl)` — the same path
    `RegisterCustomFunctionsOnChecker` already uses.
  - **Shared names need `MergeFunction` (not `AddFunction`)**: `string`
    (extend the stdlib's), and `ip` (the member `<cidr>.ip()` shares the
    name with the global `ip()` constructor — member-vs-global flag
    disambiguates).
  - **Receiver methods** use `MakeMemberOverloadDecl` (receiver is the
    first decl arg) → reach codegen as `kCallExpr` with `has_target=true`
    — the exact string_ext receiver shape M12 already handles.
  - **Bare type literals** `net.IP` / `net.CIDR`:
    `AddVariable(MakeVariableDecl("net.IP", TypeType(arena, ipType)))`
    (mirrors `optional_type`).  They reach the AST as a single
    `kIdentExpr` (the qualified-name resolver swallows the dot).
  - **`ip.isCanonical`** is a **global function literally named
    `"ip.isCanonical"`** (`has_target=false`), NOT a receiver or a select
    — codegen routes on the function name; no namespace special-casing.
  - **`==` and `type()` need no decl** — the standard `equals` / `type`
    overloads resolve on the abstract types for free.

The probe's overload-id strings (`net_ip_string`, `net_cidr_containsIP_ip`,
…) become the Slice-D `overload_table.cc` seeds (adjust to the repo id
convention).

### 3.2 Runtime representation of `net.IP` / `net.CIDR`

  - **New `CelKind`s** (`CEL_IP`, `CEL_CIDR` — next tail values after
    `CEL_LIST_HOST = 17`) vs a tagged opaque encoding.  New kinds are
    cleanest for `type()` / `==` dispatch.
  - Payload: store the **parsed 16-byte form + family + (CIDR) prefix
    len**, not the source string — `containsIP` / `masked` /
    classification need bit math, and `==` needs normalized bytes.
    `string()` re-canonicalizes from bytes.
  - WAT-first trace (`wat/m18_ip_parse_string.wat`,
    `wat/m18_cidr_contains.wat`) freezes the value layout + the
    parse/contain kernel ABI before C is written.

## 4. File structure (self-hosted, mirrors M16 §4.1)

All kernels in one `cel_net_ext.c` (per the one-file convention):

  - `compiler_v2/runtime/cel_net_ext.h` — public ABI: parse
    (`cel_ip_parse_at_v`, `cel_cidr_parse_at_v`), `isIP`,
    `is_canonical`, `family`, the 5 classification predicates,
    `contains_ip` / `contains_cidr`, `cidr_ip`, `masked`,
    `prefix_length`, `string`-conversions, `==` (or route through
    `cel_equals` with new-kind arms).
  - `compiler_v2/runtime/cel_net_ext.c` — IPv4/IPv6 parser +
    canonicaliser + the kernels (the bulk of the work).
  - `compiler_v2/runtime/cel_net_ext_test.cc` — parse matrix
    (valid/invalid v4+v6, zone rejection, canonical vs not, boundary
    masks /0 /32 /128), containment, masking, classification truth
    tables, equality.
  - Wiring: `wasm_exports.txt`, `runtime_catalogue.cc` (arities),
    `overload_table.cc` seeds, and the **custom checker decls** in
    `parse_and_check.cc` (the new part — no library).
  - Equality/type integration: new-kind arms in `cel_equals` /
    `cel_type_of` (runtime) so `==` and `type()` work on IP/CIDR.

## 5. Test strategy

  - Native kernel matrix (`cel_net_ext_test.cc`): the parse edge cases
    are the load-bearing half (every invalid form the corpus rejects;
    v4-in-v6 mapping; canonical detection; /0 and max-prefix masks).
  - Host e2e (`e2e/m18_test.cc`): every function × v4/v6, the
    string-arg overloads of `containsIP`/`containsCIDR`, `type()`/
    `string()` round-trips, and the eval-error rows.
  - Conformance lock: `network_ext.textproto` 0 → target ~69 PASS;
    `.baseline` bump; README regen.

## 6. Slicing

  - **Slice 0 — probe + WAT.**  *Checker probe: DONE (GREEN, see
    `m18-ast-probe-findings.md`).*  **Remaining:** WAT traces freezing
    §3.2 (IP value layout + parse + contains ABI:
    `wat/m18_ip_parse_string.wat`, `wat/m18_cidr_contains.wat`).
  - **Slice A — `ip` type + parse/validate.**  `ip()`, `isIP`,
    `ip.isCanonical`, `string(ip)`, `type(ip)`, `==` on IP.  IPv4 +
    IPv6 parser + canonicaliser (the hardest single piece).
  - **Slice B — IP predicates.**  `family`, `isLoopback`,
    `isUnspecified`, `isGlobalUnicast`, `isLinkLocalUnicast`,
    `isLinkLocalMulticast` (bit-pattern classification).
  - **Slice C — `cidr` type + ops.**  `cidr()`, `string(cidr)`,
    `type(cidr)`, `==`, `containsIP` (×2 overloads), `containsCIDR`
    (×2), `ip()`, `masked()`, `prefixLength()`.
  - **Slice D — checker decls wiring + conformance lock.**  Register
    the self-declared decls; `m18_test.cc`; conformance + baseline +
    README.

## 7. Risks

  - ~~**Self-declared decls are new ground.**~~ **RETIRED by the
    Slice-0 probe** — self-declaration works for all 69 shapes; no
    string-typed fallback needed.
  - **IPv6 parsing is the real work, and it's fiddly.**  `::`
    compression, v4-mapped forms, zone IDs, canonical-form rules.
    Corpus subtlety the probe surfaced: `ip('::ffff:192.168.0.1')`
    (dotted-decimal v4-mapped) is **rejected** ("IPv4-mapped IPv6
    address is not allowed"), but `ip('::ffff:c0a8:1')` (hex
    v4-mapped) is **accepted** and compares **equal** to
    `ip('192.168.0.1')`.  So v4-mapped handling is *form-sensitive* —
    pin both.  Pin every parse case to the corpus's exact eval-error
    strings (enumerated in the findings doc §"Eval-error message
    strings"); consider vendoring a small parser over hand-rolling.
  - **Equality/type() integration touches shared runtime.**  New
    `CelKind`s ripple into `cel_equals`, `cel_type_of`, the host
    pretty-printer, and any closed-`switch` over kinds (each gets a
    new arm or a loud `ABSL_CHECK`).
  - **No conformance credit for `string()`-only modeling.**  If we
    skip distinct kinds and model IP/CIDR as strings, the
    `type(...) == net.IP` rows fail — so the type work is load-bearing
    for the row count, not optional polish.

## 8. Open questions

  - ~~§3.1 checker API~~ — **RESOLVED** (probe; see §3.1).
  - ~~`isIP(s, ver)` 2-arg form~~ — **RESOLVED**: does NOT appear in the
    corpus; declaring it is optional (out of M18 scope unless we want
    spec parity).
  - **§3.2 — still open** (runtime representation): new `CelKind`s
    (`CEL_IP`/`CEL_CIDR`) vs opaque tag; 16-byte-always vs
    version-tagged payload.  The sole remaining Slice-0 item; resolve
    WAT-first.
  - **Parser: vendor vs hand-roll** — decide once the parse matrix is
    enumerated (the findings doc's error-string table + the v4-mapped
    form-sensitivity are the seed of that matrix).

## 9. Closeout gate (copy into the PR)

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] Kernel matrix: valid/invalid v4+v6 parse, canonical, masks,
        containment, classification, equality — positive + negative +
        boundary.
  - [ ] `m18_test.cc` e2e: every function × v4/v6 + eval-error rows.
  - [ ] Slice-0 WATs assemble + (once kernels land) run via
        `wat_runner`.
  - [ ] `network_ext.textproto` → PASS target met; `.baseline` bump;
        `conformance/README.md` regenerated (drift gate clean).
  - [ ] New `CelKind`s handled in every closed kind-switch (equality,
        type, pretty-printer) — no silent default.
  - [ ] `testing-checklist.md` row + this doc closed out.
