const BASE_URL = '';

const PRESSURE_POINT_COLOR = '#87bef2';
const PRESSURE_ALERT_COLOR = '#ef4444';
const PRESSURE_WARN_COLOR = '#f59e0b';
const PRESSURE_GAP_MS = 5 * 60 * 1000;
const PRESSURE_WARN_DEVIATION = 2.0;
const PRESSURE_ALERT_DEVIATION = 4.0;
const ZONE_MARKER_COLOR = 'rgba(96, 165, 250, 0.72)'; // yard, scheduled -- matches .ag-ctrl-yard (#60a5fa) on CONFIG
const ZONE_MARKER_COLOR_FIELD = 'rgba(34, 197, 94, 0.72)'; // field, scheduled -- matches .ag-ctrl-field (#22c55e) on CONFIG
const MANUAL_ZONE_MARKER_COLOR = 'rgba(147, 197, 253, 0.9)'; // yard, manual -- brighter blue (#93c5fd)
const MANUAL_ZONE_MARKER_COLOR_FIELD = 'rgba(134, 239, 172, 0.9)'; // field, manual -- brighter green (#86efac)
const DAILY_FILE_POLL_MS = 30 * 1000;

function showToast(msg, isError = false) {
  const t = document.getElementById('toast');
  document.getElementById('toast-msg').textContent = msg;
  t.style.borderLeftColor = isError ? '#ef4444' : '#22c55e';
  t.style.color = isError ? '#ef4444' : '#22c55e';
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 3500);
}

let zoneTableCache = null;
let weatherStateCache = null;
let chartMode = 'today';
let currentTodayFileName = '';
let dailyFilePollTimer = null;
let dailyFileCheckInProgress = false;
let manualZoneMarkers = [];
let manualRunsCacheInitialized = false;

Highcharts.setOptions({
  time: {
    useUTC: false
  }
});

function getChartDayRange(date = new Date()) {
  const start = new Date(date);
  start.setHours(0, 0, 0, 0);

  return {
    min: start.getTime(),
    max: start.getTime() + (24 * 60 * 60 * 1000) - 1
  };
}

function setChartDayRange() {
  const range = getChartDayRange();
  currentFullRange = range;
  chart.xAxis[0].setExtremes(range.min, range.max, false, false);
}

function getDateFromHistoryFileName(fileName) {
  const match = String(fileName || '').match(/(\d{2})(\d{2})(\d{2})\.(?:csv|txt)$/i);
  if (!match) return null;

  const [, day, month, year] = match;
  return new Date(2000 + Number(year), Number(month) - 1, Number(day), 12, 0, 0, 0);
}

function parseLocalTimestamp(datePart, timePart, fallbackDate = null) {
  const date = (datePart || '').trim().split('-').map(Number);
  const time = (timePart || '').trim().split(':').map(Number);
  if (date.length !== 3 || time.length < 2) return NaN;

  let [year, month, day] = date;
  const [hour, minute, second = 0] = time;
  const currentYear = new Date().getFullYear();

  if (fallbackDate && (year < 2020 || year > currentYear + 1)) {
    year = fallbackDate.getFullYear();
    month = fallbackDate.getMonth() + 1;
    day = fallbackDate.getDate();
  }

  return new Date(year, month - 1, day, hour, minute, second, 0).getTime();
}

function parseTimeMinutes(timeStr) {
  const match = String(timeStr || '').match(/^(\d{1,2}):(\d{2})$/);
  if (!match) return null;
  const hours = Number(match[1]);
  const minutes = Number(match[2]);
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return null;
  return hours * 60 + minutes;
}

function zoneMarkerColor(controller) {
  const c = String(controller || '').toLowerCase();
  if (c === 'field') return ZONE_MARKER_COLOR_FIELD;
  return ZONE_MARKER_COLOR;
}

function manualZoneMarkerColor(controller) {
  const c = String(controller || '').toLowerCase();
  if (c === 'field') return MANUAL_ZONE_MARKER_COLOR_FIELD;
  return MANUAL_ZONE_MARKER_COLOR;
}

function makeZonePlotLine(value, labelText = '', color = ZONE_MARKER_COLOR) {
  const line = {
    id: 'zone-' + value + '-' + labelText,
    value,
    color,
    width: 1,
    dashStyle: 'ShortDash',
    zIndex: 2
  };

  if (labelText) {
    line.label = {
      text: labelText,
      rotation: 0,
      y: 14,
      style: {
        color,
        fontSize: '10px',
        fontFamily: "'Share Tech Mono', monospace"
      }
    };
  }

  return line;
}

function makeManualZonePlotLine(value, relay, kind, color) {
  return {
    id: 'manual-' + kind + '-' + relay + '-' + value,
    value,
    color,
    width: 1,
    dashStyle: 'ShortDash',
    zIndex: 2,
    label: {
      text: 'M' + relay + (kind === 'stop' ? '▼' : '▲'),
      rotation: 0,
      y: 14,
      style: {
        color,
        fontSize: '10px',
        fontFamily: "'Share Tech Mono', monospace"
      }
    }
  };
}

function addManualZoneMarker(relay, controller, kind, ts = Date.now()) {
  const line = makeManualZonePlotLine(ts, relay, kind, manualZoneMarkerColor(controller));
  manualZoneMarkers.push(line);
  if (chartMode === 'today') chart.xAxis[0].addPlotLine(line);
}

// Retries a few times before giving up -- this is the first fetch the page
// makes, so it's the one most likely to hit a dead/half-open TCP session
// left over from an overnight idle period (see net-utils.js). A single
// failure here must not be allowed to permanently blank the zone pulldowns
// and status bars for the rest of the session.
async function loadZoneTable() {
  if (zoneTableCache) return zoneTableCache;
  let lastErr;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const res = await fetch(BASE_URL + '/load-zone-table?ts=' + Date.now(), { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      zoneTableCache = await res.json();
      return zoneTableCache;
    } catch (e) {
      lastErr = e;
      if (attempt < 2) await new Promise(r => setTimeout(r, 1000 * (attempt + 1)));
    }
  }
  throw lastErr;
}

// Firmware-computed weather_adjust_pct per zone (config.js's CONFIG page
// already reads this correctly for its own zone rows -- this chart's plot
// lines never did, so they stayed at the nominal/unadjusted schedule times
// even when weather auto-adjust was actively changing a zone's effective
// run time). Cached same as zoneTableCache -- invalidated at the same
// points (loadTodayCSV()/pageshow) rather than polled continuously.
async function loadWeatherStateForChart() {
  if (weatherStateCache) return weatherStateCache;
  try {
    const res = await fetch(BASE_URL + '/weather-state?ts=' + Date.now(), { cache: 'no-store' });
    if (res.ok) {
      const data = await res.json();
      weatherStateCache = (data && typeof data === 'object') ? data : {};
    }
  } catch (e) {
    console.error('loadWeatherStateForChart:', e);
  }
  if (!weatherStateCache) weatherStateCache = {};
  if (!Array.isArray(weatherStateCache.zones)) weatherStateCache.zones = [];
  return weatherStateCache;
}

