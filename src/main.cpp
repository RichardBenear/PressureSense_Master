//*******************************
// Author: Richard Benear 6/7/2026
//*******************************

#include <Arduino.h>
#include "Display.h"
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <NTPClient.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <cmath>	// For fabs()
#include <vector>
#include <map>
#include "FS.h"
#include "SD.h"
#include "secrets.h"
#include "SPIFFS.h"

#define SD_CS D4 				//pin 22	
#define TFT_CS D9 				// pin 20
#define TFT_RST D3 				// pin 21
#define ADC_SAMPLES 10			// number of sensor ADC samples to average
#define SAMPLE_RATE 20000		// PSI sample rate 20 sec in msec
#define ZONES_ALL_OFF_PSI 59	// All Zones off if above this value
#define PRESSURE_WARN_DEVIATION 2.0f	// PSI deviation from zone target to report WARN — matches index.js
#define PRESSURE_ALERT_DEVIATION 4.0f	// PSI deviation from zone target to report HIGH/LOW — matches index.js
#define SENSOR_PIN 2			// Water Pressure sensor on pin GPIO2, ADC, D2
#define TIME_ZONE -3600 * 6		// Mountain Time
#define BUFFER_SIZE 256			// Buffer size for streaming file contents to client in chunks
#define LORA_TX_PIN D5
#define LORA_RX_PIN D1
#define LORA_RESET_PIN TFT_RST
#define LORA_BAUD 115200
#define LORA_MASTER_ADDRESS 10
#define LORA_NETWORK_ID 18
#define LORA_BAND 915000000UL
#define LORA_SPREADING_FACTOR 8
#define LORA_BANDWIDTH 7			// RYLR998 bandwidth code 7 = 125 kHz
#define LORA_CODING_RATE 1			// RYLR998 coding-rate code 1 = 4/5
#define LORA_PREAMBLE 8
#define LORA_TX_POWER_DBM 14
#define LORA_YARD_ADDRESS 1
#define LORA_FIELD_ADDRESS 2

// Since this pressure sensor is designed to run on 5.0 volts but is running
// on 3.3v here, then scale: Pressure Sensor specification:
//    Output 4.5v = 100 PSI
//    Output 0.5v = 0 PSI
//    Slope = 4.0 volts/100 PSI = 0.04 volts/PSI
// Pressure Sensor Scaled:
//    Scaled PSI Range = (3.3v-0.5v)/(5.0v-0.5v) * 100 = 62.2 PSI full range
//    Slope = delta x / delta y: 3.3-0.5-0.5/62.2 = .037 volts/PSI
#define NOMINAL_VOLTS_PER_PSI 0.037
// nominal slope in volts/PSI, based on one point calibration at 48 psi
// measured from analog gauge

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Create an Event Source on /events
AsyncEventSource events("/events");

// Unlike AsyncWebSocket (capped via ws.cleanupClients()), AsyncEventSource
// enforces no client cap at all -- a browser tab that reconnects repeatedly
// (or several tabs/devices left open) can accumulate SSE clients faster
// than the 8s heartbeat's ack-timeout reaps dead ones, eating into the
// device's fixed LWIP socket budget (CONFIG_LWIP_MAX_ACTIVE_TCP = 16) that
// the WS client, HTTP request handling, and outbound weather-API fetches
// all share too. See events.onConnect() below.
#define MAX_SSE_CLIENTS 2  //6
#define MAX_POST_BODY_BYTES 8192
#define HEALTH_LOG_INTERVAL_MS 60000UL

// Create a WebSocket on /ws for the dedicated indoor display
AsyncWebSocket ws("/ws");

HardwareSerial LoRaSerial(1);

// Timer variables
unsigned long lastTime = 0;
unsigned long timerDelay = SAMPLE_RATE;
unsigned long lastSseHeartbeatMillis = 0;

const float adcVoltageScale = 1.032f;
const float sensorCalLowPressure = 30.0f;
const float sensorCalLowVoltage = 1.5f;
const float sensorCalHighPressure = 60.0f;
const float sensorCalHighVoltage = 2.9f;
const float sensorPsiPerVolt = (sensorCalHighPressure - sensorCalLowPressure) /
															 (sensorCalHighVoltage - sensorCalLowVoltage);
const float sensorPressureIntercept = sensorCalLowPressure - (sensorPsiPerVolt * sensorCalLowVoltage);

// Simulation mode — bypasses ADC, injects a fixed PSI value
bool  simMode = false;
float simPsi  = 0.0f;

// variables
float currentPressure = 0.0;
float calibOffset = 0.0;
double ADCvoltage = 0.0;
float rawPressure = 0.0;
int sensorRateSec = 30;
bool schedulesEnabled = true;
String currentLocation  = "";   // cached from site.json's name field, updated on every save
bool   firstReadingDone = false; // guards WS initial-state push on connect

// Set by handleWeatherFetchNow(), consumed by serviceWeatherTask() on its
// next loop() tick -- the actual fetch (a blocking HTTPS call up to 10s)
// must never run directly inside an AsyncWebServer request handler, since
// those execute on the async_tcp task, which IS registered with the 5s
// task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S=5, PANIC=1) and reboots the
// device if a handler blocks past it. Deferring to serviceWeatherTask()
// reuses the same call already proven safe for the daily automatic fetch,
// which runs on loop()'s task instead.
bool weatherFetchNowRequested = false;

bool sdCardLock = false;
unsigned long sdCardLockStartedMillis = 0;

// Last successfully-read SD usage totals, served by /sd-usage whenever
// sdCardLock is held so that handler never touches the SD/SPI bus while a
// logData() write or a /get-data-file stream is mid-transaction elsewhere.
uint64_t cachedSdTotalBytes = 0;
uint64_t cachedSdUsedBytes = 0;

// millis() timestamp of the last periodic ws.textAll(buildSensorUpdateJson())
// broadcast to WS clients (Indoor/Relay units), 0 = none sent yet since boot.
// Surfaced via /sd-usage so the web UI can show remote-unit link health.
unsigned long wsLastBroadcastMillis = 0;
unsigned long lastHealthLogMillis = 0;

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", TIME_ZONE, 60000);	 // Synchronize time every minute

// Variables to save date and time
String formattedDate = "";
String currentDayStamp = "";
String currentTimeStamp = "";
String currentDailyFilename = "";

IPAddress IPmessage;

static float pressureHistory[CHART_BUFFER_SIZE];
static uint8_t zoneHistory[CHART_BUFFER_SIZE];
static int   historyCount = 0;

// Save reading number on RTC memory
RTC_DATA_ATTR int readingID = 0;

enum LoRaCommand : uint8_t {
	CMD_RELAY_ON = 0x01,
	CMD_RELAY_OFF = 0x02,
	// 0x03 reserved (formerly RUN_TIME, now folded into RELAY_ON)
	CMD_ALL_OFF = 0x04,
	CMD_STATUS_REQUEST = 0x05,
	CMD_RESET = 0x06,
	// 0x08 reserved (formerly ZONE_NAME, now folded into RELAY_ON)
};

enum LoRaResponse : uint8_t {
	RESP_ACK = 0x80,
	RESP_ERROR = 0x81,
	RESP_STATUS = 0x82,
};

static const uint8_t LORA_FLAG_CRC = 0x01;
static const uint8_t LORA_FLAG_RESPONSE_REQUIRED = 0x02;
static const uint8_t LORA_FLAG_TARGET_FIELD = 0x04;
static const uint8_t LORA_ZONE_NAME_MAX_LEN = 20;

String loraLastController = "";
uint8_t loraLastZoneNumber = 0;
bool loraSentIdleAllOff = false;
String loraRxLine = "";

bool zoneDelayPending = false;
unsigned long zoneDelayReadyAt = 0;
int zoneDelayAddress = 0;
uint8_t zoneDelayRelay = 0;
uint16_t zoneDelayRunMinutes = 0;
String zoneDelayController = "";
String zoneDelayName = "";

struct ManualZoneRun {
	bool active;
	String controller;
	uint8_t relay;
	int address;
	unsigned long endMillis;
	uint16_t totalRunMinutes;
	bool isProgram;
	String programLetter;
	int zoneIndex;
	bool delayPending;
	unsigned long delayReadyAt;
	uint8_t pendingRelay;
	int pendingZoneIndex;
	uint16_t pendingRunMinutes;
};

static const uint8_t MAX_MANUAL_ZONE_RUNS = 24;
ManualZoneRun manualZoneRuns[MAX_MANUAL_ZONE_RUNS];

// Weather auto-adjust: how many minutes each (controller,program) has
// actually run today, accumulated once per sample tick in loop(). There is
// no existing per-run duration log to derive this from after the fact (only
// a per-sample CSV row) -- see the implementation plan's water-applied gap.
// RAM-only; consumed and reset to zero by serviceWeatherTask() at day
// rollover. A mid-day reboot loses partial-day accumulation -- accepted,
// low-impact, self-correcting limitation.
// Keyed per-zone (not per-program) so fetchAndApplyWeatherUpdate() can apply
// each zone's own calibrated mm_per_min (calibration.json) to its own
// minutes, rather than one blanket site-wide rate -- the whole point of the
// zone calibration feature. zone.number is already read every sample for
// the display, so no Display.h/Display.cpp change is needed to get it here.
struct WaterAppliedAccumulator {
	String controller;
	int zoneNumber;
	float minutesAppliedToday;
};
static const uint8_t MAX_WEATHER_ZONE_SLOTS = 32;	// generous headroom above the current real zone count (~20)
WaterAppliedAccumulator waterAppliedAccumulators[MAX_WEATHER_ZONE_SLOTS];

// One night's computed result for a single zone -- the new source of truth
// for the weather budget (see weather_state.json's "zones" array). Replaces
// the old single site-wide deficit/adjustPct: each zone's own deficit is
// driven by the shared ET0/rain plus its own applied water, never combined
// with other zones' depths (different zones water different physical areas,
// so their mm values aren't addable -- see the implementation plan).
struct ZoneWeatherResult {
	String controller;
	String program;	// which program this zone belongs to -- carried through only for the program-level log aggregate below
	int zoneNumber;
	double deficitMm;
	int weatherAdjustPct;
	double appliedMm;	// that zone's own applied depth last night -- carried through only for the controller/program-level log aggregates below
};

// weather_log.json's per-day history stays at controller granularity (not
// per-zone -- would balloon the 90-day log for no benefit, since the only
// consumer is the chart's coarse "past 5 days" bars). deficitMm/appliedMm
// are area-weighted averages of that controller's zones (still depths, so
// area-weighting applies); adjustPct is a plain mean (a dimensionless
// ratio, not a depth -- area-weighting doesn't apply to it).
struct ControllerLogAggregate {
	String controller;
	double deficitMm;
	double appliedMm;
	int adjustPct;
};

// Same area-weighted/plain-mean pattern as ControllerLogAggregate, but keyed
// by (controller, program) -- a controller's programs can water disjoint
// physical areas (e.g. field's program A/B split), so each program's
// deficit/applied history must stay independent rather than being blended
// into the controller's single number. Logged nested inside each
// controller's weather_log.json entry (see appendWeatherLogRecord).
struct ProgramLogAggregate {
	String controller;
	String program;
	double deficitMm;
	double appliedMm;
	int adjustPct;
};

String lookupZoneName(String controller, uint8_t relay);
String lookupZoneProgram(String controller, uint8_t relay);
String loadSite();
bool saveSite(JSONVar siteVar);
String loadControllers();
bool saveControllers(JSONVar controllersVar, String filename = "controllers.json");
JSONVar buildSiteVarFromParts(JSONVar siteFields, JSONVar weatherFields);
String buildCombinedZoneDocJson();
int applyRunAdjustments(int baseRunMinutes, int seasonalPct, int weatherPct);
int lookupZoneDelaySeconds(String controller, String program);
int lookupZoneWeatherAdjustPct(String controller, int zoneNumber);
bool consumeSkipNextRun(String controller, String program);
String loadWeatherState();
String loadCalibrationData();
double lookupZoneAreaFt2(String controller, int zoneNumber);
double lookupZoneMmPerMin(String controller, int zoneNumber);
double jsonNumberOr(JSONVar parent, const char *key, double fallback);
String jsonStringOr(JSONVar parent, const char *key, String fallback);
bool jsonBoolOr(JSONVar parent, const char *key, bool fallback);

uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < length; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (uint8_t bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
	}
	return crc;
}

String bytesToHex(const uint8_t *data, size_t length) {
	const char hexDigits[] = "0123456789ABCDEF";
	String out = "";
	out.reserve(length * 2);
	for (size_t i = 0; i < length; i++) {
		out += hexDigits[(data[i] >> 4) & 0x0F];
		out += hexDigits[data[i] & 0x0F];
	}
	return out;
}

int hexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

bool hexToBytes(const String &hex, uint8_t *out, size_t maxLength, size_t &outLength) {
	outLength = 0;
	if ((hex.length() % 2) != 0) return false;
	if ((size_t)(hex.length() / 2) > maxLength) return false;

	for (int i = 0; i < hex.length(); i += 2) {
		int high = hexNibble(hex.charAt(i));
		int low = hexNibble(hex.charAt(i + 1));
		if (high < 0 || low < 0) return false;
		out[outLength++] = (uint8_t)((high << 4) | low);
	}
	return true;
}

bool verifyPacketCrc(const uint8_t *packet, size_t length) {
	if (length < 5) return false;
	uint16_t expected = ((uint16_t)packet[length - 2] << 8) | packet[length - 1];
	return crc16Ccitt(packet, length - 2) == expected;
}

void loraSendRawCommand(const String &command, uint16_t settleMs = 120) {
	LoRaSerial.print(command);
	LoRaSerial.print("\r\n");
	Serial.println("[LoRa] > " + command);
	delay(settleMs);
}

void initLoRa() {
	LoRaSerial.begin(LORA_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
	delay(150);

	loraSendRawCommand("AT");
	loraSendRawCommand("AT+ADDRESS=" + String(LORA_MASTER_ADDRESS));
	loraSendRawCommand("AT+NETWORKID=" + String(LORA_NETWORK_ID));
	loraSendRawCommand("AT+BAND=" + String(LORA_BAND));
	loraSendRawCommand("AT+PARAMETER=" + String(LORA_SPREADING_FACTOR) + "," +
	                   String(LORA_BANDWIDTH) + "," +
	                   String(LORA_CODING_RATE) + "," +
	                   String(LORA_PREAMBLE));
	loraSendRawCommand("AT+CRFOP=" + String(LORA_TX_POWER_DBM));
	Serial.println("[LoRa] RYLR998 serial initialized");
}

int loraAddressForController(String controller) {
	controller.trim();
	controller.toLowerCase();
	if (controller == "yard") return LORA_YARD_ADDRESS;
	if (controller == "field") return LORA_FIELD_ADDRESS;
	return -1;
}

const char *loraCommandName(uint8_t command) {
	switch (command) {
		case CMD_RELAY_ON: return "RELAY_ON";
		case CMD_RELAY_OFF: return "RELAY_OFF";
		case CMD_ALL_OFF: return "ALL_OFF";
		case CMD_STATUS_REQUEST: return "STATUS_REQUEST";
		case CMD_RESET: return "RESET";
		default: return "UNKNOWN";
	}
}

uint8_t loraFlagsForAddress(int address, uint8_t flags) {
	if (address == LORA_FIELD_ADDRESS) flags |= LORA_FLAG_TARGET_FIELD;
	return flags;
}

bool sendLoraPacket(int address, uint8_t command, uint8_t relay, uint8_t flags = LORA_FLAG_CRC | LORA_FLAG_RESPONSE_REQUIRED) {
	if (address <= 0) return false;

	uint8_t packet[5];
	packet[0] = command;
	packet[1] = relay;
	packet[2] = loraFlagsForAddress(address, flags);
	uint16_t crc = crc16Ccitt(packet, 3);
	packet[3] = (uint8_t)(crc >> 8);
	packet[4] = (uint8_t)(crc & 0xFF);

	String payload = bytesToHex(packet, sizeof(packet));
	String at = "AT+SEND=" + String(address) + "," + String(payload.length()) + "," + payload;
	loraSendRawCommand(at);
	Serial.printf("[LoRa] sent %s relay %u to addr %d payload %s\n",
	              loraCommandName(command), relay, address, payload.c_str());
	return true;
}

bool sendLoraRelayOn(int address, uint8_t relay, uint8_t minutes, String name) {
	if (address <= 0) return false;

	if (name.length() > LORA_ZONE_NAME_MAX_LEN) name = name.substring(0, LORA_ZONE_NAME_MAX_LEN);
	uint8_t nameLen = (uint8_t)name.length();

	uint8_t packet[5 + LORA_ZONE_NAME_MAX_LEN + 2];
	packet[0] = CMD_RELAY_ON;
	packet[1] = relay;
	packet[2] = loraFlagsForAddress(address, LORA_FLAG_CRC | LORA_FLAG_RESPONSE_REQUIRED);
	packet[3] = minutes;
	packet[4] = nameLen;
	for (uint8_t i = 0; i < nameLen; i++) packet[5 + i] = (uint8_t)name[i];

	size_t crcLen = 5 + nameLen;
	uint16_t crc = crc16Ccitt(packet, crcLen);
	packet[crcLen] = (uint8_t)(crc >> 8);
	packet[crcLen + 1] = (uint8_t)(crc & 0xFF);

	String payload = bytesToHex(packet, crcLen + 2);
	String at = "AT+SEND=" + String(address) + "," + String(payload.length()) + "," + payload;
	loraSendRawCommand(at);
	Serial.printf("[LoRa] sent RELAY_ON relay %u minutes %u name \"%s\" to addr %d payload %s\n",
	              relay, minutes, name.c_str(), address, payload.c_str());
	return true;
}

void sendLoraAllOffToRemotes() {
	sendLoraPacket(LORA_YARD_ADDRESS, CMD_ALL_OFF, 0);
	sendLoraPacket(LORA_FIELD_ADDRESS, CMD_ALL_OFF, 0);
}

bool hasActiveManualZones() {
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (manualZoneRuns[i].active) return true;
	}
	return false;
}

int findManualZoneRun(String controller, uint8_t relay) {
	controller.trim();
	controller.toLowerCase();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (manualZoneRuns[i].active &&
		    manualZoneRuns[i].relay == relay &&
		    manualZoneRuns[i].controller == controller) {
			return i;
		}
	}
	return -1;
}

int findFreeManualZoneRun() {
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (!manualZoneRuns[i].active) return i;
	}
	return -1;
}

// Finds the existing accumulator slot for (controller,zoneNumber), or claims
// the first empty slot and assigns it. Returns -1 only if every slot is
// already assigned to a different zone (shouldn't happen with the current
// ~20-zone real count against a 32-slot ceiling, but fail safe rather than
// overrun).
int findOrCreateWaterAccumulatorSlot(String controller, int zoneNumber) {
	int freeSlot = -1;
	for (uint8_t i = 0; i < MAX_WEATHER_ZONE_SLOTS; i++) {
		if (waterAppliedAccumulators[i].controller == controller &&
		    waterAppliedAccumulators[i].zoneNumber == zoneNumber &&
		    waterAppliedAccumulators[i].controller.length() > 0) {
			return i;
		}
		if (freeSlot < 0 && waterAppliedAccumulators[i].controller.length() == 0) freeSlot = i;
	}
	if (freeSlot >= 0) {
		waterAppliedAccumulators[freeSlot].controller = controller;
		waterAppliedAccumulators[freeSlot].zoneNumber = zoneNumber;
		waterAppliedAccumulators[freeSlot].minutesAppliedToday = 0.0f;
	}
	return freeSlot;
}

