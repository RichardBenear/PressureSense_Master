#!/usr/bin/env python3
"""
zone_calibration.py
-------------------
Reads an SVG irrigation map and zone_mappings.txt, computes irrigated area
per zone from the map geometry, estimates flow rate from avgpsi using the
pressure-flow square-root law, and calculates mm_per_min for each zone.

Drip zones bypass the pressure-flow law and use a directly specified total_gpm.

Usage:
    python3 zone_calibration.py

Outputs:
    zone_calibration.json   -- machine-readable, for controllers.json integration
    zone_calibration.txt    -- human-readable summary table

Dependencies:
    pip install svgpathtools lxml

Configuration (edit CONFIGURATION section below):
    SVG_FILE          -- path to your SVG map
    MAPPINGS_FILE     -- path to zone_mappings.txt
    CONTROLLERS_FILE  -- path to a controllers.json export (for avgpsi lookup)
    REAL_WIDTH_FT   -- real-world width of property boundary
    REAL_HEIGHT_FT  -- real-world height of property boundary
    BOUNDARY_LAYER  -- SVG layer name of the outer property fence
    HEAD_TYPES      -- sprinkler head specs (rated_gpm, rated_psi, label)
    ZONE_HEAD_ASSIGNMENTS -- per-zone head type, count, or drip total_gpm
"""

import json, re, math
from datetime import datetime
from pathlib import Path
from lxml import etree
from svgpathtools import parse_path

# =============================================================================
# CONFIGURATION — edit these values as needed
# =============================================================================

SVG_FILE        = Path('../images/sprinkler_mapVer8.svg')
MAPPINGS_FILE   = Path('../data/zone_mappings.txt')
CONTROLLERS_FILE = Path('../data/controllers.json')
OUTPUT_JSON     = Path('../calib/zone_calibration.json')
OUTPUT_TXT      = Path('../calib/zone_calibration.txt')

REAL_WIDTH_FT   = 577.6    # ft — long side of the property boundary rectangle
REAL_HEIGHT_FT  = 331.0    # ft — short side of the property boundary rectangle
BOUNDARY_LAYER  = 'zProperty Fence'

# ---------------------------------------------------------------------------
# Head type catalog
# Each entry: rated_gpm at rated_psi.
# flow_gpm = rated_gpm * head_count * sqrt(actual_psi / rated_psi)
# ---------------------------------------------------------------------------
HEAD_TYPES = {
    'biggun':   {'rated_gpm': 45.0, 'rated_psi': 45.0, 'label': 'Big Gun'},
    'rotor5':   {'rated_gpm':  5.0, 'rated_psi': 45.0, 'label': 'Rotor'},
    'popup2':   {'rated_gpm':  2.0, 'rated_psi': 30.0, 'label': 'Pop-up'},
    'popup3':   {'rated_gpm':  3.0, 'rated_psi': 30.0, 'label': 'Pop-up'},
    'rotor3':   {'rated_gpm':  3.0, 'rated_psi': 45.0, 'label': 'Rotor'},
}

# Synthetic catalog entries -- not real rated heads, but calibration.html's
# head_type dropdown and the pressure-flow formula both special-case these
# two keys (no rated_gpm/rated_psi -- flow comes from total_gpm_override
# instead), so they're part of the catalog the UI ships with.
HEAD_CATALOG_EXTRA = {
    'drip':   {'rated_gpm': None, 'rated_psi': None, 'label': 'Drip'},
    'custom': {'rated_gpm': None, 'rated_psi': None, 'label': 'Custom'},
}

