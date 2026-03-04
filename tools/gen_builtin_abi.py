#!/usr/bin/env python3
"""
Generate C ABI declarations for the builtin package from lib/builtin/*.sl and
lib/platform/platform.sl, and patch lib/builtin/builtin.h in-place between fenced markers.

Fence markers in builtin.h:
  // BEGIN generated code
  // END generated code
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
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


@dataclass(frozen=True)
class EnumDecl:
    name: str
    base_type: str
    members: list[tuple[str, str]]
    file: str


@dataclass(frozen=True)
class AliasDecl:
    name: str
    type_expr: str
    file: str


@dataclass(frozen=True)
class StructDecl:
    name: str
    fields: list[tuple[str, str]]
    file: str


BuiltinDecl = EnumDecl | AliasDecl | StructDecl


def die(msg: str) -> "None":
    print(f"gen_builtin_abi.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


def strip_line_comments(src: str) -> str:
    return re.sub(r"//.*$", "", src, flags=re.MULTILINE)


def split_top_level_commas(src: str) -> list[str]:
    out: list[str] = []
    cur: list[str] = []
    paren = 0
    bracket = 0
    for ch in src:
        if ch == "," and paren == 0 and bracket == 0:
            seg = "".join(cur).strip()
            if seg:
                out.append(seg)
            cur = []
            continue
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren -= 1
            if paren < 0:
                die(f"unbalanced ')' in parameter list: {src!r}")
        elif ch == "[":
            bracket += 1
        elif ch == "]":
            bracket -= 1
            if bracket < 0:
                die(f"unbalanced ']' in parameter list: {src!r}")
        cur.append(ch)
    if paren != 0 or bracket != 0:
        die(f"unbalanced delimiters in parameter list: {src!r}")
    seg = "".join(cur).strip()
    if seg:
        out.append(seg)
    return out


def split_name_and_type(src: str) -> tuple[str, str]:
    parts = src.strip().split(None, 1)
    if len(parts) != 2:
        die(f"expected '<name> <type>', got: {src!r}")
    name = parts[0]
    type_expr = parts[1].strip()
    if not re.fullmatch(r"[A-Za-z_]\w*", name):
        die(f"invalid identifier {name!r} in: {src!r}")
    if not type_expr:
        die(f"missing type in: {src!r}")
    return name, type_expr


def parse_param_list(src: str) -> list[tuple[str, str]]:
    params: list[tuple[str, str]] = []
    pending_names: list[str] = []
    segments = split_top_level_commas(src)
    for i, seg in enumerate(segments):
        parts = seg.split(None, 1)
        if len(parts) == 1:
            token = parts[0]
            if not token:
                continue

            next_has_named_type = False
            if i + 1 < len(segments):
                next_parts = segments[i + 1].split(None, 1)
                if len(next_parts) == 2 and re.fullmatch(r"[A-Za-z_]\w*", next_parts[0]):
                    next_has_named_type = True

            if pending_names and not next_has_named_type:
                for n in pending_names:
                    params.append((n, token))
                pending_names = []
                continue

            if next_has_named_type:
                if not re.fullmatch(r"[A-Za-z_]\w*", token):
                    die(f"invalid parameter name {token!r} in segment {seg!r}")
                pending_names.append(token)
                continue

            params.append((f"arg{len(params)}", token))
            continue
        name = parts[0]
        type_expr = parts[1].strip()
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            die(f"invalid parameter name {name!r} in segment {seg!r}")
        names = pending_names + [name]
        for n in names:
            params.append((n, type_expr))
        pending_names = []
    if pending_names:
        die(f"parameter names without type: {', '.join(pending_names)}")
    return params


def parse_fn_type_expr(type_expr: str) -> tuple[str, list[tuple[str, str]]] | None:
    s = type_expr.strip()
    m = re.match(r"fn\s*\(", s)
    if m is None:
        return None
    open_i = s.find("(", m.start())
    depth = 0
    close_i = -1
    for i in range(open_i, len(s)):
        ch = s[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                close_i = i
                break
            if depth < 0:
                die(f"invalid function type expression: {type_expr!r}")
    if close_i < 0 or depth != 0:
        die(f"invalid function type expression: {type_expr!r}")
    params_src = s[open_i + 1 : close_i].strip()
    ret_expr = s[close_i + 1 :].strip()
    if not ret_expr:
        ret_expr = "void"
    params = parse_param_list(params_src) if params_src else []
    return ret_expr, params


def parse_array_type_expr(type_expr: str) -> tuple[str, str] | None:
    s = type_expr.strip()
    if not (s.startswith("[") and s.endswith("]")):
        return None
    inner = s[1:-1].strip()
    parts = inner.split()
    if len(parts) < 2:
        die(f"unsupported array type expression: {type_expr!r}")
    elem_expr = " ".join(parts[:-1]).strip()
    len_expr = parts[-1].strip()
    if not elem_expr or not len_expr:
        die(f"unsupported array type expression: {type_expr!r}")
    return elem_expr, len_expr


def split_ptr_ref_type(type_expr: str) -> tuple[str, str]:
    ops: list[str] = []
    s = type_expr.strip()
    while s.startswith("*") or s.startswith("&"):
        ops.append(s[0])
        s = s[1:].strip()
    return s, "".join(ops)


def map_base_type(base: str, known_types: set[str]) -> str:
    if base in BUILTIN_C_TYPE:
        return BUILTIN_C_TYPE[base]
    if not re.fullmatch(r"[A-Za-z_]\w*", base):
        die(f"unsupported type expression: {base!r}")
    if base in known_types:
        return f"__sl_{base}"
    return f"__sl_{base}"


def map_type_expr(type_expr: str, known_types: set[str]) -> str:
    if parse_fn_type_expr(type_expr) is not None:
        die(f"function type cannot be used as a plain type expression: {type_expr!r}")
    if parse_array_type_expr(type_expr) is not None:
        die(f"array type cannot be used as a plain type expression: {type_expr!r}")
    base, ops = split_ptr_ref_type(type_expr)
    c_base = map_base_type(base, known_types)
    return c_base + ("*" * len(ops))


def emit_params(params: list[tuple[str, str]], known_types: set[str]) -> str:
    if not params:
        return "void"
    c_parts: list[str] = []
    for name, type_expr in params:
        c_parts.append(f"{map_type_expr(type_expr, known_types)} {name}")
    return ", ".join(c_parts)


def emit_named_type(name: str, type_expr: str, known_types: set[str], allow_array: bool) -> str:
    fn_sig = parse_fn_type_expr(type_expr)
    if fn_sig is not None:
        ret_expr, params = fn_sig
        ret_c = map_type_expr(ret_expr, known_types)
        return f"{ret_c} (*{name})({emit_params(params, known_types)})"

    arr = parse_array_type_expr(type_expr)
    if arr is not None:
        if not allow_array:
            die(f"array type not supported in this declaration context: {type_expr!r}")
        elem_expr, len_expr = arr
        elem_c = map_type_expr(elem_expr, known_types)
        if len_expr.startswith("."):
            return f"{elem_c} {name}[]"
        if re.fullmatch(r"\d+", len_expr):
            return f"{elem_c} {name}[{len_expr}]"
        die(f"unsupported array length expression: {len_expr!r}")

    return f"{map_type_expr(type_expr, known_types)} {name}"


def parse_enum_members(body: str, enum_name: str) -> list[tuple[str, str]]:
    members: list[tuple[str, str]] = []
    for raw_line in body.splitlines():
        line = raw_line.strip().rstrip(",")
        if not line:
            continue
        mm = re.fullmatch(r"([A-Za-z_]\w*)\s*=\s*(.+)", line)
        if mm is None:
            die(f"unsupported enum member syntax in {enum_name}: {line!r}")
        members.append((mm.group(1), mm.group(2).strip()))
    if not members:
        die(f"enum {enum_name!r} has no members")
    return members


def parse_decls_from_source(src: str, file_label: str) -> list[CoreDecl]:
    decls: list[CoreDecl] = []
    decl_pattern = re.compile(
        r"(?:pub\s+)?struct\s+([A-Za-z_]\w*)\s*\{([^}]*)\}"
        r"|(?:pub\s+)?type\s+([A-Za-z_]\w*)\s+([^\n]+)"
        r"|(?:pub\s+)?enum\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\{([^}]*)\}",
        flags=re.DOTALL,
    )
    for m in decl_pattern.finditer(src):
        if m.group(1) is not None:
            name = m.group(1)
            body = m.group(2)
            fields: list[tuple[str, str]] = []
            for raw_line in body.splitlines():
                line = raw_line.strip()
                if not line:
                    continue
                fields.append(split_name_and_type(line))
            decls.append(StructDecl(name=name, fields=fields, file=file_label))
            continue

        if m.group(3) is not None:
            name = m.group(3)
            type_expr = m.group(4).strip()
            decls.append(AliasDecl(name=name, type_expr=type_expr, file=file_label))
            continue

        if m.group(5) is not None:
            name = m.group(5)
            base_type = m.group(6)
            body = m.group(7)
            members = parse_enum_members(body, name)
            decls.append(EnumDecl(name=name, base_type=base_type, members=members, file=file_label))
            continue

        die(f"internal parser error in {file_label}")
    return decls


def load_builtin_decls(builtin_dir: Path) -> tuple[list[BuiltinDecl], set[str]]:
    sl_files = sorted(builtin_dir.glob("*.sl"))
    if not sl_files:
        die(f"no .sl files found in {builtin_dir}")

    decls: list[BuiltinDecl] = []
    type_names: set[str] = set()
    for path in sl_files:
        src = strip_line_comments(path.read_text(encoding="utf-8"))
        file_decls = parse_decls_from_source(src, str(path))
        for d in file_decls:
            if d.name in type_names:
                die(f"duplicate type declaration {d.name!r} in {path}")
            type_names.add(d.name)
            decls.append(d)
    return decls, type_names


def emit_builtin_abi(decls: list[BuiltinDecl], known_types: set[str]) -> str:
    struct_order: list[str] = [d.name for d in decls if isinstance(d, StructDecl)]
    if "Allocator" not in struct_order:
        die("missing struct 'Allocator' in builtin package")
    if "Context" not in struct_order:
        die("missing struct 'Context' in builtin package")

    out: list[str] = []
    out.append(
        "/* Code generated by tools/gen_builtin_abi.py from lib/builtin/*.sl and "
        "lib/platform/platform.sl. DO NOT EDIT. */"
    )
    out.append("")

    for name in struct_order:
        out.append(f"typedef struct __sl_{name} __sl_{name};")
    out.append("")

    for d in decls:
        if isinstance(d, EnumDecl):
            out.append("typedef enum {")
            for member_name, expr in d.members:
                out.append(f"    __sl_{d.name}_{member_name} = {expr},")
            out.append(f"}} __sl_{d.name};")
            out.append("")
            continue

        if isinstance(d, AliasDecl):
            fn_sig = parse_fn_type_expr(d.type_expr)
            if fn_sig is not None:
                ret_expr, params = fn_sig
                ret_c = map_type_expr(ret_expr, known_types)
                out.append(
                    f"typedef {ret_c} (*__sl_{d.name})({emit_params(params, known_types)});"
                )
            else:
                out.append(f"typedef {map_type_expr(d.type_expr, known_types)} __sl_{d.name};")
            out.append("")
            continue

        if isinstance(d, StructDecl):
            out.append(f"struct __sl_{d.name} {{")
            for field_name, type_expr in d.fields:
                out.append(
                    f"    {emit_named_type(field_name, type_expr, known_types, allow_array=True)};"
                )
            out.append("};")
            out.append("")
            continue

        die("internal declaration dispatch error")

    return "\n".join(out)


def patch_header_with_block(header_text: str, generated_block: str) -> str:
    begin_i = header_text.find(BEGIN_MARKER)
    end_i = header_text.find(END_MARKER)
    if begin_i < 0 or end_i < 0 or end_i < begin_i:
        die("missing or invalid fence markers in builtin header")

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
    group.add_argument("--types", help="Path to lib/builtin/types.sl")
    group.add_argument("--builtin-dir", help="Path to lib/builtin directory")
    ap.add_argument("--platform", required=True, help="Path to lib/platform/platform.sl")
    ap.add_argument("--header", required=True, help="Path to lib/builtin/builtin.h")
    args = ap.parse_args()

    builtin_dir = Path(args.builtin_dir) if args.builtin_dir else Path(args.types).parent
    decls, type_names = load_builtin_decls(builtin_dir)
    header_path = Path(args.header)

    generated_block = emit_builtin_abi(decls, type_names)
    header_text = header_path.read_text(encoding="utf-8")
    patched_header = patch_header_with_block(header_text, generated_block)
    write_if_changed(header_path, patched_header)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
