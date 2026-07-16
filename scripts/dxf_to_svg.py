#!/usr/bin/env python3
"""
dxf_to_svg.py — Convert a QCAD/AutoCAD DXF drawing to a clean SVG.

This converter was written for landscape/irrigation drawings exported from
QCAD, but works for general 2D DXF files. It aims to reproduce what you see
in QCAD: real drawing coordinates (in the drawing's own units), thin strokes,
the original layer colors, and the QCAD `qs:layer` attribute on every element
so downstream tooling can still tell layers apart.

Supported entity types:
    LINE, CIRCLE, ARC, ELLIPSE, LWPOLYLINE, POLYLINE, SPLINE, POINT,
    TEXT, MTEXT, HATCH (solid + pattern fills)

Not rendered (by design — see notes at the bottom of this file):
    INSERT (block references), DIMENSION, IMAGE

If your drawing uses blocks or images and you want them in the SVG, explode
the blocks first in QCAD (Modify -> Explode) — exploded geometry converts
normally. Images are external files referenced by path, not embedded data.

------------------------------------------------------------------------------
REQUIREMENTS
    Python 3.8+
    ezdxf      ->  pip install ezdxf

USAGE
    python dxf_to_svg.py drawing.dxf
    python dxf_to_svg.py drawing.dxf output.svg
    python dxf_to_svg.py drawing.dxf output.svg --stroke 0.02

If no output path is given, the SVG is written next to the input file with the
same name and a .svg extension.
------------------------------------------------------------------------------
"""

import argparse
import math
import sys
from pathlib import Path

try:
    import ezdxf
    from ezdxf import bbox
    from ezdxf.colors import aci2rgb
except ImportError:
    sys.exit(
        "This script needs the 'ezdxf' library.\n"
        "Install it with:  pip install ezdxf"
    )


# ---------------------------------------------------------------------------
# Color handling
# ---------------------------------------------------------------------------
def resolve_color(entity, doc) -> str:
    """
    Resolve an entity's effective color to a #rrggbb hex string.

    DXF stores color as an AutoCAD Color Index (ACI). Special values:
        256 = BYLAYER  -> use the entity's layer color
        0   = BYBLOCK  -> inherited; we fall back to black
        7   = white/black depending on background (QCAD shows it black)
    """
    color = entity.dxf.color
    if color == 256:                      # BYLAYER
        aci = doc.layers.get(entity.dxf.layer).dxf.color
    elif color == 0:                      # BYBLOCK
        aci = 7
    else:
        aci = color
    aci = abs(aci)                        # negative ACI = layer turned off
    if aci == 7:
        return "#000000"                  # treat 7 as black on a white page
    try:
        r, g, b = aci2rgb(aci)
        return f"#{r:02x}{g:02x}{b:02x}"
    except Exception:
        return "#000000"


# ---------------------------------------------------------------------------
# Small numeric/geometry helpers
# ---------------------------------------------------------------------------
def fmt(value: float) -> str:
    """Format a coordinate to 4 decimals, trimming trailing zeros."""
    s = f"{value:.4f}"
    return s.rstrip("0").rstrip(".") if "." in s else s


def arc_to_points(cx, cy, r, start_deg, end_deg, step_deg=5.0):
    """Approximate a DXF arc (CCW from start to end) as a list of points."""
    sweep = (end_deg - start_deg) % 360
    n = max(8, int(sweep / step_deg))
    return [
        (cx + r * math.cos(math.radians(start_deg + sweep * i / n)),
         cy + r * math.sin(math.radians(start_deg + sweep * i / n)))
        for i in range(n + 1)
    ]


# ---------------------------------------------------------------------------
# HATCH boundary extraction
#
# Hatches don't store a simple corner list. Each boundary loop is either a
# PolylinePath (has .vertices) or an EdgePath made of individual edges
# (lines, arcs, ellipse arcs, splines). We walk whichever form is present and
# return an ordered point list per loop.
# ---------------------------------------------------------------------------
def _edge_to_points(edge):
    name = type(edge).__name__
    pts = []
    if name == "LineEdge":
        pts = [(edge.start[0], edge.start[1]), (edge.end[0], edge.end[1])]

    elif name == "ArcEdge":
        cx, cy = edge.center[0], edge.center[1]
        r = edge.radius
        a0 = math.radians(edge.start_angle)
        a1 = math.radians(edge.end_angle)
        if edge.ccw:
            if a1 < a0:
                a1 += 2 * math.pi
        else:
            if a0 < a1:
                a0 += 2 * math.pi
        sweep = a1 - a0
        n = max(6, int(abs(sweep) / math.radians(5)))
        pts = [
            (cx + r * math.cos(a0 + sweep * i / n),
             cy + r * math.sin(a0 + sweep * i / n))
            for i in range(n + 1)
        ]

    elif name == "EllipseEdge":
        cx, cy = edge.center[0], edge.center[1]
        mx, my = edge.major_axis[0], edge.major_axis[1]
        major_len = math.hypot(mx, my)
        minor_len = major_len * edge.ratio
        rot = math.atan2(my, mx)
        a0 = math.radians(getattr(edge, "start_param", 0))
        a1 = math.radians(getattr(edge, "end_param", 360))
        n = 24
        for i in range(n + 1):
            t = a0 + (a1 - a0) * i / n
            ex, ey = major_len * math.cos(t), minor_len * math.sin(t)
            pts.append((cx + ex * math.cos(rot) - ey * math.sin(rot),
                        cy + ex * math.sin(rot) + ey * math.cos(rot)))

    elif name == "SplineEdge":
        src = getattr(edge, "fit_points", None) or getattr(edge, "control_points", None)
        if src:
            pts = [(p[0], p[1]) for p in src]

    return pts


