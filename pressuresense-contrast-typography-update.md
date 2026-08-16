# PressureSense — Contrast & Typography Update

## Objective
Apply improved text contrast and a larger type scale across the whole site (`style.css` is shared by `index.html`, `config.html`, `calibration.html`, `map.html`, `files.html` — every change below should propagate to all of them automatically since it's one stylesheet, but **visually re-check every page**, not just the dashboard).

A reference mockup with the finished look is attached (`pressuresense-mockup.html`) — open it side-by-side while working. It was built by hand-editing a copy of the real CSS, so class names and structure match the live site exactly. Two exceptions in the mockup that do **not** apply to the real site:
- It replaces `body.ag-page::before`'s photo background (`water_waves.jpg`) with a flat gradient, purely because that image wasn't available in the mockup environment. **Keep the real photo background** — see the "Panel background opacity" section below for how to adapt this.
- It doesn't include `highcharts.js` — the real chart should keep using Highcharts as-is; only its container's typography/contrast context changes, not the charting library.

Do not change: layout, grid structure, spacing/padding values, JS behavior, breakpoints, or anything not called out below.

---

## 1. Color changes

Everything below is a **direct hex swap** — find every selector using the "old" value and replace with "new." Do this for `style.css` in full, not just the selectors I list explicitly (I didn't have visibility into the entire file — anywhere `#8ab0ca`, `#546c81`, or `#60a5fa` appears, including on `config.html`/`calibration.html`/`map.html`-specific rules like `.ag-map-stat span`, `.ag-map-view-btn`, `.ag-help-text`, `.ag-field-label`, etc., apply the matching category below).

| Role | Old color | New color | Where it's used |
|---|---|---|---|
| **Panel titles** (section headers inside a card) | `#8ab0ca` | `#f4b740` (amber) | `.ag-card-label`, `.ag-chart-title` |
| **Field-level labels** (small labels under a value, inside a stat block) | `#8ab0ca` | `#c9a468` (muted amber — same family as panel titles, dialed back) | `.ag-stat-label`; also apply to `.ag-stat-unit`, `.ag-psi-unit` since they're the same micro-label role |
| **Status bar labels** (SYSTEM, LOCATION, LAST READ, etc.) | `#8ab0ca` | `#b6e6de` (bright muted teal) | `.ag-status-item` |
| **General secondary text** (nav links, footer, chart legend, zone meta, firmware version, gauge tick labels, chart filename line) | `#8ab0ca` | `#d6e6f4` (bright blue-white) | `.ag-nav a`, `.ag-footer`, `.ag-legend-item`, `.ag-zone-meta`, `.ag-version`, `.ag-stat-wx-sub`, `.ag-chart-file`, the `0`/`100` tick labels on the gauge SVG, and any other selector currently at `#8ab0ca` not covered by the three categories above (e.g. `.ag-map-stat span`, `.ag-help-text` if it exists) |
| **Muted state text** (e.g. the "READY" idle state) | `#546c81` | `#9eb3c4` | `.ag-manual-state.muted` — this one was an actual contrast bug (3.03:1, fails even AA) |
| **Primary blue accent** (data values, active states) | `#60a5fa` | `#7ec1ff` | `.ag-status-item .val`, `.ag-stat-value`, `.ag-manual-state`, `.ag-nav a:hover:not(.active)` color, `.ag-btn-ghost` color |
| **Success/warn/error accents** | `#22c55e` / `#f59e0b` / `#ef4444` | `#34d873` / `#fbbf24` / `#f87171` | `.ag-stat-value.ok/.warn/.err`, status dot, toast border/text if same base colors are reused |
| **Base body text** | `#d4dce4` | `#e4eaf0` | `body.ag-page` |
| **Brand wordmark / zone name** | `#e8f4fd` | `#f2f7fc` | `.ag-brand`, `.ag-zone-name` |

### Important follow-up: the active-nav-pill gradient
The original active/selected control style — white text on a `linear-gradient(135deg, #0d6efd 0%, #0052cc 100%)` — measures **4.50:1**, which is AA-pass but has essentially zero margin and fails AAA. I already fixed this for `.ag-nav a.active`:

```css
.ag-nav a.active {
  background: linear-gradient(135deg, #0047b3 0%, #003380 100%); /* was #0d6efd → #0052cc */
  color: white;
}
```
New ratio: 8.23:1 (light end) to 11.76:1 (dark end) — passes AAA throughout.

**This same `#0d6efd`→`#0052cc` white-on-gradient pattern is very likely reused elsewhere** (I saw it in `.ag-btn-primary` and it's probably also behind `.ag-range-btn.active`/`:hover`). Apply the same darker gradient (`#0047b3` → `#0033 80`) anywhere white/near-white text sits directly on it, for consistency and to clear AAA everywhere, not just in nav.

---

## 2. Typography scale changes

| Selector | Old size | New size |
|---|---|---|
| `.ag-brand` | 22px | 24px |
| `.ag-nav a` | 12px | 13.5px |
| `.ag-status-bar` (base) | 11px | 12.5px |
| `.ag-card-label` | 10px | 12.5px |
| `.ag-stat-label` | 9px | 11.5px |
| `.ag-stat-value` | 16px | 22px |
| `.ag-zone-name` | 15px | 19px |
| `.ag-zone-meta` | 10px | 13px |
| `.ag-chart-title` | 14px | 17px |
| `.ag-range-btn` | 10px | 12.5px |
| `.ag-chart-legend` | 11px | 13px |
| `.ag-chart-file` | (inherits ~11px) | 13px |
| `.ag-select` | 11px | 13px |
| `.ag-btn` | 12px | 14px |
| `.ag-btn-sm` | 11px | 12.5px |
| `.ag-manual-state` | 10px | 12px |
| `.ag-toast` | 11px | 12.5px |
| `.ag-footer` | 10px | 12px |
| `.ag-version` | 10px | 11px |
| `body.ag-page` (base) | unset (browser default) | 15px explicit |

Apply the same proportional bump (roughly +2–3px, or +15–25%) to any other sub-11px selector you find on `config.html`/`calibration.html`/`map.html`-specific rules that weren't in my sample — e.g. `.ag-map-stat span` (10px), `.ag-map-view-btn` (11px), `.ag-map-loading` (13px is borderline OK, can leave). Anything under ~12px for body copy is the pattern to hunt down and fix.

---

## 3. Panel background opacity (adapt, don't copy verbatim)

The mockup made card/header/footer backgrounds solid because it had no photo behind them. On the real site, keep the photo but **raise the opacity of the translucent panels sitting on top of it** so text contrast doesn't fluctuate depending on what part of the photo is behind a given panel. Suggested new alpha values (same colors, just more opaque):

| Selector | Old | New |
|---|---|---|
| `.ag-header` | `rgba(23,32,43,0.88)` / `rgba(21,31,42,0.84)` | `rgba(23,32,43,0.97)` / `rgba(21,31,42,0.95)` |
| `.ag-status-bar` | `rgba(23,32,43,0.82)` / `rgba(21,31,42,0.78)` | `rgba(23,32,43,0.97)` / `rgba(21,31,42,0.95)` |
| `.ag-footer` | `rgba(23,32,43,0.82)` / `rgba(21,31,42,0.78)` | `rgba(23,32,43,0.97)` / `rgba(21,31,42,0.95)` |
| `.ag-gauge-card`, `.ag-stat`, `.ag-zone-card` | `rgba(23,32,43,0.84)` / `rgba(21,31,42,0.8)` | `rgba(23,32,43,0.95)` / `rgba(21,31,42,0.92)` |
| `.ag-chart-card` | `rgba(23,32,43,0.86)` / `rgba(21,31,42,0.82)` | `rgba(23,32,43,0.96)` / `rgba(21,31,42,0.93)` |

Border color `#2a3f54` can optionally lighten slightly to `#33495f` for a touch more definition against the now-more-opaque panels — not contrast-critical, purely aesthetic, safe to skip if you want minimal diffs.

---

## 4. WCAG validation (required before calling this done)

Run every text/background color pair you touch through a contrast checker (WebAIM's Contrast Checker or the `axe` browser extension is fine) and confirm:

- **Minimum bar: WCAG AA** — 4.5:1 for normal text, 3:1 for text ≥24px regular or ≥18.66px bold.
- **Target: WCAG AAA** — 7:1 for normal text, 4.5:1 for large text — everywhere it's reasonably achievable without breaking the visual design (all the specific hex swaps above were chosen to hit AAA against their actual background; verify after your edits land since real backgrounds — photo + gradient — may shift the effective contrast slightly differently than my flat-color mockup did).
- Check colored text (amber/teal labels, blue values, status colors) against **every background it can appear on** — e.g. `.ag-card-label` sits on `.ag-gauge-card`, `.ag-stat`, and `.ag-zone-card`, which each have a slightly different background gradient.
- Pay particular attention to any hover/active states — those often get missed in a contrast pass because they're not visible by default.

If anything fails AA after your edits, don't ship it — pick a slightly brighter/darker variant of the same hue and re-check. If something fails only AAA, that's acceptable but flag it so it's a known tradeoff rather than an oversight.

---

## 5. Testing checklist

- [ ] `index.html` (dashboard) — all sections above
- [ ] `config.html` — form labels, inputs, help text
- [ ] `calibration.html`
- [ ] `map.html` — `.ag-map-stat`, `.ag-map-tool`, `.ag-map-view-btn`, `.ag-map-loading`
- [ ] `files.html`
- [ ] Mobile breakpoints (≤900px, ≤480px) — font sizes in the media queries were not exhaustively covered above; sanity-check nav/footer/status-bar sizing still looks proportionate at small widths
- [ ] Run a contrast check on every page, not just the ones with the most obvious labels
