// stikka-server bridge: replaces mqtt_bridge.cpp when built with
// -D STIKKA_SERVER (see the *-server env in platformio.ini). Implements the
// same mqtt_bridge.h entry points, speaking the node protocol v1 documented
// in stikka-server's README / src/protocol.ts:
//
//   1. Enroll:   POST {server}/api/node/enroll {code, chip_id, firmware}
//                -> {node_id, node_secret, name, ws_url}  (stored in NVS)
//   2. Connect:  WSS ws_url, Authorization: Bearer <node_id>.<node_secret>
//   3. Talk:     hello / status / wifi.scan / job.update  ->  server
//                welcome / job.offer / config.set / status.request  <- server
//   4. Jobs:     job.offer carries a URL; the raw bytes are downloaded over
//                HTTPS (sha256-checked) and handed to the selected target.
//
// Everything runs on the Arduino main thread (same non-thread-safe
// assumption as the rest of the firmware). Job downloads block loop(), so
// the WebSocket is pumped between chunks to keep answering server pings.
#ifdef STIKKA_SERVER

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
#include "status_led.h"
#include "targets/target.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

static const size_t kMaxPayloadBytes = 8u * 1024u * 1024u;
static const unsigned long kEnrollRetryMs = 15000;
static const unsigned long kScanIntervalMs = 24UL * 60UL * 60UL * 1000UL;

static WebSocketsClient ws;
static bool wsConfigured = false;
static String wsConfiguredFor;
static bool wsUp = false;

static unsigned long lastEnrollAttemptMs = 0;
static String lastEnrollError;
static unsigned long lastStatusMs = 0;
static bool statusDirty = false;

static String nodeState = "idle";  // idle | printing | error
static String nodeError;           // empty = none

static bool scanPending = true;    // send a wifi.scan on first connect, then daily
static bool scanRunning = false;
static unsigned long lastScanMs = 0;

static bool timeConfigured = false;

struct JobOffer {
  String id;
  String type;
  size_t size = 0;
  String sha256;
  String url;
  int copies = 1;
};
static const int kQueueMax = 4;
static JobOffer jobQueue[kQueueMax];
static int jobQueueLen = 0;

// Server redelivers pending jobs on reconnect; remember what was already
// handled so a job is never printed twice.
static const int kSeenMax = 16;
static String seenJobs[kSeenMax];
static int seenNext = 0;

static bool jobSeen(const String& id) {
  for (int i = 0; i < kSeenMax; i++)
    if (seenJobs[i] == id) return true;
  return false;
}
static void markJobSeen(const String& id) {
  seenJobs[seenNext] = id;
  seenNext = (seenNext + 1) % kSeenMax;
}

// ───────────────────────── helpers ─────────────────────────

static String chipId() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%012llx", (unsigned long long)ESP.getEfuseMac());
  return String(buf);
}

