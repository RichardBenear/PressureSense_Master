const BASE_URL = '';

function showToast(msg, isError = false) {
  const t = document.getElementById('toast');
  document.getElementById('toast-msg').textContent = msg;
  t.style.borderLeftColor = isError ? '#f87171' : '#34d873';
  t.style.color = isError ? '#f87171' : '#34d873';
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 3500);
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(1) + ' MB';
}

function renderFileEntry(name, size) {
  const sizeStr = size !== undefined ? formatBytes(size) : '—';
  return `<div class="ag-file-entry">
    <span class="ag-file-name">${name}</span>
    <span class="ag-file-size">${sizeStr}</span>
  </div>`;
}

function normFiles(data) {
  if (Array.isArray(data)) return data;
  if (data && Array.isArray(data.files)) return data.files;
  return [];
}

function getFileName(file) {
  return typeof file === 'string' ? file : file.name;
}

function isHiddenSystemFile(name) {
  return String(name || '').replace(/^\/+/, '').toLowerCase() === 'system volume information';
}

function isJsonFile(name) {
  return String(name || '').toLowerCase().endsWith('.json');
}

async function loadSpiffsFiles() {
  const list = document.getElementById('spiffsFileList');
  const sel = document.getElementById('deleteSPIFFSFileSelector');
  const viewSel = document.getElementById('spiffsJsonFileSelector');
  try {
    const res = await fetch(BASE_URL + '/list-spiffs-files');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const files = normFiles(await res.json()).filter(f => !isHiddenSystemFile(getFileName(f)));

    if (!files.length) {
      list.innerHTML = '<div class="ag-file-empty">No files found.</div>';
      sel.options.length = 0;
      viewSel.options.length = 0;
      return;
    }

    list.innerHTML = '';
    sel.options.length = 0;
    viewSel.options.length = 0;
    files.forEach(f => {
      const name = getFileName(f);
      const size = typeof f === 'object' ? f.size : undefined;
      list.insertAdjacentHTML('beforeend', renderFileEntry(name, size));
      const opt = document.createElement('option');
      opt.value = opt.textContent = name;
      sel.add(opt);

      if (isJsonFile(name)) {
        const viewOpt = document.createElement('option');
        viewOpt.value = viewOpt.textContent = name;
        viewSel.add(viewOpt);
      }
    });
  } catch (e) {
    console.error('loadSpiffsFiles:', e);
    list.innerHTML = '<div class="ag-file-empty">Error loading files.</div>';
  }
}

async function loadSdFiles() {
  const list = document.getElementById('sdCardFileList');
  const delSel = document.getElementById('deleteSdFileSelector');
  const viewSel = document.getElementById('sdFileSelector');
  try {
    const res = await fetch(BASE_URL + '/list-sd-card-files');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const files = normFiles(await res.json()).filter(f => !isHiddenSystemFile(getFileName(f)));

    if (!files.length) {
      list.innerHTML = '<div class="ag-file-empty">No files found.</div>';
      delSel.options.length = 0;
      viewSel.options.length = 0;
      return;
    }

    list.innerHTML = '';
    delSel.options.length = 0;
    viewSel.options.length = 0;
    files.forEach(f => {
      const name = getFileName(f);
      const size = typeof f === 'object' ? f.size : undefined;
      list.insertAdjacentHTML('beforeend', renderFileEntry(name, size));
      [delSel, viewSel].forEach(sel => {
        const opt = document.createElement('option');
        opt.value = opt.textContent = name;
        sel.add(opt);
      });
    });
  } catch (e) {
    console.error('loadSdFiles:', e);
    list.innerHTML = '<div class="ag-file-empty">Error loading files.</div>';
  }
}

async function deleteSpiffsFile() {
  const file = document.getElementById('deleteSPIFFSFileSelector').value;
  if (!file) { alert('Please select a file to delete.'); return; }
  if (!confirm(`Delete SPIFFS file: ${file}?`)) return;
  try {
    const res = await fetch(BASE_URL + '/delete-file?filename=' + encodeURIComponent(file));
    const text = await res.text();
    showToast(text || 'File deleted.');
    await loadSpiffsFiles();
  } catch (e) {
    showToast('Failed to delete file.', true);
  }
}