# ---------------------------------------------------------------------------
# Zone head assignments
# Key: (controller_lower, zone_number_int)
#
# Sprinkler zones:
#   { 'type': '<key from HEAD_TYPES>', 'head_count': N }
#   flow_gpm = rated_gpm * head_count * sqrt(actual_psi / rated_psi)
#
# Drip zones:
#   { 'type': 'drip', 'total_gpm': N }
#   flow_gpm = total_gpm  (pressure law does not apply to drip)
#   Calculate total_gpm as:
#     (line_length_ft / emitter_spacing_ft) * num_lines * emitter_gph / 60
#
# A zone can optionally have a SECOND head type on the same valve --
# { 'type2': '<key from HEAD_TYPES>', 'head_count2': N } -- always a rated
# head (never drip), summed into flow_gpm alongside slot 1.
#
# Zone 9 drip calculation:
#   330 ft / 1.5 ft spacing * 6 lines * 0.5 GPH / 60 = 11 GPM (drip, slot 1)
# Zone 9 parallel zone 8:
#   5 heads * 3 GPM = 15 GPM (pop-up 3 gpm x5, slot 2)
#   Zones 8 and 9 run in parallel — their flows are combined into zone 9 total.
#   Zone 8 SVG area is included in zone 9 aggregation via zone_mappings.txt.
#   Combined flow: 11 (drip) + 15 (popup3 x5) = 26 GPM total for the zone pair.
# ---------------------------------------------------------------------------
ZONE_HEAD_ASSIGNMENTS = {
    # Field zones
    ('field',  1): {'type': 'biggun',  'head_count': 1},   # North Big Gun
    ('field',  2): {'type': 'rotor5',  'head_count': 1},   # Block Wall
    ('field',  3): {'type': 'rotor5',  'head_count': 1},   # East Fence
    ('field',  4): {'type': 'rotor5',  'head_count': 1},   # East Center
    ('field',  5): {'type': 'rotor5',  'head_count': 1},   # East Daylillies
    ('field',  6): {'type': 'rotor5',  'head_count': 1},   # Center
    ('field',  7): {'type': 'rotor5',  'head_count': 1},   # North of Shed
    ('field',  9): {'type': 'drip', 'total_gpm': 11.0,      # Vineyard drip (slot 1)
        'type2': 'popup3', 'head_count2': 5,                # + zone 8 parallel heads (slot 2)
        'notes': 'Drip: 330ft x 6 lines / 1.5ft spacing x 0.5GPH = 11GPM + zone 8 parallel (5 heads x 3GPM = 15GPM)'},
    ('field', 10): {'type': 'biggun',  'head_count': 1},   # South Big Gun
    ('field', 11): {'type': 'rotor5',  'head_count': 1},   # West Center
    ('field', 12): {'type': 'rotor5',  'head_count': 1},   # North Fence
    # Yard zones
    ('yard',   1): {'type': 'rotor5',  'head_count': 1},   # Garage
    ('yard',   2): {'type': 'rotor5',  'head_count': 1},   # South Yard
    ('yard',   3): {'type': 'rotor5',  'head_count': 1},   # North Yard
    ('yard',   4): {'type': 'rotor5',  'head_count': 1},   # West Yard
    ('yard',   5): {'type': 'popup2',  'head_count': 1},   # Around Pool
    ('yard',   7): {'type': 'rotor5',  'head_count': 1},   # West Fence
    ('yard',   8): {'type': 'popup2',  'head_count': 1},   # South Fence
    ('yard',  10): {'type': 'popup2',  'head_count': 1},   # Block Wall
    ('yard',  11): {'type': 'popup2',  'head_count': 1},   # Patio
}

# =============================================================================
# SVG GEOMETRY HELPERS
# =============================================================================

QS_NS = 'http://qcad.org/namespaces/svg'

def shoelace(points):
    n = len(points)
    if n < 3:
        return 0.0
    area = sum(
        points[i][0] * points[(i + 1) % n][1] -
        points[(i + 1) % n][0] * points[i][1]
        for i in range(n)
    )
    return abs(area) / 2.0

def path_area(d_str, samples=400):
    try:
        path = parse_path(d_str)
        pts = [(path.point(i / samples).real, path.point(i / samples).imag)
               for i in range(samples)]
        return shoelace(pts)
    except Exception:
        return 0.0