static String randomId() {
  char buf[20];
  snprintf(buf, sizeof(buf), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  return String(buf);
}

// ISO 8601 UTC. Falls back to the epoch until NTP has synced; the server
// only requires a syntactically valid timestamp.
static String nowIso() {
  time_t t = time(nullptr);
  if (t < 1700000000) return "1970-01-01T00:00:00Z";
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

static bool parseUrl(const String& url, bool& tls, String& host, uint16_t& port, String& path) {
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return false;
  String scheme = url.substring(0, schemeEnd);
  scheme.toLowerCase();
  if (scheme == "https" || scheme == "wss") tls = true;
  else if (scheme == "http" || scheme == "ws") tls = false;
  else return false;
  String rest = url.substring(schemeEnd + 3);
  int slash = rest.indexOf('/');
  String hostport = slash < 0 ? rest : rest.substring(0, slash);
  path = slash < 0 ? "/" : rest.substring(slash);
  int colon = hostport.lastIndexOf(':');
  if (colon > 0) {
    host = hostport.substring(0, colon);
    port = (uint16_t)hostport.substring(colon + 1).toInt();
  } else {
    host = hostport;
    port = tls ? 443 : 80;
  }
  return !host.isEmpty() && port != 0;
}

static String bearer() { return "Bearer " + cfg.serverNodeId + "." + cfg.serverNodeSecret; }

static bool isPaired() { return !cfg.serverNodeId.isEmpty() && !cfg.serverNodeSecret.isEmpty() && !cfg.serverWsUrl.isEmpty(); }

bool mqttIsConnected() { return wsUp; }

String serverBridgeStatus() {
  if (cfg.serverUrl.isEmpty()) return "no server URL configured";
  if (isPaired()) return String("paired as ") + cfg.printerName + (wsUp ? ", connected" : ", disconnected");
  if (!cfg.serverPairingCode.isEmpty()) return lastEnrollError.isEmpty() ? "enrolling..." : "enrolling: " + lastEnrollError;
  return lastEnrollError.isEmpty() ? "not paired -- enter a pairing code" : "not paired: " + lastEnrollError;
}

// ───────────────────────── outgoing messages ─────────────────────────

static void sendDoc(JsonDocument& doc, const char* type) {
  if (!wsUp) return;
  doc["v"] = 1;
  doc["type"] = type;
  doc["id"] = randomId();
  doc["ts"] = nowIso();
  String out;
  serializeJson(doc, out);
  markLedEvent(LedEventType::tx);
  ws.sendTXT(out);
}

static void fillPrinter(JsonObject printer) {
  printer["protocol"] = cfg.printerProtocol;
  printer["method"] = cfg.printerMethod;
  printer["connected"] = setupWizardComplete();
  printer["dpi"] = cfg.dpi;

  JsonObject label = printer["label"].to<JsonObject>();
  label["width"] = cfg.labelWidth;
  label["length"] = cfg.labelLength;
  label["isRound"] = false;
  label["verticalOffset"] = 0;
  label["cut"] = false;

  JsonObject cap = printer["capabilities"].to<JsonObject>();
  cap["type"] = cfg.printerProtocol;
  cap["dpi"] = cfg.dpi;
  cap["zplCompression"] = cfg.zplCompressionSupported;
  cap["qlPrintheadPx"] = cfg.qlPrintheadPx;
  cap["qlInvalidateBytes"] = cfg.qlInvalidateBytes;
  cap["qlAutoCut"] = cfg.qlAutoCut;
  cap["qlFeedMarginDots"] = cfg.qlFeedMarginDots;
  cap["qlRightMarginDots"] = cfg.qlRightMarginDots;
  cap["seikoMaxDots"] = cfg.seikoMaxDots;
  cap["seikoDensity"] = cfg.seikoDensity;
  cap["seikoSpeed"] = cfg.seikoSpeed;
  JsonObject capLabel = cap["label"].to<JsonObject>();
  capLabel["width"] = cfg.labelWidth;
  capLabel["length"] = cfg.labelLength;
  capLabel["isRound"] = false;
  capLabel["verticalOffset"] = 0;
  capLabel["cut"] = false;
}

static void sendHello() {
  JsonDocument doc;
  doc["name"] = cfg.printerName;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["chip_id"] = chipId();
  fillPrinter(doc["printer"].to<JsonObject>());
  sendDoc(doc, "hello");
}

static void sendStatus() {
  JsonDocument doc;
  doc["name"] = cfg.printerName;
  doc["state"] = nodeState;
  if (nodeError.isEmpty()) {
    doc["error"] = nullptr;
  } else {
    JsonObject e = doc["error"].to<JsonObject>();
    e["code"] = "printer_error";
    e["message"] = nodeError.substring(0, 500);
  }
  fillPrinter(doc["printer"].to<JsonObject>());
  JsonObject roll = doc["roll"].to<JsonObject>();  // no roll sensor wired
  roll["state"] = "ok";
  roll["source"] = "default";
  JsonObject node = doc["node"].to<JsonObject>();
  node["firmware"] = FIRMWARE_VERSION;
  node["uptime_s"] = (uint32_t)(millis() / 1000UL);
  node["rssi_dbm"] = WiFi.RSSI();
  node["free_heap"] = ESP.getFreeHeap();
  sendDoc(doc, "status");
  lastStatusMs = millis();
  statusDirty = false;
}

static void sendJobUpdate(const String& jobId, const char* status, const String& message = "") {
  JsonDocument doc;
  doc["job_id"] = jobId;
  doc["status"] = status;
  if (!message.isEmpty()) doc["message"] = message.substring(0, 500);
  sendDoc(doc, "job.update");
}

static void sendWifiScan(int count) {
  JsonDocument doc;
  JsonArray aps = doc["aps"].to<JsonArray>();
  for (int i = 0; i < count && (int)aps.size() < 30; i++) {
    JsonObject ap = aps.add<JsonObject>();
    ap["bssid"] = WiFi.BSSIDstr(i);
    ap["rssi"] = WiFi.RSSI(i);
    ap["channel"] = WiFi.channel(i);
  }
  sendDoc(doc, "wifi.scan");
}

// Kept for API compatibility with the MQTT build; maps its phase strings
// onto the protocol's node states.
void publishStatus(const char* phase, const char* lastError) {
  const String p = phase;
  nodeState = p == "printing" ? "printing" : (p == "error" ? "error" : "idle");
  nodeError = lastError ? lastError : "";
  statusDirty = true;
}

// ───────────────────────── enrollment ─────────────────────────

static void tryEnroll() {
  if (cfg.serverUrl.isEmpty() || cfg.serverPairingCode.isEmpty()) return;
  const unsigned long now = millis();
  if (lastEnrollAttemptMs != 0 && now - lastEnrollAttemptMs < kEnrollRetryMs) return;
  lastEnrollAttemptMs = now;

  bool tls;
  String host, path;
  uint16_t port;
  if (!parseUrl(cfg.serverUrl, tls, host, port, path)) {
    lastEnrollError = "invalid server URL";
    cfg.serverPairingCode = "";
    saveConfig();
    return;
  }

  dbgPrintln("[server] enrolling at " + cfg.serverUrl, LogLevel::LOG_INFO);
  JsonDocument req;
  req["code"] = cfg.serverPairingCode;
  req["chip_id"] = chipId();
  req["firmware"] = FIRMWARE_VERSION;
  String body;
  serializeJson(req, body);

  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;
  http.setTimeout(15000);
  // Caddy redirects http -> https; a POST must keep its method (307/308).
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const String url = cfg.serverUrl + "/api/node/enroll";
  bool began;
  if (tls) {
    secure.setInsecure();
    began = http.begin(secure, url);
  } else {
    began = http.begin(plain, url);
  }
  if (!began) {
    lastEnrollError = "could not open connection";
    return;
  }
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  const String resp = code > 0 ? http.getString() : "";
  http.end();

  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, resp) || !doc["node_id"].is<const char*>() || !doc["node_secret"].is<const char*>()) {
      lastEnrollError = "unexpected response from server";
      return;
    }
    cfg.serverNodeId = doc["node_id"].as<String>();
    cfg.serverNodeSecret = doc["node_secret"].as<String>();
    cfg.serverWsUrl = doc["ws_url"] | "";
    const String name = doc["name"] | "";
    if (!name.isEmpty()) cfg.printerName = name;
    cfg.serverPairingCode = "";
    lastEnrollError = "";
    saveConfig();
    dbgPrintln("[server] enrolled as " + cfg.printerName + " (" + cfg.serverNodeId + ")", LogLevel::LOG_INFO);
  } else if (code == 400 || code == 403) {
    lastEnrollError = "pairing code rejected (invalid, used or expired)";
    cfg.serverPairingCode = "";
    saveConfig();
    dbgPrintln("[server] " + lastEnrollError, LogLevel::LOG_ERROR);
  } else if (code == 429) {
    lastEnrollError = "rate limited, retrying";
  } else {
    lastEnrollError = code > 0 ? "HTTP " + String(code) + " " + resp.substring(0, 80) : "connection failed: " + HTTPClient::errorToString(code);
    dbgPrintln("[server] enroll failed: " + lastEnrollError, LogLevel::LOG_WARN);
  }
}