async function deleteSdFile() {
  const file = document.getElementById('deleteSdFileSelector').value;
  if (!file) { alert('Please select a file to delete.'); return; }
  if (!confirm(`Delete SD file: ${file}?`)) return;
  try {
    const res = await fetch(BASE_URL + '/delete-file?filename=' + encodeURIComponent(file));
    const text = await res.text();
    showToast(text || 'File deleted.');
    await loadSdFiles();
  } catch (e) {
    showToast('Failed to delete file.', true);
  }
}

async function showSpiffsFileContents() {
  const file = document.getElementById('spiffsJsonFileSelector').value;
  const pre = document.getElementById('fileContents');
  if (!file) { alert('Please select a SPIFFS JSON file.'); return; }
  pre.textContent = 'Loading...';
  try {
    const res = await fetch(BASE_URL + '/get-spiffs-json-file?filename=' + encodeURIComponent(file) + '&ts=' + Date.now(), { cache: 'no-store' });
    if (!res.ok) throw new Error(await res.text());
    const text = await res.text();
    try {
      pre.textContent = JSON.stringify(JSON.parse(text), null, 2);
    } catch (e) {
      pre.textContent = text;
    }
  } catch (e) {
    pre.textContent = 'Error loading SPIFFS file: ' + e.message;
  }
}

async function showSdFileContents() {
  const file = document.getElementById('sdFileSelector').value;
  const pre = document.getElementById('fileContents');
  if (!file) { alert('Please select a file.'); return; }
  pre.textContent = 'Loading...';
  try {
    const res = await fetch(BASE_URL + '/get-data-file?filename=' + encodeURIComponent(file));
    if (!res.ok) throw new Error('HTTP ' + res.status);
    pre.textContent = await res.text();
  } catch (e) {
    pre.textContent = 'Error loading file: ' + e.message;
  }
}

async function resetESP32() {
  if (!confirm('Reset the ESP32? The device will reboot and the page will reconnect in ~12 seconds.')) return;
  try {
    await fetch(BASE_URL + '/reset');
  } catch (e) {
    // Disconnect on reset is expected
  }
  showToast('ESP32 is rebooting. Reconnecting in 12 seconds…');
  setTimeout(() => location.reload(), 12000);
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

async function loadFooterStatus() {
  try {
    const res = await fetch(BASE_URL + '/sd-usage');
    if (!res.ok) return;
    const usage = await res.json();
    const percent = Number(usage.percent || 0).toFixed(1);
    const footer = document.getElementById('footer-sd');
    const wifi = document.getElementById('footer-wifi');
    const uptime = document.getElementById('footer-uptime');
    const sdFree = document.getElementById('sd-free');
    const uptimeDisplay = document.getElementById('uptime-display');
    if (footer) footer.textContent = percent + '% full';
    if (wifi) wifi.textContent = usage.wifi || '—';
    if (uptime) uptime.textContent = formatUptime(usage.uptimeSec);
    if (sdFree) sdFree.textContent = formatBytes(Number(usage.free || 0));
    if (uptimeDisplay) uptimeDisplay.textContent = formatUptime(usage.uptimeSec);
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
  sseSource.addEventListener('server-log', e => console.log('Server log:', e.data));
}

document.addEventListener('DOMContentLoaded', async () => {
  await Promise.all([loadSpiffsFiles(), loadSdFiles(), loadFooterStatus()]);

  document.getElementById('refresh-spiffs').addEventListener('click', loadSpiffsFiles);
  document.getElementById('refresh-sd').addEventListener('click', async () => {
    await loadSdFiles();
    await loadFooterStatus();
  });
  document.getElementById('deleteSPIFFSFileBtn').addEventListener('click', deleteSpiffsFile);
  document.getElementById('deleteSdFileBtn').addEventListener('click', deleteSdFile);
  document.getElementById('showSpiffsFileContentsBtn').addEventListener('click', showSpiffsFileContents);
  document.getElementById('showSdFileContentsBtn').addEventListener('click', showSdFileContents);
  document.getElementById('resetBtn').addEventListener('click', resetESP32);

  startSSE();
  window.addEventListener('pagehide', stopSSE);
  // A sleep/wake cycle can leave sseSource stuck at readyState OPEN on a
  // dead connection (see net-utils.js) -- force-recreate it and refresh the
  // data that would otherwise sit stale until the next manual reload.
  watchForWake(() => { stopSSE(); startSSE(); loadFooterStatus(); });
});
