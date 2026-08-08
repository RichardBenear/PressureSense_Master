# PressureSense

PressureSense_Master is the master controller for an irrigation pressure-monitoring and zone-scheduling system. It runs on a Seeed Studio XIAO ESP32-C6, reads a 0-100 PSI analog pressure sensor, schedules and runs sprinkler zones, and commands a pair of remote relay boards ("Yard" and "Field") over LoRa. A web UI served directly from the ESP32's SPIFFS partition provides live charting, schedule/zone configuration, a sprinkler-system map, and SD/SPIFFS file management. An on-board 3.5" SPI TFT mirrors live status, and a dedicated WebSocket feed drives a separate "Indoor unit" display.

## System overview

- **PressureSense_Master (this repo)** — the scheduler and LoRa master. Decides which zone should be active from the zone table, current pressure, and time of day; reads the pressure sensor; logs data to SD; serves the web UI; and commands the Yard/Field boards over LoRa.
- **Yard / Field remotes** (separate firmware - PressureSense_ValveNode) — dumb relay executors. They accept LoRa relay commands, switch outputs, run their own safety timers, and ACK/ERROR/STATUS back to the master (see [LoRa protocol](#lora-protocol) below).
- **Indoor unit** (separate client - PressureSense_Indoor) — connects to the master's `/ws` WebSocket and consumes the same `sensorUpdate`/`configData` JSON the CHART page uses, for a secondary indoor display (see [WebSocket protocol](#websocket-protocol) below).
- **PressureSense_App** — a Cloudflare Worker + Durable Object that relays that same `/ws` feed to the public internet. It never talks to the master directly; the **Indoor unit** acts as the bridge, holding one outbound connection to the master's local `/ws` and one outbound connection to this Worker's `/device` endpoint, forwarding frames in both directions. This repo has no code running on the master or the Indoor unit — it's purely the cloud side.

![Architecture Diagram](images/PressureSenseArch.jpg)

## Hardware

![Enclosure](images/MasterEnclosure.jpg)   
![Block Diagram](images/PressureSenseMaster.jpg)  

| Component | Notes |
| --- | --- |
| MCU | Seeed Studio XIAO ESP32-C6 |
| Pressure sensor | Analog, two-point calibrated (30 PSI @ 1.5V, 60 PSI @ 2.9V), sampled via `analogReadMilliVolts()` |
| Display | 3.5" SPI TFT, ILI9488 driver via LovyanGFX (`src/Display.h`/`.cpp`) |
| LoRa | RYLR998 module on `HardwareSerial1`, AT-command configured at boot |
| Storage | SD card (daily CSV data logs, plus original copies of calibration/weather files kept as a rollback, plus `water_waves.jpg` (background image, manually provisioned)) + SPIFFS (web UI, `site.json`/`controllers.json`, per-zone flow `calibration.json`, `weather_state.json`/`weather_cache.json`/`weather_log.json`, OTA partitions) |
| Enclosure | 3D-printable design in `FreeCAD/` (native `.FCStd` source) |

Pin assignments, sensor calibration constants, and LoRa radio parameters are all `#define`d near the top of [src/main.cpp](src/main.cpp).

## Web UI

Served from SPIFFS at the device's IP, or `http://pressure-sense.local/` via mDNS (Windows PCs need Apple's free [Bonjour Print Services](https://support.apple.com/kb/DL999) installed once for `.local` names to resolve — macOS and iOS support this natively). Five pages share `data/style.css`:

| Page | Files | Purpose |
| --- | --- | --- |
| CHART | `index.html` / `index.js` | Live pressure gauge, OK/WARN/HIGH/LOW status badge, scheduled-zone card, manual zone/program start-stop, Highcharts pressure history (today or any saved day) with an optional ET0/precipitation weather overlay, status bar (location, sample rate, seasonal adjustments, zone delays, calibration offset) |
| CONFIG | `config.html` / `config.js` | Zone schedule editor: per-controller program cards (start time, day pips, duration, end time, overlap warning) with a popover to edit start/days/zone-delay/seasonal-adjustment; zones support drag-to-reorder, inline name/PSI/run editing with live-recalculated start times, add/delete. "Save Zone Config" saves only the schedule (to `controllers.json`, or a named seasonal preset); Location, Sensor Rate, and Weather Auto-Adjust settings each save independently to `site.json`; "Set as Active Schedule" promotes a loaded preset to be the live schedule |
| CALIB | `calibration.html` / `calibration.js` | Per-zone irrigation flow calibration (head specs + SVG-measured area → `mm_per_min`) with an editable Head Catalog, import from the `zone_calibration.py` script's output, and PSI calibration (moved here from CONFIG) |
| MAP | `map.html` / `map.js` | Renders the sprinkler layout SVG, highlights whichever zone(s) are currently active, full-yard or zoomed yard-only view with pan/zoom |
| FILES | `files.html` / `files.js` | Browse/delete SPIFFS and SD card files, preview JSON/text file contents, reboot the ESP32 |

CHART and MAP get live updates over Server-Sent Events at `/events`; the Indoor unit instead uses the JSON WebSocket at `/ws`.

## Key features

- **Zone scheduling** — `controllers.json` (a bare array: controllers → programs → zones) drives automatic activation; see [Zone schedule format](#zone-schedule-format) below.
- **Overlap detection** — the CONFIG page warns when two programs on the same controller share a day and their derived time windows intersect.
- **Manual control** — start/stop an individual zone or an entire lettered program (A-D) outside the schedule, each with its own run timer.
- **Seasonal adjustment** — a per-program percentage (`seasonal_adjust_pct`) that scales that program's scheduled run times up or down.
- **Zone-to-zone delay** — a per-program pause (`zone_delay_sec`) before a chained zone's relay turns on; it shortens that zone's own watering time and never shifts the nominal schedule.
- **Weather auto-adjust** — fetches daily ET0/precipitation from Open-Meteo, tracks a per-zone soil-water deficit, and scales each zone's run time (or skips a program entirely ahead of significant forecast rain) accordingly; enabled/tuned from the CONFIG page, with a 90-day history (`weather_log.json`) chartable on the CHART page.
- **Pressure calibration** — a one-point offset (entered as "actual PSI" against the live raw reading, computed client-side and persisted in `site.json`'s `psi_offset`) layered on top of the two-point factory calibration; set from the CALIB page.
- **Pressure status** — the CHART badge, the chart point colors, and the LoRa master itself (`buildPressureStatus()` in `main.cpp`, kept in sync with `updateStatStatus()` in `index.js`) all classify the live reading against the active zone's target PSI as OK / WARN / HIGH / LOW.
- **Simulation mode** — inject a fixed PSI value in place of the ADC reading, for testing the UI/alerts without water flowing.
- **Data logging** — one CSV per day on the SD card; CHART loads the current day live or any historical day from the file picker.
- **OTA updates** — ElegantOTA, mounted on the same web server.
- **All-off safety** — `ALL_OFF` is sent to both remotes before every zone transition.
- **Pressure vs. schedule state** — zone relay control (`serviceRemoteZoneControl()`), weather auto-adjust's water-applied accumulators, and the deviation alert are all driven by the schedule (`checkActiveZone()`'s `zoneNumber`/`zoneAvgPsi`, and `manualZoneRuns[]` for manual runs), not by pressure. `ZONES_ALL_OFF_PSI`/`currentPressure` only drive the informational `allOff` field broadcast over `/ws` and the TFT gauge's color threshold. (These used to also gate the items above — pressure on this system's tank setup snaps to ~62 PSI on a refill and takes hours to decay back down, so requiring PSI to first drop below `ZONES_ALL_OFF_PSI` could delay, or with a tight enough schedule entirely skip, turning a zone's relay on. Fixed — see git history.)

## Zone schedule format

`data/controllers.json` is the single source of truth for irrigation scheduling — a bare array, read directly by the firmware (`checkActiveZone()` in `main.cpp`) and edited on the CONFIG page. Site identity, sensor rate, PSI calibration, and weather/auto-adjust settings live separately in `data/site.json`; the CONFIG page's `/load-zone-table` endpoint recombines both files into one document for rendering, but they're saved independently (schedule via `/submit-zone-form`, site settings via `/submit-site-form`):

```json
[
  {
    "id": "yard",
    "name": "Yard",
    "programs": [
      {
        "id": "A",
        "days": [0, 1, 2, 3, 4, 5, 6],
        "start": "05:00",
        "zone_delay_sec": 0,
        "seasonal_adjust_pct": 0,
        "zones": [
          { "znumber": 1, "zname": "Garage", "avgpsi": 47, "run": 45 }
        ]
      }
    ]
  }
]
```

`site.json` holds the rest:

```json
{
  "name": "Silver Creek Ranch",
  "sensor_interval_sec": 30,
  "psi_offset": 0.0,
  "latitude": 43.6, "longitude": -116.6, "timezone": "America/Denver",
  "mm_per_min_default": 0.25,
  "weather": { "enabled": true, "auto_adjust": true, "reference_deficit_mm": 6.0, "max_deficit_mm": 25.0, "max_adjust_pct": 150, "min_adjust_pct": 0, "rain_skip_threshold_mm": 6.0 }
}
```

- `days` is an integer array, `0` = Sunday … `6` = Saturday.
- A zone's actual start time is **derived**, never stored: the first zone in a program starts at the program's `start`; every later zone starts when the previous one's `run` ends. `zone_delay_sec` only pauses that zone's relay turn-on — it shortens the zone's own watering time and never shifts this schedule.
- `seasonal_adjust_pct` and `zone_delay_sec` live on the program, not per zone.
- The CONFIG page's "Load External"/"Save As" mechanism supports named schedule presets (e.g. `controllers_spring.json`, `controllers_summer.json`) alongside the live `controllers.json` — "Set as Active Schedule" promotes whichever preset is currently loaded/edited to be the live schedule.

## LoRa protocol

The master and the Yard/Field ValveNode remotes are separate firmware projects (this repo and `PressureSense_ValveNode`) that speak a shared wire protocol over LoRa. This section documents that protocol from the master's side (`initLoRa()`, `sendLoraPacket()`/`sendLoraRelayOn()`, `handleLoraReceiveLine()` in `src/main.cpp`); see `PressureSense_ValveNode/README.md` for the relay-side implementation (RadioLib/SX1262 radio driver, packet CRC verification, per-zone run timers, master-silence watchdog). This repo is the source of truth for the wire format; that repo is the source of truth for how the relay board executes it.

### Radio link

The master uses a **RYLR998** module (AT-command firmware over `HardwareSerial1`, TX/RX on `D5`/`D1`), configured at boot in `initLoRa()`. The ValveNode remotes use a different chip (SX1262 via RadioLib), but both sides are set to the same over-the-air parameters so they can talk to each other:

| Parameter | Value |
| --- | --- |
| Frequency | 915 MHz |
| Spreading factor | 8 |
| Bandwidth | 125 kHz |
| Coding rate | 4/5 |
| Preamble | 8 symbols |
| TX power | 14 dBm |
| Network ID | 18 |

| Node | LoRa address |
| --- | --- |
| Master (this repo) | 10 |
| Yard | 1 |
| Field | 2 |

Outbound packets are sent via the RYLR998's `AT+SEND=<address>,<length>,<hex-payload>` command; inbound packets arrive as `+RCV=<address>,<length>,<hex-payload>,<rssi>,<snr>` lines that `handleLoraReceiveLine()` parses.

### Packet structure

```
[Command/Status] [Relay#/Status] [Flags] [Payload...] [CRC-H] [CRC-L]
      1 byte           1 byte      1 byte    N bytes    1 byte  1 byte
```

The first 3 bytes are a fixed header; everything after is variable-length payload, followed by a 2-byte application-level CRC-16/CCITT (polynomial `0x1021`, initial value `0xFFFF`, computed over every preceding byte) — separate from the LoRa radio's own over-the-air frame integrity check, which the RYLR998/RadioLib handle transparently underneath this.

**Commands (Master → ValveNode)**, sent by `sendLoraPacket()` / `sendLoraRelayOn()`:

| Code | Name | Payload | Relay byte |
| --- | --- | --- | --- |
| 0x01 | RELAY_ON | `minutes` (0-255, 0 = no timer) + `len` + zone name (ASCII, up to 20 chars) | Relay number (1-16) |
| 0x02 | RELAY_OFF | none | Relay number |
| 0x04 | ALL_OFF | none | 0 |
| 0x05 | STATUS_REQUEST | none | 0 |
| 0x06 | RESET | none | 0 |

`ALL_OFF` is sent to both Yard and Field before every scheduled zone transition and when stopping all zones (`sendLoraAllOffToRemotes()`); `RELAY_ON` carries the run-time timer and zone name directly (older 0x03/0x08 follow-up commands are retired).

**Responses (ValveNode → Master)**, handled by `handleLoraReceiveLine()`:

| Code | Name | Payload |
| --- | --- | --- |
| 0x80 | ACK | none |
| 0x81 | ERROR | error code byte |
| 0x82 | STATUS | 2 bytes: relay state bitmask |

The master verifies each response's CRC and logs ACK/ERROR/STATUS to serial for diagnostics — it does not currently retry on a missing ACK or track sequence numbers, so a dropped response is only visible in the serial log, not surfaced to the schedule logic.

**Flags byte:**

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `LORA_FLAG_CRC` | Packet includes the trailing CRC-16 (always set) |
| 1 | `LORA_FLAG_RESPONSE_REQUIRED` | Sender expects an ACK/ERROR response (always set) |
| 2 | `LORA_FLAG_TARGET_FIELD` | Yard and Field share one LoRa channel — 0 targets Yard, 1 targets Field. Set automatically from the destination address by `loraFlagsForAddress()`. |
| 3-7 | Reserved | |

### Protocol example

Master turns on relay 3 for 5 minutes, named "Front Lawn", targeting Yard:

```
0x01 0x03 0x03 0x05 0x0A "Front Lawn" 0xXX 0xXX
 cmd  rly flg  min  len     name       CRC-H/L
```

(`flags = 0x03` = `LORA_FLAG_CRC | LORA_FLAG_RESPONSE_REQUIRED`, targeting Yard since bit 2 is 0), sent as `AT+SEND=1,<len>,<hex>`.

ValveNode responds:

```
0x80 0x00 0x03 0xXX 0xXX   // ACK, status 0, relay 3
```

## WebSocket protocol

The master serves one WebSocket endpoint, `/ws`, consumed by two sibling repos: **PressureSense_Indoor** (a touchscreen client with a direct LAN connection) and, through that same Indoor unit acting as a bridge, **PressureSense_App** (a Cloudflare Worker that fans the feed out to remote browsers and relays a narrow set of commands back in — the Worker has no path to the master except through that bridge). This section documents the wire format from the master's side (`buildSensorUpdateJson()`, `buildConfigDataJson()`, `buildManualZoneRunsJson()`, `onWsEvent()` in `src/main.cpp`); see those repos' own READMEs for how each client consumes it and where they deliberately depart from the master's own behavior.

### Server → client messages

Sent via `ws.textAll()` (broadcast to every connected client) or `client->text()` (just the requester), depending on the row below:

| `type` | When sent | Notes |
| --- | --- | --- |
| `sensorUpdate` | Broadcast every sample cycle (`sensorRateSec`); also sent to a client alone right after `WS_EVT_CONNECT` if a reading already exists | Live PSI + active-zone/schedule snapshot — see field reference below |
| `manualZoneStatus` | Broadcast after any `manualZone`/`manualProgram` command changes a run, and every sample cycle | Current manual/program runs — see field reference below. Deliberately kept out of `sensorUpdate` |
| `configData` | To the requester, reply to `getConfig` | `location`, `sampleRateSec`, `calibOffset`, `zones` (the same combined site+controllers document the CONFIG page loads over HTTP — see [Zone schedule format](#zone-schedule-format)) |
| `fileList` | To the requester, reply to `getFiles` | `sdFiles` / `spiffsFiles` arrays |
| `schedule` | To the requester, reply to `getSchedule` | `controllers` — `controllers.json` contents only, never `site.json`/`psi_offset` |
| `schedulesEnabled` | To the requester, reply to `getSchedulesEnabled`; also broadcast to every client after `setSchedulesEnabled` | `enabled` bool |
| `weatherState` | To the requester, reply to `getWeatherState` | Same shape as `GET /weather-state` (firmware-owned runtime deficit/adjust-pct/skip state) plus `type` |
| `weatherLog` | To the requester, reply to `getWeatherLog` | Same shape as `GET /weather_log.json` (`log` array, up to 90 days) plus `type` |
| `weatherCache` | To the requester, reply to `getWeatherCache` | Same shape as `GET /weather_cache.json` (raw Open-Meteo response) plus `type` |
| `calibration` | To the requester, reply to `getCalibration` | Same shape as `GET /calibration.json` (`zones` array) plus `type` |
| `weatherSettings` | To the requester, reply to `getWeatherSettings` | Narrow, read-only subset of `site.json`'s `weather` block (`auto_adjust`, `reference_deficit_mm`, `max_deficit_mm`, `rain_skip_threshold_mm`, `min_adjust_pct`, `max_adjust_pct`, `mm_per_min_default`) — never the full `site.json` |
| `ack` | To the requester, reply to `saveZones`, `deleteFile`, `reset`, `saveSchedule`, `setSchedulesEnabled`, `manualZone`, `manualProgram` | `{cmd, success, message}` (`sendWsAck()`) — `cmd` echoes back which command it's acknowledging |

### Client → server commands

Sent as `{"cmd": "...", ...}`; dispatched in `onWsEvent()`'s `WS_EVT_DATA` case:

| `cmd` | Fields | Effect |
| --- | --- | --- |
| `getConfig` | — | Replies with `configData` |
| `getFiles` | — | Replies with `fileList` |
| `getSchedule` | — | Replies with `schedule` |
| `saveSchedule` | `controllers` | Overwrites `controllers.json` only; acked |
| `getSchedulesEnabled` | — | Replies with `schedulesEnabled` |
| `setSchedulesEnabled` | `enabled` | Pause/resume automatic scheduling; acked to the requester, then `schedulesEnabled` broadcast to all clients |
| `getWeatherState` | — | Replies with `weatherState` |
| `getWeatherLog` | — | Replies with `weatherLog` |
| `getWeatherCache` | — | Replies with `weatherCache` |
| `getCalibration` | — | Replies with `calibration` |
| `getWeatherSettings` | — | Replies with `weatherSettings` |
| `manualZone` | `action` (`start`/`stop`/`stopall`), `controller`, `znumber`, `run` | Start/stop a manual zone run; acked, then `manualZoneStatus` broadcast to all clients |
| `manualProgram` | `action` (`start`/`stop`/`next`), `controller`, `program` | Start/stop/advance a manual lettered-program run; acked, then `manualZoneStatus` broadcast to all clients |
| `saveZones` | `zones: {site, weather, controllers}` | Legacy combined site+controllers save in one command; acked. Not used by the current web UI (which saves over HTTP via `/submit-site-form`/`/submit-zone-form` instead) — kept for any future WS-only client |
| `deleteFile` | `source` (`spiffs`/`sd`), `filename` | Deletes a file; acked |
| `reset` | — | Acks, then reboots the ESP32 |

Only `getSchedule`, `saveSchedule`, `getSchedulesEnabled`, `setSchedulesEnabled`, `manualZone`, `manualProgram`, `getWeatherState`, `getWeatherLog`, `getWeatherCache`, `getCalibration`, and `getWeatherSettings` are ever relayed from a remote browser through PressureSense_App and the Indoor unit back to the master — `getConfig`/`getFiles`/`saveZones`/`deleteFile`/`reset` are only reachable from a client with a direct LAN connection to `/ws`.

### `sensorUpdate` field reference

| Field | Type | Notes |
| --- | --- | --- |
| `psi` | float | Calibrated live pressure (1 decimal) |
| `rawPsi` | float | Pressure before the calibration offset |
| `sampleRateSec` | int | Current sample interval, seconds |
| `adcVoltage` | float | Raw ADC voltage behind the reading (3 decimals) |
| `zoneNumber` | string | Active zone ID; `"0"` means all zones off |
| `zoneName` | string | Active zone's human-readable name |
| `zoneAvgPsi` | float | Active zone's target PSI |
| `status` | string | `OK` / `WARN` / `HIGH` / `LOW`, from `buildPressureStatus()` (mirrors `updateStatStatus()` in `data/index.js`) |
| `controller` | string | `Yard` \| `Field` \| `OFF` |
| `days` | string | Active-days abbreviation, e.g. `"Mo We Fr"`, or `"NONE"` when idle (`formatDaysForDisplay()`) |
| `start` | string | Scheduled start `"HH:MM"`; `"00:00"` means no fixed start |
| `run` | string | Runtime in minutes |
| `remaining` | string | Minutes left in the active run, e.g. `"12m"` (`getZoneRemainingMinutes()`); `""` when not computable |
| `mapKey` | string | `lower(controller):zoneNumber`, e.g. `"yard:3"` (`buildMapKey()`) — matches the MAP page's zone-coverage keys |
| `allOff` | bool | `true` when `currentPressure >= ZONES_ALL_OFF_PSI` (59 PSI) |
| `simMode` | bool | `true` while simulation mode is injecting a synthetic PSI reading |
| `time` / `date` | string | Wall clock at the master |
| `location` | string | Site label from `site.json` |

**Don't key idle/active-zone logic off `allOff`.** It's only the informational pressure-recovery heuristic described in [Key features](#key-features) above — on this system's pressure-tank setup a refill snaps to ~62 PSI and takes hours to decay back down, so `allOff` reads "not idle" for most of a normal idle cycle. Every downstream client (Indoor, PressureSense_App) keys idle/active-zone state off `zoneNumber`/`controller` (or an empty `manualZoneStatus` runs list) instead — both had this exact bug at one point, found and fixed by auditing every repo for the same `allOff` dependency. Only the master's own PSI gauge color threshold still legitimately uses it.

### `manualZoneStatus` field reference

Sent as `{"type":"manualZoneStatus","runs":[...]}` (`buildManualZoneRunsJson()`); each entry in `runs`:

| Field | Type | Notes |
| --- | --- | --- |
| `controller` | string | `Yard` / `Field` |
| `relay` | int | Relay/zone number |
| `remainingSec` | int | Seconds left in the current zone's run |
| `totalRunMinutes` | int | That zone's configured run length |
| `program` | bool | `true` if this run is part of a lettered program rather than a single manual zone |
| `programLetter` | string | Program letter (A-D) when `program` is true, else empty |

A run is left out of `runs` while it's paused between zones during a zone-to-zone delay (`delayPending`), not just once it ends.

## Data log format

One row per sample, appended to a daily CSV on the SD card (filename from `generateDailyFilename()`):

```
readingID,date,time,psi,zoneNumber,zoneAvgPsi
```

## HTTP API

All routes are registered in `setup()` in `src/main.cpp`. Static assets (the `data/` folder) are served directly from SPIFFS at `/` — except `/water_waves.jpg`, which is served from the SD card by an explicit route instead (see table below); it was moved out of `data/`/SPIFFS because at 221KB it was consuming a large share of the mostly-full SPIFFS partition.

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/get-data-file` | GET | Fetch a CSV log file by name |
| `/water_waves.jpg` | GET | Background image for all 5 pages' `body.ag-page::before` CSS layer — served from the SD card (not SPIFFS) via the same locked/chunked pattern as `/get-data-file`; 404s cleanly if the SD card hasn't been provisioned with the file yet |
| `/get-daily-filename` | GET | Current day's log filename |
| `/list-sd-card-files`, `/list-spiffs-files`, `/list-json-files` | GET | File listings for the FILES page |
| `/get-spiffs-json-file` | GET | Read a JSON file from SPIFFS |
| `/delete-file` | GET | Delete a file from SD or SPIFFS |
| `/load-zone-table`, `/load-sd-zone-table` (compat alias), `/load-spiffs-zone-table?filename=` | GET | `site.json` + `controllers.json` recombined into one document for the CONFIG page (see [Zone schedule format](#zone-schedule-format)); `/load-spiffs-zone-table` also serves a named schedule preset or any other SPIFFS `.json` by filename |
| `/submit-zone-form?filename=` | POST | Save only the schedule — to `controllers.json` by default, or a named preset (e.g. `controllers_summer.json`) if `filename` is given |
| `/submit-site-form` | POST | Save site identity, sensor rate, and weather/auto-adjust settings to `site.json` (independent of the schedule save) |
| `/submit-calib-offset` | POST | Save just the PSI calibration offset to `site.json` (CALIB page) |
| `/calibration.json`, `/submit-calibration-form` | GET / POST | Per-zone flow calibration (`mm_per_min`, head specs, SVG area) — CALIB page |
| `/weather-state`, `/weather_cache.json`, `/weather_log.json` | GET | Firmware-computed weather auto-adjust state, the last raw Open-Meteo response, and the 90-day deficit/adjustment history |
| `/weather-fetch-now` | GET | Queue an on-demand weather fetch (runs on the next `loop()` tick, not inline, to avoid blocking the request task) |
| `/get-schedules-enabled`, `/set-schedules-enabled` | GET / POST | Pause / resume automatic scheduling |
| `/manual-zones` | GET | Current manual zone/program run status |
| `/manual-zone`, `/manual-program` | POST | Start/stop a manual zone or lettered program; `/manual-program` also takes `action: "next"` to skip a running program straight to its next zone (or stop it, if the current zone is the last one) |
| `/set-sim-pressure`, `/clear-sim` | GET | Enable / disable simulation mode |
| `/sd-usage` | GET | SD card usage + WiFi/uptime, for page footers |
| `/spiffs-usage` | GET | SPIFFS usage (total/used/free/percent) — added to diagnose SPIFFS filling up and causing save failures; check this before adding any new file to `data/` |
| `/ping` | GET | Lightweight health check: uptime, heap stats, WS client count |
| `/reset` | GET | Reboot the ESP32 |
| `/events` | SSE | Live `new-readings` push for CHART/MAP |
| `/ws` | WebSocket | `sensorUpdate` / `configData` / `manualZoneStatus` JSON feed + command interface for the Indoor unit and (via its relay bridge) PressureSense_App — see [WebSocket protocol](#websocket-protocol) below |

## Getting started

1. **Secrets** — copy `include/secrets.example.h` to `include/secrets.h` and fill in your WiFi SSID/password. `include/secrets.h` is git-ignored.
2. **Build & flash firmware**:
   ```
   pio run --target upload
   ```
3. **Upload the web UI** (everything in `data/`) to SPIFFS:
   ```
   pio run --target uploadfs
   ```
4. **Monitor serial output**:
   ```
   pio device monitor
   ```
5. Visit `http://pressure-sense.local/` (mDNS) or the IP address printed on boot. **Windows users:** `.local` hostnames aren't resolved by Windows out of the box — install Apple's free [Bonjour Print Services](https://support.apple.com/kb/DL999) once, or just use the IP address shown on the TFT/serial log. macOS and iOS resolve `.local` natively, no install needed.

Subsequent firmware updates can go over the air via ElegantOTA at `<device-ip>/update` once the device is reachable on WiFi.

### Boot sequence (serial log markers)

```
[1]  TFT display init
[1b] LoRa radio init (RYLR998 AT config)
[2]  WiFi connect + mDNS (pressure-sense.local)
[3]  SPIFFS mount
[4]  SD card mount
[5]  NTP time sync
[6]  Daily log file + saved location
[7]  Web server + OTA started
```

## Project structure

```
src/main.cpp            Firmware: WiFi/SPIFFS/SD/NTP setup, sensor reads, scheduling,
                         LoRa master protocol, HTTP/WS/SSE server, OTA
src/Display.h/.cpp      LovyanGFX TFT UI (mirrors the web CHART page on-device)
include/secrets.h       WiFi credentials (git-ignored; see secrets.example.h)
data/                   Web UI served from SPIFFS -- index/config/calibration/map/files .html + .js, style.css
data/zone-utils.js      Shared schedule math (derived start/end times, day-array matching,
                         overlap detection) used by both config.js and index.js
data/site.json          Site identity/sensor-rate/calibration/weather config -- checked in as the
data/controllers.json   default seed data so a fresh flash ships a working setup instead of an
                         empty one (see Zone schedule format above); overwritten by whatever you
                         save from the CONFIG page afterward
data/controllers_spring.json   Seasonal schedule presets -- starting points to customize via the
data/controllers_summer.json   CONFIG page's Load External/Save As, then "Set as Active Schedule"
data/controllers_fall.json     to make one live
calib/                  Zone flow calibration tooling: zone_calibration.py's own output
                         (zone_calibration.json/.txt), independent of the device's calibration.json
reference/               Git-tracked copies of files that actually live on the device's SD card
                         or SPIFFS in production, kept here as a safety net since there's no
                         automated way to regenerate/redeploy them (unlike data/ -> SPIFFS via
                         `pio run --target uploadfs`).
reference/weather_log.json     Sample of the 90-day weather deficit/adjustment history.
reference/water_waves.jpg      Background image for style.css's body.ag-page::before layer.
                         NOT in data/ (SPIFFS) -- served from the SD card instead via the
                         /water_waves.jpg route (src/main.cpp), because at 221KB it was the
                         largest single file in data/ and was pushing SPIFFS to ~96% full,
                         which was failing schedule saves. Must be manually copied to the SD
                         card's root as water_waves.jpg after any SD card swap/reformat -- there
                         is no automated deploy path for it. Do not move this file back into
                         data/ without addressing SPIFFS headroom first -- check current usage
                         via GET /spiffs-usage.
scripts/zone_calibration.py     Computes per-zone mm_per_min from head specs + SVG-measured area
scripts/dxf_to_svg.py           Converts a QCAD/AutoCAD DXF sprinkler drawing to SVG for the MAP page
scripts/remove_qcad_trial.py    Strips QCAD trial-version watermark artifacts from exported SVGs
docs/                   Architecture reference diagram (PressureSenseMaster.drawio.png)
FreeCAD/                Enclosure CAD source (enclosure.FCStd)
images/                 Reference photos
platformio.ini          Board/framework/library config (seeed_xiao_esp32c6, Arduino framework)
partitions.csv          Custom partition table (dual OTA app slots + SPIFFS)
```
