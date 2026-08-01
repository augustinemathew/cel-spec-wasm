"""Drift guard for e2e/test_taxonomy.json.

Fails when the taxonomy manifest and e2e/BUILD.bazel disagree: a suite
added/renamed in the BUILD without a manifest entry (or vice versa), a
suite classified against an undeclared surface, or a manifest source
file that does not exist.  Keeps the machine-readable classification
(consumed by scripts/coverage/) honest.
"""

import json
import os
import re
import unittest

_E2E_DIR = os.path.dirname(os.path.abspath(__file__))


def _load_manifest() -> dict:
    with open(os.path.join(_E2E_DIR, "test_taxonomy.json"), encoding="utf-8") as f:
        return json.load(f)


def _build_suite_names() -> set[str]:
    """Test-suite target names declared in e2e/BUILD.bazel.

    Matches both `link_mode_e2e_cc_test` and plain `cc_test` blocks;
    non-test targets (cc_library / cc_binary / py_test) are excluded.
    """
    with open(os.path.join(_E2E_DIR, "BUILD.bazel"), encoding="utf-8") as f:
        text = f.read()
    names = re.findall(
        r'(?:link_mode_e2e_cc_test|cc_test)\(\s*name = "([^"]+)"', text
    )
    return set(names)


class TaxonomyValidateTest(unittest.TestCase):

    def setUp(self):
        self.manifest = _load_manifest()
        self.surfaces = set(self.manifest["surfaces"])
        self.suites = self.manifest["suites"]

    def test_every_build_suite_is_classified(self):
        missing = _build_suite_names() - set(self.suites)
        self.assertFalse(
            missing,
            f"suites in e2e/BUILD.bazel missing from test_taxonomy.json: "
            f"{sorted(missing)}",
        )

    def test_every_classified_suite_exists_in_build(self):
        stale = set(self.suites) - _build_suite_names()
        self.assertFalse(
            stale,
            f"suites in test_taxonomy.json absent from e2e/BUILD.bazel "
            f"(renamed or deleted?): {sorted(stale)}",
        )

    def _external_e2e_workloads(self) -> dict:
        return {
            name: entry
            for name, entry in self.manifest.get(
                "external_e2e_workloads", {}
            ).items()
            if name != "_comment"
        }

    def test_surfaces_are_declared_and_nonempty(self):
        entries = (
            list(self.suites.items())
            + list(self.manifest.get("external_workloads", {}).items())
            + list(self._external_e2e_workloads().items())
        )
        for name, entry in entries:
            with self.subTest(suite=name):
                self.assertTrue(entry["surfaces"], f"{name} has no surfaces")
                undeclared = set(entry["surfaces"]) - self.surfaces
                self.assertFalse(
                    undeclared,
                    f"{name} references undeclared surfaces: "
                    f"{sorted(undeclared)}",
                )

    def test_suite_sources_exist(self):
        for name, entry in self.suites.items():
            with self.subTest(suite=name):
                self.assertTrue(
                    os.path.exists(os.path.join(_E2E_DIR, entry["source"])),
                    f"{name}: source {entry['source']} not found in e2e/",
                )

    def test_external_e2e_workload_targets_exist(self):
        # These live outside e2e/BUILD.bazel; each names its
        # BUILD file, and that file must declare a target of the same
        # name — the drift guard mirroring the suite<->BUILD checks.
        repo_root = os.path.dirname(_E2E_DIR)
        for name, entry in self._external_e2e_workloads().items():
            with self.subTest(workload=name):
                build_path = os.path.join(repo_root, entry["build_file"])
                self.assertTrue(
                    os.path.exists(build_path),
                    f"{name}: build_file {entry['build_file']} not found",
                )
                with open(build_path, encoding="utf-8") as f:
                    self.assertIn(
                        f'name = "{name}"',
                        f.read(),
                        f"{name} not declared in {entry['build_file']}",
                    )

    def test_every_surface_is_used(self):
        used: set[str] = set()
        for entry in self.suites.values():
            used.update(entry["surfaces"])
        for entry in self.manifest.get("external_workloads", {}).values():
            used.update(entry["surfaces"])
        # `lang/enums` is knowingly e2e-uncovered today (oracle +
        # conformance skips only) — the gap the coverage report exists
        # to surface; every other declared surface must be claimed.
        unused = self.surfaces - used - {"lang/enums"}
        self.assertFalse(
            unused, f"declared surfaces no suite claims: {sorted(unused)}"
        )


if __name__ == "__main__":
    unittest.main()
