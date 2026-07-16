# PressureSense Web Server Hang — Coding Agent Implementation Brief

## Objective

Diagnose and correct the condition where the PressureSense ESP32 web server stops serving browser pages or API responses while the existing WebSocket connection continues delivering PSI data.

The behavior strongly suggests that the ESP32 remains connected to Wi-Fi and the existing WebSocket remains alive, but the HTTP server cannot accept or complete new requests.

The most likely causes are:

1. TCP socket exhaustion from accumulated Server-Sent Events connections.
2. Blocking HTTPS work inside an `ESPAsyncWebServer` callback.
3. Shared `static String` request-body buffers in asynchronous POST handlers.
4. Excessive filesystem, JSON, and dynamic `String` work inside async networking callbacks.
5. An SD-card lock that may remain set after an interrupted file download.
6. Heap fragmentation or shrinking maximum allocation size during long uptime.

Implement the work in the order below. Do not make all architectural changes at once. Add diagnostics first, then make the minimum high-confidence fixes.

---

# Phase 1 — Add Diagnostics Before Changing Behavior

## 1. Expand the `/ping` diagnostic response

Add the following data:

- Current free heap
- Minimum free heap since boot
- Maximum allocatable heap block
- Largest free 8-bit-capable heap block
- WebSocket client count
- SSE client count
- Wi-Fi RSSI
- Uptime
- `sdCardLock` state
- Milliseconds since the last WebSocket PSI broadcast

Add:

```cpp
#include "esp_heap_caps.h"
```

Suggested fields:

```cpp
ESP.getFreeHeap()
ESP.getMinFreeHeap()
ESP.getMaxAllocHeap()
heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
ws.count()
events.count()
WiFi.RSSI()
millis()
sdCardLock
millis() - wsLastBroadcastMillis
```

Example JSON structure:

```json
{
  "status": "ok",
  "uptime_ms": 1234567,
  "free_heap": 180000,
  "min_free_heap": 132000,
  "max_alloc_heap": 84000,
  "largest_block": 82000,
  "ws_clients": 1,
  "sse_clients": 2,
  "wifi_rssi": -58,
  "sd_locked": false,
  "ws_last_broadcast_age_ms": 1250
}
```

## 2. Add periodic health logging

Every 30 or 60 seconds, print one compact health line:

```cpp
Serial.printf(
    "[HEALTH] free=%u min=%u maxAlloc=%u largest=%u WS=%u SSE=%u RSSI=%d sdLock=%d\n",
    ESP.getFreeHeap(),
    ESP.getMinFreeHeap(),
    ESP.getMaxAllocHeap(),
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
    ws.count(),
    events.count(),
    WiFi.RSSI(),
    sdCardLock
);
```

Do not print this every loop iteration.

## 3. Add SSE connection logging

Inside `events.onConnect()`, log:

- New SSE client connection
- Current SSE client count
- Whether an old client was closed because the maximum was reached

Also log WebSocket connection and disconnection counts.

## Acceptance criteria

After this phase:

- `/ping` returns all diagnostic fields.
- Serial health logs appear at a controlled interval.
- SSE and WebSocket client counts are visible.
- No functional behavior has otherwise changed.

---

# Phase 2 — Remove the Highest-Risk Blocking Handler

## 4. Eliminate synchronous weather fetching from AsyncWebServer callbacks

The normal weather fetch is already deferred to `loop()` through `weatherFetchNowRequested`.

The debug weather endpoint still appears to perform blocking TLS and HTTP work directly from an async request handler.

Do not call these directly inside an `ESPAsyncWebServer` handler:

```cpp
WiFiClientSecure
HTTPClient::begin()
HTTPClient::GET()
HTTPClient::getString()
```

Disable the debug endpoint in production or convert it to the same deferred pattern as the normal fetch.

Preferred behavior:

```cpp
void handleDebugWeatherFetch(AsyncWebServerRequest *request) {
    if (weatherFetchNowRequested) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"message\":\"Weather fetch already pending\"}");
        return;
    }

    weatherFetchNowRequested = true;

    request->send(202, "application/json",
                  "{\"ok\":true,\"message\":\"Weather fetch queued\"}");
}
```

The actual HTTPS operation must run from `serviceWeatherTask()` in `loop()`.

## Acceptance criteria

- No route callback performs a synchronous outbound HTTPS request.
- The weather request handler returns immediately.
- Weather fetch success/failure is reported later through existing status data, SSE, WebSocket, or a status endpoint.
- A stalled remote weather service cannot block the async TCP task.

---

# Phase 3 — Reduce SSE Socket Exhaustion Risk