def polygon_area(points_str):
    nums = list(map(float, re.findall(r'[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?', points_str)))
    return shoelace(list(zip(nums[::2], nums[1::2])))

def element_area(el):
    tag = el.tag.split('}')[-1]
    if tag == 'path':
        return path_area(el.get('d', ''))
    if tag == 'polygon':
        return polygon_area(el.get('points', ''))
    if tag == 'rect':
        return float(el.get('width', 0)) * float(el.get('height', 0))
    if tag == 'circle':
        return math.pi * float(el.get('r', 0)) ** 2
    return 0.0

# =============================================================================
# SCALE FROM PROPERTY BOUNDARY
# =============================================================================

def get_scale(svg_root):
    xs, ys = [], []
    for el in svg_root.iter():
        if el.get(f'{{{QS_NS}}}layer', '') != BOUNDARY_LAYER:
            continue
        tag = el.tag.split('}')[-1]
        if tag == 'line':
            xs += [float(el.get('x1', 0)), float(el.get('x2', 0))]
            ys += [float(el.get('y1', 0)), float(el.get('y2', 0))]
        elif tag == 'path':
            try:
                path = parse_path(el.get('d', ''))
                for i in range(50):
                    pt = path.point(i / 50)
                    xs.append(pt.real); ys.append(pt.imag)
            except Exception:
                pass
    if not xs:
        print(f"WARNING: no elements on layer '{BOUNDARY_LAYER}' — using unit scale")
        return 1.0, 1.0
    return REAL_WIDTH_FT / (max(xs) - min(xs)), REAL_HEIGHT_FT / (max(ys) - min(ys))

# =============================================================================
# PARSE zone_mappings.txt
# =============================================================================

def parse_mappings(path):
    mappings = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '-' not in line:
                continue
            parts = [p.strip() for p in line.split('-')]
            if len(parts) < 3:
                continue
            layer = parts[0].lower()
            if not layer.startswith(('yd_cov', 'f_cov')):
                continue
            ctrl = next((kw for kw in ('yard', 'field') if kw in parts[1].lower()), None)
            zone_num = next(
                (int(m.group(1)) for part in parts[1:]
                 for m in [re.search(r'\b(\d+)\b', part)] if m),
                None
            )
            if ctrl and zone_num is not None:
                mappings.append({
                    'layer': layer,
                    'controller': ctrl,
                    'zone_number': zone_num,
                    'description': ' - '.join(parts[2:])
                })
    return mappings

# =============================================================================
# LOAD avgpsi FROM controllers.json
# =============================================================================

def _iter_controllers_zones(path):
    """Yields (controller_id_lower, zone_dict) across a controllers.json
    export's nested controllers[].programs[].zones[]. controllers.json is a
    bare top-level array (no wrapping object) since the site.json/
    controllers.json split -- unlike the old combined zone_data.json, there's
    no "controllers" key to unwrap here."""
    with open(path) as f:
        doc = json.load(f)
    for controller in doc:
        ctrl = str(controller.get('id', '')).lower()
        if ctrl == 'off':
            continue
        for program in controller.get('programs', []):
            for zone in program.get('zones', []):
                yield ctrl, zone

def load_psi_map(path):
    psi_map = {}
    try:
        for ctrl, zone in _iter_controllers_zones(path):
            try:
                psi_map[(ctrl, int(zone['znumber']))] = float(zone['avgpsi'])
            except (KeyError, ValueError):
                pass
    except FileNotFoundError:
        print(f"WARNING: {path} not found — PSI values will be 0")
    return psi_map

def load_zone_names(path):
    name_map = {}
    try:
        for ctrl, zone in _iter_controllers_zones(path):
            try:
                name_map[(ctrl, int(zone['znumber']))] = str(zone.get('zname', ''))
            except (KeyError, ValueError):
                pass
    except FileNotFoundError:
        pass  # already warned in load_psi_map
    return name_map

