# M18 — `network_ext` extension (IP / CIDR)

Status: **plan — drafted 2026-05-24, not yet started.**  Target:
`tests/simple/testdata/network_ext.textproto` (69 rows, currently
0 PASS — all SKIP under `ext_unimpl`).  Conformance is the definition
of done.

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
| `==` on `net.IP` / `net.CIDR` | operator | structural equality | bool |

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

### 3.1 Declaring the functions + types without a cel-cpp library

We already hand-declare decls in two places:
`parse_and_check.cc` (`AddVariable`, and the custom-fn path
`RegisterCustomFunctionsOnChecker`).  Slice 0 probe answers:

  - Can cel-cpp's `TypeCheckerBuilder` register **abstract type
    decls** (`net.IP`, `net.CIDR`) + the ~16 function overloads
    directly (no library wrapper)?  What's the API
    (`AddFunction` / `MergeFunction` / `AddTypeDecl`)?
  - How does a receiver overload (`<cidr>.containsIP(...)`) get
    declared so it reaches codegen as a `kCallExpr` with target —
    same shape as string_ext receivers (probe Q from M12)?
  - Does `type(ip(...))` need `net.IP` registered as an **ident** of
    type `type(net.IP)` (mirrors how `optional_type` was handled in
    M14)?

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

  - **Slice 0 — probe + WAT (mandatory).**  Checker probe answering
    §3.1 (can we declare the types + overloads without a library, and
    what AST shapes result).  WAT traces freezing §3.2 (IP value
    layout + parse + contains ABI).  Milestone doc reconciled to the
    evidence (as M16 did).
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

  - **Self-declared decls are new ground.**  We've never registered
    abstract *types* in the checker without a cel-cpp library — Slice
    0 must prove the API works (else fall back to representing IP/CIDR
    as opaque/string-typed with runtime-only semantics, losing
    `type()`-distinctness rows).
  - **IPv6 parsing is fiddly.**  `::` compression, v4-mapped
    (`::ffff:1.2.3.4`), zone IDs (rejected), canonical-form rules
    (lowercase, `::` placement).  Pin every case to the corpus, not
    intuition; consider vendoring a small parser rather than
    hand-rolling.
  - **Equality/type() integration touches shared runtime.**  New
    `CelKind`s ripple into `cel_equals`, `cel_type_of`, the host
    pretty-printer, and any closed-`switch` over kinds (each gets a
    new arm or a loud `ABSL_CHECK`).
  - **No conformance credit for `string()`-only modeling.**  If we
    skip distinct kinds and model IP/CIDR as strings, the
    `type(...) == net.IP` rows fail — so the type work is load-bearing
    for the row count, not optional polish.

## 8. Open questions (for Slice 0)

  - §3.1 — exact checker API for self-declared types + overloads;
    whether receiver overloads need anything beyond M12's pattern.
  - §3.2 — new `CelKind`s vs opaque tag; 16-byte-always vs
    version-tagged payload.
  - Does `isIP(s, ver)` (2-arg version) appear, and what `ver` values
    (4 / 6)?  Confirm from the corpus during Slice 0.
  - Vendor a parser (e.g. a tiny inet_pton-style routine) vs
    hand-roll — decide once the parse matrix is enumerated.

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
