#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def c_escape(s: str) -> str:
    return (
        s.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def write_file(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def sanitize_name(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    s = re.sub(r"_+", "_", s)
    if s == "":
        s = "_"
    if s[0].isdigit():
        s = "_" + s
    return s


def to_printf_fmt(message: str) -> tuple[str, int]:
    out: list[str] = []
    i = 0
    arg_count = 0
    while i < len(message):
        if message.startswith("{s}", i):
            out.append("%s")
            arg_count += 1
            i += 3
            continue
        if message[i] == "%":
            out.append("%%")
        else:
            out.append(message[i])
        i += 1
    return ("".join(out), arg_count)


def load_entries(path: Path) -> list[dict[str, object]]:
    id_re = re.compile(r"^HOP([1-9][0-9]*)$")
    category_ranges = {
        "syntactic": (1000, 1999),
        "semantic": (2000, 2999),
        "codegen": (3000, 3999),
        "compiler": (4000, 4999),
    }
    seen_diag_ids: set[str] = set()
    seen_enum: set[str] = set()
    entries: list[dict[str, object]] = []

    if path.suffix.lower() == ".jsonl":
        rows = _load_rows_jsonl(path)
    else:
        rows = _load_rows_json_object(path)

    for diag_id, row, where in rows:
        if diag_id in seen_diag_ids:
            fail(f"{where}: duplicate diagnostic id '{diag_id}'")
        seen_diag_ids.add(diag_id)
        entries.append(_validate_entry(path, diag_id, row, where, id_re, category_ranges, seen_enum))

    if len(entries) == 0:
        fail(f"{path}: diagnostics map must not be empty")

    return entries


def _load_rows_json_object(path: Path) -> list[tuple[str, dict[str, object], str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        fail(f"{path}: invalid JSON: {e}")

    if not isinstance(data, dict):
        fail(f"{path}: top-level JSON value must be an object")

    rows: list[tuple[str, dict[str, object], str]] = []
    for diag_id, v in data.items():
        if not isinstance(v, dict):
            fail(f"{path}:{diag_id}: expected object")
        rows.append((diag_id, dict(v), f"{path}:{diag_id}"))
    return rows


def _load_rows_jsonl(path: Path) -> list[tuple[str, dict[str, object], str]]:
    rows: list[tuple[str, dict[str, object], str]] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for lineno, line in enumerate(lines, start=1):
        s = line.strip()
        if s == "" or s.startswith("#") or s.startswith("//"):
            continue
        try:
            obj = json.loads(s)
        except json.JSONDecodeError as e:
            fail(f"{path}:{lineno}: invalid JSON object: {e}")
        if not isinstance(obj, dict):
            fail(f"{path}:{lineno}: expected object")
        diag_id = obj.get("id")
        if not isinstance(diag_id, str):
            fail(f"{path}:{lineno}: missing or invalid string field 'id'")
        payload = dict(obj)
        payload.pop("id", None)
        rows.append((diag_id, payload, f"{path}:{lineno}:{diag_id}"))
    return rows


def _validate_entry(
    path: Path,
    diag_id: str,
    v: dict[str, object],
    where: str,
    id_re: re.Pattern[str],
    category_ranges: dict[str, tuple[int, int]],
    seen_enum: set[str],
) -> dict[str, object]:
    m = id_re.match(diag_id)
    if m is None:
        fail(f"{where}: invalid diagnostic id '{diag_id}', expected HOP<positive-int>")
    diag_num = int(m.group(1))

    message = v.get("message")
    if not isinstance(message, str) or message == "":
        fail(f"{where}: 'message' must be a non-empty string")

    name = v.get("name")
    if name is None:
        enum_suffix = "_" + diag_id
    else:
        if not isinstance(name, str) or name == "":
            fail(f"{where}: 'name' must be a non-empty string when provided")
        enum_suffix = sanitize_name(name)

    enum_name = "H2Diag_" + enum_suffix
    if enum_name in seen_enum:
        fail(f"{where}: duplicate enum name '{enum_name}'")
    seen_enum.add(enum_name)

    diag_type = v.get("type", "error")
    if not isinstance(diag_type, str):
        fail(f"{where}: 'type' must be a string when provided")
    if diag_type not in ("error", "warning"):
        fail(f"{where}: 'type' must be 'error' or 'warning'")

    hint = v.get("hint", "")
    if hint is None:
        hint = ""
    if not isinstance(hint, str):
        fail(f"{where}: 'hint' must be a string when provided")

    category = v.get("category")
    if not isinstance(category, str):
        fail(f"{where}: 'category' must be a string")
    if category not in category_ranges:
        fail(
            f"{where}: 'category' must be one of " + ", ".join(sorted(category_ranges.keys()))
        )
    lo, hi = category_ranges[category]
    if diag_num < lo or diag_num > hi:
        fail(f"{where}: id must be in range HOP{lo}-HOP{hi} for category '{category}'")

    printf_fmt, arg_count = to_printf_fmt(message)
    if arg_count > 2:
        fail(f"{where}: currently supports at most two '{{s}}' placeholders")

    return {
        "id": diag_id,
        "enum": enum_name,
        "message": message,
        "fmt": printf_fmt,
        "hint": hint,
        "type": diag_type,
        "arg_count": arg_count,
        "category": category,
    }


def render_enum_inc(entries: list[dict[str, object]], rel_source: str) -> str:
    lines: list[str] = []
    lines.append("/* Code generated by tools/gen_diagnostics.py. DO NOT EDIT. */")
    lines.append(f"/* Source: {rel_source} */")
    lines.append("H2Diag_NONE = 0,")
    for e in entries:
        lines.append(f'{e["enum"]}, /* {e["id"]}: {e["message"]} */')
    lines.append("")
    return "\n".join(lines)


def render_data_c(entries: list[dict[str, object]], rel_source: str) -> str:
    lines: list[str] = []
    lines.append("/* Code generated by tools/gen_diagnostics.py. DO NOT EDIT. */")
    lines.append(f"/* Source: {rel_source} */")
    lines.append('#include "libhop-impl.h"')
    lines.append("")
    lines.append("H2_API_BEGIN")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char* id;")
    lines.append("    const char* messageFmt;")
    lines.append("    const char* hint;")
    lines.append("    H2DiagType  type;")
    lines.append("    uint8_t     argCount;")
    lines.append("} H2DiagInfo;")
    lines.append("")
    lines.append("static const H2DiagInfo g_hopDiagInfo[H2Diag__COUNT] = {")
    lines.append('    [H2Diag_NONE] = { "", "no error", NULL, H2DiagType_ERROR, 0 },')
    for e in entries:
        hint = "NULL" if e["hint"] == "" else f"\"{c_escape(str(e['hint']))}\""
        diag_type = "H2DiagType_WARNING" if e["type"] == "warning" else "H2DiagType_ERROR"
        lines.append(
            f'    [{e["enum"]}] = {{ "{c_escape(str(e["id"]))}", "{c_escape(str(e["fmt"]))}",'
            f" {hint}, {diag_type}, {e['arg_count']} }},"
        )
    lines.append("};")
    lines.append("")
    lines.append("const char* H2DiagId(H2DiagCode code) {")
    lines.append("    uint32_t i = (uint32_t)code;")
    lines.append("    if (i >= (uint32_t)H2Diag__COUNT || g_hopDiagInfo[i].id == NULL) {")
    lines.append('        return "HOP????";')
    lines.append("    }")
    lines.append("    return g_hopDiagInfo[i].id;")
    lines.append("}")
    lines.append("")
    lines.append("const char* H2DiagMessage(H2DiagCode code) {")
    lines.append("    uint32_t i = (uint32_t)code;")
    lines.append("    if (i >= (uint32_t)H2Diag__COUNT || g_hopDiagInfo[i].messageFmt == NULL) {")
    lines.append('        return "unknown diagnostic";')
    lines.append("    }")
    lines.append("    return g_hopDiagInfo[i].messageFmt;")
    lines.append("}")
    lines.append("")
    lines.append("const char* _Nullable H2DiagHint(H2DiagCode code) {")
    lines.append("    uint32_t i = (uint32_t)code;")
    lines.append("    if (i >= (uint32_t)H2Diag__COUNT) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    return g_hopDiagInfo[i].hint;")
    lines.append("}")
    lines.append("")
    lines.append("H2DiagType H2DiagTypeOfCode(H2DiagCode code) {")
    lines.append("    uint32_t i = (uint32_t)code;")
    lines.append("    if (i >= (uint32_t)H2Diag__COUNT) {")
    lines.append("        return H2DiagType_ERROR;")
    lines.append("    }")
    lines.append("    return g_hopDiagInfo[i].type;")
    lines.append("}")
    lines.append("")
    lines.append("uint8_t H2DiagArgCount(H2DiagCode code) {")
    lines.append("    uint32_t i = (uint32_t)code;")
    lines.append("    if (i >= (uint32_t)H2Diag__COUNT) {")
    lines.append("        return 0;")
    lines.append("    }")
    lines.append("    return g_hopDiagInfo[i].argCount;")
    lines.append("}")
    lines.append("")
    lines.append("H2_API_END")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", "--json", dest="input", required=True, type=Path)
    ap.add_argument("--enum-out", required=True, type=Path)
    ap.add_argument("--c-out", required=True, type=Path)
    args = ap.parse_args()

    entries = load_entries(args.input)
    rel_source = str(args.input).replace("\\", "/")

    write_file(args.enum_out, render_enum_inc(entries, rel_source))
    write_file(args.c_out, render_data_c(entries, rel_source))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
