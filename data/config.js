const BASE_URL = '';

const DEFAULT_SITE = { name: '', sensor_interval_sec: 30, psi_offset: 0.0, latitude: 0, longitude: 0, timezone: '', mm_per_min_default: 0.25 };
const DEFAULT_WEATHER_SETTINGS = { enabled: false, auto_adjust: false, reference_deficit_mm: 6.0, max_deficit_mm: 25.0, max_adjust_pct: 150, min_adjust_pct: 0, rain_skip_threshold_mm: 6.0 };

let zoneDoc = { site: { ...DEFAULT_SITE }, weather: { ...DEFAULT_WEATHER_SETTINGS }, controllers: [] };
// Firmware-owned runtime state from /weather-state -- never merged into
// zoneDoc, never part of the /submit-zone-form payload (decision #1).
let weatherState = { programs: [] };
// Keyed by controller id -- Yard and Field each get their own Chart.js
// instance/data cache, since each now tracks its own deficit (see
// implementation plan: different controllers water physically separate
// areas, so their numbers are never blended into one chart).
let weatherDeficitCharts = {};
let weatherChartDataCaches = {};
// Per-zone irrigation calibration from /calibration.json (calibration.html
// owns it) -- never merged into zoneDoc, same separation as weatherState.
let calibrationData = { zones: [] };
let dirty = false;
let popoverState = null; // { controllerId, isNew, draft }
let dragState = null;    // { controllerId, programId, fromIdx }

function setDirty(val) {
  dirty = val;
  const el = document.getElementById('unsaved-indicator');
  el.textContent = dirty ? 'YES' : 'NONE';
  el.style.color = dirty ? '#f59e0b' : '#5ba4d9';
}

function showToast(msg, isError = false) {
  const t = document.getElementById('toast');
  document.getElementById('toast-msg').textContent = msg;
  t.style.borderLeftColor = isError ? '#ef4444' : '#22c55e';
  t.style.color = isError ? '#ef4444' : '#22c55e';
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 3500);
}

function setCurrentZoneFile(fileName) {
  const el = document.getElementById('current-zone-file');
  if (el) el.textContent = fileName || '—';
}

function getSaveZoneFilename() {
  return (document.getElementById('save-zone-filename')?.value || 'controllers.json').trim();
}

function setSaveZoneFilename(fileName) {
  const el = document.getElementById('save-zone-filename');
  if (el) el.value = fileName || 'controllers.json';
}

function canonicalJson(value) {
  return JSON.stringify(value);
}

