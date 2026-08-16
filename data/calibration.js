const BASE_URL = '';

const EMPTY_CALIB_DOC = { generated: '', svg_file: '', scale_x_ft_per_unit: 0, scale_y_ft_per_unit: 0, zones: [], head_catalog: {} };
let calibDoc = { ...EMPTY_CALIB_DOC };
let dirty = false;

// Built-in catalog keys the Python script ships with -- protected from
// deletion in the Head Catalog UI (spec section 5: "built-ins protected").
const BUILTIN_HEAD_KEYS = ['biggun', 'rotor5', 'rotor3', 'popup2', 'popup3', 'drip', 'custom'];

// Must stay byte-identical to zone_calibration.py's calc_mm_per_min constant
// (calib/calibration-page.md section 2) or the two tools will silently
// disagree about the same zone's mm_per_min.
const MM_PER_MIN_FACTOR = 40.7431;

let lastSeenRawPressure = null;

function setDirty(val) {
  dirty = val;
  const el = document.getElementById('unsaved-indicator');
  el.textContent = dirty ? 'YES' : 'NONE';
  el.style.color = dirty ? '#fbbf24' : '#5ba4d9';
}

function showToast(msg, isError = false) {
  const t = document.getElementById('toast');
  document.getElementById('toast-msg').textContent = msg;
  t.style.borderLeftColor = isError ? '#f87171' : '#34d873';
  t.style.color = isError ? '#f87171' : '#34d873';
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 3500);
}

