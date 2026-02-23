#!/usr/bin/env python3
"""
Generate C ABI declarations for the core package from lib/core/*.sl and
lib/platform/platform.sl, and
patch lib/core/core.h in-place between fenced markers.

Fence markers in core.h:
  // BEGIN generated code
  // END generated code

This currently emits:
  - __sl_Allocator
  - __sl_Context
  - enum __sl_PlatformOps
and legacy aliases used by older generated/runtime C:
  - __sl_mem_Allocator
  - __sl_MainContext
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


BUILTIN_C_TYPE = {
    "void": "void",
    "bool": "__sl_bool",
    "str": "__sl_str",
    "u8": "__sl_u8",
    "u16": "__sl_u16",
    "u32": "__sl_u32",
    "u64": "__sl_u64",
    "i8": "__sl_i8",
    "i16": "__sl_i16",
    "i32": "__sl_i32",
    "i64": "__sl_i64",
    "uint": "__sl_uint",
    "int": "__sl_int",
    "f32": "__sl_f32",
    "f64": "__sl_f64",
}

BEGIN_MARKER = "// BEGIN generated code"
END_MARKER = "// END generated code"


def die(msg: str) -> "None":
    print(f"gen_core_abi.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


def strip_line_comments(src: str) -> str:
    return re.sub(r"//.*$", "", src, flags=re.MULTILINE)


def parse_structs(src: str) -> dict[str, list[str]]:
    structs: dict[str, list[str]] = {}
    pattern = re.compile(r"pub\s+struct\s+([A-Za-z_]\w*)\s*\{([^}]*)\}", flags=re.DOTALL)
    for match in pattern.finditer(src):
        name = match.group(1)
        body = match.group(2)
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        structs[name] = lines
    return structs


def parse_enums(src: str) -> dict[str, tuple[str, list[tuple[str, int]]]]:
    enums: dict[str, tuple[str, list[tuple[str, int]]]] = {}
    pattern = re.compile(
        r"(?:pub\s+)?enum\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\{([^}]*)\}",
        flags=re.DOTALL,
    )
    for match in pattern.finditer(src):
        name = match.group(1)
        base_type = match.group(2)
        body = match.group(3)
        members: list[tuple[str, int]] = []
        for raw_line in body.splitlines():
            line = raw_line.strip().rstrip(",")
            if not line:
                continue
            mm = re.fullmatch(r"([A-Za-z_]\w*)\s*=\s*(-?\d+)", line)
            if not mm:
                die(f"unsupported enum member syntax in {name}: {line!r}")
            members.append((mm.group(1), int(mm.group(2))))
        if not members:
            die(f"enum {name!r} has no members")
        enums[name] = (base_type, members)
    return enums


def split_type_expr(type_expr: str) -> tuple[str, int]:
    ptr_depth = 0
    t = type_expr.strip()
    while t.startswith("*"):
        ptr_depth += 1
        t = t[1:].strip()
    if not re.match(r"^[A-Za-z_]\w*$", t):
        die(f"unsupported type expression: {type_expr!r}")
    return t, ptr_depth


def map_base_type(base: str, known_structs: set[str]) -> str:
    if base in BUILTIN_C_TYPE:
        return BUILTIN_C_TYPE[base]
    if base in known_structs:
        return f"__sl_{base}"
    return f"__sl_{base}"


def map_type(type_expr: str, known_structs: set[str]) -> str:
    base, ptr_depth = split_type_expr(type_expr)
    c_type = map_base_type(base, known_structs)
    return c_type + ("*" * ptr_depth)


def parse_named_typed_token(line: str) -> tuple[str, str]:
    parts = line.split()
    if len(parts) != 2:
        die(f"expected '<name> <type>', got: {line!r}")
    return parts[0], parts[1]


def parse_allocator_signature(line: str) -> tuple[str, list[tuple[str, str]]]:
    match = re.match(r"impl\s+fn\s*\((.*)\)\s+([*A-Za-z_]\w*)\s*$", line)
    if not match:
        die(f"invalid Allocator impl signature: {line!r}")
    params_raw = [x.strip() for x in match.group(1).split(",") if x.strip()]
    return_type = match.group(2).strip()
    params: list[tuple[str, str]] = []
    pending_names: list[str] = []
    for p in params_raw:
        parts = p.split()
        if len(parts) == 1:
            pending_names.append(parts[0])
            continue
        if len(parts) == 2:
            name, type_expr = parts
            names = pending_names + [name]
            for n in names:
                params.append((n, type_expr))
            pending_names = []
            continue
        die(f"invalid parameter segment: {p!r}")
    if pending_names:
        die(f"parameter names without type: {', '.join(pending_names)}")
    return return_type, params


def load_core_structs(core_dir: Path) -> dict[str, list[str]]:
    structs: dict[str, list[str]] = {}
    sl_files = sorted(core_dir.glob("*.sl"))
    if not sl_files:
        die(f"no .sl files found in {core_dir}")
    for path in sl_files:
        src = strip_line_comments(path.read_text(encoding="utf-8"))
        parsed = parse_structs(src)
        for name, lines in parsed.items():
            if name in structs:
                die(f"duplicate struct {name!r} in core package ({path})")
            structs[name] = lines
    return structs


def load_platform_enums(platform_path: Path) -> dict[str, tuple[str, list[tuple[str, int]]]]:
    src = strip_line_comments(platform_path.read_text(encoding="utf-8"))
    return parse_enums(src)


def emit_core_abi(
    structs: dict[str, list[str]], platform_enums: dict[str, tuple[str, list[tuple[str, int]]]]
) -> str:
    if "Allocator" not in structs:
        die("missing 'pub struct Allocator' in core package")
    if "Context" not in structs:
        die("missing 'pub struct Context' in core package")
    known_structs = set(structs.keys())

    alloc_lines = structs["Allocator"]
    if len(alloc_lines) != 1:
        die("Allocator must contain exactly one impl line")
    ret_type, params = parse_allocator_signature(alloc_lines[0])

    ctx_fields = [parse_named_typed_token(line) for line in structs["Context"]]
    if "PlatformOps" not in platform_enums:
        die("missing 'enum PlatformOps ...' in lib/platform/platform.sl")
    platform_ops_members = platform_enums["PlatformOps"][1]

    out: list[str] = []
    out.append(
        "/* Code generated by tools/gen_core_abi.py from lib/core/*.sl and "
        "lib/platform/platform.sl. DO NOT EDIT. */"
    )
    out.append("")
    out.append("typedef struct __sl_Allocator __sl_Allocator;")
    out.append("struct __sl_Allocator {")
    out.append(f"    {map_type(ret_type, known_structs)} (*impl)(")
    for i, (name, type_expr) in enumerate(params):
        comma = "," if i + 1 < len(params) else ""
        out.append(f"        {map_type(type_expr, known_structs)} {name}{comma}")
    out.append("        );")
    out.append("};")
    out.append("")
    out.append("typedef struct {")
    for name, type_expr in ctx_fields:
        out.append(f"    {map_type(type_expr, known_structs)} {name};")
    out.append("} __sl_Context;")
    out.append("")
    out.append("/* Legacy aliases for compatibility with older generated/runtime C. */")
    out.append("typedef __sl_Allocator __sl_mem_Allocator;")
    out.append("typedef __sl_Context   __sl_MainContext;")
    out.append("")
    out.append("enum __sl_PlatformOps {")
    for member_name, value in platform_ops_members:
        out.append(f"    __sl_PlatformOp_{member_name} = {value},")
    out.append("};")
    out.append("")
    return "\n".join(out)


def patch_header_with_block(header_text: str, generated_block: str) -> str:
    begin_i = header_text.find(BEGIN_MARKER)
    end_i = header_text.find(END_MARKER)
    if begin_i < 0 or end_i < 0 or end_i < begin_i:
        die("missing or invalid fence markers in core header")

    begin_line_end = header_text.find("\n", begin_i)
    if begin_line_end < 0:
        die("invalid begin marker line")

    end_line_start = header_text.rfind("\n", 0, end_i)
    if end_line_start < 0:
        end_line_start = 0
    else:
        end_line_start += 1

    before = header_text[: begin_line_end + 1]
    after = header_text[end_line_start:]

    block = generated_block.rstrip("\n") + "\n"
    return before + block + after


def write_if_changed(path: Path, text: str) -> bool:
    old = None
    if path.exists():
        old = path.read_text(encoding="utf-8")
    if old == text:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--types", help="Path to lib/core/types.sl")
    group.add_argument("--core-dir", help="Path to lib/core directory")
    ap.add_argument("--platform", required=True, help="Path to lib/platform/platform.sl")
    ap.add_argument("--header", required=True, help="Path to lib/core/core.h")
    ap.add_argument("--stamp", help="Optional stamp file to touch/write")
    args = ap.parse_args()

    if args.core_dir:
        structs = load_core_structs(Path(args.core_dir))
    else:
        types_path = Path(args.types)
        structs = load_core_structs(types_path.parent)
    platform_enums = load_platform_enums(Path(args.platform))
    header_path = Path(args.header)

    generated_block = emit_core_abi(structs, platform_enums)
    header_text = header_path.read_text(encoding="utf-8")
    patched_header = patch_header_with_block(header_text, generated_block)
    write_if_changed(header_path, patched_header)

    if args.stamp:
        stamp = Path(args.stamp)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text("ok\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
