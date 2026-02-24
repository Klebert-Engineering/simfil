#!/usr/bin/env python3
"""Validate MODEL_COLUMN_TYPE structs for stable field types.

The validator intentionally stays lightweight. It does not require a full C++
parser and focuses on practical wire-safety checks:
  - reject bare `int` / `long`
  - reject pointers/references in tagged structs
  - require top-level fields to be fixed-width scalar aliases, nested tagged
    structs/unions, or simfil::ColumnTypeField<...>
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


TAG_MACRO = "MODEL_COLUMN_TYPE("

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

DECL_SKIP_CONTAINS = (
    "operator ",
    " virtual ",
)


@dataclass
class TaggedStruct:
    file: pathlib.Path
    name: str
    start_line: int
    body: str


def strip_comments(text: str) -> str:
    text = re.sub(r"//.*?$", "", text, flags=re.MULTILINE)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return text


def find_matching_brace(text: str, open_idx: int) -> int:
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
    raw = path.read_text(encoding="utf-8")
    text = strip_comments(raw)
    out: list[TaggedStruct] = []
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
    t = re.sub(r"\s+", " ", type_text.strip())
    t = re.sub(r"\bconst\b|\bvolatile\b", "", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def base_type(type_text: str) -> str:
    t = normalize_type(type_text)
    # drop trailing qualifiers around declarators and arrays
    t = t.replace(" *", "*").replace(" &", "&")
    return t


def top_level_declarations(body: str) -> list[tuple[int, str]]:
    """Return (line_offset, decl_without_semicolon) at body depth 0."""
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
    for _line, decl in top_level_declarations(body):
        if decl.startswith(TAG_MACRO):
            return True
    return False


def parse_decl_type(decl: str) -> str | None:
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
        # labels / bitfields
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
    if not type_name:
        return True
    if type_name.startswith("simfil::ColumnTypeField<"):
        return True
    if re.match(r"^[A-Za-z_]\w*_$", type_name):
        # Template parameter type.
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
    errors: list[str] = []
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

        if re.search(r"\bint\b|\blong\b", type_name):
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: disallowed non-fixed-width type in declaration `{decl}`"
            )
            continue
        if "*" in type_name or "&" in type_name:
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: pointers/references are not allowed in column structs (`{decl}`)"
            )
            continue
        if not is_allowed_type(type_name, tagged_names, nested_types, fixed_enum_names):
            errors.append(
                f"{struct.file}:{struct.start_line + rel_line - 1}: "
                f"{struct.name}: unsupported field type `{type_name}` in `{decl}`"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", help="Source/header files to validate")
    args = parser.parse_args()

    all_structs: list[TaggedStruct] = []
    for f in args.files:
        path = pathlib.Path(f)
        if not path.exists():
            print(f"{path}: file not found", file=sys.stderr)
            return 2
        all_structs.extend(extract_tagged_structs(path))

    tagged_names = {s.name for s in all_structs}
    fixed_enum_names: set[str] = set()
    enum_pattern = re.compile(
        r"\benum(?:\s+class)?\s+([A-Za-z_]\w*)\s*:\s*(?:std::)?(?:u?int(?:8|16|32|64)_t)\b"
    )
    for f in args.files:
        text = strip_comments(pathlib.Path(f).read_text(encoding="utf-8"))
        fixed_enum_names.update(m.group(1) for m in enum_pattern.finditer(text))

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
