"""Floating-Pane documents the daemon emits validate against the embedded API schema.

Fixtures mirror real daemon output; the mux suite covers producing them.
"""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator
from referencing import Registry

ROOT = Path(__file__).resolve().parents[1]
SIGNALS = {
    "generation": 0,
    "bells": 0,
    "notifications": 0,
    "progress": None,
    "commands": 0,
    "command": None,
    "title_changes": 0,
    "cwd_changes": 0,
}
SESSION = {"id": "0:1", "name": "work", "revision": 4}
CENTERED = {"kind": "centered", "columns": 40, "rows": 12}


def listing(pane: str, **layer: Any) -> dict[str, Any]:
    return {
        "id": pane,
        "tab": "0:1",
        "tab_position": 1,
        "focused": layer.get("layer") == "float",
        "column": 21,
        "row": 7,
        "columns": 38,
        "rows": 10,
        **layer,
        "process": {"state": "running", "pid": 4242},
        "terminal_generation": 1,
        "observed_title": "",
        "signals": SIGNALS,
    }


def result(command: str, **fields: Any) -> dict[str, Any]:
    return {
        "schema": "lemma.command-result/v1",
        "command": command,
        "status": "applied",
        "session": SESSION,
        **fields,
    }


FLOAT = listing("1:1", layer="float", z=0, suspended=False, placement=CENTERED)
TILED = listing("0:1", layer="tiled")
TAB_STATE: dict[str, Any] = {
    "id": "0:1",
    "position": 1,
    "title": "shell",
    "active": True,
    "focused_pane": "1:1",
    "previous_pane": "0:1",
    "zoomed": False,
    "layout_suspended": False,
    "geometry": {"columns": 80, "rows": 23},
    "layout": {"pane": "0:1"},
    "floats": {"visible": True, "panes": ["1:1"]},
}
REQUEST: dict[str, Any] = {
    "schema": "lemma.proc/v1",
    "on_error": "continue",
    "commands": [
        {
            "id": "float",
            "command": "pane.float",
            "session": {"id": "0:1"},
            "tab": {"id": "0:1"},
            "placement": CENTERED,
            "argv": ["htop"],
            "focus": "preserve",
        },
        {
            "command": "pane.place",
            "pane": {"result": "float"},
            "placement": {
                "kind": "relative",
                "width_percent": 80,
                "height_percent": 60,
            },
        },
        {
            "command": "pane.place",
            "pane": {"result": "float"},
            "placement": {
                "kind": "absolute",
                "column": 2,
                "row": 1,
                "columns": 20,
                "rows": 6,
            },
        },
        {
            "command": "tab.floats",
            "session": {"id": "0:1"},
            "tab": {"position": 1},
            "visible": False,
        },
    ],
}
RESULT: dict[str, Any] = {
    "schema": "lemma.proc-result/v1",
    "ok": False,
    "results": [
        {
            "index": 0,
            "id": "float",
            "result": result(
                "pane.float", tab="0:1", pane="1:1", terminal_generation=1
            ),
        },
        {
            "index": 1,
            "result": {
                **result("pane.place", tab="0:1", pane="1:1", terminal_generation=1),
                "status": "unavailable",
                "error": {"reason": "float_suspended", "retryable": False},
            },
        },
        {"index": 2, "result": result("pane.list", panes=[TILED, FLOAT])},
        {"index": 3, "result": result("tab.inspect", tab="0:1", tab_state=TAB_STATE)},
    ],
}


class FloatingPaneSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        schema = json.loads(
            (ROOT / "schema/lemma-api-v1.schema.json").read_text(encoding="utf-8")
        )
        Draft202012Validator.check_schema(schema)
        cls.validator = Draft202012Validator(schema, registry=Registry())

    def assert_valid(self, document: dict[str, Any]) -> None:
        errors = [error.message for error in self.validator.iter_errors(document)]
        self.assertEqual(errors, [])

    def assert_invalid(self, document: dict[str, Any]) -> None:
        self.assertFalse(self.validator.is_valid(document))

    def test_float_requests_and_results_validate(self) -> None:
        self.assert_valid(REQUEST)
        self.assert_valid(RESULT)

    def test_float_requests_are_closed(self) -> None:
        for index, field, value in (
            (0, "title", "no"),
            (0, "placement", {"kind": "centered", "columns": 2, "rows": 5}),
            (
                0,
                "placement",
                {"kind": "centered", "columns": 10, "rows": 5, "column": 0},
            ),
            (
                1,
                "placement",
                {"kind": "relative", "width_percent": 0, "height_percent": 60},
            ),
            (
                2,
                "placement",
                {"kind": "float", "column": 0, "row": 0, "columns": 5, "rows": 5},
            ),
            (3, "visible", "no"),
        ):
            with self.subTest(index=index, field=field):
                request = copy.deepcopy(REQUEST)
                request["commands"][index][field] = value
                self.assert_invalid(request)
        missing = copy.deepcopy(REQUEST)
        del missing["commands"][3]["visible"]
        self.assert_invalid(missing)

    def test_pane_records_state_their_layer(self) -> None:
        for mutate in (
            lambda panes: panes[1].pop("placement"),
            lambda panes: panes[1].pop("layer"),
            lambda panes: panes[0].update(z=0),
            lambda panes: panes[1].update(z=8),
            lambda panes: panes[1].update(layer="floating"),
        ):
            document = copy.deepcopy(RESULT)
            mutate(document["results"][2]["result"]["panes"])
            self.assert_invalid(document)

    def test_tab_state_reports_its_float_layer(self) -> None:
        for mutate in (
            lambda floats: floats.pop("visible"),
            lambda floats: floats.update(panes=["1:1", "1:1"]),
            lambda floats: floats.update(order="back-to-front"),
        ):
            document = copy.deepcopy(RESULT)
            mutate(document["results"][3]["result"]["tab_state"]["floats"])
            self.assert_invalid(document)


if __name__ == "__main__":
    unittest.main()