def hatch_boundary_points(path):
    """Return an ordered point list for one hatch boundary loop."""
    pts = []
    if getattr(path, "edges", None):
        for edge in path.edges:
            ep = _edge_to_points(edge)
            if pts and ep and pts[-1] == ep[0]:
                pts.extend(ep[1:])      # avoid duplicating the shared joint
            else:
                pts.extend(ep)
    elif hasattr(path, "vertices"):
        try:
            pts = [(v[0], v[1]) for v in path.vertices]
        except Exception:
            pts = []
    return pts


# ---------------------------------------------------------------------------
# Main conversion
# ---------------------------------------------------------------------------
def convert(infile: str, outfile: str, stroke: float = 0.0098) -> None:
    doc = ezdxf.readfile(infile)
    msp = doc.modelspace()

    # Compute true bounding box from the geometry itself. We do NOT trust the
    # DXF header's $EXTMIN/$EXTMAX because those can be stale after editing.
    box = bbox.extents(msp)
    minx, miny = box.extmin[0], box.extmin[1]
    maxx, maxy = box.extmax[0], box.extmax[1]
    w, h = maxx - minx, maxy - miny
    pad = max(w, h) * 0.01               # 1% breathing room around the drawing
    minx -= pad
    miny -= pad
    w += 2 * pad
    h += 2 * pad

    # DXF Y points up; SVG Y points down. Flip Y so the drawing isn't mirrored.
    def tx(x):
        return x

    def ty(y):
        return (maxy + pad) - y

    def stroke_style(color):
        return f"stroke:{color};stroke-width:{stroke:.4f};"

    def fill_style(color):
        return f"fill:{color};stroke:none;"

    out = ['<?xml version="1.0" encoding="UTF-8"?>']
    out.append(
        f'<svg width="{w:.4f}in" height="{h:.4f}in" '
        f'viewBox="{minx:.4f} {miny - pad:.4f} {w:.4f} {h:.4f}" '
        f'version="1.1" xmlns="http://www.w3.org/2000/svg" '
        f'style="stroke-linecap:round;stroke-linejoin:round;fill:none" '
        f'xmlns:qs="http://qcad.org/namespaces/svg" '
        f'xmlns:xlink="http://www.w3.org/1999/xlink">'
    )
    out.append("    <g>")

    skipped = {}

    for e in msp:
        t = e.dxftype()
        layer = e.dxf.layer
        color = resolve_color(e, doc)
        try:
            if t == "LINE":
                x1, y1 = e.dxf.start.x, e.dxf.start.y
                x2, y2 = e.dxf.end.x, e.dxf.end.y
                out.append(
                    f'        <line x1="{fmt(tx(x1))}" y1="{fmt(ty(y1))}" '
                    f'x2="{fmt(tx(x2))}" y2="{fmt(ty(y2))}" '
                    f'style="{stroke_style(color)}" qs:layer="{layer}"/>'
                )

            elif t == "CIRCLE":
                cx, cy, r = e.dxf.center.x, e.dxf.center.y, e.dxf.radius
                out.append(
                    f'        <circle cx="{fmt(tx(cx))}" cy="{fmt(ty(cy))}" '
                    f'r="{fmt(r)}" style="{stroke_style(color)}" qs:layer="{layer}"/>'
                )

            elif t == "ARC":
                cx, cy, r = e.dxf.center.x, e.dxf.center.y, e.dxf.radius
                pts = arc_to_points(cx, cy, r, e.dxf.start_angle, e.dxf.end_angle)
                d = "M " + " L ".join(f"{fmt(tx(x))},{fmt(ty(y))}" for x, y in pts)
                out.append(f'        <path d="{d}" style="{stroke_style(color)}" qs:layer="{layer}"/>')

            elif t in ("LWPOLYLINE", "POLYLINE"):
                if t == "LWPOLYLINE":
                    pts = [(p[0], p[1]) for p in e.get_points("xy")]
                    closed = e.closed
                else:
                    pts = [(v.dxf.location.x, v.dxf.location.y) for v in e.vertices]
                    closed = e.is_closed
                if pts:
                    d = "M " + " L ".join(f"{fmt(tx(x))},{fmt(ty(y))}" for x, y in pts)
                    if closed:
                        d += " Z"
                    out.append(f'        <path d="{d}" style="{stroke_style(color)}" qs:layer="{layer}"/>')

            elif t in ("ELLIPSE", "SPLINE"):
                # ezdxf flattens both into points within a 0.01-unit tolerance.
                pts = [(p.x, p.y) for p in e.flattening(0.01)]
                if pts:
                    d = "M " + " L ".join(f"{fmt(tx(x))},{fmt(ty(y))}" for x, y in pts)
                    out.append(f'        <path d="{d}" style="{stroke_style(color)}" qs:layer="{layer}"/>')

            elif t == "POINT":
                cx, cy = e.dxf.location.x, e.dxf.location.y
                out.append(
                    f'        <circle cx="{fmt(tx(cx))}" cy="{fmt(ty(cy))}" '
                    f'r="{stroke * 1.5:.4f}" style="{fill_style(color)}" qs:layer="{layer}"/>'
                )

            elif t in ("TEXT", "MTEXT"):
                px, py = e.dxf.insert.x, e.dxf.insert.y
                if t == "TEXT":
                    txt = e.dxf.text
                    height = e.dxf.height
                else:
                    txt = e.plain_text()
                    height = e.dxf.char_height
                txt = txt.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                out.append(
                    f'        <text x="{fmt(tx(px))}" y="{fmt(ty(py))}" '
                    f'font-size="{fmt(height)}" fill="{color}" qs:layer="{layer}">{txt}</text>'
                )

            elif t == "HATCH":
                # One SVG <path> holds every boundary loop. fill-rule:evenodd
                # makes inner loops cut holes out of outer ones.
                subpaths = []
                for path in e.paths:
                    pts = hatch_boundary_points(path)
                    if len(pts) >= 3:
                        subpaths.append(
                            "M " + " L ".join(f"{fmt(tx(x))},{fmt(ty(y))}" for x, y in pts) + " Z"
                        )
                if subpaths:
                    d = " ".join(subpaths)
                    is_solid = getattr(e.dxf, "solid_fill", 0) == 1
                    # Solid fills are opaque; pattern fills (ANSI31, AR-CONC, ...)
                    # are drawn as a light flat tint so linework under them shows.
                    opacity = "1.0" if is_solid else "0.35"
                    out.append(
                        f'        <path d="{d}" style="fill:{color};'
                        f'fill-opacity:{opacity};fill-rule:evenodd;stroke:none;" '
                        f'qs:layer="{layer}"/>'
                    )

            else:
                # INSERT / DIMENSION / IMAGE / anything exotic: count and skip.
                skipped[t] = skipped.get(t, 0) + 1

        except Exception:
            # Never let one malformed entity abort the whole conversion.
            skipped[t] = skipped.get(t, 0) + 1

    out.append("    </g>")
    out.append("</svg>")

    Path(outfile).write_text("\n".join(out), encoding="utf-8")

    print(f"Wrote {outfile}")
    print(f"Drawing size: {w:.2f} x {h:.2f} (drawing units), stroke width {stroke}")
    if skipped:
        summary = ", ".join(f"{n}x {name}" for name, n in sorted(skipped.items()))
        print(f"Skipped (not rendered): {summary}")
        if "INSERT" in skipped:
            print("  -> To include block geometry, explode blocks in QCAD first "
                  "(Modify > Explode), then re-run.")


def main():
    parser = argparse.ArgumentParser(
        description="Convert a DXF drawing to a clean, layer-preserving SVG."
    )
    parser.add_argument("input", help="Path to the input .dxf file")
    parser.add_argument("output", nargs="?", help="Path for the output .svg "
                        "(default: same name as input, .svg extension)")
    parser.add_argument("--stroke", type=float, default=0.0098,
                        help="Stroke width in drawing units (default: 0.0098, "
                             "matching QCAD's thin lines)")
    args = parser.parse_args()

    in_path = Path(args.input)
    if not in_path.exists():
        sys.exit(f"Error: file not found: {in_path}")

    out_path = Path(args.output) if args.output else in_path.with_suffix(".svg")

    try:
        convert(str(in_path), str(out_path), stroke=args.stroke)
    except ezdxf.DXFStructureError as ex:
        sys.exit(f"Error: '{in_path}' is not a valid DXF file ({ex}).")


if __name__ == "__main__":
    main()