function escapeHtml(str) {
  return String(str == null ? '' : str).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function psiBarColor(psi) {
  const v = Number(psi);
  if (v >= 36) return '#0d6efd';
  if (v >= 25) return '#f59e0b';
  return '#ef4444';
}

function ctrlBadgeClass(ctrl) {
  const c = (ctrl || '').toLowerCase();
  if (c === 'yard') return 'ag-ctrl-yard';
  if (c === 'field') return 'ag-ctrl-field';
  return 'ag-ctrl-off';
}

function countAllZones(doc) {
  return (doc.controllers || []).reduce((sum, c) =>
    sum + (c.programs || []).reduce((s2, p) => s2 + p.zones.length, 0), 0);
}

// ---------- Program popover ----------

function findController(controllerId) {
  return zoneDoc.controllers.find(c => c.id === controllerId);
}

function findProgram(controllerId, programId) {
  const controller = findController(controllerId);
  return controller ? controller.programs.find(p => p.id === programId) : null;
}

function computePopoverOverlapWarning(controllerId, draft) {
  const controller = findController(controllerId);
  const existingProgram = !popoverState.isNew ? findProgram(controllerId, draft.id) : null;
  const draftZones = existingProgram ? existingProgram.zones : [];
  const draftInterval = getProgramInterval({ start: draft.start, seasonal_adjust_pct: draft.seasonal_adjust_pct, zones: draftZones });

  const programs = controller.programs
    .filter(p => p.id !== draft.id)
    .map(p => {
      const iv = getProgramInterval(p);
      return { id: p.id, start: iv.start, end: iv.end, days: p.days };
    });
  programs.push({ id: draft.id, start: draftInterval.start, end: draftInterval.end, days: draft.days });

  const overlaps = findOverlaps(programs).filter(pair => pair.includes(draft.id));
  if (!overlaps.length) return '';
  const others = overlaps.map(pair => pair.find(id => id !== draft.id));
  return `<span class="ag-overlap-warning">&#9888; Overlaps Program ${others.join(', ')}</span>`;
}

function renderProgramPopoverHtml(controllerId, draft, isNew) {
  const dayToggles = DAY_PIP_LABELS.map((lbl, i) => {
    const on = draft.days.includes(i) ? ' on' : '';
    return `<button type="button" class="ag-day-toggle${on}" data-day="${i}">${lbl}</button>`;
  }).join('');

  let programIdField;
  if (isNew) {
    const used = new Set(findController(controllerId).programs.map(p => p.id));
    const options = ['A', 'B', 'C', 'D'].map(id =>
      `<option value="${id}" ${id === draft.id ? 'selected' : ''} ${used.has(id) ? 'disabled' : ''}>${id}${used.has(id) ? ' (used)' : ''}</option>`
    ).join('');
    programIdField = `<select class="ag-select popover-program-id">${options}</select>`;
  } else {
    programIdField = `<span class="ag-program-badge">${draft.id}</span>`;
  }

  const seasonalOptions = [-40, -20, -10, 0, 10, 20, 40].map(v =>
    `<option value="${v}" ${v === draft.seasonal_adjust_pct ? 'selected' : ''}>${v > 0 ? '+' : ''}${v}%</option>`
  ).join('');

  return `
    <div class="ag-program-popover">
      <div class="ag-field-row"><span class="ag-field-label">Program</span>${programIdField}</div>
      <div class="ag-field-row"><span class="ag-field-label">Start time</span><input type="time" class="ag-input popover-start" value="${draft.start}"></div>
      <div class="ag-field-row"><span class="ag-field-label">Days</span><div class="ag-day-toggle-row">${dayToggles}</div></div>
      <div class="ag-field-row"><span class="ag-field-label">Zone delay</span><input type="number" min="0" class="ag-input popover-delay" value="${draft.zone_delay_sec}" style="width:70px; flex:none;"><span class="ag-input-unit">sec</span></div>
      <div class="ag-field-row"><span class="ag-field-label">Seasonal adj.</span><select class="ag-select popover-seasonal">${seasonalOptions}</select></div>
      <div class="ag-popover-overlap-warning">${computePopoverOverlapWarning(controllerId, draft)}</div>
      <div class="ag-popover-actions">
        <button type="button" class="ag-btn ag-btn-primary ag-btn-sm popover-confirm-btn">Confirm</button>
        <button type="button" class="ag-btn ag-btn-ghost ag-btn-sm popover-cancel-btn">Cancel</button>
      </div>
    </div>`;
}

function popoverEscHandler(e) {
  if (e.key === 'Escape') closeProgramPopover();
}

function getPopoverOverlay() {
  let overlay = document.getElementById('program-popover-overlay');
  if (!overlay) {
    overlay = document.createElement('div');
    overlay.id = 'program-popover-overlay';
    overlay.className = 'ag-popover-backdrop';
    document.body.appendChild(overlay);
    overlay.addEventListener('click', (e) => { if (e.target === overlay) closeProgramPopover(); });
  }
  return overlay;
}

function refreshPopoverOverlapOnly() {
  const overlay = document.getElementById('program-popover-overlay');
  if (!overlay) return;
  overlay.querySelector('.ag-popover-overlap-warning').innerHTML =
    computePopoverOverlapWarning(popoverState.controllerId, popoverState.draft);
}

function attachPopoverHandlers(overlay) {
  const startInput = overlay.querySelector('.popover-start');
  startInput.addEventListener('input', () => {
    popoverState.draft.start = startInput.value;
    refreshPopoverOverlapOnly();
  });

  overlay.querySelectorAll('.ag-day-toggle').forEach(btn => {
    btn.addEventListener('click', () => {
      const day = Number(btn.dataset.day);
      const idx = popoverState.draft.days.indexOf(day);
      if (idx >= 0) popoverState.draft.days.splice(idx, 1);
      else popoverState.draft.days.push(day);
      btn.classList.toggle('on');
      refreshPopoverOverlapOnly();
    });
  });

  overlay.querySelector('.popover-delay').addEventListener('input', (e) => {
    popoverState.draft.zone_delay_sec = Number(e.target.value) || 0;
  });

  overlay.querySelector('.popover-seasonal').addEventListener('change', (e) => {
    popoverState.draft.seasonal_adjust_pct = Number(e.target.value) || 0;
    refreshPopoverOverlapOnly();
  });

  const programIdSelect = overlay.querySelector('.popover-program-id');
  if (programIdSelect) {
    programIdSelect.addEventListener('change', () => {
      popoverState.draft.id = programIdSelect.value;
      refreshPopoverOverlapOnly();
    });
  }

  overlay.querySelector('.popover-confirm-btn').addEventListener('click', confirmProgramPopover);
  overlay.querySelector('.popover-cancel-btn').addEventListener('click', closeProgramPopover);
}

function showPopover() {
  const overlay = getPopoverOverlay();
  overlay.innerHTML = renderProgramPopoverHtml(popoverState.controllerId, popoverState.draft, popoverState.isNew);
  overlay.classList.add('open');
  attachPopoverHandlers(overlay);
  document.addEventListener('keydown', popoverEscHandler);
}

function openProgramPopover(controllerId, programId) {
  const program = findProgram(controllerId, programId);
  if (!program) return;
  popoverState = {
    controllerId,
    isNew: false,
    draft: {
      id: programId,
      start: program.start,
      days: program.days.slice(),
      zone_delay_sec: program.zone_delay_sec,
      seasonal_adjust_pct: program.seasonal_adjust_pct
    }
  };
  showPopover();
}

function openAddProgramPopover(controllerId) {
  const controller = findController(controllerId);
  const used = new Set(controller.programs.map(p => p.id));
  const nextId = ['A', 'B', 'C', 'D'].find(id => !used.has(id));
  if (!nextId) return;
  popoverState = {
    controllerId,
    isNew: true,
    draft: { id: nextId, start: '', days: [], zone_delay_sec: 0, seasonal_adjust_pct: 0 }
  };
  showPopover();
}

function closeProgramPopover() {
  const overlay = document.getElementById('program-popover-overlay');
  if (overlay) overlay.classList.remove('open');
  popoverState = null;
  document.removeEventListener('keydown', popoverEscHandler);
  renderConfigPage();
}

function confirmProgramPopover() {
  const { controllerId, isNew, draft } = popoverState;
  const controller = findController(controllerId);

  if (isNew) {
    controller.programs.push({
      id: draft.id,
      days: draft.days.slice(),
      start: draft.start,
      zone_delay_sec: draft.zone_delay_sec,
      seasonal_adjust_pct: draft.seasonal_adjust_pct,
      zones: []
    });
  } else {
    const program = findProgram(controllerId, draft.id);
    program.days = draft.days.slice();
    program.start = draft.start;
    program.zone_delay_sec = draft.zone_delay_sec;
    program.seasonal_adjust_pct = draft.seasonal_adjust_pct;
  }
  setDirty(true);
  closeProgramPopover();
}

// ---------- Program header + zone table rendering ----------

// Mirrors firmware's gate exactly (lookupWeatherAdjustPct/consumeSkipNextRun
// in main.cpp both check weather.auto_adjust before doing anything) -- the
// UI must use the same gate everywhere it reads weatherState, or it will
// show/apply a percentage that firmware itself isn't actually applying.
function isWeatherAutoAdjustEnabled() {
  return Boolean(zoneDoc.weather && zoneDoc.weather.auto_adjust);
}

function renderProgramHeaderRow(controller, program) {
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  const weatherPctFn = (zone) => findZoneWeatherAdjustPct(weatherState, controller.id, zone.znumber, autoAdjustEnabled);
  const interval = getProgramInterval(program, weatherPctFn);
  const endTime = minsToTime(interval.end);
  const crossesMidnight = interval.end >= 1440;
  const overlapPairs = findControllerOverlaps(controller).filter(pair => pair.includes(program.id));
  const overlapHtml = overlapPairs.length
    ? `<span class="ag-overlap-warning">&#9888; Overlaps Program ${overlapPairs.map(pair => pair.find(id => id !== program.id)).join(', ')}</span>`
    : '';

  const weatherEntry = (weatherState.programs || []).find(p => p.controller === controller.id && p.program === program.id);
  const skipBadgeHtml = (autoAdjustEnabled && weatherEntry && weatherEntry.skip_next_run)
    ? `<span class="ag-weather-skip-badge">&#9748; RAIN SKIP</span>` : '';

  return `
    <div class="ag-program-header" data-controller="${controller.id}" data-program="${program.id}">
      <span class="ag-program-badge">Program ${program.id}</span>
      <span class="ag-program-start">${program.start}</span>
      <span class="ag-program-days">${dayPipsHtml(program.days)}</span>
      <span class="ag-program-zonecount">${program.zones.length} zones</span>
      ${skipBadgeHtml}
      <span class="ag-program-duration">${interval.totalRun} min total</span>
      <span class="ag-program-endtime">ends ${endTime}${crossesMidnight ? ' <span class="ag-day-badge">+1</span>' : ''}</span>
      ${overlapHtml}
      <button class="ag-btn ag-btn-ghost ag-btn-sm edit-program-btn" data-controller="${controller.id}" data-program="${program.id}">&#9998;</button>
    </div>`;
}

// Colors a WX RUN % to match this codebase's existing red/green convention
// (psiBarColor, overlap warnings, ctrl badges) -- red below 100 (less
// water), green above 100 (more water), neutral at exactly 100. Now rendered
// per zone row (not in the column header) since weather_adjust_pct genuinely
// varies zone to zone.
function wxPctHtml(weatherPct) {
  const pct = Math.round(weatherPct);
  if (pct < 100) return `<span class="ag-wx-pct-low">${pct}%</span>`;
  if (pct > 100) return `<span class="ag-wx-pct-high">${pct}%</span>`;
  return `${pct}%`;
}

// Same red/amber/green thresholds as the chart's deficit gauge
// (updateWeatherTiles) -- low deficit is good (green), approaching max is a
// problem (red). Diagnostic only: helps the user see which zones are
// running short or staying well ahead, to manually tune run times --
// independent of whether "Apply to Schedules" is on, since the firmware
// computes this regardless of auto_adjust.
function deficitColorClass(deficitMm, referenceDeficitMm, maxDeficitMm) {
  if (deficitMm < referenceDeficitMm) return 'ag-deficit-low';
  if (deficitMm < maxDeficitMm * 0.8) return 'ag-deficit-mid';
  return 'ag-deficit-high';
}

function renderZoneRow(controllerId, program, zone, idx) {
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  const weatherPctFn = (z) => findZoneWeatherAdjustPct(weatherState, controllerId, z.znumber, autoAdjustEnabled);
  const weatherPct = weatherPctFn(zone);
  const wxRunMinutes = applyRunAdjustments(zone.run, program.seasonal_adjust_pct || 0, weatherPct);
  const derivedStart = getZoneDerivedStart(program, idx, weatherPctFn);
  const startLabel = minsToTime(derivedStart) + (derivedStart >= 1440 ? ' +1' : '');

  const zoneStateEntry = (weatherState.zones || []).find(z => z.controller === controllerId && Number(z.zone_number) === Number(zone.znumber));
  let deficitHtml = '<span class="ag-help-text">—</span>';
  if (zoneStateEntry) {
    const deficitMm = Number(zoneStateEntry.deficit_mm) || 0;
    const referenceDeficitMm = Number((zoneDoc.weather || {}).reference_deficit_mm) || 6.0;
    const maxDeficitMm = Number((zoneDoc.weather || {}).max_deficit_mm) || 25.0;
    deficitHtml = `<span class="${deficitColorClass(deficitMm, referenceDeficitMm, maxDeficitMm)}">${deficitMm.toFixed(1)} mm</span>`;
  }

  return `
    <tr draggable="true" data-controller="${controllerId}" data-program="${program.id}" data-zone-idx="${idx}">
      <td class="ag-drag-handle">&#9776;</td>
      <td>${zone.znumber}</td>
      <td><input class="ag-inline-input zone-name-input" value="${escapeHtml(zone.zname)}" data-field="zname"></td>
      <td><input class="ag-inline-input zone-psi-input" type="number" value="${zone.avgpsi}" data-field="avgpsi" style="width:50px;"></td>
      <td><input class="ag-inline-input zone-run-input" type="number" value="${zone.run}" data-field="run" style="width:50px;"></td>
      <td class="ag-zone-wx-run">${wxRunMinutes} ${wxPctHtml(weatherPct)}</td>
      <td class="ag-zone-starts-at">${startLabel}</td>
      <td class="ag-zone-deficit">${deficitHtml}</td>
      <td><button class="ag-btn ag-btn-danger ag-btn-sm delete-zone-btn" data-controller="${controllerId}" data-program="${program.id}" data-zone-idx="${idx}">&#10005;</button></td>
    </tr>`;
}

function renderZoneTableForProgram(controllerId, program) {
  const rows = program.zones.map((zone, idx) => renderZoneRow(controllerId, program, zone, idx)).join('');
  return `
    <table class="ag-zone-table" data-controller="${controllerId}" data-program="${program.id}">
      <thead>
        <tr>
          <th></th><th>#</th><th>Name</th><th>Target PSI</th><th>Run (min)</th><th>WX Run</th><th>Starts at</th><th>Deficit</th><th></th>
        </tr>
      </thead>
      <tbody>
        ${rows}
      </tbody>
    </table>
    <button class="ag-btn ag-btn-ghost ag-btn-sm add-zone-btn" data-controller="${controllerId}" data-program="${program.id}">+ Add zone</button>`;
}

function renderProgramGroup(controller, program) {
  return `
    <div class="ag-program-group ${ctrlBadgeClass(controller.id)}" data-controller="${controller.id}" data-program="${program.id}">
      ${renderProgramHeaderRow(controller, program)}
      ${renderZoneTableForProgram(controller.id, program)}
    </div>`;
}

function renderControllerSection(controller) {
  const groups = controller.programs.map(p => renderProgramGroup(controller, p)).join('');
  const addBtn = controller.programs.length < 4
    ? `<button class="ag-btn ag-btn-ghost ag-btn-sm add-program-btn" data-controller="${controller.id}">+ Add Program</button>`
    : '';
  return `
    <div class="ag-controller-section" data-controller="${controller.id}">
      <div class="ag-controller-header ${ctrlBadgeClass(controller.id)}">${escapeHtml(controller.name)} controller</div>
      ${groups}
      ${addBtn}
    </div>`;
}

function renderConfigPage() {
  const container = document.getElementById('config-controllers');
  container.innerHTML = zoneDoc.controllers.map(renderControllerSection).join('');
  document.getElementById('zone-count').textContent = countAllZones(zoneDoc);
}

function recalcStartsAtForProgram(table, program) {
  const controllerId = table.dataset.controller;
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  const weatherPctFn = (z) => findZoneWeatherAdjustPct(weatherState, controllerId, z.znumber, autoAdjustEnabled);
  table.querySelectorAll('tbody tr[data-zone-idx]').forEach(row => {
    const idx = Number(row.dataset.zoneIdx);
    const zone = program.zones[idx];
    const weatherPct = weatherPctFn(zone);
    const wxRunMinutes = applyRunAdjustments(zone.run, program.seasonal_adjust_pct || 0, weatherPct);
    row.querySelector('.ag-zone-wx-run').innerHTML = `${wxRunMinutes} ${wxPctHtml(weatherPct)}`;
    const derivedStart = getZoneDerivedStart(program, idx, weatherPctFn);
    const label = minsToTime(derivedStart) + (derivedStart >= 1440 ? ' +1' : '');
    row.querySelector('.ag-zone-starts-at').textContent = label;
  });
}

function updateProgramHeaderLive(controller, program) {
  const headerEl = document.querySelector(`.ag-program-header[data-controller="${controller.id}"][data-program="${program.id}"]`);
  if (!headerEl) return;
  headerEl.outerHTML = renderProgramHeaderRow(controller, program);
}

function addZoneToProgram(controllerId, programId) {
  const program = findProgram(controllerId, programId);
  const maxZnumber = program.zones.reduce((m, z) => Math.max(m, Number(z.znumber) || 0), 0);
  program.zones.push({ znumber: maxZnumber + 1, zname: '', avgpsi: 44, run: 30 });
  setDirty(true);
  renderConfigPage();
}

function deleteZoneFromProgram(controllerId, programId, idx) {
  if (!confirm('Delete this zone?')) return;
  const program = findProgram(controllerId, programId);
  program.zones.splice(idx, 1);
  setDirty(true);
  renderConfigPage();
}

// ---------- Delegated event handlers (survive innerHTML re-renders) ----------

function onConfigContainerClick(e) {
  const editBtn = e.target.closest('.edit-program-btn');
  if (editBtn) { openProgramPopover(editBtn.dataset.controller, editBtn.dataset.program); return; }

  const addProgramBtn = e.target.closest('.add-program-btn');
  if (addProgramBtn) { openAddProgramPopover(addProgramBtn.dataset.controller); return; }

  const addZoneBtn = e.target.closest('.add-zone-btn');
  if (addZoneBtn) { addZoneToProgram(addZoneBtn.dataset.controller, addZoneBtn.dataset.program); return; }

  const deleteZoneBtn = e.target.closest('.delete-zone-btn');
  if (deleteZoneBtn) {
    deleteZoneFromProgram(deleteZoneBtn.dataset.controller, deleteZoneBtn.dataset.program, Number(deleteZoneBtn.dataset.zoneIdx));
  }
}

function onConfigContainerInput(e) {
  const input = e.target;
  if (!input.matches('.zone-name-input, .zone-psi-input, .zone-run-input')) return;
  const row = input.closest('tr');
  const controllerId = row.dataset.controller, programId = row.dataset.program;
  const idx = Number(row.dataset.zoneIdx);
  const controller = findController(controllerId);
  const program = findProgram(controllerId, programId);
  const field = input.dataset.field;
  program.zones[idx][field] = (field === 'zname') ? input.value : (Number(input.value) || 0);
  setDirty(true);
  if (field === 'run') {
    recalcStartsAtForProgram(input.closest('table'), program);
    updateProgramHeaderLive(controller, program);
  }
}

function onZoneDragStart(e) {
  const row = e.target.closest('tr[draggable="true"]');
  if (!row) return;
  dragState = { controllerId: row.dataset.controller, programId: row.dataset.program, fromIdx: Number(row.dataset.zoneIdx) };
  row.classList.add('dragging');
}

function onZoneDragOver(e) {
  if (e.target.closest('tr[draggable="true"]')) e.preventDefault();
}

function onZoneDrop(e) {
  const row = e.target.closest('tr[draggable="true"]');
  if (!row || !dragState) return;
  e.preventDefault();
  if (row.dataset.controller !== dragState.controllerId || row.dataset.program !== dragState.programId) return;
  const toIdx = Number(row.dataset.zoneIdx);
  if (toIdx !== dragState.fromIdx) {
    const program = findProgram(dragState.controllerId, dragState.programId);
    const [moved] = program.zones.splice(dragState.fromIdx, 1);
    program.zones.splice(toIdx, 0, moved);
    setDirty(true);
    renderConfigPage();
  }
  dragState = null;
}

function onZoneDragEnd(e) {
  const row = e.target.closest('tr[draggable="true"]');
  if (row) row.classList.remove('dragging');
  dragState = null;
}

function attachConfigPageHandlers() {
  const container = document.getElementById('config-controllers');
  container.addEventListener('click', onConfigContainerClick);
  container.addEventListener('input', onConfigContainerInput);
  container.addEventListener('dragstart', onZoneDragStart);
  container.addEventListener('dragover', onZoneDragOver);
  container.addEventListener('drop', onZoneDrop);
  container.addEventListener('dragend', onZoneDragEnd);
}

// ---------- Load / save whole document ----------

// Fills in site/weather defaults for a doc that predates this feature, so a
// not-yet-migrated controllers.json doesn't crash the renderer.
function applyDocDefaults(doc) {
  doc.site = { ...DEFAULT_SITE, ...(doc.site || {}) };
  doc.weather = { ...DEFAULT_WEATHER_SETTINGS, ...(doc.weather || {}) };
  if (!Array.isArray(doc.controllers)) doc.controllers = [];
  return doc;
}

async function loadZoneTable(endpoint) {
  try {
    const res = await fetch(BASE_URL + endpoint + '?ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) throw new Error(await res.text());
    zoneDoc = applyDocDefaults(await res.json());
    renderConfigPage();
    loadSiteFieldsIntoForm();
    loadWeatherSettingsIntoForm();
    renderWeatherReadOnlyStatus();	// zoneDoc.weather.auto_adjust just changed; the indicator was rendered earlier from loadWeatherState() against the stale default doc
    if (endpoint === '/load-zone-table') {
      setCurrentZoneFile('controllers.json');
      setSaveZoneFilename('controllers.json');
    }
    setDirty(false);
  } catch (e) {
    console.error('loadZoneTable:', e);
    alert('Error loading zone table: ' + e.message);
  }
}

// Shared by saveZoneDoc() (Save All, targeting whatever's in the Save As
// field) and activateLoadedSchedule() ("Set as Active Schedule", always
// targeting controllers.json regardless of Save As). updateSaveAsField is
// false for both today's callers -- Save All doesn't need to touch a field
// it just read from, and Set as Active Schedule must NOT repoint Save As at
// controllers.json, since that would make the currently-loaded preset's own
// edits look like they'd been saved when they haven't.
async function saveControllersToFilename(filename, { updateSaveAsField = false } = {}) {
  if (!/^[^\/\\]+\.json$/i.test(filename) || filename.includes('..')) {
    showToast('Use a simple .json filename.', true);
    return false;
  }

  try {
    const res = await fetch(BASE_URL + '/submit-zone-form?filename=' + encodeURIComponent(filename) + '&ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(zoneDoc)
    });
    if (!res.ok) throw new Error(await res.text());
    await res.text();

    const verifyRes = await fetch(BASE_URL + '/load-spiffs-zone-table?filename=' + encodeURIComponent(filename) + '&ts=' + Date.now(), { cache: 'no-store' });
    if (!verifyRes.ok) throw new Error('Save verification failed: ' + await verifyRes.text());
    const savedDoc = await verifyRes.json();
    // /submit-zone-form is schedule-only now (site/weather save independently
    // via saveSiteSettings()/`/submit-site-form`), so its response -- and
    // therefore this verify-read -- is always a bare controllers array,
    // regardless of filename.
    const expected = zoneDoc.controllers;
    if (canonicalJson(savedDoc) !== canonicalJson(expected)) {
      throw new Error('Save verification failed: SPIFFS file does not match the edited document.');
    }

    setCurrentZoneFile(filename);
    if (updateSaveAsField) setSaveZoneFilename(filename);
    await loadSpiffsFileList();
    return true;
  } catch (e) {
    console.error('saveControllersToFilename:', e);
    showToast('Failed to save zone data.', true);
    return false;
  }
}

async function saveZoneDoc() {
  const fileName = getSaveZoneFilename();
  const ok = await saveControllersToFilename(fileName, { updateSaveAsField: false });
  if (ok) setDirty(false);
  return ok;
}

async function submitZoneForm() {
  const ok = await saveZoneDoc();
  if (ok) showToast('Zone data saved and verified in SPIFFS: ' + getSaveZoneFilename() + '.');
}

// "Set as Active Schedule" -- always writes to the real controllers.json
// regardless of the Save As field, so activating a loaded preset never
// risks overwriting that preset's own save target. Deliberately does NOT
// clear the dirty indicator or touch Save As: if a preset (e.g.
// controllers_summer.json) is loaded and edited, those edits are still
// legitimately unsaved relative to that preset after this runs, since this
// writes a copy to controllers.json rather than saving the preset itself.
async function activateLoadedSchedule() {
  if (!confirm('Set the currently loaded schedule as the ACTIVE schedule (controllers.json)? This immediately changes what the irrigation controller runs, regardless of the Save As field above.')) return;
  const ok = await saveControllersToFilename('controllers.json', { updateSaveAsField: false });
  if (ok) showToast('controllers.json is now the active schedule.');
}

// Saves ONLY site.json (site identity + weather/auto-adjust settings),
// completely independent of the schedule save above -- used by
// submitLocation()/submitSensorRate()/submitWeatherSettings(). This replaces
// the old shared /submit-zone-form + "zone_data.json" sentinel mechanism:
// that coupling meant a single client-side default change (Save As's
// fallback filename) could silently break all three of these saves at once,
// since they never wrote to a file at all, just a controllers-only preset
// under whatever name Save As happened to hold.
async function saveSiteSettings() {
  try {
    const res = await fetch(BASE_URL + '/submit-site-form?ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ site: zoneDoc.site, weather: zoneDoc.weather })
    });
    if (!res.ok) throw new Error(await res.text());
    await res.text();

    const verifyRes = await fetch(BASE_URL + '/load-zone-table?ts=' + Date.now(), { cache: 'no-store' });
    if (!verifyRes.ok) throw new Error('Save verification failed: ' + await verifyRes.text());
    const savedDoc = await verifyRes.json();
    const expected = { site: zoneDoc.site, weather: zoneDoc.weather };
    if (canonicalJson({ site: savedDoc.site, weather: savedDoc.weather }) !== canonicalJson(expected)) {
      throw new Error('Save verification failed: SPIFFS file does not match the edited document.');
    }

    setDirty(false);
    return true;
  } catch (e) {
    console.error('saveSiteSettings:', e);
    showToast('Failed to save site settings.', true);
    return false;
  }
}