## 5. Reduce the SSE client limit

Change:

```cpp
#define MAX_SSE_CLIENTS 6
```

to:

```cpp
#define MAX_SSE_CLIENTS 2
```

Two clients should cover the normal browser use case while leaving TCP sockets available for:

- Existing WebSocket connections
- New HTTP requests
- Static-file requests
- OTA
- Outbound weather HTTPS
- Browser reconnects

If two simultaneous browser dashboards are intentionally required, use three, but start with two.

## 6. Aggressively reject excess SSE clients

Inside `events.onConnect()`:

- Determine the current client count.
- If the count exceeds `MAX_SSE_CLIENTS`, close the oldest or newly connected excess client.
- Log the event.

Use only APIs supported by the installed `ESPAsyncWebServer` version. Do not invent unsupported methods.

## 7. Verify SSE heartbeat behavior

Keep a heartbeat, but ensure it is not excessively frequent. Approximately 8–15 seconds is reasonable.

The heartbeat should help dead TCP connections get detected, but it does not replace client-count enforcement.

## Optional later simplification

Consider replacing browser SSE with the existing WebSocket protocol so the web UI and indoor display share one real-time transport.

Do not perform this protocol conversion in the first fix unless necessary.

## Acceptance criteria

- SSE clients never grow beyond the configured limit.
- Opening and closing browser tabs repeatedly does not cause a steadily increasing client count.
- Sleeping and waking the PC does not prevent `/ping` or the main page from loading.
- The existing PSI WebSocket remains operational.

---

# Phase 4 — Fix Unsafe Shared POST Request Buffers

## 8. Remove every `static String body` used in asynchronous body handlers

This pattern is unsafe:

```cpp
static String body = "";

if (index == 0) {
    body = "";
}

for (size_t i = 0; i < len; i++) {
    body += (char)data[i];
}
```

A `static` variable is shared across all requests to that endpoint. Overlapping requests, retries, or aborted uploads can corrupt the body.

Replace it with per-request storage.

Suggested pattern:

```cpp
server.on(
    "/example",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {
        // Response is sent by the body callback.
    },
    nullptr,
    [](AsyncWebServerRequest *request,
       uint8_t *data,
       size_t len,
       size_t index,
       size_t total) {

        String *body = nullptr;

        if (index == 0) {
            body = new String();
            if (!body) {
                request->send(500, "text/plain", "Allocation failed");
                return;
            }

            body->reserve(total);
            request->_tempObject = body;
        } else {
            body = static_cast<String *>(request->_tempObject);
        }

        if (!body) {
            request->send(500, "text/plain", "Request buffer missing");
            return;
        }

        body->concat(reinterpret_cast<const char *>(data), len);

        if (index + len == total) {
            JSONVar parsed = JSON.parse(*body);

            delete body;
            request->_tempObject = nullptr;

            if (JSON.typeof(parsed) == "undefined") {
                request->send(400, "text/plain", "Invalid JSON");
                return;
            }

            // Validate and process parsed data here.

            request->send(200, "text/plain", "OK");
        }
    }
);
```

## Important safeguards

- Enforce a maximum request size before allocating.
- Reject unexpected large payloads.
- Always delete the request buffer after the last chunk.
- Investigate whether the installed library offers a disconnect cleanup callback for aborted requests.
- If available, free `request->_tempObject` when a request disconnects before completion.
- Prefer `AsyncCallbackJsonWebHandler` for ordinary JSON endpoints if it is compatible with the installed library version.

## Endpoints to inspect

Search the complete source for all occurrences of:

```cpp
static String body
```

Likely affected endpoint categories include:

- Site settings
- Controller configuration
- Schedules
- Calibration data
- Manual zones
- Manual programs
- Other JSON POST routes

## Acceptance criteria

- No asynchronous request-body callback uses a shared static buffer.
- Two nearly simultaneous POST requests cannot modify the same `String`.
- Invalid or oversized JSON receives a controlled HTTP error.
- Repeated saves do not steadily reduce `ESP.getMaxAllocHeap()`.

---

# Phase 5 — Protect the SD Streaming Lock

## 9. Prevent `sdCardLock` from remaining true indefinitely

The `/get-data-file` streaming path appears to release `sdCardLock` only when normal end-of-file is reached.

If the browser disconnects, sleeps, cancels the request, or loses Wi-Fi mid-download, the normal EOF cleanup may not execute.

Add a lock timestamp:

```cpp
unsigned long sdCardLockStartedMillis = 0;
```

Use helpers:

