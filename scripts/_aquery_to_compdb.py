#!/usr/bin/env python3
# _aquery_to_compdb.py — convert `bazel aquery --output=jsonproto` output
# into a clang-compatible compile_commands.json.
#
# Driven by scripts/refresh_compile_db.sh; not meant to be called by
# users directly.  Does the minimum needed for clang-tidy: per-.cc
# command line with -I, -D, -std flags; cwd set to the bazel exec root.
#
# We emit one entry per CppCompile action the aquery pulls up.  Source
# paths are prefixed with exec_root so clang-tidy's -p lookup finds the
# file regardless of where the user invokes it from.
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path


def exec_root() -> str:
  return subprocess.check_output(
      ["bazel", "info", "execution_root"], text=True
  ).strip()


def find_source(args: list[str]) -> str | None:
  """Heuristic: the first arg ending in .cc/.cpp/.cxx/.c after '-c'."""
  for i, a in enumerate(args):
    if a == "-c" and i + 1 < len(args):
      return args[i + 1]
  for a in args:
    if a.endswith((".cc", ".cpp", ".cxx", ".c")):
      return a
  return None


def sdk_path() -> str | None:
  """macOS SDK path — needed so clang-tidy finds <cstddef>, <string>, etc.

  Bazel's cc_wrapper.sh adds these implicitly at build time; clang-tidy
  runs the compile command directly, so we have to patch the sysroot
  in.  Returns None on non-darwin or if xcrun isn't available.
  """
  try:
    return subprocess.check_output(
        ["xcrun", "--show-sdk-path"], text=True, stderr=subprocess.DEVNULL
    ).strip()
  except (subprocess.CalledProcessError, FileNotFoundError):
    return None


def clang_path() -> str:
  """Prefer brew llvm's clang++; fall back to system clang++."""
  brew = "/opt/homebrew/opt/llvm/bin/clang++"
  return brew if os.path.exists(brew) else "clang++"


def build_entry(action: dict, root: str, sdk: str | None,
                clang: str) -> dict | None:
  args = action.get("arguments", [])
  if not args:
    return None
  src = find_source(args)
  if src is None:
    return None
  # Replace bazel's cc_wrapper.sh driver with a real clang++. The
  # wrapper's only job at analysis time is env-var plumbing we don't
  # need.
  if args[0].endswith("cc_wrapper.sh"):
    args = [clang] + args[1:]
  # Inject -isysroot if missing so libc++ headers resolve.
  if sdk is not None and not any(
      a == "-isysroot" or a.startswith("-isysroot=") for a in args
  ):
    args = args[:1] + ["-isysroot", sdk] + args[1:]
  return {
      "directory": root,
      "arguments": args,
      "file": src,
  }


def main() -> int:
  if len(sys.argv) != 2:
    print("usage: _aquery_to_compdb.py <aquery.json>", file=sys.stderr)
    return 2
  aquery_path = Path(sys.argv[1])
  data = json.loads(aquery_path.read_text())
  root = exec_root()
  sdk = sdk_path()
  clang = clang_path()
  entries = []
  for action in data.get("actions", []):
    if action.get("mnemonic") != "CppCompile":
      continue
    entry = build_entry(action, root, sdk, clang)
    if entry is not None:
      entries.append(entry)
  json.dump(entries, sys.stdout, indent=2)
  sys.stdout.write("\n")
  return 0


if __name__ == "__main__":
  sys.exit(main())