// ---------- Site Identity / Sensor Rate / PSI Calibration ----------

function loadSiteFieldsIntoForm() {
  const site = zoneDoc.site || {};
  document.getElementById('loc-input').value = site.name || '';
  document.getElementById('loc-display').textContent = site.name || '—';

  const rate = Number(site.sensor_interval_sec) || 30;
  document.getElementById('sensor-rate-input').value = rate;
  updateDerivedStats(rate);

}

async function submitLocation() {
  const val = document.getElementById('loc-input').value;
  zoneDoc.site.name = val;
  setDirty(true);
  const ok = await saveSiteSettings();
  if (ok) {
    document.getElementById('loc-display').textContent = val;
    showToast('Location saved.');
  }
}

function updateDerivedStats(s) {
  s = Math.max(1, s);
  document.getElementById('rpm-out').textContent = (60 / s).toFixed(1);
  document.getElementById('rph-out').textContent = Math.round(3600 / s);
}

async function submitSensorRate() {
  const val = Math.max(1, parseInt(document.getElementById('sensor-rate-input').value, 10) || 30);
  zoneDoc.site.sensor_interval_sec = val;
  setDirty(true);
  const ok = await saveSiteSettings();
  if (ok) showToast('Sensor rate saved.');
}

// ---------- Weather Auto-Adjust ----------
//
// Editable settings (site.latitude/longitude/timezone/mm_per_min_default,
// the whole weather.* block) live in zoneDoc and save through the normal
// saveZoneDoc()/submitZoneForm() whole-document path, exactly like
// seasonal_adjust_pct already does. Firmware-computed runtime state (fetch
// status, current deficit, per-program weather_adjust_pct/skip_next_run)
// comes from the separately-fetched /weather-state and is rendered from the
// module-level `weatherState` only -- never merged into zoneDoc, so a Save
// All click can never clobber it (decision #1).