# =============================================================================
# FLOW AND mm/min CALCULATIONS
# =============================================================================

def calc_slot_flow(head_key, count, actual_psi):
    """Flow for one rated-head slot (never drip) -- shared by slot 1's
    sprinkler case and slot 2, which is always a rated head."""
    head = HEAD_TYPES.get(head_key)
    if not head or actual_psi <= 0:
        return None
    return round(head['rated_gpm'] * count * math.sqrt(actual_psi / head['rated_psi']), 3)

def calc_flow(controller, zone_number, actual_psi):
    """
    Returns a dict matching calibration.json's per-zone schema fields:
    flow_gpm (slot 1 + slot 2 summed), head_type/head_count/rated_gpm/
    rated_psi/total_gpm_override (slot 1 -- the only slot that can be drip),
    head_type_2/head_count_2 (an optional second, always-rated head type on
    the same zone -- e.g. a drip line plus a handful of pop-ups on the same
    valve; their flows are summed since they water the same physical area
    at the same time, unlike different zones, which water different areas).
    """
    key = (controller, zone_number)
    assignment = ZONE_HEAD_ASSIGNMENTS.get(key)
    if not assignment:
        return {
            'flow_gpm': None, 'head_type': 'custom', 'head_count': None, 'is_drip': False,
            'rated_gpm': None, 'rated_psi': None, 'total_gpm_override': None,
            'head_type_2': None, 'head_count_2': None,
            'notes': 'Unknown -- update ZONE_HEAD_ASSIGNMENTS',
        }

    notes = assignment.get('notes', '')
    type2 = assignment.get('type2')
    count2 = assignment.get('head_count2', 1) if type2 else None
    flow2 = calc_slot_flow(type2, count2, actual_psi) if type2 else None

    if assignment['type'] == 'drip':
        gpm1 = round(assignment['total_gpm'], 3)
        total = round(gpm1 + (flow2 or 0), 3)
        return {
            'flow_gpm': total, 'head_type': 'drip', 'head_count': None, 'is_drip': True,
            'rated_gpm': None, 'rated_psi': None, 'total_gpm_override': gpm1,
            'head_type_2': type2, 'head_count_2': count2,
            'notes': notes,
        }

    head  = HEAD_TYPES[assignment['type']]
    count = assignment.get('head_count', 1)
    flow1 = calc_slot_flow(assignment['type'], count, actual_psi)
    total = None if flow1 is None else round(flow1 + (flow2 or 0), 3)
    return {
        'flow_gpm': total, 'head_type': assignment['type'], 'head_count': count, 'is_drip': False,
        'rated_gpm': head['rated_gpm'], 'rated_psi': head['rated_psi'], 'total_gpm_override': None,
        'head_type_2': type2, 'head_count_2': count2,
        'notes': notes,
    }

def calc_mm_per_min(flow_gpm, area_ft2):
    if not flow_gpm or not area_ft2:
        return None
    # gal/min * 231 in³/gal / (area_ft² * 144 in²/ft²) * 25.4 mm/in
    # = flow_gpm * 40.7431 / area_ft2
    return round(flow_gpm * 40.7431 / area_ft2, 5)

# =============================================================================
# MAIN
# =============================================================================

