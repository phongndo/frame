#!/usr/bin/env python3
"""A one-invocation Session/Tab/Pane picker using only Proc and Surface records.

Register this program with frame.command.register(..., {argv={...}}). It deliberately
has no screen subscription, timer while idle, terminal emulator, or daemon-side UI logic.
"""

from __future__ import annotations

import sys
import time
from collections import deque
from typing import Any

from frame_client import UPDATE, Client, command_context


def row_text(text: str, columns: int) -> str:
    # This dependency-free example uses one-cell ASCII fallbacks for arbitrary titles. Do not
    # confuse Python string length with terminal grapheme width or emit application controls.
    return "".join(c if " " <= c <= "~" else "?" for c in text)[:columns].ljust(columns)


class Picker:
    def __init__(self, peer: Client, context: dict[str, Any]) -> None:
        self.peer = peer
        self.context = context
        self.session = context["session"]
        self.tab = context["tab"]
        self.level = 0
        self.selected = 0
        self.entries: list[dict[str, Any]] = []
        self.message = "j/k move | l/h level | Enter go | q/Esc close | r refresh"
        geometry = peer.command("session.inspect", session={"id": self.session})[
            "session_state"
        ]["geometry"]
        self.columns = min(76, geometry["columns"])
        self.rows = min(14, max(1, geometry["rows"] - 1))
        self.surface = peer.command(
            "surface.create",
            placement={
                "kind": "float",
                "column": 0,
                "row": 0,
                "columns": self.columns,
                "rows": self.rows,
            },
        )["surface"]
        self.refresh()
        self.paint()
        peer.command("surface.focus", surface={"id": self.surface})

    def refresh(self) -> None:
        previous = self.entries[self.selected]["id"] if self.entries else None
        if self.level == 0:
            self.entries = self.peer.command("session.list")["sessions"]
        elif self.level == 1:
            self.entries = self.peer.command("tab.list", session={"id": self.session})[
                "tabs"
            ]
        else:
            self.entries = [
                pane
                for pane in self.peer.command(
                    "pane.list", session={"id": self.session}
                )["panes"]
                if pane["tab"] == self.tab
            ]
        self.selected = next(
            (i for i, item in enumerate(self.entries) if item["id"] == previous), 0
        )

    def paint(self) -> None:
        rows = [("Sessions", "Tabs", "Panes")[self.level] + " - Frame picker"]
        visible = max(0, self.rows - 2)
        start = max(0, self.selected - visible + 1)
        for index, item in enumerate(self.entries[start : start + visible], start):
            label = item.get("name", item.get("title", item["id"]))
            rows.append(("> " if index == self.selected else "  ") + str(label))
        rows += [""] * max(0, self.rows - len(rows) - 1)
        if self.rows > 1:
            rows.append(self.message)
        self.peer.send(
            UPDATE,
            {
                "schema": "frame.surface-update/v1",
                "surface": self.surface,
                "styles": [{}, {"inverse": True}],
                "rows": [
                    {
                        "row": index,
                        "runs": [
                            {
                                "column": 0,
                                "text": row_text(text, self.columns),
                                "style": 1 if text.startswith("> ") else 0,
                            }
                        ],
                    }
                    for index, text in enumerate(rows)
                ],
            },
        )

    def select(self) -> bool:
        if not self.entries:
            return False
        item = self.entries[self.selected]
        session = item["id"] if self.level == 0 else self.session
        commands: list[dict[str, Any]] = []
        if self.level > 0:
            commands.append(
                {
                    "command": "tab.select",
                    "session": {"id": session},
                    "tab": {"id": item["id"] if self.level == 1 else self.tab},
                }
            )
        if self.level == 2:
            commands.append(
                {
                    "command": "pane.focus",
                    "session": {"id": session},
                    "pane": {"id": item["id"]},
                }
            )
        switch = {
            "command": "attachment.switch",
            "connection": self.context["connection"],
            "session": {"id": session},
        }
        commands.append(switch)
        deadline = time.monotonic() + 0.5
        while True:
            result = self.peer.proc(*commands)
            if result["ok"]:
                return True
            failure = result["results"][-1]["result"] if result["results"] else {}
            reason = failure.get("error", {}).get(
                "reason", failure.get("status", "failed")
            )
            if reason != "output_pending" or time.monotonic() >= deadline:
                break
            # Retry only the uncommitted transfer, not earlier ordered mutations.
            commands = [switch]
            time.sleep(0.01)
        self.message = f"Not selected: {reason}. r refresh | q close"
        self.peer.command("surface.focus", surface={"id": self.surface})
        # Earlier successful commands may have handed focus back to a native Pane. That old
        # blur predates the acknowledged refocus above; retain other queued input and events.
        self.peer.events = deque(
            event
            for event in self.peer.events
            if event.get("surface") != self.surface
            or event["event"] != "surface.blurred"
        )
        return False

    def key(self, key: str) -> bool:
        if key in ("q", "escape"):
            return False
        if key in ("down", "j") and self.entries:
            self.selected = (self.selected + 1) % len(self.entries)
        elif key in ("up", "k") and self.entries:
            self.selected = (self.selected - 1) % len(self.entries)
        elif key in ("right", "l", "tab") and self.level < 2 and self.entries:
            chosen = self.entries[self.selected]["id"]
            if self.level == 0:
                self.session = chosen
            else:
                self.tab = chosen
            self.level += 1
            self.entries = []
            self.refresh()
        elif key in ("left", "h") and self.level > 0:
            self.level -= 1
            self.entries = []
            self.refresh()
        elif key in ("enter", "\r", "\n"):
            if self.select():
                return False
        elif key == "r":
            self.refresh()
        self.paint()
        return True

    def run(self) -> None:
        pending = b""
        special = {
            27: "enter",
            28: "tab",
            30: "escape",
            32: "up",
            33: "down",
            34: "left",
            35: "right",
        }
        arrows = {
            b"\x1b[A": "up",
            b"\x1b[B": "down",
            b"\x1b[C": "right",
            b"\x1b[D": "left",
        }
        while True:
            try:
                event = self.peer.event(0.05 if pending else None)
            except TimeoutError:
                return  # A lone legacy Escape; partial records remain bounded in Client.input.
            if event.get("surface") != self.surface:
                continue
            if event["event"] in ("surface.closed", "surface.blurred"):
                return
            if event["event"] == "surface.resized":
                if event["columns"] == 0 or event["rows"] == 0:
                    return
                self.columns, self.rows = event["columns"], event["rows"]
                self.paint()
                continue
            if event["event"] != "surface.key" or event.get("action") == 0:
                continue
            logical = special.get(event["key"])
            if logical is not None:
                pending = b""
                if not self.key(logical):
                    return
                continue
            pending += (
                event.get("text", "").encode()
                if "text" in event
                else bytes.fromhex(event["bytes_hex"])
            )
            while pending:
                if pending.startswith(b"\x1b"):
                    if pending in (b"\x1b", b"\x1b["):
                        break
                    key = arrows.get(pending[:3], "escape")
                    pending = pending[3:]
                else:
                    key = chr(pending[0])
                    pending = pending[1:]
                if not self.key(key):
                    return


def main() -> int:
    peer: Client | None = None
    try:
        context = command_context()
        peer = Client(
            context["endpoint"],
            name="navigation-picker",
            session=context["session"],
            capabilities=("proc", "surface"),
        )
        Picker(peer, context).run()
        return 0
    except (EOFError, BrokenPipeError):
        return 0  # Detach, daemon shutdown, and native revocation end the invocation.
    except (KeyError, ValueError, OSError, RuntimeError) as error:
        print(str(error)[:180], file=sys.stderr)
        return 1
    finally:
        if peer is not None:
            peer.close()  # Disconnect revokes all Surfaces, including on stale-target failure.


if __name__ == "__main__":
    raise SystemExit(main())