// ───────────────────────── jobs ─────────────────────────

static void failJob(const String& id, const String& msg) {
  dbgPrintln("[server] job " + id + " failed: " + msg, LogLevel::LOG_ERROR);
  sendJobUpdate(id, "failed", msg);
  publishStatus("error", msg.c_str());
}

static uint8_t* allocPayload(size_t n) {
  uint8_t* p = nullptr;
  if (psramFound()) p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

static String toHex(const uint8_t* d, size_t n) {
  static const char* h = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += h[d[i] >> 4];
    s += h[d[i] & 15];
  }
  return s;
}

static void processJob(const JobOffer& job) {
  dbgPrintln("[server] job " + job.id + ": " + job.type + ", " + String((unsigned long)job.size) + " bytes", LogLevel::LOG_INFO);
  sendJobUpdate(job.id, "accepted");

  // Same protocol/payload-type compatibility rules as the MQTT build.
  const bool rasterProto = cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko";
  if ((job.type == "zpl" || job.type == "image") && rasterProto)
    return failJob(job.id, "this printer only accepts pre-rasterized jobs, not " + job.type);
  if (job.type == "ql_raster" && cfg.printerProtocol != "ql") return failJob(job.id, "this printer does not support ql_raster jobs");
  if (job.type == "seiko_raster" && cfg.printerProtocol != "seiko") return failJob(job.id, "this printer does not support seiko_raster jobs");
  if (job.type != "zpl" && job.type != "image" && job.type != "ql_raster" && job.type != "seiko_raster")
    return failJob(job.id, "unsupported payload_type " + job.type);
  if (!setupWizardComplete()) return failJob(job.id, "printer is not configured yet");
  if (job.size == 0 || job.size > kMaxPayloadBytes) return failJob(job.id, "invalid payload size");

  sendJobUpdate(job.id, "downloading");
  uint8_t* buf = allocPayload(job.size);
  if (!buf) return failJob(job.id, "esp32 out of memory (" + String((unsigned long)job.size) + " bytes, freeHeap=" + String((unsigned long)ESP.getFreeHeap()) + ")");

  bool tls;
  String host, path;
  uint16_t port;
  if (!parseUrl(job.url, tls, host, port, path)) {
    free(buf);
    return failJob(job.id, "invalid payload_url");
  }

  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;
  http.setTimeout(15000);
  bool began;
  if (tls) {
    secure.setInsecure();
    began = http.begin(secure, job.url);
  } else {
    began = http.begin(plain, job.url);
  }
  if (!began) {
    free(buf);
    return failJob(job.id, "could not open payload connection");
  }
  http.addHeader("Authorization", bearer());
  const int code = http.GET();
  if (code != 200) {
    http.end();
    free(buf);
    return failJob(job.id, "download failed: HTTP " + String(code));
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long lastData = millis();
  while (got < job.size && (http.connected() || stream->available())) {
    const int avail = stream->available();
    if (avail > 0) {
      const size_t want = min((size_t)avail, job.size - got);
      const int n = stream->readBytes(buf + got, want);
      if (n > 0) {
        got += n;
        lastData = millis();
      }
    } else {
      if (millis() - lastData > 15000) break;
      delay(2);
    }
    ws.loop();  // keep answering server pings during long downloads
  }
  http.end();
  if (got != job.size) {
    free(buf);
    return failJob(job.id, "download incomplete (" + String((unsigned long)got) + "/" + String((unsigned long)job.size) + " bytes)");
  }

  uint8_t digest[32];
  mbedtls_sha256(buf, got, digest, 0);
  if (!toHex(digest, 32).equalsIgnoreCase(job.sha256)) {
    free(buf);
    return failJob(job.id, "checksum mismatch");
  }

  sendJobUpdate(job.id, "printing");
  publishStatus("printing", "");
  sendStatus();

  String err;
  const int copies = job.copies < 1 ? 1 : job.copies;
  bool ok = true;
  for (int c = 0; c < copies && ok; c++) {
    ok = targetSend(buf, got, err);
    ws.loop();
  }
  free(buf);
  if (!ok) return failJob(job.id, err.isEmpty() ? String("send to printer failed") : err);

  dbgPrintln("[server] job " + job.id + " done", LogLevel::LOG_INFO);
  sendJobUpdate(job.id, "done");
  publishStatus("ready", "");
}

// ───────────────────────── WebSocket ─────────────────────────

static void onMessage(const char* text, size_t len) {
  markLedEvent(LedEventType::rx);
  JsonDocument doc;
  if (deserializeJson(doc, text, len)) {
    dbgPrintln("[server] bad JSON from server", LogLevel::LOG_WARN);
    return;
  }
  const String type = doc["type"] | "";

  if (type == "welcome" || type == "config.set") {
    const String name = doc["name"] | "";
    // "welcome" is sent on every (re)connect and carries the server's stored
    // name, which would silently revert a name just edited in the local web UI
    // (the save triggers a reconnect). Only an explicit "config.set" from the
    // server overrides a name that is already set locally.
    const bool adopt = type == "config.set" || cfg.printerName.isEmpty() || cfg.printerName == "stikka-esp32";
    if (adopt && !name.isEmpty() && name != cfg.printerName) {
      dbgPrintln("[server] name is now \"" + name + "\"", LogLevel::LOG_INFO);
      cfg.printerName = name;
      saveConfig();
      statusDirty = true;
    }
  } else if (type == "status.request") {
    statusDirty = true;
  } else if (type == "job.offer") {
    JobOffer o;
    o.id = doc["job_id"] | "";
    o.type = doc["payload_type"] | "";
    o.size = doc["payload_size"] | 0;
    o.sha256 = doc["payload_sha256"] | "";
    o.url = doc["payload_url"] | "";
    o.copies = doc["copies"] | 1;
    if (o.id.isEmpty() || o.url.isEmpty()) return;
    if (jobSeen(o.id)) {
      dbgPrintln("[server] ignoring already handled job " + o.id, LogLevel::LOG_INFO);
      return;
    }
    for (int i = 0; i < jobQueueLen; i++)
      if (jobQueue[i].id == o.id) return;
    if (jobQueueLen < kQueueMax) jobQueue[jobQueueLen++] = o;
  }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsUp = true;
      dbgPrintln("[server] connected", LogLevel::LOG_INFO);
      sendHello();
      sendStatus();
      scanPending = true;
      break;
    case WStype_DISCONNECTED:
      if (wsUp) dbgPrintln("[server] disconnected", LogLevel::LOG_WARN);
      wsUp = false;
      break;
    case WStype_TEXT:
      onMessage((const char*)payload, length);
      break;
    default:
      break;
  }
}