bool startManualZoneRun(String controller, uint8_t relay, uint16_t runMinutes, String &message) {
	controller.trim();
	controller.toLowerCase();
	int address = loraAddressForController(controller);
	if (address <= 0 || relay == 0) {
		message = "Invalid manual zone";
		return false;
	}
	if (runMinutes == 0) {
		message = "Run time must be greater than zero";
		return false;
	}

	int idx = findManualZoneRun(controller, relay);
	if (idx < 0) idx = findFreeManualZoneRun();
	if (idx < 0) {
		message = "No manual run slots available";
		return false;
	}

	manualZoneRuns[idx].active = true;
	manualZoneRuns[idx].controller = controller;
	manualZoneRuns[idx].relay = relay;
	manualZoneRuns[idx].address = address;
	manualZoneRuns[idx].endMillis = millis() + ((unsigned long)runMinutes * 60000UL);
	manualZoneRuns[idx].totalRunMinutes = runMinutes;
	manualZoneRuns[idx].isProgram = false;
	manualZoneRuns[idx].programLetter = "";
	manualZoneRuns[idx].zoneIndex = -1;
	manualZoneRuns[idx].delayPending = false;

	sendLoraRelayOn(address, relay, (uint8_t)min<uint16_t>(runMinutes, 255), lookupZoneName(controller, relay));
	message = "Manual zone started";
	return true;
}

// `fromIndex` is an index into the matched program's own `zones[]` array
// (not a global flat-array offset, since the nested schema has no global
// array anymore) -- callers only ever pass back an index this function
// itself returned, so this is a self-contained change.
bool findNextProgramZone(String controller, String programLetter, int fromIndex,
                          int &outIndex, uint8_t &outRelay, uint16_t &outRunMinutes) {
	controller.trim();
	controller.toLowerCase();
	programLetter.trim();
	programLetter.toUpperCase();

	JSONVar controllers = JSON.parse(loadControllers());
	if (JSON.typeof(controllers) != "array") return false;

	for (int c = 0; c < controllers.length(); c++) {
		String cid = String((const char *)controllers[c]["id"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;

		JSONVar programs = controllers[c]["programs"];
		for (int p = 0; p < programs.length(); p++) {
			String pid = String((const char *)programs[p]["id"]);
			pid.trim();
			pid.toUpperCase();
			if (pid != programLetter) continue;

			JSONVar zones = programs[p]["zones"];
			int percent = (int)programs[p]["seasonal_adjust_pct"];
			for (int z = fromIndex; z < zones.length(); z++) {
				uint8_t relay = (uint8_t)(int)zones[z]["znumber"];
				int weatherPct = lookupZoneWeatherAdjustPct(controller, relay);
				int baseRunMinutes = (int)zones[z]["run"];
				uint16_t runMinutes = (uint16_t)applyRunAdjustments(baseRunMinutes, percent, weatherPct);
				if (relay == 0 || runMinutes == 0) continue;

				outIndex = z;
				outRelay = relay;
				outRunMinutes = runMinutes;
				return true;
			}
			return false;
		}
	}
	return false;
}

bool stopManualZoneRun(String controller, uint8_t relay, String &message) {
	controller.trim();
	controller.toLowerCase();
	int idx = findManualZoneRun(controller, relay);
	int address = loraAddressForController(controller);
	if (address <= 0 || relay == 0) {
		message = "Invalid manual zone";
		return false;
	}

	sendLoraPacket(address, CMD_RELAY_OFF, relay);
	if (idx >= 0) manualZoneRuns[idx].active = false;
	message = "Manual zone stopped";
	return true;
}

bool stopManualProgramRun(String controller, String &message) {
	controller.trim();
	controller.toLowerCase();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (manualZoneRuns[i].active && manualZoneRuns[i].isProgram &&
		    manualZoneRuns[i].controller == controller) {
			sendLoraPacket(manualZoneRuns[i].address, CMD_RELAY_OFF, manualZoneRuns[i].relay);
			manualZoneRuns[i].active = false;
			message = "Manual program stopped";
			return true;
		}
	}
	message = "No active program for that controller";
	return false;
}

// Skips the current zone of a running program straight to the next one,
// ignoring any inter-zone delay (that gap only matters for the unattended
// natural-expiry path in serviceManualZoneRuns() -- a manual "next" press is
// itself the human decision to move on right now). Stops the whole program,
// same as natural completion, if the current zone was the last one.
bool advanceManualProgramRun(String controller, String &message) {
	controller.trim();
	controller.toLowerCase();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (!manualZoneRuns[i].active || !manualZoneRuns[i].isProgram ||
		    manualZoneRuns[i].controller != controller) continue;

		sendLoraPacket(manualZoneRuns[i].address, CMD_RELAY_OFF, manualZoneRuns[i].relay);

		int nextIndex;
		uint8_t nextRelay;
		uint16_t nextRun;
		if (findNextProgramZone(manualZoneRuns[i].controller, manualZoneRuns[i].programLetter,
		                        manualZoneRuns[i].zoneIndex + 1, nextIndex, nextRelay, nextRun)) {
			manualZoneRuns[i].delayPending = false;
			manualZoneRuns[i].relay = nextRelay;
			manualZoneRuns[i].zoneIndex = nextIndex;
			manualZoneRuns[i].endMillis = millis() + ((unsigned long)nextRun * 60000UL);
			manualZoneRuns[i].totalRunMinutes = nextRun;
			sendLoraRelayOn(manualZoneRuns[i].address, nextRelay, (uint8_t)min<uint16_t>(nextRun, 255),
			                lookupZoneName(manualZoneRuns[i].controller, nextRelay));
			Serial.printf("[LoRa] program manual-advance: %s program %s -> relay %u\n",
			              manualZoneRuns[i].controller.c_str(),
			              manualZoneRuns[i].programLetter.c_str(), nextRelay);
			message = "Advanced to next zone";
			return true;
		}

		manualZoneRuns[i].active = false;
		Serial.printf("[LoRa] program complete (manual-advance past last zone): %s program %s\n",
		              manualZoneRuns[i].controller.c_str(), manualZoneRuns[i].programLetter.c_str());
		message = "Program complete";
		return true;
	}
	message = "No active program for that controller";
	return false;
}

bool startManualProgramRun(String controller, String programLetter, String &message) {
	controller.trim();
	controller.toLowerCase();
	programLetter.trim();
	programLetter.toUpperCase();

	int address = loraAddressForController(controller);
	if (address <= 0) {
		message = "Invalid controller";
		return false;
	}
	if (programLetter.length() == 0) {
		message = "Invalid program";
		return false;
	}

	String stopMsg;
	stopManualProgramRun(controller, stopMsg); // restart fresh

	int zoneIndex;
	uint8_t relay;
	uint16_t runMinutes;
	if (!findNextProgramZone(controller, programLetter, 0, zoneIndex, relay, runMinutes)) {
		message = "No zones found for that Controller/Program";
		return false;
	}

	int idx = findManualZoneRun(controller, relay);
	if (idx < 0) idx = findFreeManualZoneRun();
	if (idx < 0) {
		message = "No manual run slots available";
		return false;
	}

	manualZoneRuns[idx].active = true;
	manualZoneRuns[idx].controller = controller;
	manualZoneRuns[idx].relay = relay;
	manualZoneRuns[idx].address = address;
	manualZoneRuns[idx].endMillis = millis() + ((unsigned long)runMinutes * 60000UL);
	manualZoneRuns[idx].totalRunMinutes = runMinutes;
	manualZoneRuns[idx].isProgram = true;
	manualZoneRuns[idx].programLetter = programLetter;
	manualZoneRuns[idx].zoneIndex = zoneIndex;
	manualZoneRuns[idx].delayPending = false;

	sendLoraRelayOn(address, relay, (uint8_t)min<uint16_t>(runMinutes, 255), lookupZoneName(controller, relay));
	message = "Manual program started";
	return true;
}

void stopAllManualZoneRuns() {
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (!manualZoneRuns[i].active) continue;
		sendLoraPacket(manualZoneRuns[i].address, CMD_RELAY_OFF, manualZoneRuns[i].relay);
		manualZoneRuns[i].active = false;
	}
}

void stopAllZones() {
	sendLoraAllOffToRemotes();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		manualZoneRuns[i].active = false;
	}
	loraLastController = "";
	loraLastZoneNumber = 0;
	loraSentIdleAllOff = true;
	Serial.println("[LoRa] stop all zones: sent ALL_OFF to Yard and Field");
}

void serviceManualZoneRuns() {
	unsigned long now = millis();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (!manualZoneRuns[i].active) continue;

		if (manualZoneRuns[i].delayPending) {
			if ((long)(now - manualZoneRuns[i].delayReadyAt) < 0) continue; // still waiting between zones

			manualZoneRuns[i].delayPending = false;
			manualZoneRuns[i].relay = manualZoneRuns[i].pendingRelay;
			manualZoneRuns[i].zoneIndex = manualZoneRuns[i].pendingZoneIndex;
			manualZoneRuns[i].endMillis = now + ((unsigned long)manualZoneRuns[i].pendingRunMinutes * 60000UL);
			manualZoneRuns[i].totalRunMinutes = manualZoneRuns[i].pendingRunMinutes;
			sendLoraRelayOn(manualZoneRuns[i].address, manualZoneRuns[i].relay,
			                (uint8_t)min<uint16_t>(manualZoneRuns[i].pendingRunMinutes, 255),
			                lookupZoneName(manualZoneRuns[i].controller, manualZoneRuns[i].relay));
			Serial.printf("[LoRa] program advance (after delay): %s program %s -> relay %u\n",
			              manualZoneRuns[i].controller.c_str(),
			              manualZoneRuns[i].programLetter.c_str(), manualZoneRuns[i].relay);
			continue;
		}

		if ((long)(now - manualZoneRuns[i].endMillis) < 0) continue;

		sendLoraPacket(manualZoneRuns[i].address, CMD_RELAY_OFF, manualZoneRuns[i].relay);
		Serial.printf("[LoRa] manual run expired: %s relay %u\n",
		              manualZoneRuns[i].controller.c_str(), manualZoneRuns[i].relay);

		if (manualZoneRuns[i].isProgram) {
			int nextIndex;
			uint8_t nextRelay;
			uint16_t nextRun;
			if (findNextProgramZone(manualZoneRuns[i].controller, manualZoneRuns[i].programLetter,
			                        manualZoneRuns[i].zoneIndex + 1, nextIndex, nextRelay, nextRun)) {
				int delaySec = lookupZoneDelaySeconds(manualZoneRuns[i].controller, manualZoneRuns[i].programLetter);
				if (delaySec > 0) {
					manualZoneRuns[i].delayPending = true;
					manualZoneRuns[i].delayReadyAt = now + (unsigned long)delaySec * 1000UL;
					manualZoneRuns[i].pendingRelay = nextRelay;
					manualZoneRuns[i].pendingZoneIndex = nextIndex;
					manualZoneRuns[i].pendingRunMinutes = nextRun;
					Serial.printf("[LoRa] zone-to-zone delay: %s program %s waiting %d sec before relay %u\n",
					              manualZoneRuns[i].controller.c_str(), manualZoneRuns[i].programLetter.c_str(),
					              delaySec, nextRelay);
					continue;
				}
				manualZoneRuns[i].relay = nextRelay;
				manualZoneRuns[i].zoneIndex = nextIndex;
				manualZoneRuns[i].endMillis = now + ((unsigned long)nextRun * 60000UL);
				manualZoneRuns[i].totalRunMinutes = nextRun;
				sendLoraRelayOn(manualZoneRuns[i].address, nextRelay, (uint8_t)min<uint16_t>(nextRun, 255),
				                lookupZoneName(manualZoneRuns[i].controller, nextRelay));
				Serial.printf("[LoRa] program advance: %s program %s -> relay %u\n",
				              manualZoneRuns[i].controller.c_str(),
				              manualZoneRuns[i].programLetter.c_str(), nextRelay);
				continue;
			}
			Serial.printf("[LoRa] program complete: %s program %s\n",
			              manualZoneRuns[i].controller.c_str(), manualZoneRuns[i].programLetter.c_str());
		}

		manualZoneRuns[i].active = false;
	}
}

String buildManualZoneRunsJson() {
	String json = "{\"type\":\"manualZoneStatus\",\"runs\":[";
	bool first = true;
	unsigned long now = millis();
	for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
		if (!manualZoneRuns[i].active || manualZoneRuns[i].delayPending) continue;
		if (!first) json += ",";
		long remainingMs = (long)(manualZoneRuns[i].endMillis - now);
		unsigned long remainingSec = remainingMs > 0 ? (unsigned long)remainingMs / 1000UL : 0UL;
		json += "{\"controller\":\"" + manualZoneRuns[i].controller + "\",";
		json += "\"relay\":" + String(manualZoneRuns[i].relay) + ",";
		json += "\"remainingSec\":" + String(remainingSec) + ",";
		json += "\"totalRunMinutes\":" + String(manualZoneRuns[i].totalRunMinutes) + ",";
		json += "\"program\":" + String(manualZoneRuns[i].isProgram ? "true" : "false") + ",";
		json += "\"programLetter\":\"" + manualZoneRuns[i].programLetter + "\"}";
		first = false;
	}
	json += "]}";
	return json;
}

// Real-time (every loop()) resolution of a pending zone-to-zone delay wait,
// started by serviceRemoteZoneControl() when it detects a transition into a
// new zone. Runs every loop() iteration (not gated by Sample Rate) for good
// timing precision on delays as short as a few seconds.
void serviceZoneDelayTimers() {
	if (!zoneDelayPending) return;
	if ((long)(millis() - zoneDelayReadyAt) < 0) return; // still waiting

	sendLoraRelayOn(zoneDelayAddress, zoneDelayRelay, (uint8_t)min<uint16_t>(zoneDelayRunMinutes, 255), zoneDelayName);
	loraLastController = zoneDelayController;
	loraLastZoneNumber = zoneDelayRelay;
	loraSentIdleAllOff = false;
	zoneDelayPending = false;
	Serial.printf("[LoRa] zone-to-zone delay elapsed: %s relay %u now on\n",
	              zoneDelayController.c_str(), zoneDelayRelay);
}

void serviceRemoteZoneControl(const ZoneInfo &zone) {
	if (zoneDelayPending) return; // serviceZoneDelayTimers() owns resolving this

	String controller = zone.controller;
	uint8_t zoneNumber = (uint8_t)zone.number.toInt();
	// zoneNumber alone, not zone.allOff: allOff is a pressure-recovery
	// heuristic (currentPressure >= ZONES_ALL_OFF_PSI) that can still read
	// true for a while after a scheduled zone's window opens, if pressure
	// hasn't yet fallen from a recent refill (previous zone ending, or the
	// periodic low-pressure auto-refill) -- gating relay-on here on it could
	// skip turning the zone on for that check, or entirely if the schedule
	// moves on before pressure catches up.
	bool remoteActive = zoneNumber > 0 &&
	                    loraAddressForController(controller) > 0;
	bool manualActive = hasActiveManualZones();

	if (!remoteActive) {
		if (manualActive) {
			int previousAddress = loraAddressForController(loraLastController);
			if (previousAddress > 0 && loraLastZoneNumber > 0) {
				sendLoraPacket(previousAddress, CMD_RELAY_OFF, loraLastZoneNumber);
				loraLastController = "";
				loraLastZoneNumber = 0;
				Serial.println("[LoRa] scheduled zone ended while manual zones remain active");
			}
			return;
		}
		if (!loraSentIdleAllOff && !manualActive) {
			sendLoraAllOffToRemotes();
			loraSentIdleAllOff = true;
			loraLastController = "";
			loraLastZoneNumber = 0;
			Serial.println("[LoRa] idle/all-off: sent ALL_OFF to Yard and Field");
		}
		return;
	}

	String normalizedController = controller;
	normalizedController.trim();
	normalizedController.toLowerCase();
	if (normalizedController == loraLastController && zoneNumber == loraLastZoneNumber) {
		return;
	}

	if (manualActive) {
		int previousAddress = loraAddressForController(loraLastController);
		if (previousAddress > 0 && loraLastZoneNumber > 0) {
			sendLoraPacket(previousAddress, CMD_RELAY_OFF, loraLastZoneNumber);
		}
	} else {
		sendLoraAllOffToRemotes();
	}
	int address = loraAddressForController(controller);

	// Zone-to-zone delay: wait before turning this zone on, whether it's
	// continuing a chain or starting fresh from idle. The zone still turns
	// off exactly on the unmodified nominal schedule (above/below), so this
	// wait simply shortens this zone's own actual on-time by delaySec --
	// no schedule-side bookkeeping needed for that to be true.
	int delaySec = lookupZoneDelaySeconds(normalizedController, lookupZoneProgram(normalizedController, zoneNumber));
	if (delaySec > 0) {
		zoneDelayPending = true;
		zoneDelayReadyAt = millis() + (unsigned long)delaySec * 1000UL;
		zoneDelayAddress = address;
		zoneDelayRelay = zoneNumber;
		zoneDelayRunMinutes = (uint16_t)zone.run.toInt();
		zoneDelayController = normalizedController;
		zoneDelayName = zone.name;
		loraLastController = "";
		loraLastZoneNumber = 0;
		Serial.printf("[LoRa] zone-to-zone delay: %s relay %u in %d sec\n",
		              normalizedController.c_str(), zoneNumber, delaySec);
		return;
	}

	sendLoraRelayOn(address, zoneNumber, (uint8_t)min<long>(zone.run.toInt(), 255), zone.name);
	loraLastController = normalizedController;
	loraLastZoneNumber = zoneNumber;
	loraSentIdleAllOff = false;
	Serial.printf("[LoRa] active zone changed: %s zone %u\n",
	              normalizedController.c_str(), zoneNumber);
}

void handleLoraReceiveLine(String line) {
	line.trim();
	if (line.length() == 0) return;
	Serial.println("[LoRa] < " + line);

	if (!line.startsWith("+RCV=")) return;

	int firstComma = line.indexOf(',');
	int secondComma = line.indexOf(',', firstComma + 1);
	int thirdComma = line.indexOf(',', secondComma + 1);
	if (firstComma < 0 || secondComma < 0 || thirdComma < 0) {
		Serial.println("[LoRa] malformed +RCV response");
		return;
	}

	int fromAddress = line.substring(5, firstComma).toInt();
	String declaredLength = line.substring(firstComma + 1, secondComma);
	String payloadHex = line.substring(secondComma + 1, thirdComma);

	uint8_t packet[32];
	size_t packetLength = 0;
	if (!hexToBytes(payloadHex, packet, sizeof(packet), packetLength)) {
		Serial.println("[LoRa] non-hex +RCV payload");
		return;
	}

	bool crcOk = verifyPacketCrc(packet, packetLength);
	if (!crcOk) {
		Serial.printf("[LoRa] CRC failed from addr %d len %s payload %s\n",
		              fromAddress, declaredLength.c_str(), payloadHex.c_str());
		return;
	}

	switch (packet[0]) {
		case RESP_ACK:
			Serial.printf("[LoRa] ACK from addr %d relay %u status %u\n",
			              fromAddress, packet[2], packet[1]);
			break;
		case RESP_ERROR:
			Serial.printf("[LoRa] ERROR from addr %d relay %u code %u\n",
			              fromAddress, packet[2], packet[1]);
			break;
		case RESP_STATUS:
			Serial.printf("[LoRa] STATUS from addr %d status %u relay %u\n",
			              fromAddress, packet[1], packet[2]);
			break;
		default:
			Serial.printf("[LoRa] response 0x%02X from addr %d\n", packet[0], fromAddress);
			break;
	}
}

void pollLoRaResponses() {
	while (LoRaSerial.available()) {
		char c = (char)LoRaSerial.read();
		if (c == '\r') continue;
		if (c == '\n') {
			handleLoraReceiveLine(loraRxLine);
			loraRxLine = "";
			continue;
		}
		if (loraRxLine.length() < 180) {
			loraRxLine += c;
		} else {
			loraRxLine = "";
		}
	}
}