// Traverses the combined site+controllers document directly -- derived start
// times come from zone-utils.js's getZoneDerivedStart (single canonical
// implementation, kept in agreement with firmware's checkActiveZone() cursor
// math), replacing the old flat-array "carry forward the previous zone's end
// time" hack that existed only because zones used to inherit a "00:00" start.
// Feeds getZoneDerivedStart()/applyRunAdjustments() a real weatherPctFn
// (matching config.js's renderZoneRow()) so these markers track the same
// weather- and seasonal-adjusted effective times firmware actually runs on,
// not just the nominal schedule -- previously this used the defaulted
// neutral 100% and raw zone.run, so the lines never moved when weather
// auto-adjust changed a zone's effective run/start time.
// Each calendar day's chart is rendered by its own updateZoneMarkersForDate()
// call, scoped to that day's own program.days membership -- so a program
// that runs past midnight (derived start/end minutes >= 1440) has zones
// whose marker clock-time actually falls on the NEXT calendar day, which
// that day's own render never sees (it only looks at programs scheduled on
// ITS weekday, not "yesterday's overflow"). Fixed by also re-running
// yesterday's qualifying programs here, shifted back 24h, and keeping only
// the portion that lands in today's [0, 1440) window -- the actual
// overflow. Same getZoneDerivedStart/applyRunAdjustments math either way,
// just re-anchored; see zone-utils.js's findOverlaps for the same
// day-wrap-aware pattern (`end > 1440`) applied to schedule-conflict checks.
function buildZonePlotLines(doc, date, weatherState) {
  const dayStart = new Date(date);
  dayStart.setHours(0, 0, 0, 0);
  const prevDayStart = new Date(dayStart);
  prevDayStart.setDate(prevDayStart.getDate() - 1);

  const autoAdjustEnabled = Boolean(doc.weather && doc.weather.auto_adjust);

  const lines = [];
  const startValues = new Set();
  const endValues = new Map();

  function addMarker(kind, minutesFromDayStart, znumber, color) {
    if (minutesFromDayStart < 0 || minutesFromDayStart >= 24 * 60) return;
    const value = dayStart.getTime() + minutesFromDayStart * 60 * 1000;
    if (kind === 'start') {
      lines.push(makeZonePlotLine(value, 'Z' + znumber, color));
      startValues.add(value);
    } else {
      endValues.set(value, color);
    }
  }

  // minuteOffset re-anchors scheduleDayStart's derived minutes onto dayStart:
  // 0 for today's own programs, -1440 for yesterday's (so its overflow past
  // midnight lands in today's [0, 1440) window and everything else is
  // rejected by addMarker as out of range).
  function processDay(scheduleDayStart, minuteOffset) {
    (doc.controllers || []).forEach(controller => {
      (controller.programs || []).forEach(program => {
        if (!zoneRunsOnDate(program.days, scheduleDayStart)) return;

        const color = zoneMarkerColor(controller.id);
        const weatherPctFn = zone => findZoneWeatherAdjustPct(weatherState, controller.id, zone.znumber, autoAdjustEnabled);
        program.zones.forEach((zone, idx) => {
          const startMin = getZoneDerivedStart(program, idx, weatherPctFn) + minuteOffset;
          const runMin = applyRunAdjustments(zone.run, program.seasonal_adjust_pct || 0, weatherPctFn(zone));
          addMarker('start', startMin, zone.znumber, color);
          if (runMin > 0) addMarker('end', startMin + runMin, zone.znumber, color);
        });
      });
    });
  }

  processDay(dayStart, 0);
  processDay(prevDayStart, -24 * 60);

  endValues.forEach((color, value) => {
    if (!startValues.has(value)) lines.push(makeZonePlotLine(value, '', color));
  });

  return lines;
}

async function updateZoneMarkersForDate(date) {
  try {
    const doc = await loadZoneTable();
    const weatherState = await loadWeatherStateForChart();
    const dayRange = getChartDayRange(date);
    const manualLinesForDay = manualZoneMarkers.filter(l => l.value >= dayRange.min && l.value <= dayRange.max);
    chart.xAxis[0].update({ plotLines: buildZonePlotLines(doc, date, weatherState).concat(manualLinesForDay) }, true);
  } catch (e) {
    console.error('updateZoneMarkersForDate:', e);
  }
}

function psiToOffset(psi) {
  const clamped = Math.max(0, Math.min(100, psi));
  return Math.round(157 - (clamped / 100) * 157);
}

function getPressurePointColor(psi, zoneNum, avgPsi) {
  if (Number(zoneNum) !== 0 && !isNaN(avgPsi)) {
    const deviation = Math.abs(Number(psi) - Number(avgPsi));
    if (deviation >= PRESSURE_ALERT_DEVIATION) return PRESSURE_ALERT_COLOR;
    if (deviation >= PRESSURE_WARN_DEVIATION) return PRESSURE_WARN_COLOR;
  }
  return PRESSURE_POINT_COLOR;
}

function makePressurePoint(x, psi, zoneNum, avgPsi) {
  const color = getPressurePointColor(psi, zoneNum, avgPsi);
  return {
    x,
    y: Number(psi),
    color,
    marker: {
      enabled: true,
      symbol: 'circle',
      radius: 2,
      fillColor: color
    }
  };
}

// firmware's checkActiveZone() always reports the chain-derived start for
// whatever zone is currently active (there's no more "00:00 means inherit"
// convention in the nested schema), so the live active-zone snapshot's
// `start` is already effective -- no backward scan needed.
function getEffectiveZoneStart(zone) {
  return zone && zone.start ? zone.start : '—';
}

function getZoneTimeRemaining(zone) {
  if (!zone.znumber || String(zone.controller || '').toUpperCase() === 'OFF') return '—';

  const startMin = parseTimeMinutes(getEffectiveZoneStart(zone));
  const runMin = Math.max(0, Number(zone.run) || 0);
  if (startMin === null || runMin <= 0) return '—';

  const now = new Date();
  const nowSec = now.getHours() * 3600 + now.getMinutes() * 60 + now.getSeconds();
  const remainingSec = Math.max(0, (startMin + runMin) * 60 - nowSec);
  return formatRemaining(remainingSec);
}

const chartDayRange = getChartDayRange();
let currentFullRange = chartDayRange; // the full (non-zoomed) extremes for whatever is currently loaded — today's day range, or a history file's date range