static void stopWs() {
  if (wsConfigured) ws.disconnect();
  wsConfigured = false;
  wsConfiguredFor = "";
  wsUp = false;
}

static void startWs() {
  const String sig = cfg.serverWsUrl + "|" + cfg.serverNodeId + "|" + cfg.serverNodeSecret;
  if (wsConfigured && wsConfiguredFor == sig) return;
  if (wsConfigured) stopWs();

  bool tls;
  String host, path;
  uint16_t port;
  if (!parseUrl(cfg.serverWsUrl, tls, host, port, path)) {
    dbgPrintln("[server] invalid ws_url: " + cfg.serverWsUrl, LogLevel::LOG_ERROR);
    return;
  }
  dbgPrintln("[server] connecting to " + cfg.serverWsUrl, LogLevel::LOG_INFO);
  if (tls) ws.beginSSL(host.c_str(), port, path.c_str());  // no fingerprint/CA => library uses insecure TLS
  else ws.begin(host.c_str(), port, path.c_str());
  static String authHeader;  // library keeps the pointer
  authHeader = "Authorization: " + bearer();
  ws.setExtraHeaders(authHeader.c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(5000);
  wsConfigured = true;
  wsConfiguredFor = sig;
}

// ───────────────────────── entry points ─────────────────────────

void mqttBridgeReconnect() { statusDirty = true; }

void mqttBridgeSetup() {
  // Everything happens lazily in the loop once Wi-Fi is up.
}

void mqttBridgeLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wsConfigured) stopWs();
    return;
  }

  if (!timeConfigured) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    timeConfigured = true;
  }

  if (!isPaired()) {
    if (wsConfigured) stopWs();
    tryEnroll();
    return;
  }

  startWs();
  ws.loop();
  if (!wsUp) return;

  if (jobQueueLen > 0) {
    JobOffer job = jobQueue[0];
    for (int i = 1; i < jobQueueLen; i++) jobQueue[i - 1] = jobQueue[i];
    jobQueueLen--;
    markJobSeen(job.id);
    processJob(job);
    statusDirty = true;
  }

  const unsigned long now = millis();
  unsigned long intervalMs = (unsigned long)cfg.statusIntervalSec * 1000UL;
  if (intervalMs > 60000UL || intervalMs == 0) intervalMs = 60000UL;  // protocol: status at least every 60 s
  if (statusDirty || now - lastStatusMs > intervalMs) sendStatus();

  // Wi-Fi scan for server-side geolocation: on connect, then daily.
  if (scanRunning) {
    const int n = WiFi.scanComplete();
    if (n >= 0) {
      sendWifiScan(n);
      WiFi.scanDelete();
      scanRunning = false;
      lastScanMs = now;
    } else if (n == WIFI_SCAN_FAILED) {
      scanRunning = false;
    }
  } else if ((scanPending || now - lastScanMs > kScanIntervalMs) && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanNetworks(true);
    scanRunning = true;
    scanPending = false;
  }
}

#endif  // STIKKA_SERVER
