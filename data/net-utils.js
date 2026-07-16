// Shared network-resilience helpers. Loaded first (before any page script)
// on every page so both guards below are active before the first fetch().
//
// Root cause this guards against: after the PC sleeps and wakes, the browser
// can be left holding a fetch()/EventSource whose underlying TCP connection
// died silently while the machine was suspended -- no FIN/RST ever arrived,
// so there's nothing to make the browser notice on its own. A fetch()
// awaiting that dead connection then hangs forever instead of rejecting,
// and an EventSource can sit at readyState OPEN without ever reconnecting,
// which is why page loads (e.g. /load-zone-table) and the live SSE stream
// both go silent together after a sleep/wake cycle.

// 1. No fetch() call is allowed to hang past FETCH_TIMEOUT_MS. Callers that
// already pass their own `signal` are left alone. A timeout still rejects
// the promise, so existing try/catch call sites recover on their own (and
// polling loops simply retry on their next tick) -- this only bounds the
// wait, it doesn't change any call site's behavior on success.
const FETCH_TIMEOUT_MS = 10000;
const _nativeFetch = window.fetch.bind(window);
window.fetch = function (input, init) {
  init = init || {};
  if (init.signal) return _nativeFetch(input, init);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS);
  return _nativeFetch(input, Object.assign({}, init, { signal: controller.signal }))
    .finally(() => clearTimeout(timer));
};

// 2. Detects a real sleep/wake (not just an alt-tab) so callers can force a
// reconnect of anything long-lived (SSE, in this app) rather than trusting
// the browser to notice its old connection is dead. A gap shorter than
// minGapMs is treated as an ordinary visibility change and ignored.
function watchForWake(onWake, minGapMs) {
  minGapMs = minGapMs || 15000;
  let hiddenAt = null;
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { hiddenAt = Date.now(); return; }
    const gap = hiddenAt ? Date.now() - hiddenAt : 0;
    hiddenAt = null;
    if (gap >= minGapMs) onWake();
  });
  window.addEventListener('online', onWake);
}