```cpp
bool tryLockSdCard() {
    if (sdCardLock) {
        return false;
    }

    sdCardLock = true;
    sdCardLockStartedMillis = millis();
    return true;
}

void unlockSdCard() {
    sdCardLock = false;
    sdCardLockStartedMillis = 0;
}
```

Add a conservative watchdog:

```cpp
void serviceSdCardLockWatchdog() {
    if (!sdCardLock) {
        return;
    }

    if (millis() - sdCardLockStartedMillis > 60000UL) {
        Serial.println("[SD] Releasing stale SD lock after timeout");
        unlockSdCard();
    }
}
```

Call this from `loop()`.

A proper disconnect callback is preferable. Use it if the installed `ESPAsyncWebServer` version exposes one for streamed responses.

## Important caution

Do not release the lock while a valid long transfer is still active. If legitimate downloads can exceed 60 seconds, increase the timeout or update a “last progress” timestamp from the stream callback.

A better timeout model is:

```cpp
unsigned long sdCardLastActivityMillis;
```

Update it whenever a chunk is successfully read and sent. Only release the lock after no progress for the timeout interval.

## Acceptance criteria

- Canceling a CSV download cannot permanently disable SD access.
- A stale lock is automatically recovered.
- Normal long downloads are not interrupted.
- Lock and unlock events are logged.

---

# Phase 6 — Reduce Heavy Work in Async Networking Callbacks

## 10. Identify expensive callback operations

Search all HTTP and WebSocket handlers for:

- `SPIFFS.open()`
- `SD.open()`
- `readString()`
- `JSON.parse()`
- Large JSON string concatenation
- Directory enumeration
- Weather HTTPS requests
- Long loops
- `delay()`
- LoRa functions that contain `delay()`

Potentially expensive functions include:

- `buildCombinedZoneDocJson()`
- `buildConfigDataJson()`
- `buildFileListJson()`
- Configuration saves
- SD directory enumeration
- Large SPIFFS reads
- Any LoRa send called directly from a network callback

## 11. Pay special attention to LoRa command delays

`loraSendRawCommand()` contains:

```cpp
delay(settleMs);
```

with a default around 120 ms.

If an HTTP or WebSocket callback directly triggers a LoRa send, that async callback can be blocked for 120 ms or longer, especially when several commands are issued.

Convert network-triggered LoRa actions into queued commands:

```cpp
struct PendingLoraCommand {
    bool pending;
    int address;
    uint8_t command;
    uint8_t relay;
    uint16_t minutes;
    String name;
};
```

The network handler should validate, queue, and return immediately. `loop()` should perform the actual serial command.

This is lower priority than the synchronous weather fetch but should be reviewed.

## 12. Defer large operations to `loop()`

For expensive jobs:

1. Handler validates the request.
2. Handler stores or queues the requested operation.
3. Handler returns `202 Accepted`.
4. `loop()` performs filesystem, JSON, LoRa, or network work.
5. Completion is reported through WebSocket, SSE, or a status endpoint.

Do not move every small SPIFFS read unnecessarily. Concentrate on operations that can take tens or hundreds of milliseconds.

## Acceptance criteria

- No async handler contains `delay()`.
- No async handler performs outbound HTTPS.
- Large file enumeration and large JSON rebuild operations are either bounded or deferred.
- Browser requests remain responsive during weather, SD, and LoRa activity.

---

# Phase 7 — Reduce Heap Fragmentation

## 13. Reserve dynamic strings before repeated concatenation

For large JSON builders:

```cpp
String json;
json.reserve(4096);
```

Choose a realistic size based on normal output.

Use `reserve()` for:

- File list JSON
- Combined zone/config JSON
- Weather-state JSON
- Weather-log JSON
- Manual-zone status JSON
- WebSocket update JSON

## 14. Avoid unnecessary copies

Prefer passing strings by reference where appropriate:

```cpp
const String &value
```

Avoid repeatedly returning and reparsing the same file contents inside nested lookup functions.

For example, weather processing repeatedly loads and parses calibration data for individual zones. During a once-daily weather calculation, parse calibration data once and perform all lookups against that parsed object or a compact prebuilt table.

Do not retain unsafe `JSONVar` references beyond the lifetime of the owning parsed object.

## 15. Monitor maximum allocation, not just free heap

The important warning signs are:

- `free_heap` remains fairly high
- `max_alloc_heap` or `largest_block` keeps shrinking
- Large page/API responses begin failing

Use the Phase 1 diagnostics to verify whether the changes improve long-uptime memory behavior.

## Acceptance criteria

- `ESP.getMaxAllocHeap()` remains reasonably stable over repeated page loads and configuration saves.
- No large JSON endpoint fails while total free heap still appears adequate.
- Repeated browser reconnects do not produce a downward memory trend.

