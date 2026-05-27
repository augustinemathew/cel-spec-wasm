#!/usr/bin/env python3
"""Generates the runtime-helper catalogue from the `cel_runtime` C source.

The C source is the SINGLE source of truth.  Every pure-wasm helper that an
emitted expr module imports under the `cel` module is marked at its
declaration with a `// cel:codegen-export` comment, e.g.

    // cel:codegen-export
    void cel_int_add_at_vv(uint32_t out, uint32_t a, uint32_t b);

The marker carries the one fact that is NOT recoverable from the signature
alone — membership in the codegen-helper set (`cel_int_add_at_vv` is
codegen-imported; its sibling `cel_int_eq_at_vv`, reached only by tail-call
from the polymorphic dispatcher, is not).  The signature carries the rest:
arity is the parameter count, and the return shape is the return type
(`void` writes through an out-slot, `uint32_t` returns an i32).  clang
lowers these one-to-one to the wasm function types, so reading the C source
recovers the identical (arity, returns_i32) tuple the wasm type section
would — with no external tool and no wasm cross-compile.

Two outputs, selected by mode:

  --mode=textproto   the composed `CelRuntimeCatalogue` textproto (see
                     `abi/runtime_catalogue.proto`), embedded as a build
                     resource and parsed by `abi/runtime_catalogue.cc`.
                     It is the verbatim committed `cel_host` / `cel_env`
                     rows passed via `--base`
                     (`abi/runtime_host_env.textproto`, the source of
                     truth for the host/env imports) followed by the
                     `cel` rows derived from the markers.  This script is
                     only a composer — it holds no function data of its
                     own.
  --mode=names       one bare symbol name per line, in source order; the
                     `[codegen-helpers]` membership list the linker
                     `--export=` response file and `wasm_exports.txt`
                     consistency check consume.  `cel`-only — host/env are
                     imports, not exports of cel_runtime.wasm.

Usage:
  gen_runtime_catalogue.py --mode textproto --base <host_env.textproto> \
      --out <path> <runtime source .h/.c files...>
  gen_runtime_catalogue.py --mode names --out <path> <sources...>
"""

import argparse
import re
import sys

_MARKER = "cel:codegen-export"

# A `void`/`uint32_t` C function declaration or definition.  The param
# capture `[^;{)]*` stops at the first `)` — these are flat
# `(uint32_t, ...)` lists with no nested parens.
_DECL_RE = re.compile(
    r"\b(void|uint32_t)\s+([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*[;{]", re.DOTALL
)


def parse_marked_helpers(source_paths):
  """Returns ordered [(name, num_args, returns_i32)] for marked helpers.

  Scans each source line-by-line for the `// cel:codegen-export` marker,
  then parses the declaration that begins on the next line (joining
  continuation lines until the closing `)` and terminator).  Source order
  is preserved across files (caller passes them in a stable order) for
  stable diffs.  Raises on a marker with no following decl, a non-uint32_t
  parameter (the runtime-helper ABI is i32-only), or a duplicate name.
  """
  helpers = []
  seen = set()
  for path in source_paths:
    lines = open(path, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
      if "//" not in lines[i] or _MARKER not in lines[i]:
        i += 1
        continue
      # Join from the next line until the declaration terminates.
      j = i + 1
      buf = []
      while j < len(lines):
        buf.append(lines[j])
        if re.search(r"\)\s*[;{]", lines[j]):
          break
        j += 1
      decl = re.sub(r"\s+", " ", " ".join(buf)).strip()
      match = _DECL_RE.search(decl)
      if not match:
        raise SystemExit(
            f"gen_runtime_catalogue: {path}: `{_MARKER}` marker not "
            f"followed by a void/uint32_t declaration (got: {decl!r})"
        )
      ret, name, args = match.group(1), match.group(2), match.group(3).strip()
      if name in seen:
        raise SystemExit(
            f"gen_runtime_catalogue: duplicate marked helper `{name}` "
            f"(second site in {path})"
        )
      seen.add(name)
      params = (
          [p for p in args.split(",") if p.strip()] if args != "void" else []
      )
      for p in params:
        if "uint32_t" not in p:
          raise SystemExit(
              f"gen_runtime_catalogue: helper `{name}` has non-uint32_t "
              f"parameter `{p.strip()}`; the runtime-helper ABI is i32-only."
          )
      helpers.append((name, len(params), ret == "uint32_t"))
      i = j + 1
  return helpers


def emit_textproto(helpers, base_text):
  """Builds the composed CelRuntimeCatalogue textproto string.

  The catalogue is the single source of truth for every built-in but
  `cel_fn`, composed here from two sources: `base_text` — the verbatim
  committed `cel_host` / `cel_env` rows (abi/runtime_host_env.textproto),
  the source of truth for the host/env imports — followed by the `cel`
  rows DERIVED from the C markers.  This generator only composes; it is
  not itself a source of truth (hence no hand-coded function data here).
  """
  out = [
      "# Generated by //bazel:gen_runtime_catalogue — DO NOT edit by hand.",
      "# Composed from the committed cel_host/cel_env rows",
      "# (abi/runtime_host_env.textproto) followed by the `cel` rows",
      "# derived from the `// cel:codegen-export` markers in",
      "# runtime/cel_*.{h,c}.",
      "",
      base_text.rstrip("\n"),
      "",
  ]
  for name, num_args, returns_i32 in helpers:
    out += [
        "functions {",
        f'  name: "{name}"',
        "  module: CEL",
        f"  num_args: {num_args}",
        f"  returns_i32: {str(returns_i32).lower()}",
        "}",
    ]
  return "\n".join(out) + "\n"


def emit_names(helpers):
  """One bare symbol name per line, in source order."""
  return "".join(f"{name}\n" for name, _, _ in helpers)


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--mode", required=True, choices=["textproto", "names"])
  parser.add_argument("--out", required=True)
  parser.add_argument(
      "--base",
      help="committed cel_host/cel_env textproto to prepend (textproto mode)",
  )
  parser.add_argument("sources", nargs="+", help="runtime C .h/.c sources")
  args = parser.parse_args(argv)

  helpers = parse_marked_helpers(args.sources)
  if args.mode == "textproto":
    if not args.base:
      raise SystemExit("gen_runtime_catalogue: --mode=textproto requires --base")
    base_text = open(args.base, encoding="utf-8").read()
    text = emit_textproto(helpers, base_text)
  else:
    text = emit_names(helpers)
  with open(args.out, "w", encoding="utf-8") as f:
    f.write(text)
  return 0


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