/***********************************************/
// Function to send log messages to the client
void logMsg(const char *logMessage) {
	// Print to Serial Monitor
	Serial.println(logMessage);

	// Send the log message to all connected clients via SSE
	events.send(logMessage, "server-log", millis());
}

void getTimeStamp(String &day, String &time) {
	timeClient.update();

	// Get the epoch time adjusted for the timezone offset
	time_t epochTime = (time_t)timeClient.getEpochTime();

	// Convert epoch time to local time
	struct tm *ptm = gmtime(&epochTime);

	// Extract date and time components
	char dateBuffer[11];
	char timeBuffer[9];
	strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", ptm);
	strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", ptm);

	day = String(dateBuffer);
	time = String(timeBuffer);

	// Serial.println("DayStamp: " + day);
	// Serial.println("TimeStamp: " + time);
}

String loadZoneTable(fs::FS &fs, const char *filePath) {
	File file = fs.open(filePath, FILE_READ);
	if (!file) {
		Serial.printf("Failed to open zone data file %s for reading\n", filePath);
		return "[]";	// Return empty JSON array if the file doesn't exist
	}

	String zoneData = "";
	zoneData = file.readString();  // Using readString() to get the entire file content
	file.close();

	// Debugging: Check if the zoneData is correctly loaded
	Serial.println("Loaded Zone Data: " + zoneData);

	return zoneData;
}

// Weather auto-adjust: firmware-owned runtime state (last fetch status,
// current deficit, per-program weather_adjust_pct/skip_next_run). Lives in
// its own small SPIFFS file, never inside site.json -- see the
// implementation plan's decision #1 (Arduino_JSON nested-mutation risk +
// avoiding a stale browser Save All clobbering freshly-computed numbers).
String loadWeatherState() {
	String data = loadZoneTable(SPIFFS, "/weather_state.json");
	if (data.length() <= 2) return "{}";	// missing/empty file -- loadZoneTable's sentinel is "[]"
	return data;
}

// Per-zone irrigation calibration (head specs + SVG-measured area ->
// mm_per_min), edited on calibration.html. SPIFFS, like site.json/
// controllers.json/weather_state.json/weather_cache.json/weather_log.json --
// all bounded, infrequently-written config/history files. Only the
// genuinely-unbounded daily CSV sensor log stays on SD.
String loadCalibrationData() {
	String data = loadZoneTable(SPIFFS, "/calibration.json");
	if (data.length() <= 2) return "{}";	// missing/empty file -- loadZoneTable's sentinel is "[]"
	return data;
}