const chart = Highcharts.chart('chart-pressure', {
  chart: {
    height: 460,
    backgroundColor: 'rgba(13, 17, 23, 0.74)',
    style: { fontFamily: "'Share Tech Mono', monospace" },
    animation: false,
    zooming: {
      type: 'x'
    },
    panning: true,
    panKey: 'shift',
    selectionMarkerFill: 'rgba(96, 165, 250, 0.25)',
    scrollablePlotArea: {
      minWidth: 600
    },
    events: {
      // Highcharts' built-in Reset zoom button reverts to the axis's configured
      // min/max (today's range at page-load time) — wrong whenever a history
      // file is loaded. Restore whatever range is actually loaded instead.
      selection: function (event) {
        if (event.resetSelection) {
          event.preventDefault();
          chart.xAxis[0].setExtremes(currentFullRange.min, currentFullRange.max, true, false);
          return false;
        }
      }
    }
  },
  title: { text: null },
  xAxis: {
    type: 'datetime',
    dateTimeLabelFormats: { minute: '%H:%M' },
    min: chartDayRange.min,
    max: chartDayRange.max,
    tickInterval: 3600 * 1000,
    labels: { style: { color: '#8ab0ca', fontSize: '10px' } },
    lineColor: '#456a84',
    tickColor: '#456a84',
    gridLineColor: '#456a84'
  },
  yAxis: [{
    title: { text: null },
    min: 0,
    max: 70,
    tickInterval: 10,
    labels: {
      style: { color: '#8ab0ca', fontSize: '10px' },
      formatter: function () { return this.value + ' psi'; }
    },
    gridLineColor: '#456a84'
  }, {
    // Weather overlay axis (ET0/precipitation, mm) -- opposite side, hidden
    // until the overlay toggle is switched on; independent of the PSI axis
    // above, which stays untouched either way.
    title: { text: null },
    opposite: true,
    min: 0,
    visible: false,
    labels: {
      style: { color: '#8ab0ca', fontSize: '10px' },
      formatter: function () { return this.value + ' mm'; }
    },
    gridLineWidth: 0
  }],
  series: [{
    name: 'Pressure',
    data: [],
    color: '#0d6efd',
    lineWidth: 1.5,
    gapSize: PRESSURE_GAP_MS,
    gapUnit: 'value',
    marker: {
      enabled: true,
      symbol: 'circle',
      radius: 2,
      fillColor: PRESSURE_POINT_COLOR
    }
  }, {
    name: 'ET0',
    id: 'weather-et0',
    type: 'line',
    step: 'left',
    yAxis: 1,
    data: [],
    color: '#60a5fa',
    lineWidth: 1.5,
    visible: false,
    showInLegend: false,
    marker: { enabled: false }
  }, {
    name: 'Precipitation',
    id: 'weather-precip',
    type: 'column',
    yAxis: 1,
    data: [],
    color: 'rgba(34, 197, 94, 0.7)',
    visible: false,
    showInLegend: false
  }],
  plotOptions: {
    line: {
      animation: false,
      dataLabels: {
        enabled: false,
        format: "{y:.0f}", // No decimal places
      },
    },
  },
  legend: { enabled: false },
  credits: { enabled: false },
  tooltip: {
    backgroundColor: '#0d1117',
    borderColor: '#1e2d3d',
    style: { color: '#5ba4d9', fontFamily: "'Share Tech Mono', monospace", fontSize: '12px' },
    formatter: function () {
      const unit = this.series.options.yAxis === 1 ? 'mm' : 'PSI';
      return `<b>${Highcharts.dateFormat('%H:%M', this.x)}</b><br>${this.y.toFixed(1)} ${unit}`;
    }
  }
});

let weatherLogCache = null;
let weatherOverlayOn = false;

async function loadWeatherLog() {
  if (weatherLogCache) return weatherLogCache;
  const res = await fetch(BASE_URL + '/weather_log.json?ts=' + Date.now(), { cache: 'no-store' });
  if (!res.ok) throw new Error('HTTP ' + res.status);
  const data = await res.json();
  weatherLogCache = Array.isArray(data?.log) ? data.log : [];
  return weatherLogCache;
}

// Independent of the existing zone-marker plot lines (buildZonePlotLines/
// updateZoneMarkersForDate) -- this only ever touches yAxis[1] and the two
// weather-* series, never yAxis[0] (PSI) or the Pressure series.
async function toggleWeatherOverlay() {
  weatherOverlayOn = !weatherOverlayOn;
  const btn = document.getElementById('weather-overlay-toggle-btn');

  if (!weatherOverlayOn) {
    if (btn) btn.classList.remove('active');
    chart.get('weather-et0').setVisible(false, false);
    chart.get('weather-precip').setVisible(false, false);
    chart.yAxis[1].update({ visible: false, plotLines: [] }, false);
    chart.xAxis[0].setExtremes(currentFullRange.min, currentFullRange.max, true, false);
    return;
  }

  if (btn) btn.classList.add('active');
  try {
    const log = await loadWeatherLog();
    const et0Data = [], precipData = [];
    let minTs = Infinity, maxTs = -Infinity;
    log.forEach(rec => {
      const ts = parseLocalTimestamp(rec.date, '12:00');
      if (isNaN(ts)) return;
      et0Data.push([ts, Number(rec.et0_mm) || 0]);
      precipData.push([ts, Number(rec.precip_mm) || 0]);
      minTs = Math.min(minTs, ts);
      maxTs = Math.max(maxTs, ts);
    });
    chart.get('weather-et0').setData(et0Data, false);
    chart.get('weather-precip').setData(precipData, false);
    chart.get('weather-et0').setVisible(true, false);
    chart.get('weather-precip').setVisible(true, false);

    const referenceDeficit = Number((zoneTableCache && zoneTableCache.weather && zoneTableCache.weather.reference_deficit_mm)) || 6;
    chart.yAxis[1].update({
      visible: true,
      plotLines: [{
        id: 'weather-reference-deficit', value: referenceDeficit, color: '#8ab0ca',
        dashStyle: 'Dash', width: 1,
        label: { text: 'reference deficit', style: { color: '#8ab0ca', fontSize: '9px' } }
      }]
    }, false);

    // Daily ET0/precip is a multi-day trend, not a within-a-day signal -- the
    // daily fetch only ever reports *yesterday's* figures, so "today" alone
    // almost never has a logged record yet. Widen the visible window to
    // cover the weather log's own range (padded to include today) so
    // toggling this on doesn't look like a no-op; toggling off restores
    // whatever range TODAY/HISTORY had before (currentFullRange, untouched
    // by this).
    if (Number.isFinite(minTs) && Number.isFinite(maxTs)) {
      const todayRange = getChartDayRange(new Date());
      chart.xAxis[0].setExtremes(Math.min(minTs, todayRange.min), Math.max(maxTs, todayRange.max), true, false);
    } else {
      chart.redraw();
    }
  } catch (e) {
    console.error('toggleWeatherOverlay:', e);
    showToast('Failed to load weather log.', true);
    weatherOverlayOn = false;
    if (btn) btn.classList.remove('active');
  }
}

function updateGauge(psi) {
  const value = Number(psi);
  document.getElementById('gauge-arc').setAttribute('stroke-dashoffset', psiToOffset(psi));
  document.getElementById('gauge-val').textContent = Number.isFinite(value) ? value.toFixed(1) : '—';
}

let latestScheduledZone = {};

function updateZoneCard(zone) {
  const z = normalizeZone(zone);
  latestScheduledZone = z;
  renderActiveZoneCard();
  updateRuntimeStats();
}

