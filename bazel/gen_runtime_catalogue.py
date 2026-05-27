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

Two outputs, selected by mode, both derived from the same marked set:

  --mode=textproto   a `CelRuntimeCatalogue` textproto (see
                     `abi/runtime_catalogue.proto`); embedded as a build
                     resource and parsed by `abi/runtime_catalogue.cc`.
  --mode=names       one bare symbol name per line, in source order; the
                     `[codegen-helpers]` membership list the linker
                     `--export=` response file and `wasm_exports.txt`
                     consistency check consume.

Usage:
  gen_runtime_catalogue.py --mode {textproto|names} --out <path> \
      <runtime source .h/.c files...>
"""

import argparse
import re
import sys

_MARKER = "cel:codegen-export"

# Hard-coded `cel_host` / `cel_env` import sets.  Unlike the `cel` set,
# these are NOT exports of cel_runtime.wasm and have no C signature to
# derive from — they are wasmtime host trampolines (`cel_host`) and host
# environment helpers (`cel_env`) registered on the C++ side.  They are
# composed into the SAME generated catalogue so it is the single source
# of truth for every built-in but `cel_fn` (user customs, registered at
# runtime, never catalogued).  All take i32 args and return void (they
# write results through an out-slot), so returns_i32 is false throughout.
#
# Names + arities MUST match the trampolines registered in
# `eval/internal/cel_host_wasmtime.cc`; the cross-check loop there
# CHECK-fails at startup if they drift (that is what guards this
# hard-coded list against reality).
_HOST_FUNCTIONS = [
    ("cel_get_field", 4),
    ("cel_has_field", 4),
    ("cel_map_lookup", 3),
    ("cel_map_iter_open", 2),
    ("cel_list_iter_open", 2),
    ("cel_list_at", 3),
    ("cel_list_size", 2),
    ("cel_list_in", 3),
    ("cel_list_eq", 3),
    ("cel_list_concat", 3),
    ("cel_map_size", 2),
    ("cel_map_in", 3),
    ("cel_map_eq", 3),
    ("cel_message_eq", 3),
    ("cel_make_message", 2),
    ("cel_set_field", 3),
    ("resolve_message_type_name", 2),
    ("cel_timestamp_tz_accessor", 4),
    ("cel_wkt_unwrap_time", 2),
    ("cel_wkt_unwrap_wrapper", 3),
]
_ENV_FUNCTIONS = [
    ("cel_log", 4),
]

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


def _emit_fn(name, module, num_args, returns_i32):
  return [
      "functions {",
      f'  name: "{name}"',
      f"  module: {module}",
      f"  num_args: {num_args}",
      f"  returns_i32: {str(returns_i32).lower()}",
      "}",
  ]


def emit_textproto(helpers):
  """Builds the CelRuntimeCatalogue textproto string.

  Composes the catalogue from the `cel`-module helpers derived from the
  C markers and the hard-coded `cel_host` / `cel_env` import sets, so the
  one textproto is the single source of truth for every built-in but
  `cel_fn`.
  """
  out = [
      "# Generated by //bazel:gen_runtime_catalogue.  The `cel` rows are",
      "# derived from the `// cel:codegen-export` markers in",
      "# runtime/cel_*.{h,c}; the `cel_host` / `cel_env` rows are the",
      "# hard-coded import sets in the generator.  Do NOT edit by hand —",
      "# re-run the genrule.",
      "",
  ]
  for name, num_args, returns_i32 in helpers:
    out += _emit_fn(name, "CEL", num_args, returns_i32)
  for name, num_args in _HOST_FUNCTIONS:
    out += _emit_fn(name, "CEL_HOST", num_args, False)
  for name, num_args in _ENV_FUNCTIONS:
    out += _emit_fn(name, "CEL_ENV", num_args, False)
  return "\n".join(out) + "\n"


def emit_names(helpers):
  """One bare symbol name per line, in source order."""
  return "".join(f"{name}\n" for name, _, _ in helpers)


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--mode", required=True, choices=["textproto", "names"])
  parser.add_argument("--out", required=True)
  parser.add_argument("sources", nargs="+", help="runtime C .h/.c sources")
  args = parser.parse_args(argv)

  helpers = parse_marked_helpers(args.sources)
  if args.mode == "textproto":
    text = emit_textproto(helpers)
  else:
    text = emit_names(helpers)
  with open(args.out, "w", encoding="utf-8") as f:
    f.write(text)
  return 0


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
