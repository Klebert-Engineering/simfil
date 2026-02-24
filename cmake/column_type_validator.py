#!/usr/bin/env python3
"""Validate MODEL_COLUMN_TYPE structs for stable field types.

This validator is intentionally lightweight. It does *not* use a full C++
parser; instead it implements a constrained text pass that is robust enough
for our schema-like record declarations.

Scope of checks:
  - reject bare `int` / `long` (compiler-size dependent)
  - reject pointers and references in tagged structs
  - require top-level fields to be one of:
      * fixed-width scalar aliases
      * nested tagged structs/unions
      * simfil::ColumnTypeField<...>
      * fixed-width enums

Important non-goals:
  - complete C++ grammar support
  - semantic analysis of arbitrary templates/macros
  - validating non-tagged types

The script is meant to fail fast on obvious wire-layout risks while keeping
build integration simple and stable.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


# Marker that tells us a struct/class/union participates in model column IO.
TAG_MACRO = "MODEL_COLUMN_TYPE("

# Built-in type allowlist. These names are accepted as stable field types.
# We include both std:: and plain aliases to tolerate existing code style.
FIXED_WIDTH_TYPES = {
    "bool",
    "float",
    "double",
    "std::byte",
    "std::int8_t",
    "std::uint8_t",
    "std::int16_t",
    "std::uint16_t",
    "std::int32_t",
    "std::uint32_t",
    "std::int64_t",
    "std::uint64_t",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "simfil::StringId",
    "simfil::ArrayIndex",
    "StringId",
    "ArrayIndex",
}

# Declaration prefixes that should be ignored when scanning top-level members.
# These are not data fields and should not enter field-type validation.
DECL_SKIP_PREFIXES = (
    "MODEL_COLUMN_TYPE(",
    "using ",
    "typedef ",
    "friend ",
    "static ",
    "template ",
    "return ",
    "public:",
    "private:",
    "protected:",
)

# Declaration fragments that should be ignored entirely.
# These typically indicate operators or virtual declarations that are not
# persisted record fields.
DECL_SKIP_CONTAINS = (
    "operator ",
    " virtual ",
)


@dataclass
class TaggedStruct:
    """Container describing one tagged record declaration extracted from a file."""

    file: pathlib.Path
    name: str
    start_line: int
    body: str


def strip_comments(text: str) -> str:
    """Remove line/block comments before lightweight parsing.

    We strip comments first to avoid false matches when comment text contains
    braces, semicolons, or keywords that look like declarations.
    """

    text = re.sub(r"//.*?$", "", text, flags=re.MULTILINE)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return text


def find_matching_brace(text: str, open_idx: int) -> int:
    """Find matching `}` for a `{` at `open_idx`.

    Returns:
      - closing brace index if found
      - -1 if braces are unbalanced in the scanned range
    """

    # Simple depth counter is sufficient after comment stripping.
    depth = 0
    for i in range(open_idx, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def extract_tagged_structs(path: pathlib.Path) -> list[TaggedStruct]:
    """Extract all struct/class/union declarations that contain TAG_MACRO.

    The function scans declarations, then checks whether TAG_MACRO appears as
    a top-level declaration inside each body (not merely nested in a child).
    """

    raw = path.read_text(encoding="utf-8")
    text = strip_comments(raw)
    out: list[TaggedStruct] = []
    # Match declaration starts like: `struct Name {`, `class Name {`, ...
    for m in re.finditer(r"\b(struct|class|union)\s+([A-Za-z_]\w*)[^;{]*\{", text):
        name = m.group(2)
        open_idx = m.end() - 1
        close_idx = find_matching_brace(text, open_idx)
        if close_idx < 0:
            continue
        body = text[open_idx + 1 : close_idx]
        if not contains_top_level_tag_macro(body):
            continue
        start_line = text.count("\n", 0, m.start()) + 1
        out.append(TaggedStruct(file=path, name=name, start_line=start_line, body=body))
    return out


def normalize_type(type_text: str) -> str:
    """Normalize a type fragment for comparison against allowlists.

    Normalization goals:
      - collapse whitespace
      - remove cv-qualifiers (const/volatile)
      - preserve type identity sufficiently for allowlist matching
    """

    t = re.sub(r"\s+", " ", type_text.strip())
    t = re.sub(r"\bconst\b|\bvolatile\b", "", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def base_type(type_text: str) -> str:
    """Normalize type text and canonicalize pointer/reference spacing."""

    t = normalize_type(type_text)
    # Drop spacing noise around declarators so matching is stable.
    t = t.replace(" *", "*").replace(" &", "&")
    return t


def top_level_declarations(body: str) -> list[tuple[int, str]]:
    """Return top-level declarations from a struct body.

    Output format:
      [(line_offset, declaration_without_semicolon), ...]

    We intentionally only collect statements at brace depth 0 so nested helper
    declarations (nested structs/unions/enums) do not appear as fields of the
    outer record.
    """

    decls: list[tuple[int, str]] = []
    depth = 0
    line = 1
    chunk: list[str] = []
    chunk_line = 1
    for ch in body:
        if ch == "\n":
            line += 1
        if depth == 0 and not chunk and not ch.isspace():
            chunk_line = line
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1

        # We only build declaration chunks for top-level depth.
        if depth == 0:
            chunk.append(ch)
            if ch == ";":
                decl = "".join(chunk).strip()
                if decl:
                    decls.append((chunk_line, decl[:-1].strip()))
                chunk.clear()
        elif depth < 0:
            break
    return decls


def contains_top_level_tag_macro(body: str) -> bool:
    """Check whether TAG_MACRO appears as a top-level declaration in `body`."""

    for _line, decl in top_level_declarations(body):
        if decl.startswith(TAG_MACRO):
            return True
    return False


def parse_decl_type(decl: str) -> str | None:
    """Extract the field type from a simple declaration line.

    Returns:
      - normalized type string for supported declaration shapes
      - None for declarations intentionally ignored by validator policy

    This parser is conservative by design. If a declaration looks complex
    (functions, templates with unusual syntax, labels, etc.) we skip instead
    of risking an incorrect interpretation.
    """

    if not decl or decl.startswith(DECL_SKIP_PREFIXES):
        return None
    if decl.startswith("#"):
        return None
    for token in DECL_SKIP_CONTAINS:
        if token in decl:
            return None
    if "(" in decl or ")" in decl:
        return None
    if "{" in decl or "}" in decl:
        return None
    if decl.startswith(("struct ", "class ", "union ", "enum ")):
        return None
    if ":" in decl and "::" not in decl:
        # Labels or bitfields are not parsed as regular type declarations here.
        return None

    lhs = decl.split("=", 1)[0].strip()
    if " " not in lhs:
        return None
    type_part, _name = lhs.rsplit(" ", 1)
    return base_type(type_part)


def is_allowed_type(
    type_name: str,
    tagged_names: set[str],
    local_nested_types: set[str],
    fixed_enum_names: set[str],
) -> bool:
    """Return True if a normalized type name is allowed in tagged structs."""

    if not type_name:
        return True
    if type_name.startswith("simfil::ColumnTypeField<"):
        return True
    if re.match(r"^[A-Za-z_]\w*_$", type_name):
        # Template parameter placeholder type.
        return True
    if type_name in FIXED_WIDTH_TYPES:
        return True
    if type_name in tagged_names:
        return True
    if type_name in local_nested_types:
        return True
    if type_name in fixed_enum_names:
        return True
    if "::" in type_name:
        tail = type_name.split("::")[-1]
        if tail in tagged_names or tail in local_nested_types:
            return True
        if tail in fixed_enum_names:
            return True
    return False


def validate_struct(
    struct: TaggedStruct,
    tagged_names: set[str],
    fixed_enum_names: set[str],
) -> list[str]:
    """Validate one tagged struct and return list of human-readable errors."""

    errors: list[str] = []
    # Local nested type names are accepted in field declarations.
    nested_types = {
        m.group(1)
        for m in re.finditer(r"\b(?:struct|class|union)\s+([A-Za-z_]\w*)\b", struct.body)
    }
    nested_types.update(
        m.group(1)
        for m in re.finditer(
            r"\benum(?:\s+class)?\s+([A-Za-z_]\w*)\s*:\s*(?:std::)?(?:u?int(?:8|16|32|64)_t)\b",
            struct.body,
        )
    )
    for rel_line, decl in top_level_declarations(struct.body):
        type_name = parse_decl_type(decl)
        if type_name is None:
            continue

        # `int`/`long` are forbidden because their width is ABI/compiler dependent.
        if re.search(r"\bint\b|\blong\b", type_name):
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: disallowed non-fixed-width type in declaration `{decl}`"
            )
            continue
        # Pointers/references are forbidden in wire-record fields.
        if "*" in type_name or "&" in type_name:
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: pointers/references are not allowed in column structs (`{decl}`)"
            )
            continue
        # Any non-allowlisted type is reported with precise declaration context.
        if not is_allowed_type(type_name, tagged_names, nested_types, fixed_enum_names):
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: unsupported field type `{type_name}` in `{decl}`"
            )
    return errors


def main() -> int:
    """CLI entry point.

    Exit codes:
      0 - success (all tagged structs passed validation)
      1 - validation failed
      2 - usage/input error (e.g. missing file)
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", help="Source/header files to validate")
    args = parser.parse_args()

    # Extract all tagged structs from all provided files.
    all_structs: list[TaggedStruct] = []
    for f in args.files:
        path = pathlib.Path(f)
        if not path.exists():
            print(f"{path}: file not found", file=sys.stderr)
            return 2
        all_structs.extend(extract_tagged_structs(path))

    # Build lookup sets used by validation rules.
    tagged_names = {s.name for s in all_structs}
    fixed_enum_names: set[str] = set()
    enum_pattern = re.compile(
        r"\benum(?:\s+class)?\s+([A-Za-z_]\w*)\s*:\s*(?:std::)?(?:u?int(?:8|16|32|64)_t)\b"
    )
    for f in args.files:
        text = strip_comments(pathlib.Path(f).read_text(encoding="utf-8"))
        fixed_enum_names.update(m.group(1) for m in enum_pattern.finditer(text))

    # Validate each tagged struct independently and aggregate diagnostics.
    all_errors: list[str] = []
    for s in all_structs:
        all_errors.extend(validate_struct(s, tagged_names, fixed_enum_names))

    if all_errors:
        for e in all_errors:
            print(e, file=sys.stderr)
        return 1

    print(f"column-type-validator: validated {len(all_structs)} tagged structs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