// A manual run for the selected controller takes over this card entirely
// (same override the Cloudflare relay dashboard and Indoor unit display do),
// scoped to the currently-selected controller like findDisplayManualRun()'s
// other callers, for consistent precedence everywhere it's used.
function renderActiveZoneCard() {
  const run = findDisplayManualRun();
  const label = document.getElementById('zone-card-label');

  if (run) {
    label.textContent = 'MANUAL RUN ACTIVE';
    document.getElementById('current-zone-name').textContent = run.program
      ? 'PROGRAM ' + (run.programLetter || '') + ' — MANUAL RUN'
      : 'ZONE ' + run.relay + ' — MANUAL RUN';
    document.getElementById('current-zone-number').textContent = 'RELAY ' + run.relay;
    document.getElementById('current-zone-controller').textContent = run.controller || '—';
    document.getElementById('current-zone-start').textContent = formatRemaining(run.remainingSec) + ' LEFT';
    document.getElementById('current-zone-run').textContent = run.totalRunMinutes || '—';
    document.getElementById('current-zone-days').textContent = 'MANUAL';
    document.getElementById('stat-avgpsi').textContent = latestScheduledZone.avgpsi || '—';
    return;
  }

  label.textContent = 'SCHEDULED ACTIVE ZONE';
  const z = latestScheduledZone;
  document.getElementById('current-zone-name').textContent = (z.zname || '—').toUpperCase();
  document.getElementById('current-zone-number').textContent = 'ZONE ' + (z.znumber || '—');
  document.getElementById('current-zone-controller').textContent = z.controller || '—';
  document.getElementById('current-zone-start').textContent = getEffectiveZoneStart(z);
  document.getElementById('current-zone-run').textContent = z.run || '—';
  document.getElementById('current-zone-days').textContent = z.days || 'NONE';
  document.getElementById('stat-avgpsi').textContent = z.avgpsi || '—';
}

function findDisplayManualRun() {
  const controller = document.getElementById('program-controller-select').value;
  const controllerKey = String(controller || '').toLowerCase();

  const programRun = manualRunsCache.find(r =>
    r.program && String(r.controller || '').toLowerCase() === controllerKey);
  if (programRun) return programRun;

  const fallbackProgramRun = manualRunsCache.find(r => r.program);
  if (fallbackProgramRun) return fallbackProgramRun;

  for (const rowEl of document.querySelectorAll('.ag-manual-zone-row')) {
    const znumber = rowEl.querySelector('.manual-zone-select').value;
    if (!znumber) continue;
    const run = findRunByControllerRelay(controller, znumber);
    if (run) return run;
  }

  const fallbackRun = manualRunsCache.find(r => !r.program);
  return fallbackRun || null;
}

function updateRuntimeStats() {
  const run = findDisplayManualRun();
  if (run) {
    document.getElementById('stat-runtime').textContent = Number(run.totalRunMinutes) > 0 ? run.totalRunMinutes + 'm' : '—';
    document.getElementById('stat-zone-remaining').textContent = formatRemaining(Number(run.remainingSec) || 0);
    return;
  }

  const z = latestScheduledZone;
  const scheduledActive = Boolean(z.znumber) && String(z.controller || '').toUpperCase() !== 'OFF';
  document.getElementById('stat-runtime').textContent = scheduledActive && Number(z.run) > 0 ? z.run + 'm' : '—';
  document.getElementById('stat-zone-remaining').textContent = getZoneTimeRemaining(z);
}

function normalizeZone(zone) {
  if (typeof zone === 'string') {
    try {
      zone = JSON.parse(zone);
    } catch (e) {
      zone = {};
    }
  }

  zone = zone || {};
  const pick = (primary, fallback) => primary !== undefined && primary !== null ? primary : fallback;
  return {
    znumber: pick(zone.znumber, pick(zone.zoneNumber, '')),
    zname: pick(zone.zname, pick(zone.zoneName, '')),
    controller: pick(zone.controller, ''),
    days: pick(zone.days, ''),
    start: pick(zone.start, ''),
    run: pick(zone.run, ''),
    avgpsi: pick(zone.avgpsi, pick(zone.zoneAvgPsi, ''))
  };
}

function updateStatStatus(psi, avgPsi) {
  const el = document.getElementById('stat-status');
  const target = Number(avgPsi);

  if (target > 0) {
    const deviation = Number(psi) - target;
    if (Math.abs(deviation) >= PRESSURE_ALERT_DEVIATION) {
      el.textContent = deviation > 0 ? 'HIGH' : 'LOW';
      el.className = 'ag-stat-value err';
    } else if (Math.abs(deviation) >= PRESSURE_WARN_DEVIATION) {
      el.textContent = 'WARN';
      el.className = 'ag-stat-value warn';
    } else {
      el.textContent = 'OK';
      el.className = 'ag-stat-value ok';
    }
    return;
  }

  // No target available — fall back to generic safe-pressure thresholds.
  if (psi >= 35) { el.textContent = 'OK'; el.className = 'ag-stat-value ok'; }
  else if (psi >= 25) { el.textContent = 'WARN'; el.className = 'ag-stat-value warn'; }
  else { el.textContent = 'LOW'; el.className = 'ag-stat-value err'; }
}

function updateAdcVoltage(voltage) {
  const el = document.getElementById('footer-adc-voltage');
  const value = Number(voltage);
  if (!el) return;
  el.textContent = Number.isFinite(value) ? value.toFixed(2) + ' V' : '-';
}

function updateCurrentCsvFile(fileName) {
  const el = document.getElementById('chart-current-file');
  if (el) el.textContent = fileName || '—';
}

async function getCurrentDailyFilename() {
  const res = await fetch(BASE_URL + '/get-daily-filename?ts=' + Date.now(), { cache: 'no-store' });
  if (!res.ok) throw new Error('HTTP ' + res.status);
  return (await res.text()).trim();
}

let sseSource = null;

function startSSE() {
  if (sseSource) return;
  sseSource = new EventSource(BASE_URL + '/events');

  sseSource.addEventListener('open', () => {
    document.getElementById('system-status').textContent = 'ONLINE';
    document.getElementById('system-status').style.color = '';
    document.querySelector('.ag-status-dot').classList.remove('offline');
  });

  sseSource.addEventListener('new-readings', function (e) {
    const obj = JSON.parse(e.data);
    const psi = obj['Current Pressure'];
    const zone = normalizeZone(obj['Active Zone']);

    updateGauge(psi);
    updateZoneCard(zone);
    updateStatStatus(psi, zone.avgpsi);
    updateAdcVoltage(obj['ADC Voltage']);
    document.getElementById('last-read').textContent = new Date().toLocaleTimeString();

    chart.series[0].addPoint(makePressurePoint(Date.now(), psi, zone.znumber, Number(zone.avgpsi)), true, false, false);
  });

  sseSource.addEventListener('error', () => {
    document.getElementById('system-status').textContent = 'OFFLINE';
    document.getElementById('system-status').style.color = '#ef4444';
    document.querySelector('.ag-status-dot').classList.add('offline');
  });
}

function stopSSE() {
  if (sseSource) { sseSource.close(); sseSource = null; }
}