// Mirrors lookupZoneDelaySeconds()'s structure exactly -- fresh parse every
// call, never a boot-time-cached table, so a saved calibration.json edit
// takes effect on the very next watering decision with no reboot needed.
// Falls back to site.mm_per_min_default (site.json) when calibration.json
// is missing, has no entry for this zone, or that entry's mm_per_min is
// zero/absent -- never silently zeroes out a calculation, same discipline
// as lookupZoneWeatherAdjustPct().
double lookupZoneMmPerMin(String controller, int zoneNumber) {
	controller.trim();
	controller.toLowerCase();

	double fallback = 0.25;
	JSONVar site = JSON.parse(loadSite());
	if (JSON.typeof(site) == "object") fallback = jsonNumberOr(site, "mm_per_min_default", 0.25);
	if (fallback <= 0) fallback = 0.25;

	JSONVar calib = JSON.parse(loadCalibrationData());
	if (JSON.typeof(calib) != "object") return fallback;
	JSONVar zones = calib["zones"];
	if (JSON.typeof(zones) != "array") return fallback;

	for (int i = 0; i < zones.length(); i++) {
		String cid = String((const char *)zones[i]["controller"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;
		if ((int)zones[i]["zone_number"] != zoneNumber) continue;
		double mm = jsonNumberOr(zones[i], "mm_per_min", 0.0);
		return mm > 0 ? mm : fallback;
	}
	return fallback;
}

// Same fresh-parse structure as lookupZoneMmPerMin() -- used only to
// area-weight zone depths into the controller-level log aggregate (see
// ControllerLogAggregate). Returns 0 (not a guessed fallback) when missing,
// so a zone with no calibration entry simply doesn't contribute to the
// weighted average rather than skewing it with a fabricated area.
double lookupZoneAreaFt2(String controller, int zoneNumber) {
	controller.trim();
	controller.toLowerCase();

	JSONVar calib = JSON.parse(loadCalibrationData());
	if (JSON.typeof(calib) != "object") return 0;
	JSONVar zones = calib["zones"];
	if (JSON.typeof(zones) != "array") return 0;

	for (int i = 0; i < zones.length(); i++) {
		String cid = String((const char *)zones[i]["controller"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;
		if ((int)zones[i]["zone_number"] != zoneNumber) continue;
		return jsonNumberOr(zones[i], "area_ft2", 0.0);
	}
	return 0;
}

bool isSafeJsonFilename(String filename) {
	filename.trim();
	if (!filename.endsWith(".json")) return false;
	if (filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0) return false;
	if (filename.indexOf("..") >= 0) return false;
	return filename.length() > 5;
}

bool isHiddenSystemPath(String filename) {
	filename.trim();
	while (filename.startsWith("/")) filename.remove(0, 1);
	filename.toLowerCase();
	return filename == "system volume information" || filename.startsWith("system volume information/");
}

// `fs` defaults to SPIFFS -- only the daily CSV sensor log passes SD
// explicitly; every JSON config/history file (site/controllers/weather
// state/cache/log/calibration) uses the SPIFFS default.
bool writeRawJsonFile(const String &json, String filename, fs::FS &fs = SPIFFS) {
	if (!isSafeJsonFilename(filename)) return false;

	String fullPath = "/" + filename;
	File file = fs.open(fullPath.c_str(), FILE_WRITE);
	if (!file) return false;

	bool ok = (bool)file.print(json);
	file.close();
	return ok;
}

// ---------------------------------------------------------------------------
// Weather auto-adjust -- budget calculation + weather_state.json persistence.
//
// JSONVar safety note (applies to every function below): every JSONVar here
// is parsed and read from inside the SAME function that created it, never
// passed as a parameter or return value, and never mutated-then-restringified
// in place. The Arduino_JSON deep-copy risk that motivated decision #1 is
// specifically about copying entries BETWEEN separately-parsed structures --
// plain chained-subscript reads within one parse (the same pattern
// checkActiveZone() already relies on every sample tick) are fine.
// ---------------------------------------------------------------------------

double jsonNumberOr(JSONVar parent, const char *key, double fallback) {
	JSONVar value = parent[key];
	if (JSON.typeof(value) == "undefined" || JSON.typeof(value) == "null") return fallback;
	double result = (double)value;
	if (isnan(result)) return fallback;
	return result;
}

String jsonStringOr(JSONVar parent, const char *key, String fallback) {
	JSONVar value = parent[key];
	if (JSON.typeof(value) != "string") return fallback;
	return String((const char *)value);
}

bool jsonBoolOr(JSONVar parent, const char *key, bool fallback) {
	JSONVar value = parent[key];
	if (JSON.typeof(value) != "boolean") return fallback;
	return (bool)value;
}

String formatUtcIso8601(time_t utcEpoch) {
	struct tm *t = gmtime(&utcEpoch);
	char buf[24];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
	         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	return String(buf);
}

// Failure path: flip only last_fetch_status, via a literal substring patch
// (no JSON parse/rebuild at all) so every other field -- especially every
// zone's deficit_mm/weather_adjust_pct, every program's skip_next_run, and
// last_fetch_date (which the retry gate depends on staying unchanged) --
// survives untouched even if something upstream is in a weird state.
void markWeatherFetchError() {
	String existing = loadWeatherState();
	int statusIdx = existing.indexOf("\"last_fetch_status\":\"");
	if (statusIdx < 0) {
		writeRawJsonFile("{\"last_fetch_status\":\"error\"}", "weather_state.json");
		return;
	}
	int valueStart = statusIdx + strlen("\"last_fetch_status\":\"");
	int valueEnd = existing.indexOf('"', valueStart);
	if (valueEnd < 0) return;
	writeRawJsonFile(existing.substring(0, valueStart) + "error" + existing.substring(valueEnd),
	                  "weather_state.json");
}

// Rebuilds weather_state.json from scratch via string concatenation (the
// same safe pattern the deleted pre-redesign setSeasonalAdjustment used).
// Each zone gets its own deficit_mm/weather_adjust_pct (ZoneWeatherResult --
// different zones water different physical areas, so their numbers are
// never combined here, only the log's controller-level aggregate does
// that). skip_next_run stays one uniform decision applied to every program,
// since it's driven by tomorrow's rain forecast, not deficit.
String buildWeatherStateJson(String lastFetchUtc, String lastFetchStatus, String lastFetchDate,
                              ZoneWeatherResult *zoneResults, int zoneResultCount,
                              double forecastRain24hMm, double forecastRain7dMm, bool skipNextRun) {
	JSONVar controllers = JSON.parse(loadControllers());

	String json = "{";
	json += "\"last_fetch_utc\":\"" + lastFetchUtc + "\",";
	json += "\"last_fetch_status\":\"" + lastFetchStatus + "\",";
	json += "\"last_fetch_date\":\"" + lastFetchDate + "\",";
	json += "\"forecast_rain_24h_mm\":" + String(forecastRain24hMm, 2) + ",";
	json += "\"forecast_rain_7d_mm\":" + String(forecastRain7dMm, 2) + ",";

	json += "\"zones\":[";
	for (int i = 0; i < zoneResultCount; i++) {
		if (i > 0) json += ",";
		json += "{\"controller\":\"" + zoneResults[i].controller + "\",\"zone_number\":" + String(zoneResults[i].zoneNumber) + ","
		        "\"deficit_mm\":" + String(zoneResults[i].deficitMm, 2) + ","
		        "\"weather_adjust_pct\":" + String(zoneResults[i].weatherAdjustPct) + "}";
	}
	json += "],";

	json += "\"programs\":[";
	bool first = true;
	if (JSON.typeof(controllers) == "array") {
		for (int c = 0; c < controllers.length(); c++) {
			String controllerId = String((const char *)controllers[c]["id"]);
			JSONVar programs = controllers[c]["programs"];
			for (int p = 0; p < programs.length(); p++) {
				String programId = String((const char *)programs[p]["id"]);
				if (!first) json += ",";
				json += "{\"controller\":\"" + controllerId + "\",\"program\":\"" + programId + "\","
				        "\"skip_next_run\":" + String(skipNextRun ? "true" : "false") + "}";
				first = false;
			}
		}
	}
	json += "]}";
	return json;
}

// Passthrough rebuild of an existing log row's controllers[] sub-array --
// used only when preserving older rows in appendWeatherLogRecord(). Old-
// format rows (written before the per-controller log split) simply have no
// "controllers" array, which degrades gracefully to "[]" here rather than
// fabricating data that was never recorded -- a one-time, self-resolving
// transition cost as the 90-day log rolls forward.
// Passthrough rebuild of an existing controller log entry's nested
// programs[] sub-array -- used only when preserving older rows in
// appendWeatherLogRecord(). Old-format rows (written before the
// per-program log split) simply have no "programs" array, which degrades
// gracefully to "[]" here, the same one-time transition cost the
// controller-level split itself already accepted.
String serializeProgramsArrayFromJson(JSONVar programsArray) {
	if (JSON.typeof(programsArray) != "array") return "[]";
	String json = "[";
	for (int i = 0; i < programsArray.length(); i++) {
		if (i > 0) json += ",";
		json += "{\"program\":\"" + jsonStringOr(programsArray[i], "program", "") + "\","
		        "\"deficit_mm\":" + String(jsonNumberOr(programsArray[i], "deficit_mm", 0), 2) + ","
		        "\"water_applied_mm\":" + String(jsonNumberOr(programsArray[i], "water_applied_mm", 0), 2) + ","
		        "\"adjust_pct\":" + String((int)jsonNumberOr(programsArray[i], "adjust_pct", 0)) + "}";
	}
	json += "]";
	return json;
}

String serializeControllersArrayFromJson(JSONVar controllersArray) {
	if (JSON.typeof(controllersArray) != "array") return "[]";
	String json = "[";
	for (int i = 0; i < controllersArray.length(); i++) {
		if (i > 0) json += ",";
		json += "{\"controller\":\"" + jsonStringOr(controllersArray[i], "controller", "") + "\","
		        "\"deficit_mm\":" + String(jsonNumberOr(controllersArray[i], "deficit_mm", 0), 2) + ","
		        "\"water_applied_mm\":" + String(jsonNumberOr(controllersArray[i], "water_applied_mm", 0), 2) + ","
		        "\"adjust_pct\":" + String((int)jsonNumberOr(controllersArray[i], "adjust_pct", 0)) + ","
		        "\"skipped\":" + String(jsonBoolOr(controllersArray[i], "skipped", false) ? "true" : "false") + ","
		        "\"programs\":" + serializeProgramsArrayFromJson(controllersArray[i]["programs"]) + "}";
	}
	json += "]";
	return json;
}

// Appends one day's record to weather_log.json on SPIFFS, keeping at most the
// last 90 days. Existing records are read back out and rebuilt manually
// (never JSON.stringify()'d as a sub-object slice) -- this runs once a day
// indefinitely, so it gets the most conservative treatment. Per-controller
// breakdown (ControllerLogAggregate) with a nested per-program breakdown
// (ProgramLogAggregate) inside each -- not per-zone, since the 90-day log
// would balloon for no benefit beyond what the chart's per-program lines
// actually need.
void appendWeatherLogRecord(String date, double et0Mm, double precipMm,
                             ControllerLogAggregate *aggregates, int aggregateCount,
                             ProgramLogAggregate *programAggregates, int programAggregateCount,
                             bool skipped) {
	static const int MAX_LOG_DAYS = 90;

	JSONVar parsed = JSON.parse(loadZoneTable(SPIFFS, "/weather_log.json"));
	JSONVar logArray = (JSON.typeof(parsed) == "object") ? parsed["log"] : JSONVar();
	int existingCount = (JSON.typeof(logArray) == "array") ? logArray.length() : 0;
	int keepFrom = (existingCount >= MAX_LOG_DAYS) ? (existingCount - (MAX_LOG_DAYS - 1)) : 0;

	String json = "{\"log\":[";
	bool first = true;
	for (int i = keepFrom; i < existingCount; i++) {
		if (!first) json += ",";
		json += "{\"date\":\"" + jsonStringOr(logArray[i], "date", "") + "\","
		        "\"et0_mm\":" + String(jsonNumberOr(logArray[i], "et0_mm", 0), 2) + ","
		        "\"precip_mm\":" + String(jsonNumberOr(logArray[i], "precip_mm", 0), 2) + ","
		        "\"controllers\":" + serializeControllersArrayFromJson(logArray[i]["controllers"]) + "}";
		first = false;
	}
	if (!first) json += ",";
	json += "{\"date\":\"" + date + "\",\"et0_mm\":" + String(et0Mm, 2) + ",\"precip_mm\":" + String(precipMm, 2) + ",\"controllers\":[";
	for (int i = 0; i < aggregateCount; i++) {
		if (i > 0) json += ",";
		json += "{\"controller\":\"" + aggregates[i].controller + "\","
		        "\"deficit_mm\":" + String(aggregates[i].deficitMm, 2) + ","
		        "\"water_applied_mm\":" + String(aggregates[i].appliedMm, 2) + ","
		        "\"adjust_pct\":" + String(aggregates[i].adjustPct) + ","
		        "\"skipped\":" + String(skipped ? "true" : "false") + ","
		        "\"programs\":[";
		bool firstProgram = true;
		for (int pi = 0; pi < programAggregateCount; pi++) {
			if (programAggregates[pi].controller != aggregates[i].controller) continue;
			if (!firstProgram) json += ",";
			json += "{\"program\":\"" + programAggregates[pi].program + "\","
			        "\"deficit_mm\":" + String(programAggregates[pi].deficitMm, 2) + ","
			        "\"water_applied_mm\":" + String(programAggregates[pi].appliedMm, 2) + ","
			        "\"adjust_pct\":" + String(programAggregates[pi].adjustPct) + "}";
			firstProgram = false;
		}
		json += "]}";
	}
	json += "]}";
	json += "]}";

	writeRawJsonFile(json, "weather_log.json");
}

// The full daily sequence: fetch -> cache raw response on SPIFFS -> compute
// each zone's own soil-water-deficit budget -> rebuild weather_state.json ->
// append one weather_log.json record -> reset the water-applied
// accumulators. Any failure along the way calls markWeatherFetchError() and
// returns without touching anything else.
void fetchAndApplyWeatherUpdate() {
	JSONVar site = JSON.parse(loadSite());
	if (JSON.typeof(site) != "object") { markWeatherFetchError(); return; }

	JSONVar weatherSettings = site["weather"];
	if (JSON.typeof(weatherSettings) != "object") {
		markWeatherFetchError();
		return;
	}

	double lat = jsonNumberOr(site, "latitude", 0.0);
	double lon = jsonNumberOr(site, "longitude", 0.0);
	String timezone = jsonStringOr(site, "timezone", "");
	// Per-zone mm_per_min now comes from lookupZoneMmPerMin() (calibration.json,
	// falling back to site.mm_per_min_default per zone) in the accumulator loop
	// below -- no single site-wide rate is read here anymore.

	// referenceDeficitMm is the deficit level at which a zone's configured,
	// unmodified run-time is considered "correct" (100%) -- not a goal the
	// system tries to reach/maintain. Below it the soil is less thirsty than
	// that reference condition, so adjust_pct comes out under 100; above it,
	// over 100. See weather-auto-adjust.md's Adjustment Percentage Calculation
	// section for the full explanation (renamed from target_deficit_mm,
	// which read like a literal goal -- it isn't one).
	double referenceDeficitMm = jsonNumberOr(weatherSettings, "reference_deficit_mm", 6.0);
	double maxDeficitMm = jsonNumberOr(weatherSettings, "max_deficit_mm", 25.0);
	int maxAdjustPct = (int)jsonNumberOr(weatherSettings, "max_adjust_pct", 150);
	int minAdjustPct = (int)jsonNumberOr(weatherSettings, "min_adjust_pct", 0);
	double rainSkipThresholdMm = jsonNumberOr(weatherSettings, "rain_skip_threshold_mm", 6.0);
	if (referenceDeficitMm <= 0) referenceDeficitMm = 6.0;
	if (maxDeficitMm <= 0) maxDeficitMm = 25.0;

	String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
	             "&longitude=" + String(lon, 4) +
	             "&daily=et0_fao_evapotranspiration,precipitation_sum,rain_sum,precipitation_probability_max" +
	             "&timezone=" + (timezone.length() > 0 ? timezone : "auto") +
	             "&forecast_days=7&past_days=1";

	WiFiClientSecure secureClient;
	secureClient.setInsecure();
	// WiFiClientSecure's mbedtls handshake loop has its OWN timeout,
	// entirely separate from HTTPClient::setTimeout() below -- it defaults
	// to 120s (NetworkClientSecure's constructor), so a stalled handshake
	// could block this call (and therefore the whole loop() task, since
	// this now runs via serviceWeatherTask() rather than a web request
	// handler) for up to two minutes, which is exactly what caused erratic
	// 30s sensor sample timing on real hardware. Bound it explicitly.
	secureClient.setHandshakeTimeout(5);	// seconds
	HTTPClient http;
	http.setTimeout(10000);

	bool fetchOk = false;
	String body = "";
	if (http.begin(secureClient, url)) {
		int httpCode = http.GET();
		if (httpCode == HTTP_CODE_OK) {
			body = http.getString();
			fetchOk = true;
		} else {
			Serial.printf("[weather] fetch failed, HTTP %d\n", httpCode);
		}
	} else {
		Serial.println("[weather] http.begin() failed");
	}
	http.end();

	if (!fetchOk) { markWeatherFetchError(); return; }

	JSONVar weatherResp = JSON.parse(body);
	JSONVar daily = (JSON.typeof(weatherResp) == "object") ? weatherResp["daily"] : JSONVar();
	if (JSON.typeof(daily) != "object") { markWeatherFetchError(); return; }

	writeRawJsonFile(body, "weather_cache.json");

	JSONVar et0Array = daily["et0_fao_evapotranspiration"];
	JSONVar precipArray = daily["precipitation_sum"];
	JSONVar timeArray = daily["time"];
	if (JSON.typeof(et0Array) != "array" || JSON.typeof(precipArray) != "array" || et0Array.length() == 0) {
		markWeatherFetchError();
		return;
	}

	double et0Yesterday = (double)et0Array[0];
	double precipYesterday = (double)precipArray[0];
	double forecastRain24h = (precipArray.length() > 1) ? (double)precipArray[1] : 0.0;

	double forecastRain7d = 0;
	int forecastUpper = precipArray.length() < 8 ? precipArray.length() : 8;
	for (int i = 1; i < forecastUpper; i++) forecastRain7d += (double)precipArray[i];

	String yesterdayDate = (JSON.typeof(timeArray) == "array" && timeArray.length() > 0)
		? String((const char *)timeArray[0]) : currentDayStamp;

	JSONVar prevState = JSON.parse(loadWeatherState());
	JSONVar prevZones = (JSON.typeof(prevState) == "object") ? prevState["zones"] : JSONVar();

	// Walk every zone that exists in controllers.json today -- not just zones
	// that happened to run yesterday. A zone that didn't run still loses
	// water to ET0 and its deficit must still grow; only its applied-water
	// term is 0 for it. Each zone's own deficit/adjust_pct never gets
	// combined with another zone's -- different zones water different
	// physical areas (see the implementation plan).
	ZoneWeatherResult zoneResults[MAX_WEATHER_ZONE_SLOTS];
	int zoneResultCount = 0;
	JSONVar controllersArr = JSON.parse(loadControllers());
	for (int c = 0; c < controllersArr.length() && zoneResultCount < MAX_WEATHER_ZONE_SLOTS; c++) {
		String controllerId = String((const char *)controllersArr[c]["id"]);
		JSONVar programsArr = controllersArr[c]["programs"];
		for (int p = 0; p < programsArr.length() && zoneResultCount < MAX_WEATHER_ZONE_SLOTS; p++) {
			String programId = String((const char *)programsArr[p]["id"]);
			JSONVar zonesArr = programsArr[p]["zones"];
			for (int z = 0; z < zonesArr.length() && zoneResultCount < MAX_WEATHER_ZONE_SLOTS; z++) {
				int zoneNumber = (int)zonesArr[z]["znumber"];

				double prevZoneDeficit = 0.0;
				if (JSON.typeof(prevZones) == "array") {
					for (int pz = 0; pz < prevZones.length(); pz++) {
						String pCid = jsonStringOr(prevZones[pz], "controller", "");
						pCid.toLowerCase();
						if (pCid == controllerId && (int)jsonNumberOr(prevZones[pz], "zone_number", -1) == zoneNumber) {
							prevZoneDeficit = jsonNumberOr(prevZones[pz], "deficit_mm", 0.0);
							break;
						}
					}
				}

				double zoneAppliedMm = 0.0;
				for (uint8_t a = 0; a < MAX_WEATHER_ZONE_SLOTS; a++) {
					if (waterAppliedAccumulators[a].controller == controllerId && waterAppliedAccumulators[a].zoneNumber == zoneNumber) {
						zoneAppliedMm = waterAppliedAccumulators[a].minutesAppliedToday * lookupZoneMmPerMin(controllerId, zoneNumber);
						break;
					}
				}

				double newZoneDeficit = prevZoneDeficit + et0Yesterday - precipYesterday - zoneAppliedMm;
				if (newZoneDeficit < 0) newZoneDeficit = 0;
				if (newZoneDeficit > maxDeficitMm) newZoneDeficit = maxDeficitMm;

				int zoneAdjustPct = (int)round((newZoneDeficit / referenceDeficitMm) * 100.0);
				if (zoneAdjustPct < minAdjustPct) zoneAdjustPct = minAdjustPct;
				if (zoneAdjustPct > maxAdjustPct) zoneAdjustPct = maxAdjustPct;

				zoneResults[zoneResultCount].controller = controllerId;
				zoneResults[zoneResultCount].program = programId;
				zoneResults[zoneResultCount].zoneNumber = zoneNumber;
				zoneResults[zoneResultCount].deficitMm = newZoneDeficit;
				zoneResults[zoneResultCount].weatherAdjustPct = zoneAdjustPct;
				zoneResults[zoneResultCount].appliedMm = zoneAppliedMm;
				zoneResultCount++;
			}
		}
	}

	// Every accumulator slot is consumed/reset each night regardless of
	// whether it matched a zone still present in controllers.json today -- a
	// zone deleted from the schedule must not leave its slot accumulating
	// forever.
	for (uint8_t a = 0; a < MAX_WEATHER_ZONE_SLOTS; a++) {
		waterAppliedAccumulators[a].minutesAppliedToday = 0.0f;
	}

	// Controller-level aggregates, for the log only. Area-weighted average
	// (calibration.json's area_ft2) for the two depth fields -- depths from
	// different-sized zones aren't directly combinable any other way --
	// plain mean for adjust_pct, a dimensionless ratio that area-weighting
	// doesn't apply to.
	ControllerLogAggregate aggregates[8];	// generous headroom over the real ~2 controllers
	int aggregateCount = 0;
	for (int c = 0; c < controllersArr.length() && aggregateCount < 8; c++) {
		String controllerId = String((const char *)controllersArr[c]["id"]);
		double deficitWeightedSum = 0, appliedWeightedSum = 0, areaSum = 0, adjustPctSum = 0;
		int zonesInController = 0;
		for (int i = 0; i < zoneResultCount; i++) {
			if (zoneResults[i].controller != controllerId) continue;
			double area = lookupZoneAreaFt2(controllerId, zoneResults[i].zoneNumber);
			deficitWeightedSum += zoneResults[i].deficitMm * area;
			appliedWeightedSum += zoneResults[i].appliedMm * area;
			areaSum += area;
			adjustPctSum += zoneResults[i].weatherAdjustPct;
			zonesInController++;
		}
		if (zonesInController == 0) continue;
		aggregates[aggregateCount].controller = controllerId;
		aggregates[aggregateCount].deficitMm = areaSum > 0 ? (deficitWeightedSum / areaSum) : 0;
		aggregates[aggregateCount].appliedMm = areaSum > 0 ? (appliedWeightedSum / areaSum) : 0;
		aggregates[aggregateCount].adjustPct = (int)round(adjustPctSum / zonesInController);
		aggregateCount++;
	}

	// Program-level aggregates, for the log only -- same area-weighted/plain-
	// mean pattern as the controller-level aggregates above, but keyed by
	// (controller, program) since a controller's programs can water disjoint
	// physical areas (e.g. field's program A/B split) and must not be blended
	// into one number any more than yard and field should be.
	ProgramLogAggregate programAggregates[16];	// generous headroom over the real ~3 programs
	int programAggregateCount = 0;
	for (int c = 0; c < controllersArr.length() && programAggregateCount < 16; c++) {
		String controllerId = String((const char *)controllersArr[c]["id"]);
		JSONVar programsArrForAgg = controllersArr[c]["programs"];
		for (int p = 0; p < programsArrForAgg.length() && programAggregateCount < 16; p++) {
			String programId = String((const char *)programsArrForAgg[p]["id"]);
			double deficitWeightedSum = 0, appliedWeightedSum = 0, areaSum = 0, adjustPctSum = 0;
			int zonesInProgram = 0;
			for (int i = 0; i < zoneResultCount; i++) {
				if (zoneResults[i].controller != controllerId || zoneResults[i].program != programId) continue;
				double area = lookupZoneAreaFt2(controllerId, zoneResults[i].zoneNumber);
				deficitWeightedSum += zoneResults[i].deficitMm * area;
				appliedWeightedSum += zoneResults[i].appliedMm * area;
				areaSum += area;
				adjustPctSum += zoneResults[i].weatherAdjustPct;
				zonesInProgram++;
			}
			if (zonesInProgram == 0) continue;
			programAggregates[programAggregateCount].controller = controllerId;
			programAggregates[programAggregateCount].program = programId;
			programAggregates[programAggregateCount].deficitMm = areaSum > 0 ? (deficitWeightedSum / areaSum) : 0;
			programAggregates[programAggregateCount].appliedMm = areaSum > 0 ? (appliedWeightedSum / areaSum) : 0;
			programAggregates[programAggregateCount].adjustPct = (int)round(adjustPctSum / zonesInProgram);
			programAggregateCount++;
		}
	}

	// Evaluated fresh every fetch -- if tomorrow's forecast no longer shows
	// significant rain, there's no reason to keep skipping the next run just
	// because an earlier forecast did.
	bool rainSkip = (rainSkipThresholdMm > 0) && (forecastRain24h > rainSkipThresholdMm);

	time_t utcEpoch = (time_t)timeClient.getEpochTime() - TIME_ZONE;
	String lastFetchUtc = formatUtcIso8601(utcEpoch);

	String stateJson = buildWeatherStateJson(lastFetchUtc, "ok", currentDayStamp,
	                                          zoneResults, zoneResultCount,
	                                          forecastRain24h, forecastRain7d, rainSkip);
	writeRawJsonFile(stateJson, "weather_state.json");

	appendWeatherLogRecord(yesterdayDate, et0Yesterday, precipYesterday,
	                        aggregates, aggregateCount,
	                        programAggregates, programAggregateCount, rainSkip);

	Serial.printf("[weather] updated %d zone(s) across %d controller(s), skip=%s\n",
	              zoneResultCount, aggregateCount, rainSkip ? "true" : "false");
}

// Cooperative, called once per loop() iteration -- no FreeRTOS task, matching
// every other background job in this firmware. Internally gated so the real
// work (an HTTPS round-trip) only happens once a day. Gating on date-changed
// rather than an exact minute window means a reboot near midnight self-heals
// instead of only working in a 1-minute window once a day.
void serviceWeatherTask() {
	// serviceWeatherTask() itself is called every loop() iteration (not
	// timerDelay-gated), so once due, this cooldown is what stops a failed
	// fetch from retrying on every ~100ms loop tick and hammering the network
	// with back-to-back 10s-timeout HTTPS attempts. 5 minutes between
	// attempts is frequent enough to recover quickly once connectivity
	// returns, without spamming retries while it's down.
	static const unsigned long WEATHER_RETRY_COOLDOWN_MS = 5UL * 60UL * 1000UL;
	static unsigned long lastWeatherAttemptMillis = 0;

	// User-triggered "Fetch Now" (handleWeatherFetchNow()) -- bypasses the
	// enabled/day-changed/cooldown gates entirely, same as the old direct
	// call used to, but runs here on loop()'s task instead of inside the
	// request handler so a slow API response can't trip the async_tcp
	// task's watchdog.
	if (weatherFetchNowRequested) {
		weatherFetchNowRequested = false;
		lastWeatherAttemptMillis = millis();
		fetchAndApplyWeatherUpdate();
		return;
	}

	// Both site.json (weather enabled flags) and weather_state.json
	// (last_fetch_date) only ever change via a save/a fetch completing, so
	// re-reading and re-parsing them from SPIFFS on every single loop()
	// tick (this function is called unconditionally, not timerDelay-gated)
	// was pure overhead -- plus loadZoneTable()'s built-in debug print
	// meant every tick spammed the full file contents to Serial. Cache the
	// whole "is a fetch due" decision together and only re-derive it every
	// few seconds; a few seconds of lag here is imperceptible since the
	// actual fetch itself is gated to once a day, and a failed fetch is
	// additionally cooled down below regardless of this cache.
	static const unsigned long WEATHER_CHECK_INTERVAL_MS = 5000UL;
	static unsigned long lastCheckMillis = 0;
	static bool dueForFetch = false;
	unsigned long nowMs = millis();
	if (lastCheckMillis == 0 || (nowMs - lastCheckMillis) >= WEATHER_CHECK_INTERVAL_MS) {
		lastCheckMillis = nowMs;
		dueForFetch = false;

		JSONVar siteDoc = JSON.parse(loadSite());
		JSONVar weatherSettings = (JSON.typeof(siteDoc) == "object") ? siteDoc["weather"] : JSONVar();
		bool weatherEnabled = (JSON.typeof(weatherSettings) == "object") && jsonBoolOr(weatherSettings, "enabled", false);

		if (weatherEnabled) {
			int currentHourInt = currentTimeStamp.substring(0, 2).toInt();
			int currentMinuteInt = currentTimeStamp.substring(3, 5).toInt();
			bool pastFetchTime = (currentHourInt > 0) || (currentMinuteInt >= 5);

			if (pastFetchTime) {
				JSONVar state = JSON.parse(loadWeatherState());
				String lastFetchDate = (JSON.typeof(state) == "object") ? jsonStringOr(state, "last_fetch_date", "") : "";
				dueForFetch = (lastFetchDate != currentDayStamp);	// not yet fetched today
			}
		}
	}
	if (!dueForFetch) return;

	unsigned long now = millis();
	if (lastWeatherAttemptMillis != 0 && (now - lastWeatherAttemptMillis) < WEATHER_RETRY_COOLDOWN_MS) return;
	lastWeatherAttemptMillis = now;

	fetchAndApplyWeatherUpdate();
}

// site.json on-disk shape: flat site fields (name/sensor_interval_sec/
// psi_offset/latitude/longitude/timezone/mm_per_min_default) plus a nested
// "weather" object (auto-adjust settings) -- see the JSON-split refactor.
String loadSite() {
	String data = loadZoneTable(SPIFFS, "/site.json");
	if (data.length() <= 2) return "{}";	// loadZoneTable's sentinel is "[]"; site.json is an object
	return data;
}

// Takes an already-parsed JSONVar (not a String) deliberately -- every real
// caller (migration, onWsEvent's saveZones, /submit-zone-form) already holds
// a parsed JSONVar of the incoming document, so re-stringifying it just to
// re-parse it here would briefly hold two full copies of the tree in heap
// at once. On a device already running a TFT framebuffer + WiFi stack,
// that redundant round-trip for a large schedule was enough to exhaust
// heap and crash mid-save -- this was the actual cause of an observed
// reboot during Save All, not just a theoretical inefficiency.
bool saveSite(JSONVar siteVar) {
	if (JSON.typeof(siteVar) != "object") return false;
	return writeRawJsonFile(JSON.stringify(siteVar), "site.json");
}

// controllers.json on-disk shape: bare array, same shape as the old
// top-level "controllers" key. The filename param preserves the CONFIG
// page's Save-As feature as a controllers-only named schedule preset --
// site.json is never part of a named snapshot, so "site.json" is rejected
// here as a defense-in-depth guard against a mistyped preset name.
String loadControllers() {
	return loadZoneTable(SPIFFS, "/controllers.json");	// sentinel "[]" is already correct for an array
}

bool saveControllers(JSONVar controllersVar, String filename) {
	if (JSON.typeof(controllersVar) != "array") return false;
	if (!isSafeJsonFilename(filename)) return false;
	if (filename == "site.json") return false;
	return writeRawJsonFile(JSON.stringify(controllersVar), filename);
}

// Merges the old schema's sibling "site"/"weather" keys into site.json's
// flat-with-nested-weather shape via direct JSONVar key copying (see
// buildCombinedZoneDocJson()'s reverse direction) -- no stringify/re-parse
// round-trip, for the same heap-pressure reason saveSite()/saveControllers()
// now take JSONVar directly.
//
// Every assignment here goes through a named local (`val`/`weatherVal`)
// rather than `result[key] = siteFields[key]` directly. Arduino_JSON's
// operator[] returns a proxy JSONVar by value; when the right-hand side is
// itself a fresh operator[] call (a prvalue), C++ overload resolution
// prefers the library's move-assignment operator over its copy-assignment
// operator. That move overload just swaps the two temporaries' internal
// pointers and never patches the parent tree, so the write silently
// vanishes -- confirmed with a standalone repro against this exact library
// version. Assigning from a named variable (an lvalue) forces the
// copy-assignment overload instead, which does the real write-back. This
// was the actual cause of site.json saving as all-null fields.
JSONVar buildSiteVarFromParts(JSONVar siteFields, JSONVar weatherFields) {
	JSONVar result;
	if (JSON.typeof(siteFields) == "object") {
		JSONVar siteKeys = siteFields.keys();
		for (int i = 0; i < siteKeys.length(); i++) {
			String key = String((const char *)siteKeys[i]);
			JSONVar val = siteFields[key];
			result[key] = val;
		}
	}
	JSONVar weatherVal = (JSON.typeof(weatherFields) == "object") ? weatherFields : JSONVar();
	result["weather"] = weatherVal;
	return result;
}

// Reconstructs the legacy {site, weather, controllers} document shape from
// the split files, so data/config.js keeps consuming the same shape it
// always has -- no client rewrite needed for rendering. Copies each of
// site.json's keys through via JSONVar assignment (skipping the nested
// "weather" object, promoted back to a sibling key) rather than reading
// each field into a C++ double/String and reformatting it with a fixed
// decimal count -- that earlier approach truncated precision on fields
// like psi_offset, which made saveZoneDoc()'s post-save verify step fail
// (byte-for-byte comparison) whenever a saved value had more decimal
// digits than the hardcoded format allowed.
String buildCombinedZoneDocJson() {
	JSONVar siteDoc = JSON.parse(loadSite());
	JSONVar weatherDoc = (JSON.typeof(siteDoc) == "object") ? siteDoc["weather"] : JSONVar();

	JSONVar siteOnly;
	if (JSON.typeof(siteDoc) == "object") {
		JSONVar siteKeys = siteDoc.keys();
		for (int i = 0; i < siteKeys.length(); i++) {
			String key = String((const char *)siteKeys[i]);
			if (key == "weather") continue;
			JSONVar val = siteDoc[key];	// named local -- see buildSiteVarFromParts()'s comment
			siteOnly[key] = val;
		}
	}

	String siteStr = (JSON.typeof(siteOnly) == "object") ? JSON.stringify(siteOnly) : "{}";
	String weatherStr = (JSON.typeof(weatherDoc) == "object") ? JSON.stringify(weatherDoc) : "{}";
	String controllersStr = loadControllers();
	if (controllersStr.length() < 2) controllersStr = "[]";

	return "{\"site\":" + siteStr + ",\"weather\":" + weatherStr + ",\"controllers\":" + controllersStr + "}";
}

// weatherPct is a multiplier expressed as a percentage where 100 = neutral
// (e.g. 70 = run at 70%, 150 = run at 150%) -- distinct from seasonalPct,
// which is a +/- delta (e.g. 20 = +20%). weatherPct <= 0 is treated as
// "unset/disabled", defaulting to 100 -- it must never default to 0, which
// would silently zero out every scheduled run.
int applyRunAdjustments(int baseRunMinutes, int seasonalPct, int weatherPct) {
	if (baseRunMinutes <= 0) return baseRunMinutes;
	int effectiveWeatherPct = (weatherPct <= 0) ? 100 : weatherPct;
	int adjusted = (int)round(baseRunMinutes * (1.0 + seasonalPct / 100.0) * (effectiveWeatherPct / 100.0));
	if (adjusted < 1) adjusted = 1;
	return adjusted;
}

// seasonal_adjust_pct and zone_delay_sec now live directly on each program
// object in controllers.json (no more separate seasonal_adjustments.json /
// zone_delays.json side files) -- checkActiveZone() and findNextProgramZone()
// read seasonal_adjust_pct straight off the parsed program object; this is
// the one remaining lookup needed elsewhere (serviceRemoteZoneControl,
// serviceManualZoneRuns), kept with its old signature so those call sites
// don't need to change.
int lookupZoneDelaySeconds(String controller, String program) {
	controller.trim();
	controller.toLowerCase();
	program.trim();
	program.toUpperCase();
	if (program.length() == 0) program = "A";

	JSONVar controllers = JSON.parse(loadControllers());
	if (JSON.typeof(controllers) != "array") return 0;

	for (int c = 0; c < controllers.length(); c++) {
		String cid = String((const char *)controllers[c]["id"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;

		JSONVar programs = controllers[c]["programs"];
		for (int p = 0; p < programs.length(); p++) {
			String pid = String((const char *)programs[p]["id"]);
			pid.trim();
			pid.toUpperCase();
			if (pid == program) return (int)programs[p]["zone_delay_sec"];
		}
	}
	return 0;
}

// weather.auto_adjust (in site.json, user-editable) gates whether
// weather_adjust_pct is actually applied to scheduling at all -- distinct
// from weather.enabled, which only gates whether serviceWeatherTask() keeps
// fetching/computing it. This lets a user watch the computed numbers without
// trusting them yet. Returns 100 (neutral) whenever auto_adjust is off, this
// zone has no weather_state.json entry, or the file is absent. Looked up
// per-zone (not per-program) -- different zones water different physical
// areas and now carry their own independent deficit/adjustment.
int lookupZoneWeatherAdjustPct(String controller, int zoneNumber) {
	controller.trim();
	controller.toLowerCase();

	JSONVar siteDoc = JSON.parse(loadSite());
	JSONVar weatherSettings = (JSON.typeof(siteDoc) == "object") ? siteDoc["weather"] : JSONVar();
	if (JSON.typeof(weatherSettings) != "object" || !jsonBoolOr(weatherSettings, "auto_adjust", false)) return 100;

	JSONVar state = JSON.parse(loadWeatherState());
	if (JSON.typeof(state) != "object") return 100;
	JSONVar zones = state["zones"];
	if (JSON.typeof(zones) != "array") return 100;

	for (int z = 0; z < zones.length(); z++) {
		String cid = jsonStringOr(zones[z], "controller", "");
		cid.toLowerCase();
		if (cid == controller && (int)jsonNumberOr(zones[z], "zone_number", -1) == zoneNumber) {
			int pct = (int)jsonNumberOr(zones[z], "weather_adjust_pct", 100);
			return pct <= 0 ? 100 : pct;
		}
	}
	return 100;
}

// Reads skip_next_run for (controller,program); if true, clears it (string-
// concat rebuild of weather_state.json, never a parsed-JSONVar mutation) and
// returns the pre-clear value. Only ever called from checkActiveZone()'s own
// scheduled-evaluation loop -- the manual-run facility (serviceManualZoneRuns/
// startManualProgramRun/startManualZoneRun) never calls this, which is what
// structurally keeps a manual override from being able to clear a skip meant
// for the next *scheduled* run.
bool consumeSkipNextRun(String controller, String program) {
	controller.trim();
	controller.toLowerCase();
	program.trim();
	program.toUpperCase();

	// Same weather.auto_adjust gate as lookupZoneWeatherAdjustPct() -- "Apply
	// to Schedules" off means weather data affects nothing, not just the
	// percent adjustment. Returns before touching weather_state.json at all, so a
	// pending skip is left untouched (not consumed) rather than discarded --
	// it takes effect on its program's next matching day once auto_adjust is
	// re-enabled.
	JSONVar siteDoc = JSON.parse(loadSite());
	JSONVar weatherSettings = (JSON.typeof(siteDoc) == "object") ? siteDoc["weather"] : JSONVar();
	if (JSON.typeof(weatherSettings) != "object" || !jsonBoolOr(weatherSettings, "auto_adjust", false)) return false;

	JSONVar state = JSON.parse(loadWeatherState());
	if (JSON.typeof(state) != "object") return false;
	JSONVar programs = state["programs"];
	if (JSON.typeof(programs) != "array") return false;

	bool wasSkipped = false;
	String rebuilt = "{";
	rebuilt += "\"last_fetch_utc\":\"" + jsonStringOr(state, "last_fetch_utc", "") + "\",";
	rebuilt += "\"last_fetch_status\":\"" + jsonStringOr(state, "last_fetch_status", "") + "\",";
	rebuilt += "\"last_fetch_date\":\"" + jsonStringOr(state, "last_fetch_date", "") + "\",";
	rebuilt += "\"forecast_rain_24h_mm\":" + String(jsonNumberOr(state, "forecast_rain_24h_mm", 0), 2) + ",";
	rebuilt += "\"forecast_rain_7d_mm\":" + String(jsonNumberOr(state, "forecast_rain_7d_mm", 0), 2) + ",";

	// zones[] (each zone's own deficit_mm/weather_adjust_pct) is untouched by
	// a skip decision -- passed through verbatim, never re-derived here.
	JSONVar zones = state["zones"];
	rebuilt += "\"zones\":[";
	if (JSON.typeof(zones) == "array") {
		for (int z = 0; z < zones.length(); z++) {
			if (z > 0) rebuilt += ",";
			rebuilt += "{\"controller\":\"" + jsonStringOr(zones[z], "controller", "") + "\","
			           "\"zone_number\":" + String((int)jsonNumberOr(zones[z], "zone_number", 0)) + ","
			           "\"deficit_mm\":" + String(jsonNumberOr(zones[z], "deficit_mm", 0), 2) + ","
			           "\"weather_adjust_pct\":" + String((int)jsonNumberOr(zones[z], "weather_adjust_pct", 100)) + "}";
		}
	}
	rebuilt += "],";

	rebuilt += "\"programs\":[";
	bool first = true;
	for (int p = 0; p < programs.length(); p++) {
		String pid = jsonStringOr(programs[p], "program", "");
		String cid = jsonStringOr(programs[p], "controller", "");
		bool skip = jsonBoolOr(programs[p], "skip_next_run", false);
		String pidUpper = pid; pidUpper.toUpperCase();
		String cidLower = cid; cidLower.toLowerCase();
		if (cidLower == controller && pidUpper == program && skip) {
			wasSkipped = true;
			skip = false;	// consumed
		}

		if (!first) rebuilt += ",";
		rebuilt += "{\"controller\":\"" + cid + "\",\"program\":\"" + pid + "\","
		           "\"skip_next_run\":" + String(skip ? "true" : "false") + "}";
		first = false;
	}
	rebuilt += "]}";

	if (wasSkipped) writeRawJsonFile(rebuilt, "weather_state.json");
	return wasSkipped;
}

void loadSchedulesEnabled() {
	String data = loadZoneTable(SPIFFS, "/schedules_enabled.json");
	data.trim();
	if (data.length() == 0 || data == "[]") {
		schedulesEnabled = true; // file missing -- default to enabled
		return;
	}
	schedulesEnabled = !(data == "false" || data == "0");
}

bool saveSchedulesEnabled(bool enabled, String &message) {
	bool ok = writeRawJsonFile(enabled ? "true" : "false", "schedules_enabled.json");
	if (ok) schedulesEnabled = enabled;
	message = ok ? "Schedules setting saved" : "Failed to save schedules setting";
	return ok;
}

String lookupZoneName(String controller, uint8_t relay) {
	controller.trim();
	controller.toLowerCase();

	JSONVar controllers = JSON.parse(loadControllers());
	if (JSON.typeof(controllers) != "array") return "";

	for (int c = 0; c < controllers.length(); c++) {
		String cid = String((const char *)controllers[c]["id"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;

		JSONVar programs = controllers[c]["programs"];
		for (int p = 0; p < programs.length(); p++) {
			JSONVar zones = programs[p]["zones"];
			for (int z = 0; z < zones.length(); z++) {
				if ((uint8_t)(int)zones[z]["znumber"] == relay) {
					return String((const char *)zones[z]["zname"]);
				}
			}
		}
	}
	return "";
}

String lookupZoneProgram(String controller, uint8_t relay) {
	controller.trim();
	controller.toLowerCase();

	JSONVar controllers = JSON.parse(loadControllers());
	if (JSON.typeof(controllers) != "array") return "A";

	for (int c = 0; c < controllers.length(); c++) {
		String cid = String((const char *)controllers[c]["id"]);
		cid.trim();
		cid.toLowerCase();
		if (cid != controller) continue;

		JSONVar programs = controllers[c]["programs"];
		for (int p = 0; p < programs.length(); p++) {
			JSONVar zones = programs[p]["zones"];
			for (int z = 0; z < zones.length(); z++) {
				if ((uint8_t)(int)zones[z]["znumber"] == relay) {
					return String((const char *)programs[p]["id"]);
				}
			}
		}
	}
	return "A";
}

int calculateDayOfWeek(int year, int month, int day) {
	// Adjust months for Zeller's Congruence (March = 3, ..., December = 12, January = 13, February = 14)
	if (month < 3) {
		month += 12;
		year -= 1;
	}

	int k = year % 100;	 // The year within the century
	int j = year / 100;	 // The zero-based century

	// Zeller's formula to calculate the day of the week
	int dayOfWeek = (day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7;

	// Zeller's Congruence gives 0 = Saturday, adjust to match 0 = Sunday
	dayOfWeek = (dayOfWeek + 6) % 7;

	return dayOfWeek;
}

// `days` is the owning program's JSON integer array (0=Sun..6=Sat), per the
// nested controllers.json schema -- day-matching happens once per program
// now, not per zone.
bool isDayMatching(JSONVar days, int currentDay) {
	for (int i = 0; i < days.length(); i++) {
		if ((int)days[i] == currentDay) return true;
	}
	return false;
}

static const char *DAY_ABBR_MAIN[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

// Pre-formats a program's day-array into a display string ("Mo We Fr") for
// the active-zone snapshot -- the one canonical place this conversion
// happens, so Display.cpp no longer needs its own digit-string parser.
String formatDaysForDisplay(JSONVar days) {
	if (JSON.typeof(days) != "array" || days.length() == 0) return "NONE";
	String out = "";
	for (int i = 0; i < days.length(); i++) {
		int d = (int)days[i];
		if (d < 0 || d > 6) continue;
		if (out.length() > 0) out += ' ';
		out += DAY_ABBR_MAIN[d];
	}
	return out.length() > 0 ? out : "NONE";
}

int parseScheduleMinutes(String timeStr) {
	timeStr.trim();
	if (timeStr.length() < 5) return -1;

	int hour = timeStr.substring(0, 2).toInt();
	int minute = timeStr.substring(3, 5).toInt();
	if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;

	return hour * 60 + minute;
}

String formatScheduleMinutes(int totalMinutes) {
	const int minutesInDay = 24 * 60;
	int normalized = totalMinutes % minutesInDay;
	if (normalized < 0) normalized += minutesInDay;

	char buffer[6];
	snprintf(buffer, sizeof(buffer), "%02d:%02d", normalized / 60, normalized % 60);
	return String(buffer);
}

// Flat "nothing currently active" snapshot, built fresh each time rather than
// relying on a synthetic data row.
// `znumber`/`avgpsi`/`run` are stored as JSON strings here (not numbers) to
// match the established "Active Zone" snapshot contract that buildSensorUpdateJson,
// logData, and the ZoneInfo population block all consume via (const char *) casts --
// this mirrors the old pre-split flat schema's behavior, where every field was
// itself a string, even though controllers.json now stores znumber/avgpsi/run as
// real numbers.
String buildOffZoneSnapshot() {
	JSONVar off;
	off["znumber"] = "0";
	off["zname"] = "OFF";
	off["controller"] = "off";
	off["program"] = "";
	off["avgpsi"] = "0";
	off["run"] = "0";
	off["start"] = "00:00";
	off["days"] = "NONE";
	return JSON.stringify(off);
}

String checkActiveZone() {
	// Extract the date part from currentTimeStamp (assumed format: "HH:MM:SS")
	// CurrentDayStamp: "YYYY-MM-DD"
	// Not using Seconds
	int year = currentDayStamp.substring(0, 4).toInt();
	int month = currentDayStamp.substring(5, 7).toInt();
	int day = currentDayStamp.substring(8, 10).toInt();
	int currentHourInt = currentTimeStamp.substring(0, 2).toInt();
	int currentMinuteInt = currentTimeStamp.substring(3, 5).toInt();

	// Calculate the day of the week based on the date (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
	int currentDay = calculateDayOfWeek(year, month, day);

	// Load the schedule from the file system
	JSONVar controllers = JSON.parse(loadControllers());
	if (JSON.typeof(controllers) != "array" || controllers.length() == 0) {
		logMsg("No controllers available");
		return buildOffZoneSnapshot();
	}

	if (!schedulesEnabled) {
		// Scheduled irrigation is suspended; report idle exactly as the
		// "nothing currently active" fallback below does. Manual zone/program
		// runs are a separate code path entirely and are unaffected.
		return buildOffZoneSnapshot();
	}

	int currentMinutes = currentHourInt * 60 + currentMinuteInt;

	// Reconstruct the active zone from each program's schedule every sample.
	// This survives rebooting mid-chain since the cursor is recomputed from
	// program.start each time rather than persisted.
	for (int c = 0; c < controllers.length(); c++) {
		JSONVar controller = controllers[c];
		String controllerId = String((const char *)controller["id"]);

		JSONVar programs = controller["programs"];
		for (int p = 0; p < programs.length(); p++) {
			JSONVar program = programs[p];

			if (!isDayMatching(program["days"], currentDay)) continue;

			String programId = String((const char *)program["id"]);

			// Weather rain-skip: consumed (read-then-cleared) the first time
			// this program's matching day is evaluated. Re-checking on every
			// sample tick once it's already false is a harmless no-op -- this
			// is not a "once per day" guard, isDayMatching just only matches
			// one calendar day at a time. Never touched by the manual-run
			// facility, which doesn't call consumeSkipNextRun at all.
			if (consumeSkipNextRun(controllerId, programId)) {
				logMsg(("Weather rain-skip: " + controllerId + "/" + programId + " skipped today").c_str());
				continue;
			}

			int startMinutes = parseScheduleMinutes(String((const char *)program["start"]));
			if (startMinutes < 0) continue;

			int percent = (int)program["seasonal_adjust_pct"];
			String daysDisplay = formatDaysForDisplay(program["days"]);

			JSONVar zones = program["zones"];
			int cursorMinutes = startMinutes;
			for (int z = 0; z < zones.length(); z++) {
				JSONVar zone = zones[z];
				int zoneNumber = (int)zone["znumber"];
				int weatherPct = lookupZoneWeatherAdjustPct(controllerId, zoneNumber);
				int baseRunMinutes = (int)zone["run"];
				int runMinutes = applyRunAdjustments(baseRunMinutes, percent, weatherPct);
				if (runMinutes <= 0) continue;

				// Decision: zone_delay_sec is a pre-ON pause that shortens a
				// zone's own actual on-time; it does NOT shift this nominal
				// schedule cursor (see serviceRemoteZoneControl below and the
				// implementation plan's decision #1).
				if (currentMinutes >= cursorMinutes && currentMinutes < cursorMinutes + runMinutes) {
					JSONVar activeZone;
					activeZone["znumber"] = String(zoneNumber);
					activeZone["zname"] = String((const char *)zone["zname"]);
					activeZone["controller"] = controllerId;
					activeZone["program"] = programId;
					activeZone["avgpsi"] = String((int)zone["avgpsi"]);
					activeZone["run"] = String(runMinutes);
					activeZone["start"] = formatScheduleMinutes(cursorMinutes);
					activeZone["days"] = daysDisplay;
					return JSON.stringify(activeZone);
				}

				cursorMinutes += runMinutes;
				if (currentMinutes < cursorMinutes) break;
			}
		}
	}

	return buildOffZoneSnapshot();
}

void notFound(AsyncWebServerRequest *request) {
	request->send(404, "text/plain", "Not found");
}

// Initialize WiFi
void initWiFi() {
	WiFi.mode(WIFI_STA);
	WiFi.setAutoReconnect(true);
	WiFi.persistent(false);
	WiFi.setSleep(false);   // don't want wifi sleeping
	WiFi.begin(ssid, password);
	Serial.print("Connecting to WiFi ..");
	unsigned long startAttemptTime = millis();

	while (WiFi.status() != WL_CONNECTED &&
				 millis() - startAttemptTime < 10000) {	 // 10 seconds timeout
		Serial.print('.');
		delay(500);
	}

	if (WiFi.status() == WL_CONNECTED) {
		Serial.println("NOW Connected to WiFi");
	} else {
		Serial.println("Failed to connect to WiFi");
	}
}

// ---------------------------------------------------------------------------
// Weather auto-adjust -- Phase 1 hardware verification spikes.
//
// These two debug endpoints exist purely to prove, on real hardware, that
// (a) this firmware can complete an HTTPS/TLS request at all -- it never has
// before; NTP is UDP, not TLS -- and (b) weather_state.json survives a write
// and a reboot. No budget/scheduling logic depends on these yet. Safe to
// leave in place after verification; they don't conflict with anything
// added in later phases.
// ---------------------------------------------------------------------------

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

void handleDebugWeatherStateRoundTrip(AsyncWebServerRequest *request) {
	String sample =
		"{\"last_fetch_utc\":\"2026-06-26T07:05:00Z\",\"last_fetch_status\":\"ok\","
		"\"last_fetch_date\":\"2026-06-26\","
		"\"forecast_rain_24h_mm\":0.0,\"forecast_rain_7d_mm\":12.5,"
		"\"zones\":[{\"controller\":\"yard\",\"zone_number\":1,\"deficit_mm\":4.2,\"weather_adjust_pct\":100},"
		"{\"controller\":\"field\",\"zone_number\":9,\"deficit_mm\":1.1,\"weather_adjust_pct\":70}],"
		"\"programs\":[{\"controller\":\"yard\",\"program\":\"A\",\"skip_next_run\":false},"
		"{\"controller\":\"field\",\"program\":\"B\",\"skip_next_run\":false}]}";

	bool wrote = writeRawJsonFile(sample, "weather_state.json");
	String readBack = loadWeatherState();

	String result = "{\"wrote\":" + String(wrote ? "true" : "false") +
	                 ",\"matches\":" + String(readBack == sample ? "true" : "false") +
	                 ",\"readBack\":" + readBack + "}";
	request->send(wrote ? 200 : 500, "application/json", result);
}

// User-triggered "Fetch Now" (Config page) -- queues the same daily
// sequence serviceWeatherTask() runs automatically at 00:05, bypassing its
// day-changed gate entirely. Does NOT call fetchAndApplyWeatherUpdate()
// directly here -- that's a blocking HTTPS call (up to 10s) and this
// handler runs on the async_tcp task, which crashes the device via the 5s
// task watchdog if a handler blocks that long (confirmed via a real
// device reboot). serviceWeatherTask() picks the request up on its next
// loop() tick instead, where the same call has always run safely. The
// client (config.js's fetchWeatherNow()) polls /weather-state afterward
// rather than getting the fresh result in this response.
void handleWeatherFetchNow(AsyncWebServerRequest *request) {
	weatherFetchNowRequested = true;
	AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"queued\":true}");
	response->addHeader("Cache-Control", "no-store");
	request->send(response);
}

void handleDebugWeatherAccumulators(AsyncWebServerRequest *request) {
	String json = "[";
	bool first = true;
	for (uint8_t i = 0; i < MAX_WEATHER_ZONE_SLOTS; i++) {
		if (waterAppliedAccumulators[i].controller.length() == 0) continue;
		if (!first) json += ",";
		json += "{\"controller\":\"" + waterAppliedAccumulators[i].controller + "\","
		        "\"zoneNumber\":" + String(waterAppliedAccumulators[i].zoneNumber) + ","
		        "\"minutesAppliedToday\":" + String(waterAppliedAccumulators[i].minutesAppliedToday, 2) + "}";
		first = false;
	}
	json += "]";
	request->send(200, "application/json", json);
}

void handleDebugWeatherSeedAccumulator(AsyncWebServerRequest *request) {
	if (!request->hasParam("controller") || !request->hasParam("zone") || !request->hasParam("minutes")) {
		request->send(400, "text/plain", "Missing ?controller=&zone=&minutes=");
		return;
	}
	String controller = request->getParam("controller")->value();
	int zoneNumber = request->getParam("zone")->value().toInt();
	float minutes = request->getParam("minutes")->value().toFloat();

	int slot = findOrCreateWaterAccumulatorSlot(controller, zoneNumber);
	if (slot < 0) {
		request->send(500, "text/plain", "No free accumulator slot");
		return;
	}
	waterAppliedAccumulators[slot].minutesAppliedToday = minutes;
	request->send(200, "text/plain", "ok");
}

// Location/sensor-rate/PSI-offset now live in site.json (read once at boot
// below, written independently of the schedule via saveSiteSettings()/
// `/submit-site-form` on the client side) instead of separate SD-card text
// files. PSI calibration math (offset = rawPressure - userEnteredActualPsi)
// is computed client-side now, since the browser already receives live
// rawPressure via the SSE `new-readings` event.

String generateDailyFilename() {
	getTimeStamp(currentDayStamp, currentTimeStamp);

	// Extract the day, month, and year from the date string
	int year = currentDayStamp.substring(2, 4).toInt();	 // last two digits of the year
	int month = currentDayStamp.substring(5, 7).toInt();
	int dayOfMonth = currentDayStamp.substring(8, 10).toInt();

	// Format the filename as DDMMYY.csv
	char filename[12];
	snprintf(filename, sizeof(filename), "%02d%02d%02d.csv", dayOfMonth, month, year);

	return String(filename);
}

void ensureDailyLogFileExists() {
	String fileName = "/" + currentDailyFilename;
	if (!SD.exists(fileName.c_str())) {
		File file = SD.open(fileName.c_str(), FILE_WRITE);
		if (!file) {
			Serial.println("[6] FAILED to create daily log file");
			logMsg("Failed to create the daily log file");
		} else {
			Serial.println("[6] Created new daily log file");
			logMsg("Created new daily log file");
			file.close();
		}
	} else {
		Serial.println("[6] Daily log file already exists");
	}
}

void updateDailyFilename() {
	// Get the current time from NTP
	timeClient.update();
	time_t now = (time_t)timeClient.getEpochTime();
	struct tm *timeinfo = gmtime(&now);

	// Check if it is midnight
	if (timeinfo->tm_hour == 0 && timeinfo->tm_min == 0) {
		// Update the daily filename
		String newDailyFilename = generateDailyFilename();
		if (newDailyFilename != currentDailyFilename) {
			currentDailyFilename = newDailyFilename;
			Serial.println("Daily filename updated: " + currentDailyFilename);
			ensureDailyLogFileExists();
		}
	}
}

// Declare readingsJson as a global variable
JSONVar readingsJson;

String getSensorReading() {
	getTimeStamp(currentDayStamp, currentTimeStamp);

	if (simMode) {
		rawPressure = simPsi;
		currentPressure = simPsi;
	} else {
			uint32_t mvSum = 0;
			for (unsigned int i = 0; i < ADC_SAMPLES; ++i) {
				mvSum += analogReadMilliVolts(SENSOR_PIN);
			}
			double voltage = ((mvSum / (double)ADC_SAMPLES) / 1000.0) * adcVoltageScale;

			rawPressure = sensorPsiPerVolt * voltage + sensorPressureIntercept;
			if (rawPressure < 0.0f) rawPressure = 0.0f;

		// calibOffset is loaded from site.json's psi_offset at boot and
		// updated live on every site settings save (calibration math now happens client-side
		// using the live rawPressure already streamed via SSE) -- no more
		// reactive recompute here.
		currentPressure = rawPressure - calibOffset;
		ADCvoltage = voltage;
	}

	// Create the JSON object with current pressure and active zone details
	// Call checkActiveZone to get the active zone details
	String activeZoneJsonString = checkActiveZone();

	// Parse the activeZoneJsonString into JSONVar
		JSONVar activeZoneJson = JSON.parse(activeZoneJsonString);
		readingsJson["Current Pressure"] = String(currentPressure, 1);
		readingsJson["Raw Pressure"] = String(rawPressure, 1);
		readingsJson["ADC Voltage"] = String(ADCvoltage, 3);
		readingsJson["Active Zone"] = activeZoneJson;	 // Add the active zone details to the JSON

	// Convert to a JSON string and return
	return JSON.stringify(readingsJson);
}

void logData() {
	if (sdCardLock) {
		Serial.println("SD card is busy");
		return;
	}

	// Lock the SD card
	sdCardLock = true;

	// Get the current timestamp
	getTimeStamp(currentDayStamp, currentTimeStamp);
	//int currentHourInt = currentTimeStamp.substring(0, 2).toInt();
	//int currentMinuteInt = currentTimeStamp.substring(3, 5).toInt();

	// Extract the znumber field from the JSON
	String activeZoneNum = String((const char *)readingsJson["Active Zone"]["znumber"]);
	String activeZoneAvg = String((const char *)readingsJson["Active Zone"]["avgpsi"]);

	// Create the data message to be logged
	String dataMessage = String(readingID) + "," + 
								currentDayStamp + "," +
								currentTimeStamp + "," + 
								String(currentPressure) + "," +
								activeZoneNum + "," +
								activeZoneAvg + "\r\n";

	Serial.print("Saved data: ");
	Serial.println(dataMessage);

	// Open or create the daily log file in append mode
	String fileName = "/" + currentDailyFilename;
	File file = SD.open(fileName.c_str(), FILE_APPEND);

	if (!file) {
		logMsg("Failed to open file for appending");
		sdCardLock = false;
		return;
	}

	// Append the data to the file
	if (!file.print(dataMessage)) {
		logMsg("Failed to append data");
	}

	file.close();

	// Increment the reading ID
	readingID++;

	// Unlock the SD card
	sdCardLock = false;
}

void deleteFileHandler(AsyncWebServerRequest *request) {
	if (request->hasParam("filename")) {
		String filename = request->getParam("filename")->value();

		// Log the filename received
		Serial.println("Requested file for deletion: " + filename);
		if (isHiddenSystemPath(filename)) {
			request->send(403, "text/plain", "Protected system folder cannot be deleted");
			return;
		}

		if (!filename.startsWith("/")) {
			filename = "/" + filename;
		}

		bool success = false;

		// Check if the file exists on SPIFFS and try to delete it
		if (SPIFFS.exists(filename)) {
			Serial.println("File found on SPIFFS. Attempting to delete...");
			success = SPIFFS.remove(filename);
		}
		// Check if the file exists on SD card and try to delete it
		else if (SD.exists(filename)) {
			Serial.println("File found on SD card. Attempting to delete...");
			success = SD.remove(filename);
		} else {
			Serial.println("File not found on either SPIFFS or SD card.");
		}

		// Log the result of the deletion attempt
		if (success) {
			Serial.println("File deleted successfully.");
			request->send(200, "text/plain", "File deleted successfully");
		} else {
			Serial.println("Failed to delete file.");
			request->send(500, "text/plain", "Failed to delete file");
		}
	} else {
		Serial.println("File not found on either SPIFFS or SD card.");
		request->send(404, "text/plain",
									"File not found on either SPIFFS or SD card.");
	}
}

// ==================== WebSocket helpers ====================

// Sensor update broadcast — sent to all WS clients every sample cycle
// and immediately to any client that connects mid-session.
int parseTimeMinutes(const String &timeStr) {
	if (timeStr.length() < 5) return -1;
	int colon = timeStr.indexOf(':');
	if (colon <= 0) return -1;

	int hours = timeStr.substring(0, colon).toInt();
	int minutes = timeStr.substring(colon + 1, colon + 3).toInt();
	if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return -1;
	return hours * 60 + minutes;
}

String buildMapKey(String controller, String zoneNumber) {
	controller.trim();
	controller.toLowerCase();
	return controller + ":" + zoneNumber;
}

String getZoneRemainingMinutes(String start, String run) {
	int startMin = parseTimeMinutes(start);
	int runMin = run.toInt();
	int nowMin = parseTimeMinutes(currentTimeStamp);
	if (startMin < 0 || nowMin < 0 || runMin <= 0) return "";

	int remaining = startMin + runMin - nowMin;
	if (remaining < 0) remaining = 0;
	return String(remaining) + "m";
}

// Mirrors updateStatStatus() in data/index.js — keep both in sync.
String buildPressureStatus(float psi, float avgPsi) {
	if (avgPsi > 0.0f) {
		float deviation = psi - avgPsi;
		if (fabs(deviation) >= PRESSURE_ALERT_DEVIATION) return deviation > 0.0f ? "HIGH" : "LOW";
		if (fabs(deviation) >= PRESSURE_WARN_DEVIATION) return "WARN";
		return "OK";
	}

	// No target available — fall back to generic safe-pressure thresholds.
	if (psi >= 35.0f) return "OK";
	if (psi >= 25.0f) return "WARN";
	return "LOW";
}

String buildSensorUpdateJson() {
	float zoneAvgPsi = String((const char *)readingsJson["Active Zone"]["avgpsi"]).toFloat();
	String zoneNumber = String((const char *)readingsJson["Active Zone"]["znumber"]);
	String controller = String((const char *)readingsJson["Active Zone"]["controller"]);
	String start = String((const char *)readingsJson["Active Zone"]["start"]);
	String run = String((const char *)readingsJson["Active Zone"]["run"]);
	String json = "{";
	json += "\"type\":\"sensorUpdate\",";
	json += "\"psi\":"         + String(currentPressure, 1) + ",";
	json += "\"rawPsi\":"      + String(rawPressure, 1) + ",";
	json += "\"sampleRateSec\":" + String(sensorRateSec) + ",";
	json += "\"adcVoltage\":"  + String(ADCvoltage, 3) + ",";
	json += "\"zoneNumber\":\"" + zoneNumber + "\",";
	json += "\"zoneName\":\""   + String((const char *)readingsJson["Active Zone"]["zname"])   + "\",";
	json += "\"zoneAvgPsi\":"   + String(zoneAvgPsi, 1)                                        + ",";
	json += "\"status\":\""     + buildPressureStatus(currentPressure, zoneAvgPsi)             + "\",";
	json += "\"controller\":\""  + controller + "\",";
	json += "\"days\":\""       + String((const char *)readingsJson["Active Zone"]["days"])    + "\",";
	json += "\"start\":\""      + start   + "\",";
	json += "\"run\":\""        + run     + "\",";
	json += "\"remaining\":\""  + getZoneRemainingMinutes(start, run) + "\",";
	json += "\"mapKey\":\""     + buildMapKey(controller, zoneNumber) + "\",";
	json += "\"allOff\":"       + String(currentPressure >= ZONES_ALL_OFF_PSI ? "true" : "false") + ",";
	json += "\"simMode\":"      + String(simMode ? "true" : "false") + ",";
	json += "\"time\":\""       + currentTimeStamp + "\",";
	json += "\"date\":\""       + currentDayStamp  + "\",";
	json += "\"location\":\""   + currentLocation  + "\"";
	json += "}";
	return json;
}

// Config page data — response to {"cmd":"getConfig"}
String buildConfigDataJson() {
	String json = "{";
	json += "\"type\":\"configData\",";
	json += "\"location\":\""   + currentLocation       + "\",";
	json += "\"sampleRateSec\":" + String(sensorRateSec) + ",";
	json += "\"calibOffset\":"   + String(calibOffset, 2) + ",";
	json += "\"zones\":"         + buildCombinedZoneDocJson();
	json += "}";
	return json;
}

// File page data — response to {"cmd":"getFiles"}
String buildFileListJson() {
	String json = "{\"type\":\"fileList\",\"sdFiles\":[";
	File root = SD.open("/");
	File file = root.openNextFile();
	bool first = true;
	while (file) {
		if (!first) json += ",";
		json += "\"" + String(file.name()) + "\"";
		first = false;
		file = root.openNextFile();
	}
	root.close();
	json += "],\"spiffsFiles\":[";
	root = SPIFFS.open("/");
	file = root.openNextFile();
	first = true;
	while (file) {
		if (!first) json += ",";
		json += "\"" + String(file.name()) + "\"";
		first = false;
		file = root.openNextFile();
	}
	root.close();
	json += "]}";
	return json;
}

// Reassembly buffers for inbound WS text messages that span multiple
// WS_EVT_DATA calls (AwsFrameInfo's index/len/final contract) -- keyed by
// client id. Small commands (manualZone, getSchedule, etc.) always arrive in
// one call and are unaffected; saveSchedule's full controllers array is the
// one inbound payload large enough to actually span TCP segments, and was
// previously dropped silently because each fragment failed JSON.parse on
// its own.
static std::map<uint32_t, String> wsMessageBuffers;

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

void serviceSdCardLockWatchdog() {
	if (!sdCardLock) {
		return;
	}

	if (millis() - sdCardLockStartedMillis > 60000UL) {
		Serial.println("[SD] Releasing stale SD lock after timeout");
		unlockSdCard();
	}
}

String *prepareRequestBodyBuffer(AsyncWebServerRequest *request, size_t total, size_t index) {
	if (index == 0) {
		if (total > MAX_POST_BODY_BYTES) {
			request->send(413, "text/plain", "Request payload too large");
			return nullptr;
		}

		String *body = new String();
		if (!body) {
			request->send(500, "text/plain", "Allocation failed");
			return nullptr;
		}

		body->reserve(total > 0 ? total : 1);
		request->_tempObject = body;
		return body;
	}

	return static_cast<String *>(request->_tempObject);
}

void releaseRequestBodyBuffer(AsyncWebServerRequest *request) {
	String *body = static_cast<String *>(request->_tempObject);
	if (body) {
		delete body;
		request->_tempObject = nullptr;
	}
}

// Send an ack back to a specific WS client
void sendWsAck(AsyncWebSocketClient *client, const String &cmd, bool success, const String &msg) {
	String json = "{\"type\":\"ack\",\"cmd\":\"" + cmd + "\",\"success\":" +
	              String(success ? "true" : "false") + ",\"message\":\"" + msg + "\"}";
	client->text(json);
}

// WebSocket event dispatcher
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
	switch (type) {

		case WS_EVT_CONNECT:
			Serial.printf("[WS] Client #%u connected from %s (active=%u)\n",
			              client->id(), client->remoteIP().toString().c_str(), (unsigned)ws.count());
			if (firstReadingDone) client->text(buildSensorUpdateJson());
			break;

		case WS_EVT_DISCONNECT:
			Serial.printf("[WS] Client #%u disconnected (active=%u)\n", client->id(), (unsigned)ws.count());
			wsMessageBuffers.erase(client->id());
			break;

		case WS_EVT_ERROR:
			Serial.printf("[WS] Client #%u error %u\n", client->id(), *((uint16_t *)arg));
			break;

		case WS_EVT_DATA: {
			AwsFrameInfo *info = (AwsFrameInfo *)arg;
			if (info->opcode != WS_TEXT) break;

			// Accumulate fragments for this client until the final one lands;
			// info->len is the total message length across all fragments.
			String &buffer = wsMessageBuffers[client->id()];
			if (info->index == 0) buffer = "";
			for (size_t i = 0; i < len; i++) buffer += (char)data[i];
			if (!info->final || buffer.length() != info->len) break;

			String msg = buffer;
			wsMessageBuffers.erase(client->id());

			JSONVar cmd = JSON.parse(msg);
			if (JSON.typeof(cmd) == "undefined") break;
			String cmdName = String((const char *)cmd["cmd"]);

			if (cmdName == "getConfig") {
				client->text(buildConfigDataJson());

			} else if (cmdName == "getFiles") {
				client->text(buildFileListJson());

			// setLocation/setSensorRate/setCalib were retired -- site fields are
			// saved via "saveZones" below, per decision #4. (This whole WS
			// dispatcher is unused by the current UI, which saves over HTTP via
			// /submit-zone-form and /submit-site-form instead -- kept for any
			// future WS-based client.)
			} else if (cmdName == "saveZones") {
					JSONVar payload = cmd["zones"];
					bool siteOk = saveSite(buildSiteVarFromParts(payload["site"], payload["weather"]));
					bool controllersOk = saveControllers(payload["controllers"]);
					bool ok = siteOk && controllersOk;
					sendWsAck(client, "saveZones", ok, ok ? "Zones saved" : "Save failed");

			} else if (cmdName == "deleteFile") {
				String source   = String((const char *)cmd["source"]);
				String filename = String((const char *)cmd["filename"]);
				if (!filename.startsWith("/")) filename = "/" + filename;
				bool ok = false;
				if      (source == "spiffs") ok = SPIFFS.remove(filename);
				else if (source == "sd")     ok = SD.remove(filename);
				sendWsAck(client, "deleteFile", ok, ok ? "File deleted" : "Delete failed");

			} else if (cmdName == "reset") {
				sendWsAck(client, "reset", true, "Restarting");
				delay(500);
				ESP.restart();

			} else if (cmdName == "getSchedule") {
				// Schedule only -- controllers.json, never site.json/psi_offset.
				client->text("{\"type\":\"schedule\",\"controllers\":" + loadControllers() + "}");

			} else if (cmdName == "saveSchedule") {
				// Writes controllers.json only. saveControllers() physically
				// cannot reach site.json, so psi_offset is unreachable here.
				bool ok = saveControllers(cmd["controllers"]);
				sendWsAck(client, "saveSchedule", ok, ok ? "Schedule saved" : "Save failed");

			} else if (cmdName == "getSchedulesEnabled") {
				client->text("{\"type\":\"schedulesEnabled\",\"enabled\":" +
				             String(schedulesEnabled ? "true" : "false") + "}");

			} else if (cmdName == "setSchedulesEnabled") {
				bool enabled = (bool)cmd["enabled"];
				String message;
				bool ok = saveSchedulesEnabled(enabled, message);
				sendWsAck(client, "setSchedulesEnabled", ok, message);
				// Broadcast new state so every viewer reflects the pause/resume,
				// not just the requester (important for away-user safety toggle).
				ws.textAll("{\"type\":\"schedulesEnabled\",\"enabled\":" +
				           String(schedulesEnabled ? "true" : "false") + "}");

			} else if (cmdName == "manualZone") {
				String action     = String((const char *)cmd["action"]);
				String controller = String((const char *)cmd["controller"]);
				uint8_t relay      = (uint8_t)String((const char *)cmd["znumber"]).toInt();
				uint16_t runMinutes = (uint16_t)String((const char *)cmd["run"]).toInt();
				String message; bool ok = false;
				action.trim(); action.toLowerCase();
				if      (action == "start")   ok = startManualZoneRun(controller, relay, runMinutes, message);
				else if (action == "stop")    ok = stopManualZoneRun(controller, relay, message);
				else if (action == "stopall") { stopAllZones(); message = "All zones stopped"; ok = true; }
				else                           message = "Unknown manual zone action";
				sendWsAck(client, "manualZone", ok, message);
				// Manual runs are NOT in the sensorUpdate broadcast, so push the
				// authoritative run list to every viewer after any change.
				ws.textAll(buildManualZoneRunsJson());

			} else if (cmdName == "manualProgram") {
				String action     = String((const char *)cmd["action"]);
				String controller = String((const char *)cmd["controller"]);
				String program    = String((const char *)cmd["program"]);
				String message; bool ok = false;
				action.trim(); action.toLowerCase();
				if      (action == "start") ok = startManualProgramRun(controller, program, message);
				else if (action == "stop")  ok = stopManualProgramRun(controller, message);
				else if (action == "next")  ok = advanceManualProgramRun(controller, message);
				else                         message = "Unknown manual program action";
				sendWsAck(client, "manualProgram", ok, message);
				ws.textAll(buildManualZoneRunsJson());
			}
			break;
		}

		default:
			break;
	}
}

// ==================== End WebSocket helpers ====================

unsigned long lastWiFiCheckMillis = 0;
unsigned long wifiDisconnectedSince = 0;

void serviceWiFi()
{
    const unsigned long now = millis();

    if (now - lastWiFiCheckMillis < 5000)
        return;

    lastWiFiCheckMillis = now;

    if (WiFi.status() == WL_CONNECTED) {
        wifiDisconnectedSince = 0;
        return;
    }

    if (wifiDisconnectedSince == 0) {
        wifiDisconnectedSince = now;
        Serial.println("[WiFi] Connection lost");
    }

    Serial.println("[WiFi] Attempting reconnect");
    WiFi.reconnect();

    // Restart after a prolonged failure rather than remaining half-connected.
    if (now - wifiDisconnectedSince > 120000UL) {
        Serial.println("[WiFi] Reconnect timeout; restarting");
        ESP.restart();
    }
}

// -------------------- SETUP ----------------------
void setup() {
	Serial.begin(921600);
	delay(500);
	Serial.println("\n\n=== SETUP START ===");
	Serial.flush();

	// Keep every SPI device deselected while the shared bus is starting.
	pinMode(SD_CS, OUTPUT);
	digitalWrite(SD_CS, HIGH);
	pinMode(TFT_CS, OUTPUT); 
	digitalWrite(TFT_CS, HIGH);

	pinMode(LORA_RESET_PIN, OUTPUT);  // Shared TFT/RYLR998 reset
	digitalWrite(LORA_RESET_PIN, LOW);
	delay(1);
	digitalWrite(LORA_RESET_PIN, HIGH);

	analogReadResolution(12);
	analogSetPinAttenuation(SENSOR_PIN, ADC_11db);

	// --- Display ---
	// LovyanGFX must initialise SPI2 first via tft.init().
	// Do NOT call SPI.begin() before this — it locks the bus and causes
	// tft.init() to fail silently, leaving the display white.
	Serial.println("[1] Initializing TFT display...");
	Serial.flush();
	initTftDisplay();
	Serial.println("[1] TFT display init returned OK");
	Serial.flush();

	// --- LoRa ---
	// The RYLR998 reset line is shared with TFT_RST and was pulsed before
	// display init so the display is not reset after LovyanGFX starts.
	Serial.println("[1b] Initializing LoRa radio...");
	Serial.flush();
	initLoRa();
	Serial.flush();

	// --- WiFi ---
	Serial.println("[2] Initializing WiFi...");
	Serial.flush();
	initWiFi();
	if (WiFi.status() == WL_CONNECTED) {
		IPmessage = WiFi.localIP();
		Serial.print("[2] WiFi OK, IP: ");
		Serial.println(IPmessage);
		if (MDNS.begin("pressuresense")) {
			MDNS.addService("http", "tcp", 80);
			Serial.println("[2] mDNS started: pressuresense.local (http service advertised)");
		} else {
			Serial.println("[2] mDNS failed to start");
		}
	} else {
		Serial.println("[2] WiFi FAILED");
	}
	Serial.flush();
	delay(1000);

	// --- SPIFFS ---
	Serial.println("[3] Mounting SPIFFS...");
	Serial.flush();
	if (!SPIFFS.begin(true)) {
		Serial.println("[3] SPIFFS FAILED");
		Serial.flush();
		return;
	}
	Serial.println("[3] SPIFFS OK");
	Serial.flush();
	loadSchedulesEnabled();

	// --- SD Card ---
	// LovyanGFX owns SPI2 after tft.init(). Tell the Arduino SPI class to use
	// the same pins so SD.begin() joins the already-initialised bus.
	SPI.begin(19, 20, 18, SD_CS);
	Serial.printf("[4] Mounting SD card on CS pin %d...\n", SD_CS);
	Serial.flush();
	if (!SD.begin(SD_CS, SPI, 4000000)) {
		Serial.println("[4] SD card mount FAILED");
		Serial.flush();
		return;
	}
	uint8_t cardType = SD.cardType();
	if (cardType == CARD_NONE) {
		Serial.println("[4] SD card: no card detected");
		Serial.flush();
		return;
	}
	Serial.printf("[4] SD card OK, type: %s\n",
		cardType == CARD_MMC  ? "MMC"  :
		cardType == CARD_SD   ? "SD"   :
		cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
	Serial.flush();

	// One-time migration: calibration.json/weather_cache.json/weather_log.json
	// used to live on SD; they're bounded, infrequently-written files that now
	// belong on SPIFFS alongside site.json/controllers.json/weather_state.json
	// (see loadCalibrationData()'s comment). Copy any pre-existing SD-resident
	// copy over on first boot after this change, so real calibration/weather
	// history already on the device isn't lost. Leaves the SD originals in
	// place afterward as a rollback path -- SD space isn't a concern.
	const char *sdToSpiffsMigrationFiles[] = {"/calibration.json", "/weather_cache.json", "/weather_log.json"};
	for (const char *path : sdToSpiffsMigrationFiles) {
		if (!SPIFFS.exists(path) && SD.exists(path)) {
			String existing = loadZoneTable(SD, path);
			bool ok = writeRawJsonFile(existing, String(path).substring(1)); // strip leading "/"; defaults to SPIFFS
			Serial.printf("[MIGRATE] %s SD->SPIFFS: %s\n", path, ok ? "ok" : "FAILED");
		}
	}

	// --- NTP ---
	Serial.println("[5] Starting NTP client...");
	Serial.flush();
	timeClient.begin();
	timeClient.update();
	Serial.println("[5] NTP OK");
	Serial.flush();

	// --- Daily log file ---
	Serial.println("[6] Generating daily filename...");
	Serial.flush();
	currentDailyFilename = generateDailyFilename();
	Serial.println("[6] Daily filename: " + currentDailyFilename);
	Serial.flush();
	ensureDailyLogFileExists();
	Serial.flush();

	// site.name/sensor_interval_sec/psi_offset/weather.* now live in
	// site.json (decision #4 of the original config-schema refactor, split
	// further by the JSON-split refactor) -- read once here; every later
	// change is saved back through saveSite()/saveControllers().
	JSONVar siteDoc = JSON.parse(loadSite());
	if (JSON.typeof(siteDoc) == "object") {
		currentLocation = jsonStringOr(siteDoc, "name", "");
		int savedRateSec = (int)jsonNumberOr(siteDoc, "sensor_interval_sec", 0);
		if (savedRateSec > 0) sensorRateSec = savedRateSec;
		calibOffset = jsonNumberOr(siteDoc, "psi_offset", 0.0);
	}
	timerDelay = (unsigned long)sensorRateSec * 1000;
	Serial.println("[6] Location: " + currentLocation);

	////// Server Endpoints //////
	// Web Server Root URL
	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (SPIFFS.exists("/index.html")) {
			request->send(SPIFFS, "/index.html", "text/html");
		} else {
			request->send(404, "text/plain", "File not found");
		}
	});

	server.on("/sprinklers_qcad13.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!SPIFFS.exists("/sprinklers_qcad13.svg.gz")) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/sprinklers_qcad13.svg.gz", "image/svg+xml");
		response->addHeader("Content-Encoding", "gzip");
		response->addHeader("Cache-Control", "max-age=3600");
		request->send(response);
	});

	server.on("/sprinklers_map.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!SPIFFS.exists("/sprinklers_map.svg.gz")) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/sprinklers_map.svg.gz", "image/svg+xml");
		response->addHeader("Content-Encoding", "gzip");
		response->addHeader("Cache-Control", "max-age=3600");
		request->send(response);
	});

	// Self-hosted (pinned v13.0.0) instead of loaded from code.highcharts.com --
	// this device is often on unreliable/offline networks in the field, and a
	// failed/blocked CDN load throws inside index.js's top-level
	// Highcharts.setOptions() call, which halts that whole script before it
	// even registers its DOMContentLoaded handler (blank chart + blank data).
	server.on("/highcharts.js", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!SPIFFS.exists("/highcharts.js.gz")) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/highcharts.js.gz", "text/javascript");
		response->addHeader("Content-Encoding", "gzip");
		response->addHeader("Cache-Control", "max-age=3600");
		request->send(response);
	});

	server.on("/highcharts-accessibility.js", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!SPIFFS.exists("/highcharts-accessibility.js.gz")) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/highcharts-accessibility.js.gz", "text/javascript");
		response->addHeader("Content-Encoding", "gzip");
		response->addHeader("Cache-Control", "max-age=3600");
		request->send(response);
	});

	// no-cache (not no-store) -- still lets the browser send a conditional
	// GET (If-None-Match/If-Modified-Since) and get a cheap 304, but forces
	// it to actually ask the device instead of assuming a stale copy of
	// config.js/config.html is still good after a filesystem-image update.
	// Without this, browsers can keep serving a page from before a UI
	// change (e.g. the program edit-icon popover) indefinitely.
	server.serveStatic("/", SPIFFS, "/").setCacheControl("no-cache");

		server.on("/get-daily-filename", HTTP_GET, [](AsyncWebServerRequest *request) {
			String fileName = currentDailyFilename;
			AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", fileName);
			response->addHeader("Cache-Control", "no-store");
			request->send(response);
		});

	server.on("/sd-usage", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!sdCardLock) {
			cachedSdTotalBytes = SD.totalBytes();
			cachedSdUsedBytes = SD.usedBytes();
		}
		uint64_t total = cachedSdTotalBytes;
		uint64_t used = cachedSdUsedBytes;
		uint64_t freeBytes = total > used ? total - used : 0;
		float percent = total > 0 ? ((float)used / (float)total) * 100.0f : 0.0f;
		unsigned long uptimeSec = millis() / 1000;
		const char *wifiStatus = WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE";
		size_t wsClients = ws.count();
		// -1 sentinel = no periodic WS broadcast has fired yet since boot,
		// so "seconds since last broadcast" isn't meaningful yet.
		long wsAgoSec = wsLastBroadcastMillis == 0 ? -1 : (long)((millis() - wsLastBroadcastMillis) / 1000);
		// Browser tabs' /events (SSE) connections -- surfaced so a slow leak of
		// stale/zombied connections (e.g. one left behind per sleep/wake cycle
		// of a browser left open for hours) is visible from the UI instead of
		// only showing up later as "new page loads hang" once CONFIG_LWIP_MAX_
		// ACTIVE_TCP is exhausted.
		size_t sseClients = events.count();

		char json[288];
		snprintf(json, sizeof(json),
		         "{\"total\":%llu,\"used\":%llu,\"free\":%llu,\"percent\":%.1f,\"wifi\":\"%s\",\"uptimeSec\":%lu,\"wsClients\":%u,\"wsAgoSec\":%ld,\"sseClients\":%u}",
		         (unsigned long long)total,
		         (unsigned long long)used,
		         (unsigned long long)freeBytes,
		         percent,
		         wifiStatus,
		         uptimeSec,
		         (unsigned)wsClients,
		         wsAgoSec,
		         (unsigned)sseClients);
		request->send(200, "application/json", json);
	});

	server.on("/get-data-file", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (request->hasParam("filename")) {
			String fileName = request->getParam("filename")->value();
			fileName.trim();	// Trim any whitespace

			if (fileName.indexOf("..") != -1) {
				logMsg("Invalid filename.");
				request->send(400, "text/plain", "Invalid filename.");
				return;
			}

			if (!fileName.startsWith("/")) {
				fileName = "/" + fileName;
			}

			// Check if the SD card is locked
			if (sdCardLock) {
				request->send(500, "text/plain", "SD card is busy");
				logMsg("SD card is busy");
				return;
			}

			// Lock the SD card. Held until the chunked response actually
			// finishes streaming (see fill lambda below), not just until it's
			// queued -- request->send() below only registers the response;
			// the real multi-chunk SD reads happen later, one per TCP ack,
			// driven by async_tcp. Releasing the lock any earlier than that
			// let logData() (on loop()'s task) open the same SD card for a
			// concurrent write while a stream was still in flight, with no
			// serialization between the two SPI transactions.
			sdCardLock = true;

			File file = SD.open(fileName.c_str(), FILE_READ);
			if (!file) {
				String errorMessage = "Failed to open file or file does not exist. Filename: " + fileName;
				logMsg(errorMessage.c_str());
				request->send(500, "text/plain", errorMessage);
				sdCardLock = false;
			} else {
				char buffer[BUFFER_SIZE];	 // Create a buffer for reading the file

				// Create a custom response to stream the file content
					AsyncWebServerResponse *response = request->beginChunkedResponse("text/plain", [file, buffer](uint8_t *data, size_t len, size_t index) mutable -> size_t {
						if (file.available()) {
						memset(buffer, 0, BUFFER_SIZE);													 // Clear the buffer to avoid leftover data
						size_t bytesRead = file.readBytes(buffer, BUFFER_SIZE);	 // Read a chunk of the file
						memcpy(data, buffer, bytesRead);												 // Copy the exact chunk into the response data buffer
						return bytesRead;																				 // Return the exact number of bytes read
					} else {
						file.close();	 // Close the file when done
							sdCardLock = false;	// Unlock the SD card -- stream is actually done now
							return 0;			 // Indicate that we're done sending data
						}
					});
					response->addHeader("Cache-Control", "no-store");
					request->send(response);
				}
		} else {
			logMsg("Filename not specified.");
			request->send(400, "text/plain", "Filename not specified.");
		}
	});

	server.on("/list-sd-card-files", HTTP_GET, [](AsyncWebServerRequest *request) {
		String fileList = "[";
		File root = SD.open("/");
		File file = root.openNextFile();
		bool first = true;
		while (file) {
			String fileName = String(file.name());
			if (!isHiddenSystemPath(fileName)) {
				if (!first) {
					fileList += ",";
				}
				fileList += "\"" + fileName + "\"";
				first = false;
			}
			file = root.openNextFile();
		}
		root.close();
		fileList += "]";
		request->send(200, "application/json", fileList);
	});

	server.on("/list-spiffs-files", HTTP_GET, [](AsyncWebServerRequest *request) {
		String fileList = "[";
		File root = SPIFFS.open("/");
		File file = root.openNextFile();
		bool first = true;
		while (file) {
			String fileName = String(file.name());
			if (!isHiddenSystemPath(fileName)) {
				if (!first) {
					fileList += ",";
				}
				fileList += "\"" + fileName + "\"";
				first = false;
			}
			file = root.openNextFile();
		}
		root.close();
		fileList += "]";
		request->send(200, "application/json", fileList);
	});

	server.on("/get-spiffs-json-file", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!request->hasParam("filename")) {
			request->send(400, "text/plain", "Filename not specified.");
			return;
		}

		String filename = request->getParam("filename")->value();
		filename.trim();
		if (filename.startsWith("/")) filename.remove(0, 1);
		if (!isSafeJsonFilename(filename) || isHiddenSystemPath(filename)) {
			request->send(400, "text/plain", "Invalid SPIFFS JSON filename.");
			return;
		}

		String fullPath = "/" + filename;
		File file = SPIFFS.open(fullPath.c_str(), FILE_READ);
		if (!file) {
			request->send(404, "text/plain", "SPIFFS JSON file not found.");
			return;
		}

		String body = file.readString();
		file.close();
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", body);
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	server.on("/list-json-files", HTTP_GET, [](AsyncWebServerRequest *request) {
    String fileList = "[";
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    bool first = true;

    while (file) {
        // Check if the file name ends with ".json"
        String fileName = String(file.name());
        if (fileName.endsWith(".json")) {
            if (!first) {
                fileList += ",";
            }
            fileList += "\"" + fileName + "\"";
            first = false;
        }
        file = root.openNextFile();
    }
    root.close();

    fileList += "]";
    request->send(200, "application/json", fileList);
});

		// Weather settings (site.latitude/longitude/timezone/mm_per_min_default,
		// the whole `weather.*` block, zones[].mm_per_min) are intentionally
		// included in this whole-document save, exactly like seasonal_adjust_pct
		// already is -- they're user-edited config. weather_state.json is
		// intentionally NEVER part of this payload (it's firmware-computed
		// runtime state, rewritten by serviceWeatherTask()/consumeSkipNextRun);
		// this is the one place a future change could quietly reintroduce the
		// stale-Save-All-clobbers-fresh-weather-data risk decision #1 avoided.
		server.on("/submit-zone-form", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
			String *body = prepareRequestBodyBuffer(request, total, index);
			if (!body) return;

			body->concat(reinterpret_cast<const char *>(data), len);

			if (index + len != total) return;

			JSONVar jsonData = JSON.parse(*body);
			releaseRequestBodyBuffer(request);

			if (JSON.typeof(jsonData) == "undefined") {
				request->send(400, "text/plain", "Bad Request - JSON Parsing Failed");
				return;
			}

			// Schedule-only endpoint -- site.json/weather are saved independently
			// via /submit-site-form. No sentinel filename here: every call is a
			// controllers-only save, to controllers.json by default or a named
			// preset filename (e.g. a seasonal controllers_summer.json).
			String filename = "controllers.json";
			if (request->hasParam("filename")) {
				filename = request->getParam("filename")->value();
			}
			if (!isSafeJsonFilename(filename)) {
				request->send(400, "text/plain", "Invalid filename. Use a simple .json filename.");
				return;
			}
			if (filename == "site.json") {
				request->send(400, "text/plain", "'site.json' is a reserved filename and cannot be used as a preset name.");
				return;
			}

			bool ok = saveControllers(jsonData["controllers"], filename);

			if (ok) {
				request->send(200, "text/plain", "Data stored successfully");
			} else {
				request->send(500, "text/plain", "Failed to write data to file");
			}
		});

	// /get-sensor-rate, /get-location, /get-calib-offset were retired -- site
	// fields (name/sensor_interval_sec/psi_offset) are read from the `site`
	// block in /load-zone-table's response instead, per decision #4.

			// Endpoint to serve the working site+controllers document, reassembled
			// from site.json + controllers.json into the legacy combined shape
			// so data/config.js needs no rendering changes.
			server.on("/load-zone-table", HTTP_GET, [](AsyncWebServerRequest *request) {
					String zoneData = buildCombinedZoneDocJson();
				AsyncWebServerResponse *response = request->beginResponse(200, "application/json", zoneData);
				response->addHeader("Cache-Control", "no-store");
				request->send(response);
			});

			// Backward-compatible alias for older pages; the working table now lives in SPIFFS.
			server.on("/load-sd-zone-table", HTTP_GET, [](AsyncWebServerRequest *request) {
					String zoneData = buildCombinedZoneDocJson();
				AsyncWebServerResponse *response = request->beginResponse(200, "application/json", zoneData);
				response->addHeader("Cache-Control", "no-store");
				request->send(response);
			});

	// /get-seasonal-adjustments, /set-seasonal-adjustment, /get-zone-delays,
	// /set-zone-delay were retired -- seasonal_adjust_pct/zone_delay_sec are
	// now plain fields on each program object in controllers.json, edited via
	// the program popover and saved through /submit-zone-form, per decision #3.

	server.on("/get-schedules-enabled", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", schedulesEnabled ? "true" : "false");
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	// Weather auto-adjust: read-only firmware-owned state, never part of
	// /submit-zone-form's payload (decision #1). Route paths match what
	// config.js/index.js fetch literally, per the implementation plan.
	server.on("/weather-state", HTTP_GET, [](AsyncWebServerRequest *request) {
		// Splices in "fetch_pending" (ephemeral request-in-flight status,
		// never persisted to weather_state.json) so config.js's Fetch Now
		// polling loop knows when the deferred fetch (handleWeatherFetchNow()
		// / serviceWeatherTask()) has actually completed, even on failure --
		// last_fetch_date/last_fetch_utc deliberately don't change when a
		// fetch fails (markWeatherFetchError() preserves them), so they
		// can't be used as a completion signal on their own.
		String stateJson = loadWeatherState();
		if (stateJson.endsWith("}")) stateJson.remove(stateJson.length() - 1);
		stateJson += String(stateJson.endsWith("{") ? "" : ",") +
		             "\"fetch_pending\":" + (weatherFetchNowRequested ? "true" : "false") + "}";
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", stateJson);
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	server.on("/weather_cache.json", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", loadZoneTable(SPIFFS, "/weather_cache.json"));
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	server.on("/weather_log.json", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", loadZoneTable(SPIFFS, "/weather_log.json"));
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	// Zone irrigation calibration (calibration.html) -- per-zone mm_per_min
	// derived from head specs + SVG-measured area. SPIFFS, same as
	// weather_cache.json/weather_log.json; not part of site.json/controllers.json.
	server.on("/calibration.json", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", loadCalibrationData());
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	server.on("/submit-calibration-form", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		JSONVar jsonData = JSON.parse(*body);
		releaseRequestBodyBuffer(request);
		if (JSON.typeof(jsonData) == "undefined") {
			request->send(400, "text/plain", "Bad Request - JSON Parsing Failed");
			return;
		}
		if (writeRawJsonFile(JSON.stringify(jsonData), "calibration.json")) {
			request->send(200, "text/plain", "Calibration data stored successfully");
		} else {
			request->send(500, "text/plain", "Failed to write calibration data");
		}
	});

	// PSI calibration (calibration.html, moved off CONFIG -- it's a
	// calibration, belongs on the CALIB page). Patches only site.json's
	// psi_offset field: this page never loads the schedule, so it must not
	// go through /submit-zone-form's whole-document save, which would
	// clobber controllers.json with nothing. Also updates the live
	// calibOffset global immediately so calibration takes effect without a
	// reboot -- /submit-zone-form's site save never did this, but there's
	// no reason a freshly-written dedicated endpoint should carry over that
	// same gap.
	server.on("/submit-calib-offset", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		JSONVar jsonData = JSON.parse(*body);
		releaseRequestBodyBuffer(request);
		if (JSON.typeof(jsonData) != "object" || JSON.typeof(jsonData["psi_offset"]) == "undefined") {
			request->send(400, "text/plain", "Bad Request - expected {\"psi_offset\": <number>}");
			return;
		}

		JSONVar siteDoc = JSON.parse(loadSite());
		if (JSON.typeof(siteDoc) != "object") siteDoc = JSONVar();
		double newOffset = (double)jsonData["psi_offset"];
		siteDoc["psi_offset"] = newOffset;

		if (saveSite(siteDoc)) {
			calibOffset = newOffset;
			request->send(200, "text/plain", "Calibration saved");
		} else {
			request->send(500, "text/plain", "Failed to save calibration");
		}
	});

	// Site Identity / Sensor Rate / Weather & Auto-Adjust (CONFIG page) --
	// saves only site.json, independent of /submit-zone-form's schedule
	// save. Replaces the old shared-sentinel mechanism where these three
	// forms funneled through the same endpoint as the schedule save; that
	// coupling was error-prone (a client-side default change was enough to
	// silently break all three) and is retired entirely rather than patched.
	//
	// Note: this round-trips whatever psi_offset the client's in-memory copy
	// currently holds, so it can overwrite a calibration made on the CALIB
	// page in a different browser tab after this page's data was loaded --
	// unchanged behavior carried over from the old full-document save path,
	// not a new regression, not fixed here.
	server.on("/submit-site-form", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		JSONVar jsonData = JSON.parse(*body);
		releaseRequestBodyBuffer(request);
		if (JSON.typeof(jsonData) != "object") {
			request->send(400, "text/plain", "Bad Request - expected {\"site\": {...}, \"weather\": {...}}");
			return;
		}

		JSONVar siteVar = buildSiteVarFromParts(jsonData["site"], jsonData["weather"]);
		if (!saveSite(siteVar)) {
			request->send(500, "text/plain", "Failed to save site settings");
			return;
		}

		// Update live globals immediately -- mirrors setup()'s boot-time site
		// read, so Location/Sensor Rate/Weather changes take effect without a
		// reboot. Sensor Rate never did this before (even under the old full
		// save), so this closes a pre-existing gap, not just this refactor's.
		currentLocation = jsonStringOr(siteVar, "name", currentLocation);
		int newRateSec = (int)jsonNumberOr(siteVar, "sensor_interval_sec", 0);
		if (newRateSec > 0) {
			sensorRateSec = newRateSec;
			timerDelay = (unsigned long)sensorRateSec * 1000;
		}
		calibOffset = jsonNumberOr(siteVar, "psi_offset", calibOffset);

		request->send(200, "text/plain", "Site settings saved");
	});

	server.on("/set-schedules-enabled", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		bool enabled = !(*body == "false" || *body == "0");
		releaseRequestBodyBuffer(request);
		String message;
		bool ok = saveSchedulesEnabled(enabled, message);
		String json = "{\"success\":" + String(ok ? "true" : "false") + ",\"message\":\"" + message +
		              "\",\"enabled\":" + String(schedulesEnabled ? "true" : "false") + "}";
		request->send(ok ? 200 : 400, "application/json", json);
	});

	// External zone data placed in SPIFFS file based on client selection
	server.on("/load-spiffs-zone-table", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Check if the "filename" parameter is provided
    if (request->hasParam("filename")) {
        // Get the filename from the request
        String filename = request->getParam("filename")->value();
        
	        if (!isSafeJsonFilename(filename)) {
	            request->send(400, "text/plain", "Invalid filename. Use a simple .json filename.");
	            return;
	        }

        // Load the selected file from SPIFFS
       // Concatenate the root directory with the filename
        String fullPath = String("/") + filename;

        // Load the selected file from SPIFFS
        String zoneData = loadZoneTable(SPIFFS, fullPath.c_str()); // Use c_str() to pass const char*

        // If the file was loaded successfully, send the content
        if (zoneData.length() > 2) {
            request->send(200, "application/json", zoneData);
        } else {
            // If the file could not be loaded, return an error
            request->send(404, "text/plain", "File not found or empty.");
        }
    } else {
        // If the filename parameter is missing, return a bad request error
        request->send(400, "text/plain", "Filename parameter missing.");
    }
});

	server.on("/delete-file", HTTP_GET, deleteFileHandler);

	// Simulation endpoints — inject a fixed PSI value without reading the sensor
	server.on("/set-sim-pressure", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (request->hasParam("psi")) {
			float psi = request->getParam("psi")->value().toFloat();
			psi = constrain(psi, 0.0f, 100.0f);
			simPsi  = psi;
			simMode = true;
			String msg = "SIM ON: " + String(psi, 1) + " PSI";
			logMsg(msg.c_str());
			request->send(200, "text/plain", msg);
		} else {
			request->send(400, "text/plain", "Missing ?psi= parameter");
		}
	});

	server.on("/clear-sim", HTTP_GET, [](AsyncWebServerRequest *request) {
		simMode = false;
		simPsi  = 0.0f;
		logMsg("SIM OFF: live sensor restored");
		request->send(200, "text/plain", "SIM OFF");
	});

	// Config page "Fetch Now" button.
	server.on("/weather-fetch-now", HTTP_GET, handleWeatherFetchNow);

	// Weather auto-adjust Phase 1 hardware-verification spikes (see comment
	// above handleDebugWeatherFetch). Optional ?lat=&lon= query params.
	//server.on("/debug-weather-fetch", HTTP_GET, handleDebugWeatherFetch);
	//server.on("/debug-weather-state-roundtrip", HTTP_GET, handleDebugWeatherStateRoundTrip);
	//server.on("/debug-weather-accumulators", HTTP_GET, handleDebugWeatherAccumulators);
	//server.on("/debug-weather-seed-accumulator", HTTP_GET, handleDebugWeatherSeedAccumulator);

	server.on("/manual-zones", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buildManualZoneRunsJson());
		response->addHeader("Cache-Control", "no-store");
		request->send(response);
	});

	server.on("/manual-zone", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		JSONVar cmd = JSON.parse(*body);
		releaseRequestBodyBuffer(request);
		if (JSON.typeof(cmd) == "undefined") {
			request->send(400, "text/plain", "Invalid JSON");
			return;
		}

		String action = String((const char *)cmd["action"]);
		String controller = String((const char *)cmd["controller"]);
		uint8_t relay = (uint8_t)String((const char *)cmd["znumber"]).toInt();
		uint16_t runMinutes = (uint16_t)String((const char *)cmd["run"]).toInt();
		String message = "";
		bool ok = false;

		action.trim();
		action.toLowerCase();
		if (action == "start") {
			ok = startManualZoneRun(controller, relay, runMinutes, message);
		} else if (action == "stop") {
			ok = stopManualZoneRun(controller, relay, message);
		} else if (action == "stopall") {
			stopAllZones();
			message = "All zones stopped";
			ok = true;
		} else {
			message = "Unknown manual zone action";
		}

		String json = "{\"type\":\"manualZoneAck\",\"success\":" + String(ok ? "true" : "false") +
		              ",\"message\":\"" + message + "\",\"status\":" + buildManualZoneRunsJson() + "}";
		request->send(ok ? 200 : 400, "application/json", json);
	});

	server.on("/manual-program", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
	          [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		String *body = prepareRequestBodyBuffer(request, total, index);
		if (!body) return;

		body->concat(reinterpret_cast<const char *>(data), len);
		if (index + len != total) return;

		JSONVar cmd = JSON.parse(*body);
		releaseRequestBodyBuffer(request);
		if (JSON.typeof(cmd) == "undefined") {
			request->send(400, "text/plain", "Invalid JSON");
			return;
		}

		String action = String((const char *)cmd["action"]);
		String controller = String((const char *)cmd["controller"]);
		String program = String((const char *)cmd["program"]);
		String message = "";
		bool ok = false;

		action.trim();
		action.toLowerCase();
		if (action == "start") {
			ok = startManualProgramRun(controller, program, message);
		} else if (action == "stop") {
			ok = stopManualProgramRun(controller, message);
		} else if (action == "next") {
			ok = advanceManualProgramRun(controller, message);
		} else {
			message = "Unknown manual program action";
		}

		String json = "{\"type\":\"manualProgramAck\",\"success\":" + String(ok ? "true" : "false") +
		              ",\"message\":\"" + message + "\",\"status\":" + buildManualZoneRunsJson() + "}";
		request->send(ok ? 200 : 400, "application/json", json);
	});

	// Endpoint to trigger reset
	server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(200, "text/plain", "Resetting ESP32...");
		delay(1000);		// Allow time for the response to be sent
		ESP.restart();	// Reset the ESP32
	});

	server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
		String body = "{";
		body += "\"status\":\"ok\",";
		body += "\"uptime_ms\":" + String(millis()) + ",";
		body += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
		body += "\"min_free_heap\":" + String(ESP.getMinFreeHeap()) + ",";
		body += "\"max_alloc_heap\":" + String(ESP.getMaxAllocHeap()) + ",";
		body += "\"largest_block\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + ",";
		body += "\"ws_clients\":" + String(ws.count()) + ",";
		body += "\"sse_clients\":" + String(events.count()) + ",";
		body += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
		body += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
		body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
		body += "\"sd_locked\":" + String(sdCardLock ? "true" : "false") + ",";
		body += "\"ws_last_broadcast_age_ms\":" + String(millis() - wsLastBroadcastMillis);
		body += "}";

		request->send(200, "application/json", body);
	});

	events.onConnect([](AsyncEventSourceClient *client) {
		// events.count() already includes this client (added before onConnect
		// fires) -- reject it immediately if that pushes us over the cap. The
		// browser's EventSource auto-retries, so it reconnects once a stale
		// client is reaped by the next heartbeat instead of the device
		// silently running its TCP socket budget down to nothing.
		if (events.count() > MAX_SSE_CLIENTS) {
			Serial.printf("[SSE] Rejecting connection: active=%u cap=%u\n",
				              (unsigned)events.count(), MAX_SSE_CLIENTS);
			client->close();
			return;
		}

		Serial.printf(
			"[SSE] Connected: active=%u lastId=%lu\n",
			(unsigned)events.count(),
			(unsigned long)client->lastId()
		);

		client->send("connected", "status", millis(), 1000);
	});

	server.onNotFound(notFound);
	server.addHandler(&events);
	ws.onEvent(onWsEvent);
	server.addHandler(&ws);

	// /sensor-rate-input, /calib-input, /loc-input were retired -- site fields
	// save through /submit-zone-form now (decision #4).

	// Start ElegantOTA (Over The Air) updating
	// To access, use <IPaddress/update> then send the firmware.bin compiled image
	// file To upload data directory use spiffs.bin
	ElegantOTA.begin(&server);

	// Start server
	server.begin();
	Serial.println("[7] Web server started");
	Serial.println("=== SETUP COMPLETE ===");
	Serial.flush();
}

