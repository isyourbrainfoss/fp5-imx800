#!/usr/bin/env python3
"""CamX sensormodule BinaryParameter decoder for FP5 IMX800.

QTI Chromatix Parameter Parser V3 layout (this blob):
  - Schema tree: name[40] + 0xffffffff + type + size + param_id
  - Value section @ header[0xb8], size header[0xbc]

Key RE result (2026-07-29):
  For leaf ``registerData`` nodes, the schema ``type`` field is a **byte
  offset into the value section** holding the register data (UINT32, use
  low byte for byte writes).

  For ``regSetting`` arrays, schema ``type`` points near a 40-byte/entry
  table whose first u32 is the register address (operation=WRITE=1).
  Skip 0–15 bytes of header until the first plausible address.

Usage:
  python3 parse_camx_imx800.py [sensormodule.bin] [--export-dir DIR]
  python3 parse_camx_imx800.py --list-modes
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

NAME_LEN = 40

# CamX regSetting indices that program real sensor modes (see --list-modes).
# 0 = full QCFA 8192x6144 (heavy; not exported for pmOS default path)
# 1 = 4096x3072 2x2 bin (still / hi-res capture)
# 4 = 1920x1080 4x4 bin (preview / low CMA)
# 5 = 1280x720  4x4 bin (ultra-light preview)
EXPORT_MODE_INDICES = (4, 1, 5)  # default first = lowest CMA usable preview

# Non-mode tables in this blob
INIT_INDEX = 13
STREAM_ON_INDEX = 9
STREAM_OFF_INDEX = 10


def parse_tree(data: bytes, start: int = 0xD0, end: int | None = None):
    if end is None:
        end = min(len(data), 0x4B9D4)
    nodes = []
    off = start
    while off + NAME_LEN + 16 <= end:
        if struct.unpack_from("<I", data, off + NAME_LEN)[0] != 0xFFFFFFFF:
            off += 4
            continue
        raw = data[off : off + NAME_LEN]
        if raw[0] == 0 or not (65 <= raw[0] <= 122):
            off += 4
            continue
        try:
            name = raw.split(b"\x00")[0].decode("ascii")
        except Exception:
            off += 4
            continue
        if not name or not name[0].isalpha() or not all(
            c.isalnum() or c == "_" for c in name
        ):
            off += 4
            continue
        typ, size, val = struct.unpack_from("<III", data, off + NAME_LEN + 4)
        nodes.append(
            {"off": off, "name": name, "type": typ, "size": size, "vid": val}
        )
        off += NAME_LEN + 16
    return nodes


def value_section(data: bytes) -> tuple[int, bytes]:
    voff = struct.unpack_from("<I", data, 0xB8)[0]
    vsize = struct.unpack_from("<I", data, 0xBC)[0]
    return voff, data[voff : voff + vsize]


def regsetting_children(nodes, rs):
    start = nodes.index(rs)
    out = []
    for n in nodes[start + 1 :]:
        if n["name"] == "registerData":
            out.append(n)
        elif n["name"] not in ("slaveAddr", "delayUs"):
            break
    return out


def extract_regsetting(nodes, vs: bytes, rs) -> list[tuple[int, int, int]]:
    regs = regsetting_children(nodes, rs)
    n = len(regs)
    base = rs["type"]
    table_off = None
    for delta in range(0, 64, 4):
        off = base + delta
        if off + 8 > len(vs):
            continue
        ok = True
        for k in range(min(n, 5)):
            if off + k * 40 + 8 > len(vs):
                ok = False
                break
            addr, op = struct.unpack_from("<II", vs, off + k * 40)
            if not (0x100 <= addr <= 0xFFFF) or op not in (0, 1, 2, 3):
                ok = False
                break
        if not ok:
            continue
        if n == 1:
            table_off = off
            break
        addr, op = struct.unpack_from("<II", vs, off + (n - 1) * 40)
        if 0x100 <= addr <= 0xFFFF:
            table_off = off
            break
    if table_off is None:
        raise RuntimeError(f"no addr table near type {base:#x} n={n}")
    entries = []
    for i, rn in enumerate(regs):
        addr = struct.unpack_from("<I", vs, table_off + i * 40)[0]
        delay = struct.unpack_from("<I", vs, table_off + i * 40 + 32)[0]
        d = struct.unpack_from("<I", vs, rn["type"])[0] & 0xFF
        entries.append((addr, d, delay))
    return entries


def summarize_entries(ents: list[tuple[int, int, int]]) -> dict:
    by = {a: d for a, d, _ in ents}
    fll = (by.get(0x340, 0) << 8) | by.get(0x341, 0)
    llp = (by.get(0x342, 0) << 8) | by.get(0x343, 0)
    w = (by.get(0x34C, 0) << 8) | by.get(0x34D, 0)
    h = (by.get(0x34E, 0) << 8) | by.get(0x34F, 0)
    bin_en = by.get(0x900)
    bin_type = by.get(0x901)
    return {
        "n": len(ents),
        "fll": fll,
        "llp": llp,
        "width": w,
        "height": h,
        "bin_en": bin_en,
        "bin_type": bin_type,
        "by": by,
    }


def cci_lines(ents: list[tuple[int, int, int]]) -> list[str]:
    return [f"\t{{ CCI_REG8(0x{a:04x}), 0x{d:02x} }}," for a, d, _dl in ents]


def export_c_header(
    path: Path,
    init: list[tuple[int, int, int]],
    modes: list[tuple[int, list[tuple[int, int, int]], dict]],
    stream_on: list[tuple[int, int, int]],
    stream_off: list[tuple[int, int, int]],
):
    """modes: list of (camx_index, entries, summary)."""
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0 */",
        "/* Auto-generated from com.qti.sensormodule.fp5_imx800.bin",
        " * BinaryParameter RE: registerData.type = offset into value section",
        " * Multi-mode export for pmOS (preview + hi-res).",
        " * DO NOT hand-edit — regenerate via parse_camx_imx800.py",
        " */",
        "#ifndef IMX800_MODE_TABLES_H",
        "#define IMX800_MODE_TABLES_H",
        "",
        "#include <media/v4l2-cci.h>",
        "",
        "/* Init (global) — CamX regSetting index 13 */",
        "static const struct cci_reg_sequence imx800_init_regs[] = {",
    ]
    lines.extend(cci_lines(init))
    lines.append("};")
    lines.append("")

    for camx_idx, ents, s in modes:
        name = f"imx800_mode{camx_idx}_regs"
        lines.append(
            f"/* CamX mode {camx_idx}: {s['width']}x{s['height']} "
            f"FLL={s['fll']} LLP={s['llp']} bin=0x{s['bin_en']:02x}/0x{s['bin_type']:02x} */"
            if s["bin_en"] is not None
            else f"/* CamX mode {camx_idx}: {s['width']}x{s['height']} "
            f"FLL={s['fll']} LLP={s['llp']} */"
        )
        lines.append(f"static const struct cci_reg_sequence {name}[] = {{")
        lines.extend(cci_lines(ents))
        lines.append("};")
        lines.append("")

    lines.append("static const struct cci_reg_sequence imx800_stream_on_regs[] = {")
    lines.extend(cci_lines(stream_on))
    lines.append("};")
    lines.append("static const struct cci_reg_sequence imx800_stream_off_regs[] = {")
    lines.extend(cci_lines(stream_off))
    lines.append("};")
    lines.append("")

    # Mode descriptor table for the driver
    lines.append("struct imx800_mode_info {")
    lines.append("\tu32 width;")
    lines.append("\tu32 height;")
    lines.append("\tu32 fll;")
    lines.append("\tu32 llp;")
    lines.append("\tu64 pixel_rate;")
    lines.append("\tconst struct cci_reg_sequence *regs;")
    lines.append("\tunsigned int num_regs;")
    lines.append("\tunsigned int camx_index; /* debug */")
    lines.append("};")
    lines.append("")
    lines.append("/* Order: default (low-CMA preview) first, then hi-res, then ultra-light. */")
    lines.append("static const struct imx800_mode_info imx800_modes[] = {")
    for camx_idx, ents, s in modes:
        # Rough pixel-rate: width*height*30 with a small headroom; mode1 kept at 450M.
        if camx_idx == 1:
            pr = 450000000
        elif camx_idx == 4:
            pr = 120000000
        elif camx_idx == 5:
            pr = 80000000
        else:
            pr = int(s["width"] * s["height"] * 30 * 1.2)
        lines.append("\t{")
        lines.append(f"\t\t.width = {s['width']},")
        lines.append(f"\t\t.height = {s['height']},")
        lines.append(f"\t\t.fll = {s['fll']},")
        lines.append(f"\t\t.llp = {s['llp']},")
        lines.append(f"\t\t.pixel_rate = {pr}ULL,")
        lines.append(f"\t\t.regs = imx800_mode{camx_idx}_regs,")
        lines.append(f"\t\t.num_regs = {s['n']},")
        lines.append(f"\t\t.camx_index = {camx_idx},")
        lines.append("\t},")
    lines.append("};")
    lines.append("")
    lines.append(f"#define IMX800_NUM_MODES\t{len(modes)}")
    lines.append("/* Default = modes[0] (preview / reduced CMA). */")
    s0 = modes[0][2]
    lines.append(f"#define IMX800_DEFAULT_WIDTH\t{s0['width']}")
    lines.append(f"#define IMX800_DEFAULT_HEIGHT\t{s0['height']}")
    # Keep legacy macros for mode1 (hi-res) so old docs/scripts still map.
    mode1 = next((s for i, e, s in modes if i == 1), None)
    if mode1:
        lines.append(f"#define IMX800_MODE1_WIDTH\t{mode1['width']}")
        lines.append(f"#define IMX800_MODE1_HEIGHT\t{mode1['height']}")
        lines.append(f"#define IMX800_MODE1_FLL\t{mode1['fll']}")
        lines.append(f"#define IMX800_MODE1_LLP\t{mode1['llp']}")
    lines.append("")
    lines.append("#endif /* IMX800_MODE_TABLES_H */")
    lines.append("")
    path.write_text("\n".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "bin",
        nargs="?",
        default="dumps/android/camera-imx800-re-20260729-092459/"
        "com.qti.sensormodule.fp5_imx800.bin",
    )
    ap.add_argument("--export-dir", type=Path, default=None)
    ap.add_argument(
        "--list-modes",
        action="store_true",
        help="Print all regSetting summaries and exit",
    )
    args = ap.parse_args()
    path = Path(args.bin)
    if not path.is_file():
        # Try relative to repo root when run from kernel/
        alt = Path(__file__).resolve().parents[3] / path
        if alt.is_file():
            path = alt
    data = path.read_bytes()
    print(f"file {path} size {len(data)}")
    if not data.startswith(b"QTI Chromatix"):
        print("WARN: missing QTI Chromatix header")

    voff, vs = value_section(data)
    print(f"value section @{voff:#x} len {len(vs):#x}")
    print(
        "identity: slave=0x%02x id_reg=0x%02x id=0x%04x"
        % (
            struct.unpack_from("<I", vs, 0x1C)[0],
            struct.unpack_from("<I", vs, 0x28)[0],
            struct.unpack_from("<I", vs, 0x2C)[0],
        )
    )

    nodes = parse_tree(data, end=voff)
    regsettings = [n for n in nodes if n["name"] == "regSetting"]
    print(f"tree nodes={len(nodes)} regSetting={len(regsettings)}")

    if args.list_modes:
        for i, rs in enumerate(regsettings):
            try:
                ents = extract_regsetting(nodes, vs, rs)
            except Exception as e:
                print(f"  [{i:2d}] FAIL {e}")
                continue
            s = summarize_entries(ents)
            packed = s["width"] * s["height"] * 10 // 8 if s["width"] else 0
            print(
                f"  [{i:2d}] n={s['n']:3d} out={s['width']:4d}x{s['height']:<4d} "
                f"FLL={s['fll']:5d} LLP={s['llp']:5d} "
                f"bin={s['bin_en']}/{s['bin_type']} "
                f"RAW10≈{packed/1024/1024:.2f}MiB"
            )
        return 0

    init = extract_regsetting(nodes, vs, regsettings[INIT_INDEX])
    stream_on = extract_regsetting(nodes, vs, regsettings[STREAM_ON_INDEX])
    stream_off = extract_regsetting(nodes, vs, regsettings[STREAM_OFF_INDEX])

    modes_out: list[tuple[int, list, dict]] = []
    for idx in EXPORT_MODE_INDICES:
        ents = extract_regsetting(nodes, vs, regsettings[idx])
        s = summarize_entries(ents)
        modes_out.append((idx, ents, s))
        print(
            f"  export mode[{idx}]: n={s['n']} {s['width']}x{s['height']} "
            f"FLL={s['fll']} LLP={s['llp']}"
        )

    print(f"  stream_on={stream_on} stream_off={stream_off}")
    print(f"  init n={len(init)}")

    if args.export_dir:
        args.export_dir.mkdir(parents=True, exist_ok=True)
        export_c_header(
            args.export_dir / "imx800_mode_tables.h",
            init,
            modes_out,
            stream_on,
            stream_off,
        )
        for idx, ents, s in modes_out:
            (args.export_dir / f"imx800-mode{idx}-regs.txt").write_text(
                "\n".join(f"0x{a:04x} 0x{d:02x} delay={dl}" for a, d, dl in ents)
                + "\n"
            )
        (args.export_dir / "imx800-init-regs.txt").write_text(
            "\n".join(f"0x{a:04x} 0x{d:02x} delay={dl}" for a, d, dl in init) + "\n"
        )
        print(f"exported to {args.export_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