def main():
    print(f"Loading SVG: {SVG_FILE}")
    tree = etree.parse(str(SVG_FILE))
    root = tree.getroot()

    print(f"Establishing scale from '{BOUNDARY_LAYER}'...")
    scale_x, scale_y = get_scale(root)
    scale_area = scale_x * scale_y
    print(f"  Scale: {scale_x:.4f} x {scale_y:.4f} ft/unit  "
          f"({abs(scale_x - scale_y) / scale_x * 100:.1f}% aspect skew)")

    print(f"\nParsing zone mappings: {MAPPINGS_FILE}")
    mappings = parse_mappings(MAPPINGS_FILE)
    print(f"  {len(mappings)} mappings found")

    print(f"\nLoading PSI/name data: {CONTROLLERS_FILE}")
    psi_map = load_psi_map(CONTROLLERS_FILE)
    name_map = load_zone_names(CONTROLLERS_FILE)

    print("\nComputing layer areas...")
    layer_area_svg = {}
    layer_element_count = {}
    for el in root.iter():
        layer = el.get(f'{{{QS_NS}}}layer', '').lower()
        if layer.startswith(('yd_cov', 'f_cov')):
            layer_element_count[layer] = layer_element_count.get(layer, 0) + 1
            layer_area_svg[layer] = layer_area_svg.get(layer, 0.0) + element_area(el)

    # A coverage layer with elements but ~zero computed area almost always means
    # it was drawn as unfilled <line> segments (e.g. a rectangle outline) rather
    # than a closed/filled path, polygon, rect, or circle -- element_area() only
    # knows how to measure the latter. Catch that mistake here instead of letting
    # it silently produce a too-small zone area.
    AREA_WARNING_THRESHOLD = 1.0  # svg units^2, pre-scale
    for layer, count in sorted(layer_element_count.items()):
        if layer_area_svg.get(layer, 0.0) < AREA_WARNING_THRESHOLD:
            zones_for_layer = sorted({f"{m['controller']} zone {m['zone_number']}" for m in mappings if m['layer'] == layer})
            zone_note = f" ({', '.join(zones_for_layer)})" if zones_for_layer else ""
            print(f"WARNING: layer '{layer}'{zone_note} has {count} element(s) but computed area ~= 0 ft2 -- "
                  f"likely drawn as unfilled <line> segments instead of a closed/filled shape.")

    # Aggregate layers -> zones
    zone_layers   = {}
    zone_area_ft2 = {}
    for m in mappings:
        key = (m['controller'], m['zone_number'])
        zone_layers.setdefault(key, []).append(m['layer'])
        zone_area_ft2[key] = (zone_area_ft2.get(key, 0.0)
                              + layer_area_svg.get(m['layer'], 0.0) * scale_area)

    # Build result rows -- shape matches calibration.json's documented zone
    # schema directly, so this output can be used as-is to seed the device's
    # calibration.json or re-imported later via the page's Import from Script.
    results = []
    for key in sorted(zone_area_ft2):
        ctrl, znum  = key
        area_ft2    = round(zone_area_ft2[key])
        actual_psi  = psi_map.get(key, 0.0)
        flow = calc_flow(ctrl, znum, actual_psi)
        mm_per_min = calc_mm_per_min(flow['flow_gpm'], area_ft2)
        results.append({
            'controller':         ctrl,
            'zone_number':        znum,
            'zone_name':          name_map.get(key, ''),
            'layers':             zone_layers.get(key, []),
            'area_ft2':           area_ft2,
            'area_acres':         round(area_ft2 / 43560, 3),
            'avgpsi':             actual_psi,
            'head_type':          flow['head_type'],
            'head_count':         flow['head_count'],
            'is_drip':            flow['is_drip'],
            'rated_gpm':          flow['rated_gpm'],
            'rated_psi':          flow['rated_psi'],
            'total_gpm_override': flow['total_gpm_override'],
            'head_type_2':        flow['head_type_2'],
            'head_count_2':       flow['head_count_2'],
            'flow_gpm':           flow['flow_gpm'],
            'mm_per_min':         mm_per_min,
            'notes':              flow['notes'],
        })

    # Write JSON -- wrapped with the generation metadata and head_catalog,
    # matching calib/calibration-page.md section 1's documented schema.
    head_catalog = {}
    for key, h in HEAD_TYPES.items():
        head_catalog[key] = {'label': h['label'], 'rated_gpm': h['rated_gpm'], 'rated_psi': h['rated_psi']}
    head_catalog.update(HEAD_CATALOG_EXTRA)

    output = {
        'generated': datetime.now().strftime('%Y-%m-%dT%H:%M:%S'),
        'svg_file': str(SVG_FILE),
        'scale_x_ft_per_unit': round(scale_x, 4),
        'scale_y_ft_per_unit': round(scale_y, 4),
        'zones': results,
        'head_catalog': head_catalog,
    }
    with open(OUTPUT_JSON, 'w') as f:
        json.dump(output, f, indent=2)
    print(f"\nWrote {OUTPUT_JSON}")

    # Write text table
    W = 130
    lines = []
    lines.append('=' * W)
    lines.append(f"{'Zone Irrigation Calibration':^{W}}")
    lines.append(f"{'SVG: ' + str(SVG_FILE) + '   Scale: ' + f'{scale_x:.2f} x {scale_y:.2f} ft/unit':^{W}}")
    lines.append('=' * W)
    lines.append(f"{'Ctrl':<8} {'Zone':>4}  {'Name':<16}  {'Area ft²':>9}  {'Acres':>6}  "
                 f"{'PSI':>5}  {'Flow GPM':>9}  {'mm/min':>8}  {'Head / Emitter':<22}  Layers")
    lines.append('-' * W)

    for r in results:
        psi_str  = f"{r['avgpsi']:.0f}"  if r['avgpsi']    else '  —'
        flow_str = f"{r['flow_gpm']:.2f}" if r['flow_gpm']  else '   N/A'
        mm_str   = f"{r['mm_per_min']:.5f}" if r['mm_per_min'] else '   N/A'
        drip_tag = ' †' if r['is_drip'] else ''
        catalog_entry = HEAD_TYPES.get(r['head_type']) or HEAD_CATALOG_EXTRA.get(r['head_type'], {'label': r['head_type']})
        head_label = catalog_entry['label'] + (f" x{r['head_count']}" if r['head_count'] else '')
        if r.get('head_type_2'):
            catalog_entry_2 = HEAD_TYPES.get(r['head_type_2'], {'label': r['head_type_2']})
            head_label += f" + {catalog_entry_2['label']} x{r['head_count_2']}"
        lines.append(
            f"{r['controller'].capitalize():<8} {r['zone_number']:>4}  {r['zone_name'][:16]:<16}  "
            f"{r['area_ft2']:>9,.0f}  {r['area_acres']:>6.3f}  "
            f"{psi_str:>5}  {flow_str:>9}  {mm_str:>8}  "
            f"{head_label + drip_tag:<22}  {', '.join(r['layers'])}"
        )

    lines.append('-' * W)
    for ctrl in ('field', 'yard'):
        t = sum(r['area_ft2'] for r in results if r['controller'] == ctrl)
        lines.append(f"  {ctrl.capitalize()} total: {t:>10,.0f} ft²  ({t/43560:.2f} acres)")
    gt = sum(r['area_ft2'] for r in results)
    lines.append(f"  Grand total:  {gt:>10,.0f} ft²  ({gt/43560:.2f} acres)")
    lines.append('=' * W)
    lines += [
        '',
        'Notes:',
        '  Sprinkler flow: flow_gpm = rated_gpm * head_count * sqrt(actual_psi / rated_psi)',
        '  Drip flow (†):  total_gpm specified directly in ZONE_HEAD_ASSIGNMENTS',
        '                  Formula: (line_length_ft / emitter_spacing_ft) * num_lines * emitter_gph / 60',
        '  mm/min:         flow_gpm * 40.74 / area_ft2',
        '  Update head_count or total_gpm in ZONE_HEAD_ASSIGNMENTS and re-run to recalibrate.',
    ]

    table = '\n'.join(lines)
    print('\n' + table)
    with open(OUTPUT_TXT, 'w') as f:
        f.write(table + '\n')
    print(f"\nWrote {OUTPUT_TXT}")

if __name__ == '__main__':
    main()