// ----------------- LOOP ----------------------
void loop() {
	serviceWiFi();
	ElegantOTA.loop();
	ws.cleanupClients();
	pollLoRaResponses();
	serviceManualZoneRuns();
	serviceZoneDelayTimers();
	serviceSdCardLockWatchdog();
	serviceWeatherTask();

	// A dead /events (SSE) client -- e.g. a browser tab whose TCP connection
	// died silently over a sleep/wake cycle -- only gets pruned by AsyncTCP's
	// ~5s ack-timeout on the next time the server actually tries to write to
	// it. Left to the timerDelay-gated sensor broadcast below, that's up to
	// sensorRateSec (often 30s+) between opportunities, letting stale
	// connections pile up during a long idle session against the device's
	// limited concurrent-TCP-connection budget. This fixed, short interval
	// bounds how long a dead connection can sit before being reaped,
	// independent of sensorRateSec.
	if (millis() - lastSseHeartbeatMillis >= 8000) {
		lastSseHeartbeatMillis = millis();
		events.send("hb", "heartbeat", millis());
	}

	if (millis() - lastHealthLogMillis >= HEALTH_LOG_INTERVAL_MS) {
		lastHealthLogMillis = millis();
		Serial.printf(
			"[HEALTH] free=%u min=%u maxAlloc=%u largest=%u WS=%u SSE=%u RSSI=%d sdLock=%d\n",
			ESP.getFreeHeap(),
			ESP.getMinFreeHeap(),
			ESP.getMaxAllocHeap(),
			heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
			ws.count(),
			events.count(),
			WiFi.RSSI(),
			sdCardLock ? 1 : 0
		);
	}

	if ((millis() - lastTime) > timerDelay) {
		lastTime = millis();

		// Check if it's time to update the daily filename
		updateDailyFilename();

		// Send Events to the client with the Sensor Readings Every 30 seconds
		// events.send("ping", NULL, millis());
		events.send(getSensorReading().c_str(), "new-readings", millis());

		// Broadcast the same reading to all WebSocket clients (indoor display)
		firstReadingDone = true;
		ws.textAll(buildSensorUpdateJson());
		wsLastBroadcastMillis = millis();
		// Manual runs live in a separate mechanism from checkActiveZone() and
		// never appear in sensorUpdate. Broadcast their state on the same
		// cadence so late-joining viewers and natural run-expiry stay in sync.
		ws.textAll(buildManualZoneRunsJson());

		// Build zone info for the display
		ZoneInfo zone;
		zone.number     = String((const char *)readingsJson["Active Zone"]["znumber"]);
		zone.name       = String((const char *)readingsJson["Active Zone"]["zname"]);
		zone.controller = String((const char *)readingsJson["Active Zone"]["controller"]);
		zone.days       = String((const char *)readingsJson["Active Zone"]["days"]);
		zone.start      = String((const char *)readingsJson["Active Zone"]["start"]);
		zone.run        = String((const char *)readingsJson["Active Zone"]["run"]);
		zone.avgPsi     = String((const char *)readingsJson["Active Zone"]["avgpsi"]).toFloat();
		zone.allOff     = (currentPressure >= ZONES_ALL_OFF_PSI);

		// zone.number directly, not gated on zone.allOff -- see the comment
		// in serviceRemoteZoneControl() for why the pressure heuristic can't
		// be trusted to reflect the schedule promptly.
		uint8_t currentZoneNumber = (uint8_t)zone.number.toInt();
		serviceRemoteZoneControl(zone);

		// Weather auto-adjust: accumulate today's actual watering minutes per
		// zone (not program -- see WaterAppliedAccumulator). Schedule-gated
		// only (controller present and not "off") -- previously also gated
		// on the pressure-confirmed allOff heuristic, but that reads "not
		// flowing" for a while after a zone starts whenever pressure hasn't
		// yet fallen from a recent refill, which would undercount real
		// watering during that window.
		if (zone.controller.length() > 0 && zone.controller != "off") {
			int waterSlot = findOrCreateWaterAccumulatorSlot(zone.controller, (int)currentZoneNumber);
			if (waterSlot >= 0) {
				waterAppliedAccumulators[waterSlot].minutesAppliedToday += sensorRateSec / 60.0f;
			}
		}

		// Weather auto-adjust: same per-tick crediting as the scheduled path
		// above, but for Manual Program runs -- manualZoneRuns[] is populated
		// by startManualProgramRun()/serviceManualZoneRuns() and never known to
		// checkActiveZone(), so it needs its own loop here rather than being
		// folded into the block above. Gated on manualZoneRuns[].active/
		// isProgram/delayPending directly (relay-commanded state), not the
		// site-wide pressure sensor -- same reasoning as the scheduled path.
		// Ad hoc single-zone manual runs (isProgram == false) are
		// intentionally excluded -- out of scope for this feature.
		for (uint8_t i = 0; i < MAX_MANUAL_ZONE_RUNS; i++) {
			if (!manualZoneRuns[i].active || !manualZoneRuns[i].isProgram) continue;
			if (manualZoneRuns[i].delayPending) continue; // relay is physically off during the inter-zone gap

			int waterSlot = findOrCreateWaterAccumulatorSlot(manualZoneRuns[i].controller, (int)manualZoneRuns[i].relay);
			if (waterSlot >= 0) {
				waterAppliedAccumulators[waterSlot].minutesAppliedToday += sensorRateSec / 60.0f;
			}
		}

		// Rolling display history — shift left when full, append newest sample.
		if (historyCount < CHART_BUFFER_SIZE) {
			pressureHistory[historyCount] = currentPressure;
			zoneHistory[historyCount] = currentZoneNumber;
			historyCount++;
		} else {
			memmove(pressureHistory, pressureHistory + 1,
			        (CHART_BUFFER_SIZE - 1) * sizeof(float));
			memmove(zoneHistory, zoneHistory + 1,
			        (CHART_BUFFER_SIZE - 1) * sizeof(uint8_t));
			pressureHistory[CHART_BUFFER_SIZE - 1] = currentPressure;
			zoneHistory[CHART_BUFFER_SIZE - 1] = currentZoneNumber;
		}

		// Alert when live PSI is >4 below the zone's configured average.
		// zone.avgPsi > 0 already implies an active zone (checkActiveZone()
		// only populates it for the schedule's current zone) -- dropped the
		// !zone.allOff gate, which could suppress a real deviation right as
		// a zone starts, before pressure has fallen from a recent refill.
		String alertZoneName = "";
		if (zone.avgPsi > 0.0f && currentPressure < zone.avgPsi - PRESSURE_ALERT_DEVIATION) {
			alertZoneName = zone.name;
		}

		getTimeStamp(currentDayStamp, currentTimeStamp);
		updateTftDisplay(currentPressure, IPmessage, zone,
		                 currentTimeStamp, pressureHistory, zoneHistory, historyCount,
		                 (int)(timerDelay / 1000), alertZoneName);
		logData();
		Serial.println(ADCvoltage);
	}
	delay(100);
}
