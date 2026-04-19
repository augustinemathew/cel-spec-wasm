# Vendored third-party sources

`cel-cpp/` is not checked in.  To bootstrap it at the pinned revision run
`third_party/fetch_cel_cpp.sh` from the repo root.  The `MODULE.bazel` at the
root points at `third_party/cel-cpp` via `local_path_override`, so the build
will not work until the script has been run (or the directory has been
populated another way, e.g. by a git submodule).

The pinned revision is recorded in `third_party/cel-cpp.sha`.  Bumping it is
a deliberate action: update the SHA file in the same commit as any code
changes that depend on the new upstream state, and re-run the smoke tests.
