"""Regression tests for the cheap documentation gate; no daemon or shell snippets run here."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.check_docs import MARKDOWN, check, heading_anchors


class DocumentationCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.put("README.md", "# Project\n\n[Guide](docs/guide.md#guide)\n")
        self.put("AGENTS.md", "# Project\n\n[Guide](docs/guide.md)\n")
        self.put(".github/pull_request_template.md", "[Guide](../docs/guide.md)\n")
        self.put(
            "schema/frame-api-v1.schema.json",
            json.dumps(
                {
                    "type": "object",
                    "properties": {"schema": {"const": "example/v1"}},
                    "required": ["schema"],
                    "additionalProperties": False,
                }
            ),
        )
        self.put("examples/sample.json", '{"schema":"example/v1"}\n')
        self.put(
            "src/config/config.cpp",
            'constexpr std::array command_names{\n    "detach",\n    "split",\n};\n',
        )
        self.guide = (
            "# Guide\n\n"
            "```text catalog=keymap-commands\ndetach split\n```\n\n"
            "```json example=../examples/sample.json\n"
            '{"schema":"example/v1"}\n```\n'
        )
        self.put("docs/guide.md", self.guide)

    def put(self, name: str, content: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def assert_error(self, text: str) -> None:
        errors = check(self.root)
        self.assertTrue(any(text in error for error in errors), errors)

    def test_valid_documentation(self) -> None:
        self.assertEqual(check(self.root), [])

    def test_reference_links_and_images_are_checked_but_code_and_comments_are_not(
        self,
    ) -> None:
        self.put("docs/image.png", "image fixture")
        self.put(
            "README.md",
            "[Guide][guide]\n\n[guide]: docs/guide.md#guide\n\n"
            "![image](docs/image.png)\n\n"
            "`[not a link](missing)`\n\n<!-- [not a link](missing) -->\n\n"
            "```sh\nrm -rf NEVER_EXECUTE\n[not a link](missing)\n```\n",
        )
        self.assertEqual(check(self.root), [])
        (self.root / "docs/image.png").unlink()
        self.assert_error("missing link target: docs/image.png")

    def test_source_deletion_breaks_links(self) -> None:
        self.put("docs/guide.md", self.guide + "\n[Source](../src/config/config.cpp)\n")
        (self.root / "src/config/config.cpp").unlink()
        self.assert_error("missing link target: ../src/config/config.cpp")

    def test_missing_heading_and_percent_encoded_paths(self) -> None:
        self.put("docs/with space.md", "# More details\n")
        self.put(
            "README.md",
            "[Guide](docs/guide.md)\n[More](docs/with%20space.md#more-details)\n",
        )
        self.assertEqual(check(self.root), [])
        self.put("README.md", "[Guide](docs/guide.md#old-heading)\n")
        self.assert_error("missing heading: docs/guide.md#old-heading")

    def test_heading_ids_follow_inline_text_and_handle_collisions(self) -> None:
        tokens = MARKDOWN.parse(
            "# One\n# One-1\n# One\n## `Code` and [*links*](https://example.com)\n"
            "Heading with é\n---\n"
        )
        self.assertEqual(
            heading_anchors(tokens),
            {"one", "one-1", "one-2", "code-and-links", "heading-with-é"},
        )

    def test_unlinked_docs_are_rejected(self) -> None:
        self.put("docs/orphan.md", "# Orphan\n")
        self.assert_error("docs/orphan.md: not reachable")

    def test_links_cannot_escape_the_repository(self) -> None:
        self.put("docs/guide.md", self.guide + "\n[Outside](../../outside)\n")
        self.assert_error("link leaves the repository")

    def test_external_links_are_not_fetched(self) -> None:
        self.put(
            "docs/guide.md", self.guide + "\n[External](https://example.invalid)\n"
        )
        with patch(
            "urllib.request.urlopen", side_effect=AssertionError("network access")
        ):
            self.assertEqual(check(self.root), [])

    def test_snippet_drift_is_rejected(self) -> None:
        self.put("docs/guide.md", self.guide.replace("example/v1", "old/v1"))
        self.assert_error("snippet differs from ../examples/sample.json")

    def test_unreferenced_examples_and_removed_markers_are_rejected(self) -> None:
        self.put(
            "docs/guide.md", self.guide.replace(" example=../examples/sample.json", "")
        )
        self.assert_error("no marked documentation snippet")

    def test_marker_typos_and_language_mismatches_fail_closed(self) -> None:
        for guide, message in [
            (self.guide.replace("example=", "exmaple="), "unknown fence metadata"),
            (
                self.guide.replace("json example=", "lua example="),
                "language does not match",
            ),
            (self.guide.replace("sample.json", "missing.json"), "missing link target"),
        ]:
            with self.subTest(message=message):
                self.put("docs/guide.md", guide)
                self.assert_error(message)

    def test_lua_examples_are_compared_but_not_executed(self) -> None:
        self.put("examples/config.lua", 'error("only native tests may execute me")\n')
        self.put(
            "docs/guide.md",
            self.guide
            + '\n```lua example=../examples/config.lua\nerror("only native tests may execute me")\n```\n',
        )
        self.assertEqual(check(self.root), [])

    def test_schema_mismatch_is_rejected_even_when_the_snippet_matches(self) -> None:
        invalid = '{"schema":"example/v1","unknown":true}\n'
        self.put("examples/sample.json", invalid)
        self.put(
            "docs/guide.md", self.guide.replace('{"schema":"example/v1"}\n', invalid)
        )
        self.assert_error("Additional properties are not allowed")

    def test_duplicate_json_keys_and_nonfinite_numbers_are_rejected(self) -> None:
        for invalid, message in [
            ('{"schema":"example/v1","schema":"example/v1"}\n', "duplicate JSON key"),
            ('{"schema":"example/v1","value":NaN}\n', "non-JSON number"),
        ]:
            with self.subTest(message=message):
                self.put("examples/sample.json", invalid)
                self.assert_error(message)

    def test_schema_remote_references_fail_without_network(self) -> None:
        self.put(
            "schema/frame-api-v1.schema.json",
            '{"$ref":"https://example.invalid/schema"}',
        )
        with patch(
            "urllib.request.urlopen", side_effect=AssertionError("network access")
        ):
            self.assert_error("Unresolvable")

    def test_catalog_drift_duplicates_and_removal_are_rejected(self) -> None:
        for guide, message in [
            (
                self.guide.replace("detach split", "detach unknown"),
                "keymap catalog drift",
            ),
            (
                self.guide.replace("detach split", "detach split split"),
                "names must appear once",
            ),
            (
                self.guide.replace(" catalog=keymap-commands", ""),
                "document the native keymap catalog once",
            ),
        ]:
            with self.subTest(message=message):
                self.put("docs/guide.md", guide)
                self.assert_error(message)

    def test_changed_native_catalog_format_fails_closed(self) -> None:
        self.put(
            "src/config/config.cpp", "constexpr auto command_names = make_commands();\n"
        )
        self.assert_error("cannot read native command_names")


if __name__ == "__main__":
    unittest.main()
