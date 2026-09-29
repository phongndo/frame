from __future__ import annotations

import runpy
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]


def load_entrypoint() -> dict[str, Any]:
    return runpy.run_path(str(ROOT / "test"), run_name="frame_test_entrypoint")


class TestEntrypointContractTest(unittest.TestCase):
    def test_reconfigures_a_cache_that_was_built_without_tests(self) -> None:
        entrypoint = load_entrypoint()
        configure = entrypoint["configure_if_needed"]
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "build.ninja").touch()
            (build / "CMakeCache.txt").write_text(
                "FRAME_BUILD_TESTS:BOOL=OFF\n", encoding="utf-8"
            )
            calls: list[list[str]] = []
            configure.__globals__["BUILD"] = build
            configure.__globals__["run"] = lambda arguments: calls.append(arguments)

            configure()

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], str(ROOT / "scripts" / "ci" / "configure"))
        self.assertIn("-DFRAME_BUILD_TESTS=ON", calls[0])

    def test_reuses_a_test_enabled_ninja_cache(self) -> None:
        entrypoint = load_entrypoint()
        configure = entrypoint["configure_if_needed"]
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "build.ninja").touch()
            (build / "CMakeCache.txt").write_text(
                "FRAME_BUILD_TESTS:BOOL=ON\n", encoding="utf-8"
            )
            calls: list[list[str]] = []
            configure.__globals__["BUILD"] = build
            configure.__globals__["run"] = lambda arguments: calls.append(arguments)

            configure()

        self.assertEqual(calls, [])

    def test_mux_registry_owns_precise_public_domains_and_help(self) -> None:
        entrypoint = load_entrypoint()
        domains = entrypoint["MUX_DOMAINS"]
        self.assertNotIn("input", domains)
        self.assertNotIn("output", domains)
        self.assertTrue(
            domains["detach-reattach"][0].endswith(
                "test_output_while_detached_is_current_on_reattach"
            )
        )
        default_selectors = [
            selector for selector, default in domains.values() if default
        ]
        self.assertEqual(len(default_selectors), len(set(default_selectors)))
        rendered_usage = entrypoint["usage"]()
        self.assertIn("|".join(domains), rendered_usage)


if __name__ == "__main__":
    unittest.main()