function escapeHtml(str) {
  return String(str == null ? '' : str).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

// ---------- Flow / mm-per-min calculation (spec section 2, JS twin of the Python script) ----------

function isDripOrCustom(headType) {
  return headType === 'drip' || headType === 'custom';
}

// Slot 1 supports drip/custom (total_gpm_override, no rated formula); slot 2
// is always a plain rated head -- a zone needing a second independently-
// overridden total is rare enough that it's not worth duplicating the
// override UI for now. Returns 0 for an empty/unset slot 2.
function calcSlot1FlowGpm(zone, headCatalog) {
  if (isDripOrCustom(zone.head_type)) {
    return zone.total_gpm_override || 0;
  }
  const head = headCatalog[zone.head_type];
  if (!head || !head.rated_gpm || !head.rated_psi || !zone.avgpsi) return 0;
  return head.rated_gpm * (zone.head_count || 1) * Math.sqrt(zone.avgpsi / head.rated_psi);
}

function calcSlot2FlowGpm(zone, headCatalog) {
  if (!zone.head_type_2) return 0;
  const head = headCatalog[zone.head_type_2];
  if (!head || !head.rated_gpm || !head.rated_psi || !zone.avgpsi) return 0;
  return head.rated_gpm * (zone.head_count_2 || 1) * Math.sqrt(zone.avgpsi / head.rated_psi);
}

// A zone can have two head types (e.g. a drip line plus a handful of
// pop-ups on the same valve) -- their flows are summed, never averaged,
// since they're both watering the same single physical area at the same
// time (unlike different zones, which water different areas -- see the
// weather-budget implementation plan for that distinction).
function calcFlowGpm(zone, headCatalog) {
  return calcSlot1FlowGpm(zone, headCatalog) + calcSlot2FlowGpm(zone, headCatalog);
}

function calcMmPerMin(flowGpm, areaFt2) {
  if (!flowGpm || !areaFt2) return null;
  return flowGpm * MM_PER_MIN_FACTOR / areaFt2;
}

function recalcZone(zone) {
  const flow = calcFlowGpm(zone, calibDoc.head_catalog);
  zone.flow_gpm = Math.round(flow * 1000) / 1000;
  const mm = calcMmPerMin(zone.flow_gpm, zone.area_ft2);
  zone.mm_per_min = mm === null ? null : Math.round(mm * 100000) / 100000;
}

function recalcAllZones() {
  calibDoc.zones.forEach(recalcZone);
}

// ---------- Zone calibration table rendering ----------

function ctrlBadgeClass(ctrl) {
  const c = (ctrl || '').toLowerCase();
  if (c === 'yard') return 'ag-ctrl-yard';
  if (c === 'field') return 'ag-ctrl-field';
  return 'ag-ctrl-off';
}

// Yard, then Field, then anything else alphabetically -- matches
// config.html's controller order (controllers.json's own array is
// yard-first) rather than calib/calibration-page.md section 3's
// originally-specified field-first order, per the user's explicit request
// that the two pages match.
function groupZonesByController(zones) {
  const preferredOrder = ['yard', 'field'];
  const groups = {};
  zones.forEach(z => { (groups[z.controller] = groups[z.controller] || []).push(z); });
  const ids = Object.keys(groups).sort((a, b) => {
    const ia = preferredOrder.indexOf(a), ib = preferredOrder.indexOf(b);
    if (ia === -1 && ib === -1) return a.localeCompare(b);
    if (ia === -1) return 1;
    if (ib === -1) return -1;
    return ia - ib;
  });
  return ids.map(id => ({ controller: id, zones: groups[id].slice().sort((a, b) => a.zone_number - b.zone_number) }));
}

function headTypeOptionsHtml(selected) {
  return Object.keys(calibDoc.head_catalog).map(key => {
    const h = calibDoc.head_catalog[key];
    return `<option value="${key}" ${key === selected ? 'selected' : ''}>${escapeHtml(h.label || key)}</option>`;
  }).join('');
}

// Slot 2 never supports drip/custom (no second override input -- see
// calcSlot2FlowGpm), so those two keys are left out of its dropdown; an
// empty "None" option is first since most zones only need one head type.
function headTypeOptionsHtmlSlot2(selected) {
  const options = Object.keys(calibDoc.head_catalog)
    .filter(key => !isDripOrCustom(key))
    .map(key => {
      const h = calibDoc.head_catalog[key];
      return `<option value="${key}" ${key === selected ? 'selected' : ''}>${escapeHtml(h.label || key)}</option>`;
    }).join('');
  return `<option value="" ${!selected ? 'selected' : ''}>—</option>` + options;
}

function renderZoneCalibRow(zone, idx) {
  const dripOrCustom = isDripOrCustom(zone.head_type);
  const head = calibDoc.head_catalog[zone.head_type];
  const head2 = zone.head_type_2 ? calibDoc.head_catalog[zone.head_type_2] : null;

  const countCell = dripOrCustom ? ''
    : `<input type="number" min="1" max="20" class="ag-inline-input calib-head-count-input" value="${zone.head_count || 1}" data-idx="${idx}" style="width:42px;">`;
  const gpmPerHeadCell = dripOrCustom ? '—' : (head && head.rated_gpm != null ? head.rated_gpm : '—');
  const totalGpmCell = dripOrCustom
    ? `<input type="number" step="0.1" min="0" class="ag-inline-input calib-total-gpm-input" value="${zone.total_gpm_override || 0}" data-idx="${idx}" style="width:60px;">`
    : (zone.flow_gpm != null ? zone.flow_gpm.toFixed(2) : '—');
  const dripBadge = zone.is_drip ? '<span class="ag-weather-badge" style="margin-left:6px;">DRIP</span>' : '';

  const count2Cell = zone.head_type_2
    ? `<input type="number" min="1" max="20" class="ag-inline-input calib-head-count-2-input" value="${zone.head_count_2 || 1}" data-idx="${idx}" style="width:42px;">`
    : '';
  const gpmPerHead2Cell = head2 && head2.rated_gpm != null ? head2.rated_gpm : '—';

  return `
    <tr data-idx="${idx}">
      <td class="ag-calib-narrow">${zone.zone_number}</td>
      <td>${escapeHtml(zone.zone_name || '')}</td>
      <td class="ag-calib-readonly">${Number(zone.area_ft2 || 0).toLocaleString()}</td>
      <td class="ag-calib-readonly ag-calib-narrow">${zone.avgpsi || '—'}</td>
      <td><select class="ag-select calib-head-type-select" data-idx="${idx}">${headTypeOptionsHtml(zone.head_type)}</select>${dripBadge}</td>
      <td class="calib-count-cell">${countCell}</td>
      <td class="calib-gpmhead-cell">${gpmPerHeadCell}</td>
      <td><select class="ag-select calib-head-type-2-select" data-idx="${idx}">${headTypeOptionsHtmlSlot2(zone.head_type_2)}</select></td>
      <td class="calib-count-2-cell">${count2Cell}</td>
      <td class="calib-gpmhead-2-cell">${gpmPerHead2Cell}</td>
      <td class="calib-totalgpm-cell">${totalGpmCell}</td>
      <td class="ag-calib-output ag-calib-narrow">${zone.mm_per_min != null ? zone.mm_per_min.toPrecision(3) : '—'}</td>
    </tr>`;
}

function renderControllerCalibTable(controllerId, zones) {
  const rows = zones.map(z => renderZoneCalibRow(z, calibDoc.zones.indexOf(z))).join('');
  return `
    <div class="ag-controller-section ${ctrlBadgeClass(controllerId)}" data-controller="${controllerId}">
      <div class="ag-controller-header ${ctrlBadgeClass(controllerId)}">${escapeHtml(controllerId)} controller</div>
      <div class="ag-calib-table-scroll">
        <table class="ag-zone-table" data-controller="${controllerId}">
          <thead>
            <tr>
              <th class="ag-calib-narrow">#</th><th>Name</th><th style="width:80px;">Area ft&sup2;</th><th class="ag-calib-narrow">PSI</th><th>Head Type</th>
              <th style="width:50px;">Count</th><th style="width:60px;">GPM/head</th>
              <th>Head Type 2</th><th style="width:50px;">Count 2</th><th style="width:60px;">GPM/head 2</th>
              <th style="width:65px;">Total GPM</th><th style="width:60px;">mm/min</th>
            </tr>
          </thead>
          <tbody>${rows}</tbody>
        </table>
      </div>
    </div>`;
}

function renderCalibrationPage() {
  const container = document.getElementById('calib-controllers');
  const groups = groupZonesByController(calibDoc.zones);
  container.innerHTML = groups.map(g => renderControllerCalibTable(g.controller, g.zones)).join('');
  document.getElementById('calib-zone-count').textContent = calibDoc.zones.length;
  document.getElementById('calib-generated').textContent = calibDoc.generated || '—';
  document.getElementById('calib-svg-file').textContent = calibDoc.svg_file || '—';
  document.getElementById('calib-scale').textContent = (calibDoc.scale_x_ft_per_unit && calibDoc.scale_y_ft_per_unit)
    ? `${calibDoc.scale_x_ft_per_unit} x ${calibDoc.scale_y_ft_per_unit} ft/unit` : '—';
}

// Targeted update for the one row whose count/total-gpm just changed --
// avoids a full re-render (and the input-focus loss that comes with it)
// while the user is actively typing.
function updateRowOutputs(idx) {
  const row = document.querySelector(`#calib-controllers tr[data-idx="${idx}"]`);
  if (!row) return;
  const zone = calibDoc.zones[idx];
  const mmCell = row.querySelector('.ag-calib-output');
  if (mmCell) mmCell.textContent = zone.mm_per_min != null ? zone.mm_per_min.toPrecision(3) : '—';
  if (!isDripOrCustom(zone.head_type)) {
    const totalGpmCell = row.querySelector('.calib-totalgpm-cell');
    if (totalGpmCell) totalGpmCell.textContent = zone.flow_gpm != null ? zone.flow_gpm.toFixed(2) : '—';
  }
}

// ---------- Head Catalog rendering ----------

function renderHeadCatalog() {
  const body = document.getElementById('calib-head-catalog-body');
  body.innerHTML = Object.keys(calibDoc.head_catalog).map(key => {
    const h = calibDoc.head_catalog[key];
    const builtin = BUILTIN_HEAD_KEYS.includes(key);
    const noRating = (key === 'drip' || key === 'custom');
    return `
      <tr data-key="${escapeHtml(key)}">
        <td>${escapeHtml(key)}</td>
        <td><input type="text" class="ag-inline-input catalog-label-input" data-key="${escapeHtml(key)}" value="${escapeHtml(h.label || '')}"></td>
        <td><input type="number" step="0.1" class="ag-inline-input catalog-gpm-input" data-key="${escapeHtml(key)}" value="${h.rated_gpm != null ? h.rated_gpm : ''}" ${noRating ? 'disabled' : ''} style="width:70px;"></td>
        <td><input type="number" step="0.1" class="ag-inline-input catalog-psi-input" data-key="${escapeHtml(key)}" value="${h.rated_psi != null ? h.rated_psi : ''}" ${noRating ? 'disabled' : ''} style="width:70px;"></td>
        <td>${builtin ? '' : `<button class="ag-btn ag-btn-danger ag-btn-sm catalog-delete-btn" data-key="${escapeHtml(key)}">&#10005;</button>`}</td>
      </tr>`;
  }).join('');
}

function addHeadType() {
  let n = 1;
  while (calibDoc.head_catalog['custom_' + n]) n++;
  const key = 'custom_' + n;
  calibDoc.head_catalog[key] = { label: 'New Head', rated_gpm: 5.0, rated_psi: 45.0 };
  setDirty(true);
  renderHeadCatalog();
}

// ---------- Delegated event handlers ----------

function onCalibContainerInput(e) {
  const countInput = e.target.closest('.calib-head-count-input');
  if (countInput) {
    const idx = Number(countInput.dataset.idx);
    calibDoc.zones[idx].head_count = Number(countInput.value) || 1;
    recalcZone(calibDoc.zones[idx]);
    setDirty(true);
    updateRowOutputs(idx);
    return;
  }
  const totalGpmInput = e.target.closest('.calib-total-gpm-input');
  if (totalGpmInput) {
    const idx = Number(totalGpmInput.dataset.idx);
    calibDoc.zones[idx].total_gpm_override = Number(totalGpmInput.value) || 0;
    recalcZone(calibDoc.zones[idx]);
    setDirty(true);
    updateRowOutputs(idx);
    return;
  }
  const count2Input = e.target.closest('.calib-head-count-2-input');
  if (count2Input) {
    const idx = Number(count2Input.dataset.idx);
    calibDoc.zones[idx].head_count_2 = Number(count2Input.value) || 1;
    recalcZone(calibDoc.zones[idx]);
    setDirty(true);
    updateRowOutputs(idx);
  }
}

function onCalibContainerChange(e) {
  const headSelect = e.target.closest('.calib-head-type-select');
  if (headSelect) {
    const idx = Number(headSelect.dataset.idx);
    const zone = calibDoc.zones[idx];
    zone.head_type = headSelect.value;
    zone.is_drip = zone.head_type === 'drip';
    if (isDripOrCustom(zone.head_type)) {
      zone.head_count = null;
    } else if (!zone.head_count) {
      zone.head_count = 1;
    }
    recalcZone(zone);
    setDirty(true);
    renderCalibrationPage();	// column visibility changes -- needs a full re-render
    return;
  }
  const headSelect2 = e.target.closest('.calib-head-type-2-select');
  if (headSelect2) {
    const idx = Number(headSelect2.dataset.idx);
    const zone = calibDoc.zones[idx];
    zone.head_type_2 = headSelect2.value || null;
    zone.head_count_2 = zone.head_type_2 ? (zone.head_count_2 || 1) : null;
    recalcZone(zone);
    setDirty(true);
    renderCalibrationPage();	// column visibility (Count 2) changes -- needs a full re-render
  }
}

function onCatalogInput(e) {
  const labelInput = e.target.closest('.catalog-label-input');
  if (labelInput) {
    calibDoc.head_catalog[labelInput.dataset.key].label = labelInput.value;
    setDirty(true);
    return;
  }
  const gpmInput = e.target.closest('.catalog-gpm-input');
  if (gpmInput) {
    calibDoc.head_catalog[gpmInput.dataset.key].rated_gpm = Number(gpmInput.value) || null;
    recalcAllZones();
    setDirty(true);
    renderCalibrationPage();
    return;
  }
  const psiInput = e.target.closest('.catalog-psi-input');
  if (psiInput) {
    calibDoc.head_catalog[psiInput.dataset.key].rated_psi = Number(psiInput.value) || null;
    recalcAllZones();
    setDirty(true);
    renderCalibrationPage();
  }
}

function onCatalogClick(e) {
  const delBtn = e.target.closest('.catalog-delete-btn');
  if (!delBtn) return;
  const key = delBtn.dataset.key;
  if (!confirm(`Delete head type "${key}"? Zones currently using it will need a new type.`)) return;
  delete calibDoc.head_catalog[key];
  setDirty(true);
  renderHeadCatalog();
  renderCalibrationPage();
}

function attachCalibHandlers() {
  const container = document.getElementById('calib-controllers');
  container.addEventListener('input', onCalibContainerInput);
  container.addEventListener('change', onCalibContainerChange);

  const catalogBody = document.getElementById('calib-head-catalog-body');
  catalogBody.addEventListener('input', onCatalogInput);
  catalogBody.addEventListener('click', onCatalogClick);
}

// ---------- Load / Save / Import ----------

async function loadCalibrationData() {
  try {
    const res = await fetch(BASE_URL + '/calibration.json?ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) throw new Error(await res.text());
    const data = await res.json();
    calibDoc = (data && typeof data === 'object' && Array.isArray(data.zones)) ? data : { ...EMPTY_CALIB_DOC, zones: [] };
    if (!calibDoc.head_catalog) calibDoc.head_catalog = {};
    recalcAllZones();
    renderCalibrationPage();
    renderHeadCatalog();
    setDirty(false);
  } catch (e) {
    console.error('loadCalibrationData:', e);
    showToast('Error loading calibration data.', true);
  }
}

async function saveCalibration() {
  recalcAllZones();	// always self-consistent on save -- spec section 8
  calibDoc.generated = new Date().toISOString().slice(0, 19);
  try {
    const res = await fetch(BASE_URL + '/submit-calibration-form?ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(calibDoc)
    });
    if (!res.ok) throw new Error(await res.text());
    await res.text();

    const verifyRes = await fetch(BASE_URL + '/calibration.json?ts=' + Date.now(), { cache: 'no-store' });
    if (!verifyRes.ok) throw new Error('Save verification failed: ' + await verifyRes.text());
    const saved = await verifyRes.json();
    if (JSON.stringify(saved) !== JSON.stringify(calibDoc)) {
      throw new Error('Save verification failed: saved file does not match the edited document.');
    }

    renderCalibrationPage();
    setDirty(false);
    showToast('Calibration saved and verified.');
  } catch (e) {
    console.error('saveCalibration:', e);
    showToast('Failed to save calibration data.', true);
  }
}

// Merges a fresh Python-script export into the live document: area/PSI/name/
// layers (script-owned) are overwritten; head_type/head_count/
// total_gpm_override/notes (user-owned) are preserved for zones that
// already exist here. New zones come in as-is. Spec section 6.
function importFromScript(file) {
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const imported = JSON.parse(reader.result);
      const importedZones = Array.isArray(imported.zones) ? imported.zones
        : (Array.isArray(imported) ? imported : null);
      if (!importedZones) throw new Error('Unrecognized file shape -- expected a calibration export with a "zones" array.');

      let updated = 0, added = 0;
      importedZones.forEach(iz => {
        const existing = calibDoc.zones.find(z => z.controller === iz.controller && z.zone_number === iz.zone_number);
        if (existing) {
          existing.area_ft2 = iz.area_ft2;
          existing.area_acres = iz.area_acres;
          existing.avgpsi = iz.avgpsi;
          existing.zone_name = iz.zone_name || existing.zone_name;
          existing.layers = iz.layers || existing.layers;
          updated++;
        } else {
          calibDoc.zones.push({ ...iz });
          added++;
        }
      });

      if (imported.head_catalog) {
        Object.keys(imported.head_catalog).forEach(key => {
          if (!calibDoc.head_catalog[key]) calibDoc.head_catalog[key] = imported.head_catalog[key];
        });
      }
      if (imported.svg_file) calibDoc.svg_file = imported.svg_file;
      if (imported.scale_x_ft_per_unit) calibDoc.scale_x_ft_per_unit = imported.scale_x_ft_per_unit;
      if (imported.scale_y_ft_per_unit) calibDoc.scale_y_ft_per_unit = imported.scale_y_ft_per_unit;

      recalcAllZones();
      setDirty(true);
      renderCalibrationPage();
      renderHeadCatalog();
      showToast(`Imported: ${updated} zone(s) updated, ${added} new.`);
    } catch (e) {
      console.error('importFromScript:', e);
      showToast('Import failed: ' + e.message, true);
    }
  };
  reader.readAsText(file);
}

// ---------- PSI Calibration (moved from config.html -- it's a calibration) ----------
//
// site.json's psi_offset lives alongside the schedule/site config, but this
// page never loads the schedule at all -- saves go through a small
// dedicated endpoint (/submit-calib-offset) that only ever touches that one
// field, never controllers.json or the rest of site.json. The offset
// itself is still computed client-side from the live raw reading, exactly
// as it was on the CONFIG page.

async function loadPsiOffset() {
  try {
    const res = await fetch(BASE_URL + '/load-zone-table?ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) return;
    const doc = await res.json();
    const offset = doc && doc.site && doc.site.psi_offset;
    if (offset !== undefined && offset !== null) {
      document.getElementById('calib-offset-val').textContent = Number(offset).toFixed(1);
      document.getElementById('calib-offset').classList.remove('hidden');
    }
  } catch (e) { console.error('loadPsiOffset:', e); }
}

function updateCalibrationRaw(reading) {
  const raw = Number(reading && reading['Raw Pressure']);
  if (!Number.isFinite(raw)) return;
  lastSeenRawPressure = raw;
  document.getElementById('calib-raw-val').textContent = raw.toFixed(1);
  document.getElementById('calib-offset').classList.remove('hidden');
}

async function submitPsiCalibration() {
  const val = document.getElementById('calib-input').value;
  if (!val) { alert('Enter the actual PSI value.'); return; }
  if (lastSeenRawPressure === null) {
    showToast('No live pressure reading yet — wait a moment and try again.', true);
    return;
  }

  const actualPsi = Number(val);
  const newOffset = actualPsi === 0 ? 0 : (lastSeenRawPressure - actualPsi);
  try {
    const res = await fetch(BASE_URL + '/submit-calib-offset', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ psi_offset: newOffset })
    });
    if (!res.ok) throw new Error(await res.text());
    document.getElementById('calib-offset-val').textContent = newOffset.toFixed(1);
    document.getElementById('calib-offset').classList.remove('hidden');
    showToast('Calibration submitted.');
  } catch (e) {
    console.error('submitPsiCalibration:', e);
    showToast('Failed to save calibration.', true);
  }
}

// ---------- Footer status ----------

function formatUptime(totalSeconds) {
  const seconds = Math.max(0, Number(totalSeconds) || 0);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return days + 'd ' + hours + 'h';
  if (hours > 0) return hours + 'h ' + minutes + 'm';
  return minutes + 'm';
}

async function loadFooterStatus() {
  try {
    const res = await fetch(BASE_URL + '/sd-usage');
    if (!res.ok) return;
    const usage = await res.json();
    const sd = document.getElementById('footer-sd');
    const wifi = document.getElementById('footer-wifi');
    const uptime = document.getElementById('footer-uptime');
    if (sd) sd.textContent = Number(usage.percent || 0).toFixed(1) + '% full';
    if (wifi) wifi.textContent = usage.wifi || '—';
    if (uptime) uptime.textContent = formatUptime(usage.uptimeSec);
    updateFooterRemote(usage);
  } catch (e) {}
}

// Remote-unit (Indoor/Relay, via /ws) link health -- shows how many WS
// clients are connected and how long since the last periodic broadcast, so
// a stalled async_tcp task (see /get-data-file's SD-lock fix) is visible
// from the browser instead of silently freezing the other units.
function updateFooterRemote(usage) {
  const remote = document.getElementById('footer-remote');
  if (!remote) return;
  const clients = Number(usage.wsClients || 0);
  const agoSec = usage.wsAgoSec;
  // Browser tabs' own /events (SSE) connection count -- a number that only
  // ever grows across a long session (instead of sitting at the number of
  // tabs actually open) is the tell for the stale-connection leak that
  // eventually exhausts CONFIG_LWIP_MAX_ACTIVE_TCP and hangs new page loads.
  const sseSuffix = (usage.sseClients !== undefined) ? (' · ' + usage.sseClients + ' live') : '';
  if (usage.wifi !== 'ONLINE') {
    remote.textContent = 'offline';
    remote.style.color = '#f87171';
  } else if (clients === 0) {
    remote.textContent = '0 units' + sseSuffix;
    remote.style.color = '#94a3b8';
  } else if (agoSec === -1 || agoSec === undefined) {
    remote.textContent = clients + ' unit(s), starting…' + sseSuffix;
    remote.style.color = '#34d873';
  } else if (agoSec > 60) {
    remote.textContent = clients + ' unit(s), ' + agoSec + 's ago' + sseSuffix;
    remote.style.color = '#f87171';
  } else {
    remote.textContent = clients + ' unit(s), ' + agoSec + 's ago' + sseSuffix;
    remote.style.color = '#34d873';
  }
}

// Closed on 'pagehide' so a full-page navigation away doesn't leave a stale
// SSE connection open on the ESP32 -- see /sd-usage's sdCardLock fix for why
// server-side resource buildup during rapid page switching matters here.
let sseSource = null;
function stopSSE() {
  if (sseSource) { sseSource.close(); sseSource = null; }
}
function startSSE() {
  if (!window.EventSource || sseSource) return;
  sseSource = new EventSource(BASE_URL + '/events');
  sseSource.addEventListener('new-readings', e => {
    try { updateCalibrationRaw(JSON.parse(e.data)); } catch (err) {}
  });
}

document.addEventListener('DOMContentLoaded', async () => {
  attachCalibHandlers();
  await loadCalibrationData();
  await loadPsiOffset();
  await loadFooterStatus();

  document.getElementById('calib-reload-btn').addEventListener('click', loadCalibrationData);
  document.getElementById('calib-save-btn').addEventListener('click', saveCalibration);
  document.getElementById('calib-add-head-btn').addEventListener('click', addHeadType);
  document.getElementById('calib-import-btn').addEventListener('click', () => document.getElementById('calib-import-file').click());
  document.getElementById('calib-import-file').addEventListener('change', (e) => {
    if (e.target.files[0]) importFromScript(e.target.files[0]);
    e.target.value = '';
  });
  document.getElementById('calib-psi-submit').addEventListener('click', submitPsiCalibration);

  startSSE();
  window.addEventListener('pagehide', stopSSE);
  // A sleep/wake cycle can leave sseSource stuck at readyState OPEN on a
  // dead connection (see net-utils.js) -- force-recreate it and refresh the
  // data that would otherwise sit stale until the next manual reload.
  watchForWake(() => { stopSSE(); startSSE(); loadFooterStatus(); });
});