---

# Phase 8 — Loop Responsiveness

## 16. Reduce the unconditional loop delay

An unconditional:

```cpp
delay(100);
```

is not likely to be the primary web-server failure because `ESPAsyncWebServer` uses separate networking tasks.

However, it delays:

- Deferred weather work
- Queued LoRa work
- Lock watchdogs
- State-machine service functions
- Cleanup and health checks

Replace it with:

```cpp
delay(1);
```

or remove it and make all periodic work `millis()`-based.

Do not create a busy loop that starves lower-priority system tasks.

## Acceptance criteria

- Loop service functions run promptly.
- CPU watchdogs remain satisfied.
- No new excessive serial logging or high CPU utilization is introduced.

---

# Recommended Implementation Order

Implement and test in this exact sequence:

1. Add `/ping` and serial health diagnostics.
2. Disable or defer the blocking debug weather request.
3. Reduce and enforce the SSE client limit.
4. Test overnight sleep/wake and repeated browser reconnects.
5. Replace all shared `static String` POST buffers.
6. Add SD-lock disconnect recovery or inactivity watchdog.
7. Remove delays and large operations from async callbacks.
8. Add `String::reserve()` and reduce repeated JSON parsing.
9. Reduce the main loop delay.

Do not combine all phases into one unreviewable commit.

Suggested commits:

```text
1. Add server, socket, and heap diagnostics
2. Defer debug weather HTTPS request
3. Limit and log SSE clients
4. Use per-request POST body buffers
5. Recover interrupted SD stream locks
6. Queue LoRa work outside async callbacks
7. Reduce JSON heap churn and loop latency
```

---

# Reproduction and Verification Plan

## Test A — Repeated browser reconnects

1. Reboot the ESP32.
2. Record `/ping` values.
3. Open and close the PressureSense page 30–50 times.
4. Refresh repeatedly.
5. Verify:
   - SSE count returns to its normal value.
   - WebSocket count is correct.
   - `/ping` always responds.
   - `max_alloc_heap` does not trend sharply downward.

## Test B — PC sleep and wake

1. Open the PressureSense page.
2. Put the PC to sleep overnight or for at least 30 minutes.
3. Wake the PC.
4. Refresh the page.
5. Verify:
   - The page loads without rebooting the ESP32.
   - SSE count is at or below the limit.
   - Existing indoor-display WebSocket PSI still works.
   - New HTTP requests are accepted.

## Test C — Weather service failure

1. Temporarily use an invalid weather hostname or disconnect Internet access while local Wi-Fi remains active.
2. Trigger a weather fetch.
3. Verify:
   - The request handler returns immediately.
   - `/ping` and the main page remain responsive.
   - The weather operation times out only in `loop()`.
   - Weather status changes to an error state.

## Test D — Interrupted SD download

1. Start downloading a large CSV.
2. Cancel the download or close the browser.
3. Verify:
   - `sdCardLock` returns to false.
   - Logging resumes.
   - A second file download works.
   - `/sd-usage` remains responsive.

## Test E — Concurrent POST requests

1. Trigger two configuration saves nearly simultaneously.
2. Verify:
   - Each request has an independent body.
   - No mixed or invalid JSON is written.
   - Both requests receive deterministic responses.
   - Heap metrics recover after completion.

## Test F — Long uptime

Run for at least 24–72 hours while periodically recording:

- Free heap
- Minimum free heap
- Maximum allocatable heap
- Largest free block
- SSE count
- WebSocket count
- Wi-Fi RSSI
- SD-lock state

A stable system should not require a reboot to restore browser access.

---

# Likely Root-Cause Ranking

Based on the symptom that an existing PSI WebSocket continues working while new browser HTTP requests hang:

1. SSE clients consuming the limited TCP socket pool.
2. Blocking HTTPS work in an async web-server callback.
3. Shared static POST buffers and heap fragmentation.
4. Delayed or heavy filesystem/JSON/LoRa work in async callbacks.
5. Stale SD-card streaming lock.
6. General loop latency.

The first three items should be corrected before undertaking a broad server rewrite.

---

# Definition of Done

The fix is complete when:

- The PressureSense page loads after the PC sleeps and wakes.
- Existing and new HTTP requests work while PSI WebSocket updates continue.
- SSE and WebSocket client counts remain bounded.
- No async callback performs blocking HTTPS or deliberate delays.
- POST request buffers are request-specific.
- Interrupted SD downloads cannot leave the SD subsystem locked.
- Heap allocation metrics remain stable over repeated reconnects and long uptime.
- No ESP32 reboot is required to recover web access.