function parseAndPlotCSV(text, options = {}) {
  const lines = text.split('\n').filter(l => l.trim());
  const points = [];
  const fallbackDate = options.repairBadDates ? (options.rangeDate || new Date()) : null;

  lines.forEach(line => {
    const parts = line.split(',');
    if (parts.length < 5) return;
    const ts = parseLocalTimestamp(parts[1], parts[2], fallbackDate);
    const psi = parseFloat(parts[3]);
    const zoneNum = parseInt(parts[4], 10);
    const avgPsi = parseFloat(parts[5]);
    if (isNaN(ts) || isNaN(psi)) return;
    points.push(makePressurePoint(ts, psi, zoneNum, avgPsi));
  });

  chart.series[0].setData(points, false);

  if (options.rangeDate) {
    const range = getChartDayRange(options.rangeDate);
    currentFullRange = range;
    chart.xAxis[0].setExtremes(range.min, range.max, false, false);
  } else if (options.fitToData && points.length) {
    const range = { min: points[0].x, max: points[points.length - 1].x };
    currentFullRange = range;
    chart.xAxis[0].setExtremes(range.min, range.max, false, false);
  }

  chart.redraw();
  updateZoneMarkersForDate(options.rangeDate || new Date());

  document.getElementById('stat-filesize').textContent = Math.round(text.length / 1024) + ' KB';
}

async function loadTodayCSV(fileName = '') {
  const today = new Date();
  zoneTableCache = null;
  weatherStateCache = null;
  updateZoneMarkersForDate(today);
  try {
    if (!fileName) fileName = await getCurrentDailyFilename();
    updateCurrentCsvFile(fileName);
    const dataRes = await fetch(BASE_URL + '/get-data-file?filename=' + encodeURIComponent(fileName) + '&ts=' + Date.now(), { cache: 'no-store' });
    if (!dataRes.ok) return false;
    currentTodayFileName = fileName;
    parseAndPlotCSV(await dataRes.text(), {
      rangeDate: today,
      repairBadDates: true
    });
    return true;
  } catch (e) {
    console.error('loadTodayCSV:', e);
    return false;
  }
}

async function loadHistoryData(fileName) {
  try {
    updateCurrentCsvFile(fileName);
    const res = await fetch(BASE_URL + '/get-data-file?filename=' + encodeURIComponent(fileName) + '&ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    parseAndPlotCSV(await res.text(), {
      rangeDate: getDateFromHistoryFileName(fileName),
      fitToData: true,
      repairBadDates: true
    });
  } catch (e) { console.error('loadHistoryData:', e); }
}

async function loadHistoryFileList() {
  try {
    const res = await fetch(BASE_URL + '/list-sd-card-files');
    if (!res.ok) return;
    const files = (await res.json()).filter(f => /\.csv$/i.test(String(f)));
    const sel = document.getElementById('dateFileSelector');
    sel.options.length = 0;
    if (!files.length) {
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = 'No CSV files found';
      sel.add(opt);
      return;
    }
    files.sort().forEach(f => {
      const opt = document.createElement('option');
      opt.value = opt.textContent = f;
      sel.add(opt);
    });
  } catch (e) { console.error('loadHistoryFileList:', e); }
}

async function checkForDailyFileChange() {
  if (chartMode !== 'today' || dailyFileCheckInProgress) return;
  dailyFileCheckInProgress = true;
  try {
    const fileName = await getCurrentDailyFilename();
    if (!fileName) return;

    if (!currentTodayFileName) {
      currentTodayFileName = fileName;
      updateCurrentCsvFile(fileName);
      return;
    }

    if (fileName !== currentTodayFileName && await loadTodayCSV(fileName)) {
      loadHistoryFileList();
    }
  } catch (e) {
    console.error('checkForDailyFileChange:', e);
  } finally {
    dailyFileCheckInProgress = false;
  }
}

function startDailyFileWatcher() {
  if (dailyFilePollTimer) return;
  dailyFilePollTimer = setInterval(checkForDailyFileChange, DAILY_FILE_POLL_MS);
}

function stopDailyFileWatcher() {
  if (!dailyFilePollTimer) return;
  clearInterval(dailyFilePollTimer);
  dailyFilePollTimer = null;
}

// site.name/sensor_interval_sec/psi_offset and each program's
// seasonal_adjust_pct/zone_delay_sec now live in the one nested document
// already cached by loadZoneTable() -- no more separate endpoint fetches for
// any of these (decision #4).
function loadStatusBar(doc) {
  const site = (doc && doc.site) || {};
  document.getElementById('loc-display').textContent = site.name || '—';
  document.getElementById('sample-rate-display').textContent = (Number(site.sensor_interval_sec) || 30) + 's';
}

function loadProgramAdjustmentsStatusBar(doc) {
  const seasonalParts = [];
  const delayParts = [];
  (doc.controllers || []).forEach(c => {
    (c.programs || []).forEach(p => {
      if (p.seasonal_adjust_pct) {
        seasonalParts.push(c.id.toUpperCase() + '/' + p.id + ' ' + (p.seasonal_adjust_pct > 0 ? '+' : '') + p.seasonal_adjust_pct + '%');
      }
      if (p.zone_delay_sec) {
        delayParts.push(c.id.toUpperCase() + '/' + p.id + ' ' + p.zone_delay_sec + 's');
      }
    });
  });
  document.getElementById('seasonal-status-display').textContent = seasonalParts.length ? seasonalParts.join(', ') : '0%';
  document.getElementById('delay-status-display').textContent = delayParts.length ? delayParts.join(' - ') : '—';
}

function loadCalibOffsetStatusBar(doc) {
  const site = (doc && doc.site) || {};
  const el = document.getElementById('calib-offset-display');
  el.textContent = (site.psi_offset !== undefined && site.psi_offset !== null)
    ? Number(site.psi_offset).toFixed(1) + ' PSI' : '—';
}

let schedulesEnabled = true;

function renderSchedulesToggle() {
  const btn = document.getElementById('schedules-toggle-btn');
  if (schedulesEnabled) {
    btn.textContent = 'STOP SCHEDULES';
    btn.className = 'ag-btn ag-btn-danger ag-btn-sm';
  } else {
    btn.textContent = 'START SCHEDULES';
    btn.className = 'ag-btn ag-btn-primary ag-btn-sm';
  }
}

