import importlib.util
import re
import unittest
from collections.abc import Iterable
from pathlib import Path
from types import ModuleType
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, relative_path: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, ROOT / relative_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {relative_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CiChangesModule(Protocol):
    @staticmethod
    def classify_paths(paths: Iterable[str]) -> dict[str, bool]: ...

    @staticmethod
    def diff_revisions(base: str, head: str, event: str | None) -> list[str]: ...


changes = cast(CiChangesModule, load_module("ci_changes", "scripts/ci/changes.py"))


class CiChangesTests(unittest.TestCase):
    def selected(self, *paths: str) -> set[str]:
        result = changes.classify_paths(paths)
        return {lane for lane, selected in result.items() if selected}

    def test_pushes_compare_ref_endpoints(self):
        self.assertEqual(
            changes.diff_revisions("old", "new", "push"),
            ["old", "new"],
        )

    def test_pull_requests_compare_from_the_merge_base(self):
        self.assertEqual(
            changes.diff_revisions("base", "head", "pull_request"),
            ["base...head"],
        )

    def test_unrelated_documentation_selects_no_expensive_lane(self):
        self.assertEqual(
            self.selected(
                "README.md", "AGENTS.md", "docs/development.md", "docs/configuration.md"
            ),
            set(),
        )

    def test_schema_only_change_selects_native_contract_tests(self):
        self.assertEqual(self.selected("schema/frame-api-v1.schema.json"), {"cpp"})

    def test_canonical_example_changes_select_native_contract_tests(self):
        for path in ("examples/job.json", "examples/configuration.lua"):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), {"cpp"})

    def test_documentation_gate_is_unconditional_and_merge_blocking(self):
        workflow = (ROOT / ".github/workflows/quality.yml").read_text()
        self.assertIn("\n  docs:\n", workflow)
        job = re.split(r"\n  [a-z-]+:\n", workflow.split("\n  docs:\n", 1)[1])[0]
        self.assertIsNone(re.search(r"(?m)^    (?:if|needs):", job))
        self.assertIn("just docs-check", job)
        gate = workflow.split("\n  gate:\n", 1)[1]
        self.assertIn("      - docs\n", gate)
        self.assertIn("DOCS_RESULT: ${{ needs.docs.result }}", gate)
        self.assertIn('"docs:true:$DOCS_RESULT"', gate)

    def test_test_change_selects_only_cpp_correctness(self):
        self.assertEqual(self.selected("tests/core_test.cpp"), {"cpp"})

    def test_python_mux_change_selects_cpp_build_and_python_quality(self):
        self.assertEqual(
            self.selected("tests/mux/test_resize.py"),
            {"cpp", "python"},
        )

    def test_library_change_selects_cpp_correctness(self):
        self.assertEqual(
            self.selected("include/frame/id.hpp", "src/core/frame.cpp"),
            {"cpp"},
        )

    def test_application_change_selects_cpp_correctness(self):
        self.assertEqual(self.selected("apps/frame/main.cpp"), {"cpp"})

    def test_benchmark_change_selects_cpp_jobs(self):
        self.assertEqual(self.selected("benchmarks/frame_benchmark.cpp"), {"cpp"})

    def test_python_benchmark_change_selects_cpp_and_python_jobs(self):
        self.assertEqual(
            self.selected("benchmarks/compare_mux.py"),
            {"cpp", "python"},
        )

    def test_dependency_change_selects_cpp_correctness(self):
        self.assertEqual(
            self.selected("conan.lock", "third_party/ghostty"),
            {"cpp"},
        )

    def test_tidy_configuration_selects_cpp_correctness(self):
        self.assertEqual(self.selected(".clang-tidy"), {"cpp"})

    def test_ci_test_change_selects_python_and_automation_lanes(self):
        self.assertEqual(
            self.selected("tools/test_ci_changes.py"),
            {"python", "automation"},
        )

    def test_python_project_change_selects_python_lane(self):
        self.assertEqual(self.selected("pyproject.toml", "uv.lock"), {"python"})

    def test_conan_recipe_selects_cpp_and_python_lanes(self):
        self.assertEqual(self.selected("conanfile.py"), {"cpp", "python"})

    def test_workflow_change_runs_every_lane(self):
        result = changes.classify_paths([".github/workflows/quality.yml"])
        self.assertTrue(all(result.values()))

    def test_ci_script_change_runs_every_lane(self):
        result = changes.classify_paths(["scripts/ci/configure"])
        self.assertTrue(all(result.values()))

    def test_toolchain_change_validates_full_local_contract(self):
        result = changes.classify_paths(["flake.nix"])
        self.assertTrue(all(result.values()))


if __name__ == "__main__":
    unittest.main()