async function loadWeatherState() {
  try {
    const res = await fetch(BASE_URL + '/weather-state?ts=' + Date.now(), { cache: 'no-store' });
    if (res.ok) {
      const data = await res.json();
      weatherState = (data && typeof data === 'object') ? data : {};
    }
  } catch (e) {
    console.error('loadWeatherState:', e);
  }
  if (!Array.isArray(weatherState.programs)) weatherState.programs = [];
  renderWeatherReadOnlyStatus();
}

// Per-zone calibration (calibration.html owns it) -- only read here for the
// ET0 chart's irrigationScheduledMm() projection; config.js never writes it.
async function loadCalibrationData() {
  try {
    const res = await fetch(BASE_URL + '/calibration.json?ts=' + Date.now(), { cache: 'no-store' });
    if (res.ok) {
      const data = await res.json();
      calibrationData = (data && typeof data === 'object') ? data : {};
    }
  } catch (e) {
    console.error('loadCalibrationData:', e);
  }
  if (!Array.isArray(calibrationData.zones)) calibrationData.zones = [];
}

// Runs the same fetch+budget sequence the firmware runs automatically at
// 00:05, right now, on demand. Blocks for the HTTPS round trip (up to ~10s),
// so the button is disabled meanwhile rather than left clickable.
function sleep(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

// /weather-fetch-now only queues the fetch -- the actual HTTPS call runs on
// the device's loop() task via serviceWeatherTask(), not inside the request
// handler, since a slow API response there would trip the device's 5s task
// watchdog and reboot it (confirmed on real hardware). So this polls
// /weather-state's "fetch_pending" flag instead of getting the fresh
// result in one round trip. last_fetch_date/last_fetch_utc deliberately
// don't change on a failed fetch, so fetch_pending is the only reliable
// "did it finish yet" signal.
async function fetchWeatherNow() {
  const btn = document.getElementById('weather-fetch-now-btn');
  if (btn) { btn.disabled = true; btn.textContent = 'Fetching…'; }
  try {
    const queueRes = await fetch(BASE_URL + '/weather-fetch-now?ts=' + Date.now(), { cache: 'no-store' });
    if (!queueRes.ok) throw new Error(await queueRes.text());

    const MAX_POLLS = 20, POLL_INTERVAL_MS = 1000; // ~20s -- covers the up-to-10s HTTPS timeout plus scheduling lag
    let data = null;
    for (let i = 0; i < MAX_POLLS; i++) {
      await sleep(POLL_INTERVAL_MS);
      const res = await fetch(BASE_URL + '/weather-state?ts=' + Date.now(), { cache: 'no-store' });
      if (!res.ok) continue;
      data = await res.json();
      if (!data.fetch_pending) break;
    }

    if (!data || data.fetch_pending) {
      showToast('Weather fetch is taking longer than expected — check back shortly.', true);
      return;
    }

    weatherState = (data && typeof data === 'object') ? data : {};
    if (!Array.isArray(weatherState.programs)) weatherState.programs = [];
    renderWeatherReadOnlyStatus();
    renderConfigPage();	// program badges may have changed
    loadWeatherChart();

    if (weatherState.last_fetch_status === 'ok') {
      showToast('Weather data fetched.');
    } else {
      showToast('Weather fetch failed — check coordinates/connectivity.', true);
    }
  } catch (e) {
    console.error('fetchWeatherNow:', e);
    showToast('Weather fetch request failed.', true);
  } finally {
    if (btn) { btn.disabled = false; btn.textContent = '↻ Fetch Now'; }
  }
}

function loadWeatherSettingsIntoForm() {
  const site = zoneDoc.site || {};
  const weather = zoneDoc.weather || {};
  document.getElementById('weather-enabled-input').checked = Boolean(weather.enabled);
  document.getElementById('weather-auto-adjust-input').checked = Boolean(weather.auto_adjust);
  document.getElementById('weather-lat-input').value = site.latitude || 0;
  document.getElementById('weather-lon-input').value = site.longitude || 0;

  const timezoneSelect = document.getElementById('weather-timezone-input');
  const savedTimezone = site.timezone || 'America/Denver';
  timezoneSelect.value = savedTimezone;
  if (timezoneSelect.value !== savedTimezone) {
    // Saved value predates this dropdown (or was set via the API directly) --
    // add it as its own option rather than silently switching the selection
    // to whatever option happens to be first.
    const opt = document.createElement('option');
    opt.value = savedTimezone;
    opt.textContent = savedTimezone + ' (custom)';
    timezoneSelect.insertBefore(opt, timezoneSelect.firstChild);
    timezoneSelect.value = savedTimezone;
  }

  document.getElementById('weather-mm-per-min-input').value = site.mm_per_min_default || 0.25;
  document.getElementById('weather-reference-deficit-input').value = weather.reference_deficit_mm || 6.0;
  document.getElementById('weather-max-deficit-input').value = weather.max_deficit_mm || 25.0;
  document.getElementById('weather-rain-skip-input').value = weather.rain_skip_threshold_mm || 6.0;
  document.getElementById('weather-min-adjust-input').value = weather.min_adjust_pct ?? 0;
  document.getElementById('weather-max-adjust-input').value = weather.max_adjust_pct || 150;
}

function renderWeatherReadOnlyStatus() {
  const statusEl = document.getElementById('weather-status-line');
  if (weatherState.last_fetch_status === 'ok') {
    statusEl.textContent = '✓ Last fetch OK · ' + (weatherState.last_fetch_utc || '');
    statusEl.classList.remove('muted');
  } else if (weatherState.last_fetch_status === 'error') {
    statusEl.textContent = '⚠ Last fetch FAILED · ' + (weatherState.last_fetch_utc || '');
    statusEl.classList.remove('muted');
  } else {
    statusEl.textContent = 'No fetch yet.';
    statusEl.classList.add('muted');
  }

  // The one clear place to answer "is this actually doing anything right
  // now" -- mirrors the same weather.auto_adjust gate firmware applies in
  // lookupZoneWeatherAdjustPct()/consumeSkipNextRun(), so this can never
  // claim an adjustment is active when firmware isn't actually applying
  // one. No single percentage shown here anymore -- weather_adjust_pct is
  // per-zone now (different zones can genuinely differ), so a single
  // number would misrepresent it; see each controller's own tiles/zone
  // table for the real per-zone figures.
  const activeEl = document.getElementById('weather-active-indicator');
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  if (!autoAdjustEnabled) {
    activeEl.textContent = '○ ADJUSTMENTS NOT APPLIED — "Apply to Schedules" is off';
    activeEl.className = 'ag-manual-state muted';
    activeEl.style.color = '';
  } else {
    activeEl.textContent = '● ADJUSTMENTS ACTIVE — percentages vary by zone (see WX Run column below)';
    activeEl.className = 'ag-manual-state';
    activeEl.style.color = '#60a5fa';
  }
}

// One full tiles+legend+chart+toggle block per controller -- Yard and Field
// each get their own (element IDs suffixed by controllerId), since each
// tracks its own deficit now. ET0/Rain forecast values still show up
// identically in both (one shared weather fetch for the whole property);
// only deficit/irrigation genuinely differ block to block.
function renderControllerWeatherBlockHtml(controllerId) {
  return `
    <div class="ag-controller-section" data-controller="${controllerId}">
      <div class="ag-controller-header ${ctrlBadgeClass(controllerId)}">${escapeHtml(controllerId)} controller</div>
      <div class="ag-weather-tiles" id="weather-tiles-${controllerId}">
        <div class="ag-weather-tile">
          <div class="ag-weather-tile-label">Current deficit</div>
          <div class="ag-weather-tile-value"><span id="weather-deficit-out-${controllerId}">—</span> mm</div>
          <div class="ag-weather-tile-sub">reference <span id="weather-reference-out-${controllerId}">—</span> mm</div>
          <div class="ag-weather-gauge"><div class="ag-weather-gauge-fill" id="weather-deficit-gauge-${controllerId}"></div></div>
        </div>
        <div class="ag-weather-tile">
          <div class="ag-weather-tile-label">Today's adjustment</div>
          <div class="ag-weather-tile-value" id="weather-adjust-out-${controllerId}">—</div>
        </div>
        <div class="ag-weather-tile">
          <div class="ag-weather-tile-label">Forecast rain 24h</div>
          <div class="ag-weather-tile-value"><span id="weather-rain24-out-${controllerId}">—</span> mm</div>
        </div>
        <div class="ag-weather-tile">
          <div class="ag-weather-tile-label">Projected</div>
          <div class="ag-weather-tile-value" id="weather-projected-out-${controllerId}">—</div>
        </div>
      </div>
      <div class="ag-weather-legend" title="Evapotranspiration: water lost from soil evaporation plus plant transpiration -- this is what drives the soil deficit up each day, offset by rain and irrigation.">
        <span class="ag-legend-item"><span class="ag-legend-swatch" style="background:#2a78d6;"></span>ET&#8320;</span>
        <span class="ag-legend-item"><span class="ag-legend-swatch" style="background:#1baf7a;"></span>Irrigation</span>
        <span class="ag-legend-item"><span class="ag-legend-swatch" style="background:#7ab3e0;"></span>Rain</span>
        <span class="ag-legend-item"><span class="ag-legend-swatch ag-legend-swatch-forecast"></span>Forecast (translucent)</span>
        <span class="ag-legend-item"><span class="ag-legend-line"></span>Deficit</span>
      </div>
      <div class="ag-weather-chart-wrap">
        <canvas id="weather-deficit-chart-${controllerId}" height="360"></canvas>
        <div id="weather-chart-msg-${controllerId}" class="ag-help-text ag-weather-chart-msg" style="display:none;">No weather cache yet.</div>
      </div>
      <label class="ag-help-text" style="display:flex; align-items:center; gap:5px; cursor:pointer; margin-top:8px; min-width:auto;">
        <input type="checkbox" class="weather-include-rain-toggle" data-controller="${controllerId}" checked />
        Include forecast rain in deficit projection
      </label>
    </div>`;
}

function renderAllControllerWeatherBlocks() {
  const container = document.getElementById('weather-controller-blocks');
  if (!container) return;
  container.innerHTML = (zoneDoc.controllers || []).map(c => renderControllerWeatherBlockHtml(c.id)).join('');
  container.querySelectorAll('.weather-include-rain-toggle').forEach(input => {
    input.addEventListener('change', () => {
      const controllerId = input.dataset.controller;
      const cache = weatherChartDataCaches[controllerId];
      if (!cache) return;
      cache.deficitLine = calcDeficitLine(cache.pastDays, cache.forecastDays, input.checked, controllerId);
      const chart = weatherDeficitCharts[controllerId];
      if (chart) {
        chart.data.datasets.find(d => d.label === 'Deficit').data = cache.deficitLine;
        chart.update();
      }
      updateWeatherTiles(cache, controllerId);
    });
  });
}

async function submitWeatherSettings() {
  zoneDoc.weather = zoneDoc.weather || {};
  zoneDoc.weather.enabled = document.getElementById('weather-enabled-input').checked;
  zoneDoc.weather.auto_adjust = document.getElementById('weather-auto-adjust-input').checked;
  zoneDoc.weather.reference_deficit_mm = Number(document.getElementById('weather-reference-deficit-input').value) || 6.0;
  zoneDoc.weather.max_deficit_mm = Number(document.getElementById('weather-max-deficit-input').value) || 25.0;
  zoneDoc.weather.rain_skip_threshold_mm = Number(document.getElementById('weather-rain-skip-input').value) || 0;
  zoneDoc.weather.min_adjust_pct = Number(document.getElementById('weather-min-adjust-input').value) || 0;
  zoneDoc.weather.max_adjust_pct = Number(document.getElementById('weather-max-adjust-input').value) || 150;

  zoneDoc.site = zoneDoc.site || {};
  zoneDoc.site.latitude = Number(document.getElementById('weather-lat-input').value) || 0;
  zoneDoc.site.longitude = Number(document.getElementById('weather-lon-input').value) || 0;
  zoneDoc.site.timezone = document.getElementById('weather-timezone-input').value.trim();
  zoneDoc.site.mm_per_min_default = Number(document.getElementById('weather-mm-per-min-input').value) || 0.25;

  setDirty(true);
  const ok = await saveSiteSettings();
  if (ok) {
    // auto_adjust (or any other weather setting) may have just changed --
    // refresh the per-program badges and the active/inactive indicator so
    // they don't keep showing stale state until the next full page load.
    renderConfigPage();
    renderWeatherReadOnlyStatus();
    loadWeatherChart();	// target/max deficit, mm_per_min_default, etc. may have changed
    showToast('Weather settings saved.');
  }
}

// ---------- ET0 / deficit chart (Chart.js) ----------
//
// Two data sources, deliberately never mixed for the same metric: the past
// 5 days come from weather_log.json -- firmware's authoritative actual
// record, including whatever really happened (skipped runs, manual
// overrides, partial runs) -- never recalculated from raw ET0/rain. TODAY
// and the next 5 days have no actual record yet, so they come entirely from
// the Open-Meteo forecast cache. This mirrors lookupWeatherAdjustPct's
// "log is truth" principle from the firmware, extended to every actual-side
// series, not just the deficit line.

function toSupply(val) {
  // Chart.js still reserves stack space for a 0-height bar; null is the only
  // value that makes a stacked bar genuinely absent.
  return (val === null || val === undefined || val === 0) ? null : -val;
}

// Reuses applyRunAdjustments()/findZoneWeatherAdjustPct() (same formula the
// zone table itself uses) for each zone's own depth, then combines those
// depths into one controller-wide figure via an AREA-WEIGHTED AVERAGE, never
// a sum -- summing was the original bug this whole redesign exists to fix:
// each zone's mm_per_min is already normalized to that zone's own
// footprint, so adding two zones' depths together doesn't produce a depth
// that means anything (it's the same category error as adding two
// temperatures). Mirrors findControllerDeficitAvg()'s exact aggregation
// method/rationale -- see calib/calibration-page.md and
// et0-deficit-chart.md, both corrected to state this explicitly so this
// doesn't regress back to summing.
function irrigationScheduledMmForPrograms(dateStr, controllerId, programs) {
  const dow = new Date(dateStr + 'T00:00:00').getDay();
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  const fallbackMmPerMin = (zoneDoc.site && zoneDoc.site.mm_per_min_default) || 0.25;
  let weightedSum = 0, areaSum = 0;
  programs.forEach(program => {
    if (!Array.isArray(program.days) || !program.days.includes(dow)) return;
    (program.zones || []).forEach(zone => {
      const weatherPct = findZoneWeatherAdjustPct(weatherState, controllerId, zone.znumber, autoAdjustEnabled);
      const mmPerMin = lookupZoneMmPerMin(calibrationData, controllerId, zone.znumber, fallbackMmPerMin);
      const zoneDepthMm = applyRunAdjustments(zone.run, program.seasonal_adjust_pct || 0, weatherPct) * mmPerMin;
      const area = findZoneAreaFt2(calibrationData, controllerId, zone.znumber);
      weightedSum += zoneDepthMm * area;
      areaSum += area;
    });
  });
  const avgMm = areaSum > 0 ? weightedSum / areaSum : 0;
  return Math.round(avgMm * 100) / 100;
}

function irrigationScheduledMm(dateStr, controllerId) {
  const controller = (zoneDoc.controllers || []).find(c => c.id === controllerId);
  if (!controller) return 0;
  return irrigationScheduledMmForPrograms(dateStr, controllerId, controller.programs || []);
}

// Same as irrigationScheduledMm, scoped to just one program -- a controller
// with multiple programs (e.g. field's A/B split) can water disjoint
// physical areas per program, so each program's scheduled depth must stay
// independent rather than being blended into the controller-wide average.
function irrigationScheduledMmForProgram(dateStr, controllerId, programId) {
  const controller = (zoneDoc.controllers || []).find(c => c.id === controllerId);
  if (!controller) return 0;
  const program = (controller.programs || []).find(p => p.id === programId);
  if (!program) return 0;
  return irrigationScheduledMmForPrograms(dateStr, controllerId, [program]);
}

// Past days: weather_log.json's per-controller deficit_mm verbatim
// (authoritative, never recalculated). Today: findControllerDeficitAvg()'s
// area-weighted average of this controller's zones' live deficit. Forecast:
// walked forward day by day, clamped to [0, max_deficit_mm], optionally
// ignoring forecast rain when the toggle is off.
function calcDeficitLine(pastDays, forecastDays, includeRain, controllerId) {
  const line = pastDays.map(d => d.deficit);
  if (forecastDays.length === 0) return line;

  const maxDeficitMm = Number((zoneDoc.weather || {}).max_deficit_mm) || 25.0;
  let d = findControllerDeficitAvg(weatherState, calibrationData, controllerId);
  line.push(Math.round(d * 100) / 100); // today

  forecastDays.slice(1).forEach(day => {
    const rain = includeRain ? day.rain : 0;
    d += day.et0 - day.irrigation - rain;
    d = Math.max(0, Math.min(maxDeficitMm, d));
    line.push(Math.round(d * 100) / 100);
  });
  return line;
}

// One program's deficit line for the multi-line chart -- a controller with
// multiple programs can water disjoint physical areas per program (e.g.
// field's A/B split), so each program's deficit must trend independently
// rather than being blended into calcDeficitLine()'s single controller-wide
// line above. Past days come from weather_log.json's nested per-program
// breakdown (graceful 0 for older rows logged before this nesting existed --
// same one-time transition cost the controller-level split already paid).
// forecastDaysTemplate is the controller-level forecastDays array already
// built by buildChartData (date/et0/rain only -- irrigation is recomputed
// per program here since that's the one field that actually differs).
function buildProgramDeficitLine(forecastDaysTemplate, logEntries, includeRain, controllerId, program) {
  const zoneNumbers = (program.zones || []).map(z => Number(z.znumber));

  const pastDays = (logEntries || []).slice(-5).map(entry => {
    const ctrlEntry = (entry.controllers || []).find(c => c.controller === controllerId);
    const progEntry = ctrlEntry ? (ctrlEntry.programs || []).find(p => p.program === program.id) : null;
    return { deficit: progEntry ? Number(progEntry.deficit_mm) || 0 : 0 };
  });

  const line = pastDays.map(d => d.deficit);
  if (forecastDaysTemplate.length === 0) return line;

  const maxDeficitMm = Number((zoneDoc.weather || {}).max_deficit_mm) || 25.0;
  let d = findProgramDeficitAvg(weatherState, calibrationData, controllerId, zoneNumbers);
  line.push(Math.round(d * 100) / 100); // today

  forecastDaysTemplate.slice(1).forEach(day => {
    const rain = includeRain ? day.rain : 0;
    const irrigation = irrigationScheduledMmForProgram(day.date, controllerId, program.id);
    d += day.et0 - irrigation - rain;
    d = Math.max(0, Math.min(maxDeficitMm, d));
    line.push(Math.round(d * 100) / 100);
  });
  return line;
}

// Transforms the raw cache/log responses into the dataset arrays the chart
// renders, scoped to ONE controller. Returns null only when there is
// genuinely nothing to show (no log AND no cache) -- partial availability
// is handled by leaving the other half's arrays empty, not by fabricating
// zeros.
function buildChartData(cacheData, logEntries, includeRain, controllerId) {
  const daily = cacheData && cacheData.daily;
  const pastDays = (logEntries || []).slice(-5).map(entry => {
    const ctrlEntry = (entry.controllers || []).find(c => c.controller === controllerId);
    return {
      date: String(entry.date || ''),
      et0: Number(entry.et0_mm) || 0,
      rain: Number(entry.precip_mm) || 0,
      irrigation: ctrlEntry ? Number(ctrlEntry.water_applied_mm) || 0 : 0,
      deficit: ctrlEntry ? Number(ctrlEntry.deficit_mm) || 0 : 0
    };
  });

  const forecastDays = [];
  const rainProbabilityForecast = [];
  if (daily && Array.isArray(daily.time) && daily.time.length > 1) {
    const et0Arr = daily.et0_fao_evapotranspiration || [];
    const precipArr = daily.precipitation_sum || [];
    const probArr = daily.precipitation_probability_max || [];
    // Index 0 is yesterday (past_days=1) -- unused here, the log above
    // already covers actual history. Forecast block starts at index 1
    // (today); capped at index 6 for today + 5 future days.
    const upper = Math.min(daily.time.length, 7);
    for (let i = 1; i < upper; i++) {
      const date = String(daily.time[i] || '');
      forecastDays.push({ date, et0: Number(et0Arr[i]) || 0, rain: Number(precipArr[i]) || 0, irrigation: irrigationScheduledMm(date, controllerId) });
      rainProbabilityForecast.push(Number(probArr[i]) || 0);
    }
  }

  if (pastDays.length === 0 && forecastDays.length === 0) return null;

  const todayIndex = forecastDays.length > 0 ? pastDays.length : null;
  const n = pastDays.length + forecastDays.length;
  const categories = pastDays.map(d => d.date.slice(5))
    .concat(forecastDays.map((d, j) => j === 0 ? 'TODAY' : d.date.slice(5)));

  const et0Actual = new Array(n).fill(null);
  const et0Forecast = new Array(n).fill(null);
  const irrigActual = new Array(n).fill(null);
  const rainActual = new Array(n).fill(null);
  const irrigForecast = new Array(n).fill(null);
  const rainForecast = new Array(n).fill(null);
  const rainProbability = new Array(n).fill(null);

  pastDays.forEach((d, i) => {
    et0Actual[i] = d.et0;
    irrigActual[i] = toSupply(d.irrigation);
    rainActual[i] = toSupply(d.rain);
  });
  forecastDays.forEach((d, j) => {
    const i = pastDays.length + j;
    et0Forecast[i] = d.et0;
    irrigForecast[i] = toSupply(d.irrigation);
    rainForecast[i] = toSupply(d.rain);
    rainProbability[i] = rainProbabilityForecast[j];
  });

  // One deficit line per program on this controller (the actual chart
  // overlay -- see buildProgramDeficitLine) -- deficitLine below stays the
  // single blended controller-wide line, kept only for the tiles' "critical
  // in Nd" projection, which predates and is unrelated to this split.
  const controller = (zoneDoc.controllers || []).find(c => c.id === controllerId);
  const programLines = ((controller && controller.programs) || []).map(program => ({
    id: program.id,
    label: 'Program ' + program.id,
    deficitLine: buildProgramDeficitLine(forecastDays, logEntries, includeRain, controllerId, program)
  }));

  return {
    categories, todayIndex, pastDays, forecastDays,
    et0Actual, et0Forecast, irrigActual, rainActual, irrigForecast, rainForecast, rainProbability,
    deficitLine: calcDeficitLine(pastDays, forecastDays, includeRain, controllerId),
    programLines
  };
}

const WX_CHART_COLORS = {
  et0: '#2a78d6', et0ForecastFill: 'rgba(42, 120, 214, 0.3)',
  irrig: '#1baf7a', irrigForecastFill: 'rgba(27, 175, 122, 0.3)',
  rain: '#7ab3e0', rainForecastFill: 'rgba(122, 179, 224, 0.3)',
  deficit: '#e34948', target: '#f59e0b'
};

// One color per program line, in program order -- first slot matches the
// old single-line "deficit" red so a controller with only one program (e.g.
// yard) looks unchanged; additional programs (e.g. field's A/B split) get
// their own color so the legend can tell them apart.
const WX_PROGRAM_LINE_COLORS = ['#e34948', '#3b82f6', '#a855f7', '#f59e0b'];

// Draws the y=0 line manually (the axis itself hides the zero gridline) and
// labels which side is demand vs supply -- the chart's central metaphor.
const wxZeroLinePlugin = {
  id: 'wxZeroLine',
  afterDraw(chart) {
    const { ctx, chartArea, scales: { y } } = chart;
    const yZero = y.getPixelForValue(0);
    ctx.save();
    ctx.strokeStyle = '#8ab0ca';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(chartArea.left, yZero);
    ctx.lineTo(chartArea.right, yZero);
    ctx.stroke();
    ctx.font = "600 9px 'Share Tech Mono', monospace";
    ctx.fillStyle = '#8ab0ca';
    ctx.textAlign = 'left';
    ctx.fillText('demand ↑', chartArea.left + 2, yZero - 4);
    ctx.fillText('supply ↓', chartArea.left + 2, yZero + 11);
    ctx.restore();
  }
};

const wxNowMarkerPlugin = {
  id: 'wxNowMarker',
  afterDraw(chart) {
    const opts = (chart.options.plugins && chart.options.plugins.wxNowMarker) || {};
    if (opts.todayIndex == null) return;
    const { ctx, chartArea, scales: { x } } = chart;
    const xPos = x.getPixelForValue(opts.todayIndex);
    ctx.save();
    ctx.strokeStyle = WX_CHART_COLORS.target;
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(xPos, chartArea.top);
    ctx.lineTo(xPos, chartArea.bottom);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.font = "700 9px 'Share Tech Mono', monospace";
    ctx.fillStyle = WX_CHART_COLORS.target;
    ctx.textAlign = 'center';
    ctx.fillText('NOW', xPos, chartArea.top - 4);
    ctx.restore();
  }
};

const wxReferenceLinePlugin = {
  id: 'wxReferenceLine',
  afterDraw(chart) {
    const opts = (chart.options.plugins && chart.options.plugins.wxReferenceLine) || {};
    if (opts.referenceMm == null) return;
    const { ctx, chartArea, scales: { y } } = chart;
    const yPos = y.getPixelForValue(opts.referenceMm);
    if (yPos < chartArea.top || yPos > chartArea.bottom) return;
    ctx.save();
    ctx.strokeStyle = WX_CHART_COLORS.target;
    ctx.lineWidth = 1;
    ctx.setLineDash([5, 3]);
    ctx.beginPath();
    ctx.moveTo(chartArea.left, yPos);
    ctx.lineTo(chartArea.right, yPos);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.font = "600 9px 'Share Tech Mono', monospace";
    ctx.fillStyle = WX_CHART_COLORS.target;
    ctx.textAlign = 'right';
    ctx.fillText('reference ' + opts.referenceMm + 'mm', chartArea.right - 2, yPos - 4);
    ctx.restore();
  }
};

// Only label forecast rain bars above the trace-amount threshold, to avoid
// cluttering the chart with confidence percentages on near-zero days.
const wxRainProbPlugin = {
  id: 'wxRainProbLabels',
  afterDatasetsDraw(chart) {
    const opts = (chart.options.plugins && chart.options.plugins.wxRainProbLabels) || {};
    const probs = opts.probabilities || [];
    const dsIndex = chart.data.datasets.findIndex(d => d.label === 'Forecast rain');
    if (dsIndex === -1) return;
    const ds = chart.data.datasets[dsIndex];
    const meta = chart.getDatasetMeta(dsIndex);
    const { ctx } = chart;
    ds.data.forEach((val, idx) => {
      if (val === null || Math.abs(val) <= 0.5) return;
      const prob = probs[idx];
      if (!prob) return;
      const point = meta.data[idx];
      if (!point) return;
      ctx.save();
      ctx.font = "500 9px 'Share Tech Mono', monospace";
      ctx.fillStyle = WX_CHART_COLORS.rain;
      ctx.textAlign = 'center';
      ctx.fillText(prob + '%', point.x, point.y + 12);
      ctx.restore();
    });
  }
};

function renderWeatherDeficitChart(chartData, controllerId) {
  const canvas = document.getElementById('weather-deficit-chart-' + controllerId);
  const msgEl = document.getElementById('weather-chart-msg-' + controllerId);
  if (!canvas) return;

  if (!chartData) {
    canvas.style.display = 'none';
    if (msgEl) { msgEl.textContent = 'No weather cache yet.'; msgEl.style.display = ''; }
    if (weatherDeficitCharts[controllerId]) { weatherDeficitCharts[controllerId].destroy(); delete weatherDeficitCharts[controllerId]; }
    return;
  }

  canvas.style.display = '';
  if (msgEl) {
    if (chartData.forecastDays.length === 0) {
      msgEl.textContent = 'No forecast data available.';
      msgEl.style.display = '';
    } else if (chartData.pastDays.length === 0) {
      msgEl.textContent = 'No actual history yet.';
      msgEl.style.display = '';
    } else {
      msgEl.style.display = 'none';
    }
  }

  if (typeof Chart === 'undefined') return;

  const todayIndex = chartData.todayIndex;
  const maxDeficitMm = Number((zoneDoc.weather || {}).max_deficit_mm) || 25.0;
  const programLines = chartData.programLines || [];
  const allProgramDeficitValues = programLines.flatMap(pl => pl.deficitLine);
  const demandValues = chartData.et0Actual.concat(chartData.et0Forecast).map(v => v || 0).concat(allProgramDeficitValues);
  const supplyValues = chartData.irrigActual.concat(chartData.rainActual, chartData.irrigForecast, chartData.rainForecast).map(v => Math.abs(v || 0));
  const maxDemand = Math.max(12, maxDeficitMm * 1.1, ...demandValues);
  const maxSupplyMag = Math.max(10, ...supplyValues);

  // One line dataset per program -- field's A/B split (etc.) must not be
  // blended into a single deficit line, since each program waters a
  // different physical area (see buildProgramDeficitLine). A controller
  // with only one program (e.g. yard) still gets exactly one line, using
  // the same red the old single blended line used, so it looks unchanged.
  const programLineDatasets = programLines.map((pl, i) => {
    const color = WX_PROGRAM_LINE_COLORS[i % WX_PROGRAM_LINE_COLORS.length];
    return {
      label: pl.label, type: 'line', data: pl.deficitLine, borderColor: color, borderWidth: 2.5,
      pointRadius: (ctx) => todayIndex != null && ctx.dataIndex === todayIndex ? 6 : 3,
      pointBackgroundColor: color, pointBorderColor: '#fff', pointBorderWidth: 1.5,
      tension: 0.35, fill: false, order: 1,
      segment: { borderDash: (ctx) => todayIndex != null && ctx.p0DataIndex >= todayIndex ? [5, 3] : [] }
    };
  });

  const datasets = [
    { label: 'ET₀ actual', data: chartData.et0Actual, backgroundColor: WX_CHART_COLORS.et0,
      borderRadius: { topLeft: 3, topRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'demand', order: 3 },
    { label: 'ET₀ forecast', data: chartData.et0Forecast, backgroundColor: WX_CHART_COLORS.et0ForecastFill,
      borderColor: WX_CHART_COLORS.et0, borderWidth: { top: 1.5, left: 0, right: 0, bottom: 0 },
      borderRadius: { topLeft: 3, topRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'demand', order: 3 },
    { label: 'Irrigation actual', data: chartData.irrigActual, backgroundColor: WX_CHART_COLORS.irrig,
      borderRadius: { bottomLeft: 3, bottomRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'supply', order: 3 },
    { label: 'Rain actual', data: chartData.rainActual, backgroundColor: WX_CHART_COLORS.rain,
      borderRadius: { bottomLeft: 3, bottomRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'supply', order: 3 },
    { label: 'Irrigation scheduled', data: chartData.irrigForecast, backgroundColor: WX_CHART_COLORS.irrigForecastFill,
      borderColor: WX_CHART_COLORS.irrig, borderWidth: { top: 0, left: 0, right: 0, bottom: 1.5 },
      borderRadius: { bottomLeft: 3, bottomRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'supply', order: 3 },
    { label: 'Forecast rain', data: chartData.rainForecast, backgroundColor: WX_CHART_COLORS.rainForecastFill,
      borderColor: WX_CHART_COLORS.rain, borderWidth: { top: 0, left: 0, right: 0, bottom: 1.5 },
      borderRadius: { bottomLeft: 3, bottomRight: 3 }, barPercentage: 0.55, categoryPercentage: 0.75, stack: 'supply', order: 3 },
    ...programLineDatasets
  ];

  if (weatherDeficitCharts[controllerId]) { weatherDeficitCharts[controllerId].destroy(); delete weatherDeficitCharts[controllerId]; }

  weatherDeficitCharts[controllerId] = new Chart(canvas.getContext('2d'), {
    type: 'bar',
    data: { labels: chartData.categories, datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      layout: { padding: { top: 14, bottom: 4 } },
      scales: {
        x: { stacked: true, grid: { display: false }, ticks: { color: '#8ab0ca', font: { family: "'Share Tech Mono', monospace", size: 10 } } },
        y: {
          stacked: true,
          suggestedMin: -maxSupplyMag * 1.15,
          suggestedMax: maxDemand * 1.15,
          grid: { color: (ctx) => ctx.tick.value === 0 ? 'transparent' : 'rgba(42,63,84,0.5)' },
          ticks: {
            color: '#8ab0ca', font: { family: "'Share Tech Mono', monospace", size: 10 },
            callback: (val) => val === 0 ? '' : Math.abs(val) + 'mm'
          }
        }
      },
      plugins: {
        // Only worth showing once there's more than one program line to
        // tell apart -- a single-program controller (e.g. yard) stays
        // exactly as before, legend hidden.
        legend: {
          display: programLineDatasets.length > 1,
          labels: {
            filter: (item, data) => data.datasets[item.datasetIndex].type === 'line',
            color: '#8ab0ca', font: { family: "'Share Tech Mono', monospace", size: 10 }, boxWidth: 16
          }
        },
        tooltip: {
          callbacks: {
            title: (items) => items[0].label,
            label(item) {
              if (item.raw === null) return null;
              const absVal = Math.abs(item.raw);
              if (absVal === 0) return null;
              if (item.dataset.label === 'Forecast rain') {
                const prob = chartData.rainProbability[item.dataIndex];
                return ' Forecast rain: ' + absVal.toFixed(1) + 'mm' + (prob ? ' (' + prob + '% confidence)' : '');
              }
              if (item.dataset.type === 'line') return ' ' + item.dataset.label + ' deficit: ' + Number(item.raw).toFixed(1) + 'mm';
              return ' ' + item.dataset.label + ': ' + absVal.toFixed(1) + 'mm';
            },
            filter: (item) => item.raw !== null && Math.abs(item.raw) > 0
          }
        },
        wxNowMarker: { todayIndex },
        wxReferenceLine: { referenceMm: Number((zoneDoc.weather || {}).reference_deficit_mm) || null },
        wxRainProbLabels: { probabilities: chartData.rainProbability }
      }
    },
    plugins: [wxZeroLinePlugin, wxNowMarkerPlugin, wxReferenceLinePlugin, wxRainProbPlugin]
  });
}

function updateWeatherTiles(chartData, controllerId) {
  const autoAdjustEnabled = isWeatherAutoAdjustEnabled();
  const weatherSettings = zoneDoc.weather || {};
  const maxDeficit = Number(weatherSettings.max_deficit_mm) || 25.0;
  const reference = Number(weatherSettings.reference_deficit_mm) || 6.0;
  // Area-weighted average across this controller's zones -- display-only
  // summary, never fed back into scheduling (each zone uses its own deficit
  // independently; see findControllerDeficitAvg).
  const deficit = findControllerDeficitAvg(weatherState, calibrationData, controllerId);

  const deficitOut = document.getElementById('weather-deficit-out-' + controllerId);
  if (deficitOut) deficitOut.textContent = deficit.toFixed(1);
  const referenceOut = document.getElementById('weather-reference-out-' + controllerId);
  if (referenceOut) referenceOut.textContent = reference.toFixed(1);

  const gaugeFill = document.getElementById('weather-deficit-gauge-' + controllerId);
  if (gaugeFill) {
    const pct = Math.max(0, Math.min(100, (deficit / maxDeficit) * 100));
    gaugeFill.style.width = pct + '%';
    gaugeFill.style.background = deficit < reference ? '#22c55e' : (deficit < maxDeficit * 0.8 ? '#f59e0b' : '#ef4444');
  }

  const adjustOut = document.getElementById('weather-adjust-out-' + controllerId);
  if (adjustOut) {
    adjustOut.textContent = autoAdjustEnabled ? findControllerAdjustPctAvg(weatherState, controllerId) + '% avg' : 'off';
  }

  const rain24Out = document.getElementById('weather-rain24-out-' + controllerId);
  if (rain24Out) rain24Out.textContent = Number(weatherState.forecast_rain_24h_mm || 0).toFixed(1);

  const projectedEl = document.getElementById('weather-projected-out-' + controllerId);
  if (!projectedEl) return;
  if (!chartData || chartData.todayIndex == null) {
    projectedEl.textContent = '—';
    projectedEl.style.color = '';
    return;
  }
  const futureLine = chartData.deficitLine.slice(chartData.todayIndex + 1);
  const criticalIdx = futureLine.findIndex(v => v >= maxDeficit);
  if (criticalIdx === -1) {
    projectedEl.textContent = 'Safe ' + (futureLine.length || 0) + 'd';
    projectedEl.style.color = '#22c55e';
  } else {
    projectedEl.textContent = 'Critical in ' + (criticalIdx + 1) + 'd';
    projectedEl.style.color = '#ef4444';
  }
}

async function loadWeatherChart() {
  let cacheData = null;
  let logEntries = [];
  try {
    const res = await fetch(BASE_URL + '/weather_cache.json?ts=' + Date.now(), { cache: 'no-store' });
    if (res.ok) cacheData = await res.json();
  } catch (e) { console.error('loadWeatherChart cache:', e); }
  try {
    const res = await fetch(BASE_URL + '/weather_log.json?ts=' + Date.now(), { cache: 'no-store' });
    if (res.ok) {
      const logData = await res.json();
      if (logData && Array.isArray(logData.log)) logEntries = logData.log;
    }
  } catch (e) { console.error('loadWeatherChart log:', e); }

  (zoneDoc.controllers || []).forEach(controller => {
    const controllerId = controller.id;
    const toggle = document.querySelector(`.weather-include-rain-toggle[data-controller="${controllerId}"]`);
    const includeRain = toggle ? toggle.checked !== false : true;
    const chartData = buildChartData(cacheData, logEntries, includeRain, controllerId);
    weatherChartDataCaches[controllerId] = chartData;
    renderWeatherDeficitChart(chartData, controllerId);
    updateWeatherTiles(chartData, controllerId);
  });
}

// ---------- Schedule preset picker ----------
//
// Deliberately scoped to controllers.json/controllers_*.json (schedule
// presets, e.g. seasonal spring/summer/fall variants) -- this used to list
// every SPIFFS .json file, which was noisy now that site.json/weather_state
// .json/etc. are split out and irrelevant to "which schedule do I want to
// load." The FILES page (data/files.html/files.js) still has its own
// unfiltered SPIFFS browser for debugging -- don't "fix" this back to that.
async function loadSpiffsFileList() {
  try {
    const res = await fetch(BASE_URL + '/list-json-files');
    if (!res.ok) return;
    const files = await res.json();
    const sel = document.getElementById('jsonFileSelector');
    sel.options.length = 0;
    files
      .filter(f => /^controllers(_[^/\\]+)?\.json$/i.test(f))
      // controllers.json (the live schedule) always first, then presets alphabetically.
      .sort((a, b) => (a === 'controllers.json') ? -1 : (b === 'controllers.json') ? 1 : a.localeCompare(b))
      .forEach(f => {
        const opt = document.createElement('option');
        opt.value = opt.textContent = f;
        sel.add(opt);
      });
  } catch (e) { console.error('loadSpiffsFileList:', e); }
}

async function loadSpiffsZoneFile() {
  const file = document.getElementById('jsonFileSelector').value;
  if (!file) { alert('Please select a file first.'); return; }
  try {
    const res = await fetch(BASE_URL + '/load-spiffs-zone-table?filename=' + encodeURIComponent(file) + '&ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) throw new Error(await res.text());
    const loaded = await res.json();
    if (Array.isArray(loaded)) {
      // Named schedule preset (controllers-only, post-split) -- site/weather
      // config stays exactly as currently loaded, only the schedule swaps in.
      zoneDoc.controllers = loaded;
    } else {
      // Default doc or an unrelated full-doc-shaped file via the generic
      // debug file picker.
      zoneDoc = applyDocDefaults(loaded);
    }
    renderConfigPage();
    loadSiteFieldsIntoForm();
    loadWeatherSettingsIntoForm();
    renderWeatherReadOnlyStatus();
    renderAllControllerWeatherBlocks();	// controller list may differ in this file
    loadWeatherChart();
    setCurrentZoneFile(file);
    setSaveZoneFilename(file);
    setDirty(true);
    showToast('Loaded from ' + file);
  } catch (e) {
    alert('Error loading file: ' + e.message);
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
    remote.style.color = '#ef4444';
  } else if (clients === 0) {
    remote.textContent = '0 units' + sseSuffix;
    remote.style.color = '#94a3b8';
  } else if (agoSec === -1 || agoSec === undefined) {
    remote.textContent = clients + ' unit(s), starting…' + sseSuffix;
    remote.style.color = '#22c55e';
  } else if (agoSec > 60) {
    remote.textContent = clients + ' unit(s), ' + agoSec + 's ago' + sseSuffix;
    remote.style.color = '#ef4444';
  } else {
    remote.textContent = clients + ' unit(s), ' + agoSec + 's ago' + sseSuffix;
    remote.style.color = '#22c55e';
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
  sseSource.addEventListener('server-log', e => console.log('Server log:', e.data));
}

document.addEventListener('DOMContentLoaded', async () => {
  attachConfigPageHandlers();

  document.getElementById('sensor-rate-input').addEventListener('input', function () {
    updateDerivedStats(Math.max(1, parseInt(this.value) || 30));
  });

  document.getElementById('load-sd-zone-table').addEventListener('click', async () => {
    loadWeatherState();
    await loadCalibrationData();
    await loadZoneTable('/load-zone-table');
    renderAllControllerWeatherBlocks();
    loadWeatherChart();
  });
  document.getElementById('zone-form-submit').addEventListener('click', submitZoneForm);
  document.getElementById('activate-zone-schedule').addEventListener('click', activateLoadedSchedule);
  document.getElementById('loc-submit').addEventListener('click', submitLocation);
  document.getElementById('sensor-rate-submit').addEventListener('click', submitSensorRate);
  document.getElementById('load-spiffs-zone-table').addEventListener('click', loadSpiffsZoneFile);
  document.getElementById('weather-settings-submit').addEventListener('click', submitWeatherSettings);
  document.getElementById('weather-fetch-now-btn').addEventListener('click', fetchWeatherNow);
  // Per-controller "include forecast rain" toggles are wired up inside
  // renderAllControllerWeatherBlocks() itself, since those checkboxes are
  // created dynamically (one per controller), not static markup here.

  startSSE();
  window.addEventListener('pagehide', stopSSE);
  // A sleep/wake cycle can leave sseSource stuck at readyState OPEN on a
  // dead connection (see net-utils.js) -- force-recreate it and refresh the
  // data that would otherwise sit stale until the next manual reload.
  watchForWake(() => { stopSSE(); startSSE(); loadFooterStatus(); });

  // Zone/weather/calibration bootstrap -- kept in its own try/catch
  // (mirroring map.js's MAP ERROR pattern) so a failed fetch here (e.g. a
  // dead TCP session left over from an overnight idle period) surfaces
  // visibly instead of silently leaving the whole config page blank, and
  // doesn't prevent the static controls/SSE wired up above from working.
  try {
    await loadSpiffsFileList();
    await loadWeatherState();	// before loadZoneTable() so program badges render correctly on first paint
    await loadCalibrationData();	// before loadWeatherChart() -- irrigationScheduledMm() needs it
    await loadZoneTable('/load-zone-table');
    await loadFooterStatus();
    renderAllControllerWeatherBlocks();	// needs zoneDoc.controllers, populated by loadZoneTable() above
    loadWeatherChart();
  } catch (e) {
    console.error('config bootstrap:', e);
    const status = document.getElementById('system-status');
    if (status) { status.textContent = 'DATA ERROR'; status.style.color = '#ef4444'; }
  }
});