async function loadSchedulesEnabled() {
  try {
    const res = await fetch(BASE_URL + '/get-schedules-enabled?ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) return;
    const text = (await res.text()).trim();
    schedulesEnabled = text !== 'false' && text !== '0';
    renderSchedulesToggle();
  } catch (e) {
    console.error('loadSchedulesEnabled:', e);
  }
}

async function toggleSchedulesEnabled() {
  const next = !schedulesEnabled;
  try {
    const res = await fetch(BASE_URL + '/set-schedules-enabled?ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'text/plain' },
      body: next ? 'true' : 'false'
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok || body.success === false) throw new Error(body.message || 'Failed to update schedules setting.');
    schedulesEnabled = Boolean(body.enabled);
    renderSchedulesToggle();
    showToast(schedulesEnabled ? 'Schedules started.' : 'Schedules stopped.');

    // Stopping is certain -- no scheduled zone can be running once
    // schedulesEnabled is false, so reflect that immediately instead of
    // waiting for the next SSE 'new-readings' broadcast (up to ~30s later,
    // the sensor sample interval) to catch up. Starting doesn't get the
    // same treatment since re-enabling doesn't mean a zone is active now --
    // that's for the next real snapshot to report. Manual zone/program runs
    // are a separate display (findDisplayManualRun()) and untouched here.
    if (!schedulesEnabled) {
      updateZoneCard(normalizeZone({ znumber: '0', zname: 'OFF', controller: 'off', days: 'NONE', start: '00:00', run: '0', avgpsi: '0' }));
    }
  } catch (e) {
    console.error('toggleSchedulesEnabled:', e);
    showToast(e.message || 'Failed to update schedules setting.', true);
  }
}

function formatUptime(totalSeconds) {
  const seconds = Math.max(0, Number(totalSeconds) || 0);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return days + 'd ' + hours + 'h';
  if (hours > 0) return hours + 'h ' + minutes + 'm';
  return minutes + 'm';
}

function formatRemaining(seconds) {
  seconds = Math.max(0, Number(seconds) || 0);
  const min = Math.floor(seconds / 60);
  const sec = seconds % 60;
  return String(min).padStart(2, '0') + ':' + String(sec).padStart(2, '0');
}

let manualRunsCache = [];

// Takes `doc` explicitly rather than reading the module-level zoneTableCache
// directly -- populateManualZoneSelects() below is fire-and-forget at page
// load, racing setChartMode('today')'s loadTodayCSV(), which nulls
// zoneTableCache to force a same-tick refetch. Resolving against a doc
// captured once, right after this function's own await, keeps the result
// stable even if the global gets reset out from under it mid-flight.
function findProgramZones(doc, controllerId, programId) {
  const controller = (doc && doc.controllers || []).find(c => c.id === String(controllerId || '').toLowerCase());
  if (!controller) return [];
  const program = controller.programs.find(p => p.id === programId);
  return program ? program.zones.filter(z => Number(z.znumber) > 0) : [];
}

function findRunByControllerRelay(controller, relay) {
  return manualRunsCache.find(r =>
    String(r.controller || '').toLowerCase() === String(controller || '').toLowerCase() &&
    Number(r.relay) === Number(relay));
}

async function sendManualProgramCommand(payload) {
  const res = await fetch(BASE_URL + '/manual-program?ts=' + Date.now(), {
    method: 'POST',
    cache: 'no-store',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok || body.success === false) {
    throw new Error(body.message || 'Manual program command failed');
  }
  return body;
}

async function sendManualZoneCommand(payload) {
  const res = await fetch(BASE_URL + '/manual-zone?ts=' + Date.now(), {
    method: 'POST',
    cache: 'no-store',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok || body.success === false) {
    throw new Error(body.message || 'Manual zone command failed');
  }
  return body;
}

function renderManualProgramStatus() {
  const controller = document.getElementById('program-controller-select').value;
  const run = manualRunsCache.find(r =>
    r.program && String(r.controller || '').toLowerCase() === controller.toLowerCase());
  const el = document.getElementById('manual-program-status');
  if (run) {
    el.textContent = 'RUNNING ZONE ' + run.relay + ' · ' + formatRemaining(run.remainingSec) + ' LEFT';
    el.classList.remove('muted');
  } else {
    el.textContent = 'READY';
    el.classList.add('muted');
  }
  // Start/Next toggle: while this controller's program is running, the
  // button becomes NEXT (skip to the following zone, or stop if this is the
  // last one -- the firmware's advanceManualProgramRun() handles that
  // fallthrough); it reverts to START PROGRAM once the run list no longer
  // shows it, whether via STOP or the program completing on its own.
  const startBtn = document.getElementById('start-manual-program-btn');
  if (run) {
    startBtn.textContent = 'NEXT';
    startBtn.className = 'ag-btn ag-btn-ghost ag-btn-sm';
    startBtn.dataset.mode = 'next';
  } else {
    startBtn.textContent = 'START PROGRAM';
    startBtn.className = 'ag-btn ag-btn-primary ag-btn-sm';
    startBtn.dataset.mode = 'start';
  }
}

function renderManualZoneRowStatus(rowEl) {
  const controller = document.getElementById('program-controller-select').value;
  const znumber = rowEl.querySelector('.manual-zone-select').value;
  const el = rowEl.querySelector('.manual-zone-status');
  const run = znumber ? findRunByControllerRelay(controller, znumber) : null;
  if (run) {
    el.textContent = 'RUNNING · ' + formatRemaining(run.remainingSec) + ' LEFT';
    el.classList.remove('muted');
  } else {
    el.textContent = 'READY';
    el.classList.add('muted');
  }
}

function renderAllManualStatuses() {
  renderManualProgramStatus();
  document.querySelectorAll('.ag-manual-zone-row').forEach(renderManualZoneRowStatus);
  renderActiveZoneCard();
  updateRuntimeStats();
}

// Hydrates the Controller/Letter selects from the server's own manual-run
// state -- called exactly once per page life (from refreshManualControlState,
// on load/navigation-back), never from the 5s poll or the change listeners,
// so it never fights the user while they're actively picking a different
// zone to start. Unconditional application is safe precisely because of that
// one-time-only calling convention.
function syncManualSelectionControls(runs) {
  const controllerSelect = document.getElementById('program-controller-select');
  const programSelect = document.getElementById('program-letter-select');
  if (!controllerSelect || !programSelect) return;

  const activeRuns = Array.isArray(runs) ? runs.filter(run => run) : [];
  const activeRun = activeRuns.length > 0 ? activeRuns[activeRuns.length - 1] : null;
  if (!activeRun) return;

  const desiredController = String(activeRun.controller || '').trim();
  let desiredProgram = String(activeRun.programLetter || '').trim().toUpperCase();

  // Raw (non-program) manual zone starts don't carry a programLetter from the
  // firmware -- the Controller/Letter selects are just a UI filter for
  // narrowing the zone list in that case, not data tied to the run itself.
  // Reverse-lookup which program the running relay belongs to, scoped to
  // this run's own controller (relay numbers aren't guaranteed unique across
  // controllers, each of which numbers its own LoRa relays independently).
  if (!desiredProgram && zoneTableCache && activeRun.relay !== undefined && activeRun.relay !== null) {
    const relayValue = String(activeRun.relay).trim();
    const controllerDoc = (zoneTableCache.controllers || []).find(c => c.id === desiredController.toLowerCase());
    const matchingProgram = (controllerDoc?.programs || []).flatMap(program =>
      (program.zones || []).filter(zone => String(zone.znumber) === relayValue).map(() => program.id)
    )[0];
    if (matchingProgram) desiredProgram = String(matchingProgram).trim().toUpperCase();
  }

  // <select>.value assignment is case-sensitive -- the firmware reports
  // controller lowercased (e.g. "field") while the <option value="Field">
  // markup is capitalized, so assigning the server's raw-cased string
  // silently fails (no exact match -> browser resets the select to blank).
  // Match case-insensitively but assign the matched <option>'s own value.
  if (desiredController) {
    const matchedOption = Array.from(controllerSelect.options || []).find(option =>
      String(option.value || '').trim().toLowerCase() === desiredController.toLowerCase()
    );
    if (matchedOption) controllerSelect.value = matchedOption.value;
  }

  if (desiredProgram) {
    const matchedOption = Array.from(programSelect.options || []).find(option =>
      String(option.value || '').trim().toUpperCase() === desiredProgram
    );
    if (matchedOption) programSelect.value = matchedOption.value;
  }
}

function syncManualZoneSelections(runs) {
  const activeZoneRuns = (Array.isArray(runs) ? runs : []).filter(run => !run?.program && run?.relay !== undefined && run?.relay !== null);
  if (activeZoneRuns.length === 0) return;

  const targetRelay = String(activeZoneRuns[activeZoneRuns.length - 1].relay || '').trim();
  if (!targetRelay) return;

  const rowEls = Array.from(document.querySelectorAll('.ag-manual-zone-row'));
  const targetRow = rowEls.find(rowEl => {
    const select = rowEl.querySelector('.manual-zone-select');
    return select && String(select.value || '').trim() === targetRelay;
  }) || rowEls.find(rowEl => {
    const select = rowEl.querySelector('.manual-zone-select');
    return select && !String(select.value || '').trim();
  }) || rowEls[0];

  const select = targetRow?.querySelector('.manual-zone-select');
  if (!select) return;
  const hasOption = Array.from(select.options || []).some(option => String(option.value || '').trim() === targetRelay);
  if (hasOption) {
    select.value = targetRelay;
  }
}

function runKey(run) {
  return String(run.controller || '').toLowerCase() + ':' + run.relay;
}

function applyManualRunsUpdate(newRuns) {
  const nextRuns = Array.isArray(newRuns) ? newRuns : [];

  if (!manualRunsCacheInitialized) {
    manualRunsCacheInitialized = true;
    if (nextRuns.length > 0) {
      const now = Date.now();
      nextRuns.forEach(r => addManualZoneMarker(r.relay, r.controller, 'start', now + 1000));
    }
  } else {
    // A program advancing straight to its next zone (NEXT, or a zero-delay
    // natural advance) reports the switch as a single runs-list update --
    // the old zone's "stop" and the new zone's "start" land in the same
    // diff pass. Drawing both at the exact same timestamp would stack their
    // plot lines/labels on top of each other and hide one, so the start
    // marker gets a +1s nudge -- invisible at this chart's timescale, but
    // enough to keep both legible.
    const now = Date.now();
    const newKeys = new Set(nextRuns.map(runKey));
    const oldKeys = new Set(manualRunsCache.map(runKey));
    manualRunsCache.forEach(r => { if (!newKeys.has(runKey(r))) addManualZoneMarker(r.relay, r.controller, 'stop', now); });
    nextRuns.forEach(r => { if (!oldKeys.has(runKey(r))) addManualZoneMarker(r.relay, r.controller, 'start', now + 1000); });
  }
  manualRunsCache = nextRuns;
  renderAllManualStatuses();
}

function applyZoneDefaultRunTime(doc, rowEl) {
  const znumber = rowEl.querySelector('.manual-zone-select').value;
  const controller = document.getElementById('program-controller-select').value;
  const program = document.getElementById('program-letter-select').value;
  const zone = findProgramZones(doc, controller, program).find(z => String(z.znumber) === String(znumber));
  if (!zone) return;
  const runSelect = rowEl.querySelector('.manual-zone-run');
  if ([...runSelect.options].some(o => o.value === String(zone.run))) {
    runSelect.value = String(zone.run);
  }
}

async function populateManualZoneSelects() {
  const doc = await loadZoneTable();
  const controllerSelect = document.getElementById('program-controller-select');
  const programSelect = document.getElementById('program-letter-select');
  if (!controllerSelect || !programSelect) return;

  const controller = controllerSelect.value;
  const program = programSelect.value;
  const matches = findProgramZones(doc, controller, program);

  document.querySelectorAll('.ag-manual-zone-row').forEach((rowEl, rowIndex) => {
    const select = rowEl.querySelector('.manual-zone-select');
    const previousValue = select.value;
    const options = ['<option value="">Select Zone</option>']
      .concat(matches.map(z => `<option value="${z.znumber}">Zone ${z.znumber} — ${z.zname}</option>`));
    select.innerHTML = options.join('');

    if (matches.some(z => String(z.znumber) === previousValue)) {
      select.value = previousValue;
    } else if (matches[rowIndex]) {
      select.value = matches[rowIndex].znumber;
    } else {
      select.value = '';
    }

    applyZoneDefaultRunTime(doc, rowEl);
  });

  renderAllManualStatuses();
}

// One-time hydration entry point -- called on page load and on a bfcache
// pageshow restore, never on the 5s poll or the controller/letter change
// listeners. Order matters: fetch real run state first, hydrate the
// Controller/Letter selects from it, THEN rebuild the zone row's options
// (scoped to the now-correct controller/program) before finally selecting
// the actually-running relay in its row -- selecting it any earlier would
// silently no-op since that <option> wouldn't exist yet.
async function refreshManualControlState() {
  // loadZoneTable() alongside the runs fetch -- syncManualSelectionControls's
  // relay->program reverse lookup reads the module-level zoneTableCache
  // directly, and the pageshow bfcache-restore call site nulls that cache
  // just before calling this, so it must be freshly (re)populated here
  // rather than assumed already loaded.
  await Promise.all([loadManualRunsStatus(), loadZoneTable()]);
  syncManualSelectionControls(manualRunsCache);
  await populateManualZoneSelects();
  syncManualZoneSelections(manualRunsCache);
  renderAllManualStatuses();
}

async function startManualProgram() {
  const controller = document.getElementById('program-controller-select').value;
  const program = document.getElementById('program-letter-select').value;
  try {
    const body = await sendManualProgramCommand({ action: 'start', controller, program });
    applyManualRunsUpdate(Array.isArray(body.status?.runs) ? body.status.runs : []);
    showToast('Manual program started.');
  } catch (e) {
    console.error('startManualProgram:', e);
    showToast(e.message || 'Failed to start manual program.', true);
  }
}

async function stopManualProgram() {
  const controller = document.getElementById('program-controller-select').value;
  try {
    const body = await sendManualProgramCommand({ action: 'stop', controller });
    applyManualRunsUpdate(Array.isArray(body.status?.runs) ? body.status.runs : []);
    showToast('Manual program stopped.');
  } catch (e) {
    console.error('stopManualProgram:', e);
    showToast(e.message || 'Failed to stop manual program.', true);
  }
}

async function nextManualProgram() {
  const controller = document.getElementById('program-controller-select').value;
  try {
    const body = await sendManualProgramCommand({ action: 'next', controller });
    applyManualRunsUpdate(Array.isArray(body.status?.runs) ? body.status.runs : []);
    showToast(body.message || 'Advanced to next zone.');
  } catch (e) {
    console.error('nextManualProgram:', e);
    showToast(e.message || 'Failed to advance program.', true);
  }
}

function startOrNextManualProgram() {
  const btn = document.getElementById('start-manual-program-btn');
  if (btn.dataset.mode === 'next') nextManualProgram();
  else startManualProgram();
}

async function startManualZoneRow(rowEl) {
  const controller = document.getElementById('program-controller-select').value;
  const znumber = rowEl.querySelector('.manual-zone-select').value;
  const run = rowEl.querySelector('.manual-zone-run').value;
  if (!znumber) { showToast('Please select a zone.', true); return; }
  try {
    const body = await sendManualZoneCommand({ action: 'start', controller, znumber, run });
    applyManualRunsUpdate(Array.isArray(body.status?.runs) ? body.status.runs : []);
    showToast('Zone ' + znumber + ' started.');
  } catch (e) {
    console.error('startManualZoneRow:', e);
    showToast(e.message || 'Failed to start zone.', true);
  }
}

async function stopManualZoneRow(rowEl) {
  const controller = document.getElementById('program-controller-select').value;
  const znumber = rowEl.querySelector('.manual-zone-select').value;
  if (!znumber) { showToast('Please select a zone.', true); return; }
  try {
    const body = await sendManualZoneCommand({ action: 'stop', controller, znumber });
    applyManualRunsUpdate(Array.isArray(body.status?.runs) ? body.status.runs : []);
    showToast('Zone ' + znumber + ' stopped.');
  } catch (e) {
    console.error('stopManualZoneRow:', e);
    showToast(e.message || 'Failed to stop zone.', true);
  }
}

async function loadManualRunsStatus() {
  try {
    const res = await fetch(BASE_URL + '/manual-zones?ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) return;
    const status = await res.json();
    applyManualRunsUpdate(Array.isArray(status?.runs) ? status.runs : []);
  } catch (e) {
    console.error('loadManualRunsStatus:', e);
  }
}

function tickRuntimeCountdowns() {
  manualRunsCache.forEach(r => { r.remainingSec = Math.max(0, Number(r.remainingSec) - 1); });
  renderAllManualStatuses();
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

function setChartMode(mode) {
  chartMode = mode;
  const histSel = document.getElementById('history-selector');
  chart.series[0].setData([], false);
  if (mode === 'today') {
    setChartDayRange();
    updateZoneMarkersForDate(new Date());
  }
  chart.redraw();

  if (mode === 'today') {
    histSel.classList.add('hidden');
    loadTodayCSV().then(() => {
      startSSE();
      startDailyFileWatcher();
    });
  } else if (mode === 'history') {
    histSel.classList.remove('hidden');
    stopSSE();
    stopDailyFileWatcher();
    loadHistoryFileList();
  }

  requestAnimationFrame(() => chart.reflow());
}

document.addEventListener('DOMContentLoaded', async () => {
  // Start live PSI/chart streaming immediately -- this must not be gated on
  // the zone-table/status-bar bootstrap below, or a single failed fetch
  // there (e.g. a dead TCP session left over from an overnight idle period)
  // would silently take live data down with everything else. loadTodayCSV()
  // (called by setChartMode) already has its own try/catch.
  setChartMode('today');

  document.getElementById('schedules-toggle-btn').addEventListener('click', toggleSchedulesEnabled);

  // Scoped to [data-range] specifically -- the weather overlay toggle below
  // shares .ag-range-btn for visual consistency but is an independent
  // on/off switch, not a member of this TODAY/HISTORY mutually-exclusive set.
  document.querySelectorAll('.ag-range-btn[data-range]').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.ag-range-btn[data-range]').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      setChartMode(btn.dataset.range);
    });
  });

  document.getElementById('weather-overlay-toggle-btn').addEventListener('click', toggleWeatherOverlay);

  document.getElementById('loadHistoryBtn').addEventListener('click', () => {
    const sel = document.getElementById('dateFileSelector');
    if (sel.value) loadHistoryData(sel.value);
    else alert('Please select a file.');
  });

  document.getElementById('start-manual-program-btn').addEventListener('click', startOrNextManualProgram);
  document.getElementById('stop-manual-program-btn').addEventListener('click', stopManualProgram);
  document.getElementById('program-controller-select').addEventListener('change', populateManualZoneSelects);
  document.getElementById('program-letter-select').addEventListener('change', populateManualZoneSelects);

  document.querySelectorAll('.ag-manual-zone-row').forEach(rowEl => {
    rowEl.querySelector('.manual-zone-select').addEventListener('change', () => {
      applyZoneDefaultRunTime(zoneTableCache, rowEl);
      renderManualZoneRowStatus(rowEl);
      updateRuntimeStats();
    });
    rowEl.querySelector('.manual-zone-start-btn').addEventListener('click', () => startManualZoneRow(rowEl));
    rowEl.querySelector('.manual-zone-stop-btn').addEventListener('click', () => stopManualZoneRow(rowEl));
  });

  const chartEl = document.getElementById('chart-pressure');
  if (window.ResizeObserver && chartEl) {
    new ResizeObserver(() => chart.reflow()).observe(chartEl);
  }
  window.addEventListener('resize', () => chart.reflow());
  // In addition to stopSSE() being called on the TODAY/HISTORY toggle,
  // close it on navigation away too -- see /sd-usage's sdCardLock fix for
  // why stale SSE connections piling up during rapid page switching matters.
  window.addEventListener('pagehide', stopSSE);

  // A sleep/wake cycle can leave sseSource stuck at readyState OPEN on a
  // dead connection (see net-utils.js) -- force-recreate it and refresh the
  // data that would otherwise sit stale until the next manual reload.
  watchForWake(() => {
    if (chartMode === 'today') { stopSSE(); startSSE(); }
    loadFooterStatus();
    loadManualRunsStatus();
  });

  // Zone table / status bars / manual-run polling -- kept in its own
  // try/catch (mirroring map.js's MAP ERROR pattern) so a failed fetch here
  // surfaces visibly instead of silently leaving the pulldowns and status
  // bars blank forever, and can't take the live SSE stream started above
  // down with it.
  try {
    const doc = await loadZoneTable();
    loadStatusBar(doc);
    loadProgramAdjustmentsStatusBar(doc);
    loadCalibOffsetStatusBar(doc);
    await loadSchedulesEnabled();
    await loadFooterStatus();
    await refreshManualControlState();
    const dataStatus = document.getElementById('footer-data-status');
    if (dataStatus) { dataStatus.textContent = 'OK'; dataStatus.style.color = '#22c55e'; }
  } catch (e) {
    console.error('dashboard bootstrap:', e);
    const dataStatus = document.getElementById('footer-data-status');
    if (dataStatus) { dataStatus.textContent = 'ERROR'; dataStatus.style.color = '#ef4444'; }
  }
  setInterval(loadManualRunsStatus, 5000);
  setInterval(tickRuntimeCountdowns, 1000);
});

window.addEventListener('pageshow', event => {
  if (!event.persisted) return;
  zoneTableCache = null;
  weatherStateCache = null;
  updateZoneMarkersForDate(new Date());
  if (chartMode === 'today') loadTodayCSV();
  refreshManualControlState();
});
