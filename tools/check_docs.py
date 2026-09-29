"""Check local documentation links, native catalogs, and canonical examples without executing them."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

from jsonschema import Draft202012Validator
from jsonschema.exceptions import SchemaError, ValidationError
from markdown_it import MarkdownIt
from markdown_it.token import Token
from referencing import Registry
from referencing.exceptions import Unresolvable

ROOT = Path(__file__).resolve().parents[1]
MARKDOWN = MarkdownIt("commonmark").enable("table")


def inline_text(tokens: list[Token]) -> str:
    parts: list[str] = []
    for token in tokens:
        if token.children is not None:
            parts.append(inline_text(token.children))
        elif token.type in {"text", "code_inline"}:
            parts.append(token.content)
        elif token.type in {"softbreak", "hardbreak"}:
            parts.append(" ")
    return "".join(parts)


def heading_anchors(tokens: list[Token]) -> set[str]:
    """GitHub-style heading IDs, including collisions with already suffixed headings."""
    anchors: set[str] = set()
    for index, token in enumerate(tokens):
        if token.type != "heading_open":
            continue
        text = inline_text(tokens[index + 1].children or [])
        base = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
        anchor = base
        suffix = 0
        while anchor in anchors:
            suffix += 1
            anchor = f"{base}-{suffix}"
        anchors.add(anchor)
    return anchors


def links(tokens: list[Token]) -> list[str]:
    result: list[str] = []
    for token in tokens:
        if token.type in {"link_open", "image"}:
            result.append(
                str(token.attrGet("href" if token.type == "link_open" else "src"))
            )
        if token.children:
            result.extend(links(token.children))
    return result


def local_target(root: Path, source: Path, destination: str) -> tuple[Path, str] | None:
    url = urlsplit(destination)
    if url.scheme or url.netloc:
        return None  # External links are deliberately not fetched in CI.
    path = unquote(url.path)
    if path.startswith("/"):
        raise ValueError(f"use a repository-relative link: {destination}")
    target = (source.parent / path).resolve() if path else source
    if not target.is_relative_to(root):
        raise ValueError(f"link leaves the repository: {destination}")
    if not target.exists():
        raise ValueError(f"missing link target: {destination}")
    return target, unquote(url.fragment)


def unique_fields(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> Any:
    raise ValueError(f"non-JSON number: {value}")


def read_json(path: Path) -> Any:
    return json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_fields,
        parse_constant=reject_constant,
    )


def keymap_commands(root: Path) -> list[str]:
    source = (root / "src/config/config.cpp").read_text(encoding="utf-8")
    arrays = re.findall(r"constexpr std::array command_names\{(.*?)\};", source, re.S)
    if len(arrays) != 1 or not re.fullmatch(r'(?:\s*"[a-z0-9_]+",)+\s*', arrays[0]):
        raise ValueError(
            "cannot read native command_names; review its format and the docs checker"
        )
    return re.findall(r'"([a-z0-9_]+)"', arrays[0])


def check(root: Path) -> list[str]:
    root = root.resolve()
    documents = [
        root / "README.md",
        root / "AGENTS.md",
        root / ".github/pull_request_template.md",
        *sorted((root / "docs").rglob("*.md")),
    ]
    errors: list[str] = []
    parsed: dict[Path, list[Token]] = {}
    edges: dict[Path, set[Path]] = {}
    referenced: set[Path] = set()
    catalog_count = 0

    def tokens(path: Path) -> list[Token]:
        if path not in parsed:
            parsed[path] = MARKDOWN.parse(path.read_text(encoding="utf-8"))
        return parsed[path]

    for document in documents:
        location = str(document.relative_to(root))
        try:
            content = tokens(document)
            edges[document] = set()
        except (OSError, UnicodeError) as error:
            errors.append(f"{location}: {error}")
            continue
        for destination in links(content):
            try:
                resolved = local_target(root, document, destination)
                if resolved is None:
                    continue
                target, fragment = resolved
                edges[document].add(target)
                if (
                    fragment
                    and target.suffix == ".md"
                    and fragment not in heading_anchors(tokens(target))
                ):
                    raise ValueError(f"missing heading: {destination}")
            except (OSError, ValueError) as error:
                errors.append(f"{location}: {error}")
        for token in content:
            if token.type != "fence":
                continue
            fields = token.info.split()
            if len(fields) <= 1:
                continue  # Unmarked fences are illustrative, never executed.
            line = token.map[0] + 1 if token.map else 1
            try:
                if len(fields) == 2 and fields[1].startswith("example="):
                    destination = fields[1].removeprefix("example=")
                    resolved = local_target(root, document, destination)
                    if resolved is None or resolved[1]:
                        raise ValueError(
                            "example must name a local file without a fragment"
                        )
                    example = resolved[0]
                    if example.parent != root / "examples" or example.suffix not in {
                        ".json",
                        ".lua",
                    }:
                        raise ValueError(
                            "canonical examples must be .json/.lua files directly in examples/"
                        )
                    if fields[0] != example.suffix[1:]:
                        raise ValueError(
                            "example language does not match its file extension"
                        )
                    referenced.add(example)
                    if token.content != example.read_text(encoding="utf-8"):
                        raise ValueError(
                            f"snippet differs from {destination}; synchronize from that file"
                        )
                elif fields == ["text", "catalog=keymap-commands"]:
                    catalog_count += 1
                    expected = keymap_commands(root)
                    actual = token.content.split()
                    if sorted(actual) != sorted(expected):
                        missing = sorted(set(expected) - set(actual))
                        extra = sorted(set(actual) - set(expected))
                        raise ValueError(
                            f"keymap catalog drift: missing={missing}, extra={extra}; names must appear once"
                        )
                else:
                    raise ValueError(f"unknown fence metadata: {token.info}")
            except (OSError, ValueError) as error:
                errors.append(f"{location}:{line}: {error}")

    if catalog_count != 1:
        errors.append(
            "document the native keymap catalog once with catalog=keymap-commands"
        )

    reachable: set[Path] = set()
    pending = [root / "README.md"]
    while pending:
        document = pending.pop()
        if document not in reachable:
            reachable.add(document)
            pending.extend(edges.get(document, set()))
    for document in documents[3:]:
        if document not in reachable:
            errors.append(
                f"{document.relative_to(root)}: not reachable through Markdown links from README.md"
            )

    try:
        schema = read_json(root / "schema/frame-api-v1.schema.json")
        Draft202012Validator.check_schema(schema)
        # An empty registry refuses remote references instead of making network requests.
        validator = Draft202012Validator(schema, registry=Registry())
        for example in sorted((root / "examples").iterdir()):
            location = str(example.relative_to(root))
            if example not in referenced:
                errors.append(
                    f"{location}: no marked documentation snippet references this example"
                )
            if example.suffix == ".json":
                try:
                    validator.validate(read_json(example))
                except (OSError, ValueError, ValidationError, Unresolvable) as error:
                    errors.append(f"{location}: {error}")
    except (OSError, ValueError, SchemaError) as error:
        errors.append(f"example schema validation: {error}")
    return errors


def main() -> int:
    errors = check(ROOT)
    if errors:
        print("\n".join(errors))
        return 1
    print("Documentation links, catalogs, schemas, and example snippets match.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
