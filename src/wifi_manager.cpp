#include "wifi_manager.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"

static DNSServer dnsServer;
static bool dnsServerActive = false;
static bool apModeActive = false;
static bool mdnsActive = false;
static String mdnsHostname;
static unsigned long lastWifiAttemptMs = 0;
// While a manual scan is in progress (or just failed), station connect retries
// are held off: a pending WiFi.begin() keeps the radio busy and makes
// scanNetworks() fail or return nothing.
static unsigned long scanHoldUntilMs = 0;
static int scanRetriesLeft = 0;
static unsigned long wifiDisconnectedSinceMs = 0;
static int lastWifiStatus = -1;
static bool lastApModeState = false;

static bool pendingConnectActive = false;
static unsigned long pendingConnectStartedMs = 0;

static const unsigned long WIFI_RETRY_EVERY_MS = 5000;
static const unsigned long WIFI_FALLBACK_AP_AFTER_MS = 20000;
static const unsigned long PENDING_CONNECT_TIMEOUT_MS = 15000;
static const char* AP_PASSWORD = "stikkaesp32";
static const uint16_t DNS_PORT = 53;
// Overrides the ESP32 Arduino core's default soft-AP IP (192.168.4.1).
static const IPAddress AP_IP(1, 2, 3, 4);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool wifiApModeActive() {
  return apModeActive;
}

const char* wifiApPassword() {
  return AP_PASSWORD;
}

String wifiFallbackApSsid() {
  uint32_t mac = (uint32_t)ESP.getEfuseMac();
  String suffix = String(mac, HEX);
  suffix.toUpperCase();
  return String("Stikka-") + suffix;
}

// mDNS hostnames are DNS labels: letters/digits/hyphens only, no leading or
// trailing hyphen. cfg.printerName is freeform user text (default
// "stikka-esp32", but web-UI-editable to anything), so it needs sanitizing
// before use here rather than assuming it's already a valid label.
String sanitizeMdnsHostname(const String& raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    } else if (out.length() > 0 && out[out.length() - 1] != '-') {
      out += '-';
    }
  }
  while (out.length() > 0 && out[0] == '-') out.remove(0, 1);
  while (out.length() > 0 && out[out.length() - 1] == '-') out.remove(out.length() - 1, 1);
  if (out.isEmpty()) out = "stikka-esp32";
  return out;
}

static void stopMdns() {
  if (!mdnsActive) return;
  MDNS.end();
  mdnsActive = false;
  mdnsHostname = "";
}

String wifiMdnsHostname() {
  return mdnsActive ? mdnsHostname : String();
}

// Restarted (not just started once) on every fresh station connection --
// covers both a plain reconnect and cfg.printerName having changed via the
// web UI (handleSave() forces a Wi-Fi disconnect/reconnect cycle after
// saving, so the new name takes effect here without a separate hook).
static void startMdns() {
  stopMdns();
  const String hostname = sanitizeMdnsHostname(cfg.printerName);
  if (!MDNS.begin(hostname.c_str())) {
    dbgPrintln("[mdns] responder failed to start", LogLevel::LOG_WARN);
    return;
  }
  MDNS.addService("http", "tcp", 80);
  mdnsActive = true;
  mdnsHostname = hostname;
  // LOG_ERROR here for the same reason as the "[wifi] connected, ip=" line
  // above: guaranteed to survive even the quietest configured log level,
  // since this -- like the IP -- is how you find the device.
  dbgPrint("[mdns] responder started, browse to http://");
  dbgPrint(hostname);
  dbgPrintln(".local/", LogLevel::LOG_ERROR);
}

void wifiRestartMdns() {
  if (WiFi.status() == WL_CONNECTED) startMdns();
}

static void ensureFallbackAp() {
  if (apModeActive) return;
  // Explicit, not assumed left over from wifiManagerSetup(): dropToStaOnly()
  // below can turn this back down to WIFI_STA whenever a connected device
  // never needed the AP, so it has to be re-armed here every time.
  WiFi.mode(WIFI_AP_STA);
  const String ssid = wifiFallbackApSsid();
  if (WiFi.softAP(ssid.c_str(), AP_PASSWORD)) {
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    apModeActive = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    dnsServerActive = true;
    dbgPrint("[wifi] fallback AP started: ");
    dbgPrint(ssid);
    dbgPrint(" (ip=");
    dbgPrint(WiFi.softAPIP());
    dbgPrintln(")", LogLevel::LOG_WARN);
  }
}

// Drops the radio to WIFI_STA (no AP interface at all) once we know the
// fallback AP isn't needed. Distinct from wifiDisableFallbackAp() below:
// that one only runs its teardown when apModeActive is true, i.e. only after
// *this* code explicitly started an AP via ensureFallbackAp() -- a device
// that connects to station Wi-Fi on its very first attempt never sets
// apModeActive, but wifiManagerSetup() still leaves it in WIFI_AP_STA from
// boot. Left alone, the SoftAP interface stays enabled with the SDK's
// default/unconfigured AP config, which broadcasts a stray "ESP-<hex>"
// network for as long as the device is up -- observed in the field, one per
// already-connected device. Safe to call unconditionally on every confirmed
// station connection regardless of AP history.
static void dropToStaOnly() {
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
}

void wifiDisableFallbackAp() {
  if (!apModeActive) return;
  if (dnsServerActive) {
    dnsServer.stop();
    dnsServerActive = false;
  }
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  dbgPrintln("[wifi] fallback AP disabled", LogLevel::LOG_INFO);
}

static void connectWifi() {
  // A manual WLAN scan (wifiStartScan(), web UI) and a station connect/
  // retry attempt can't safely share this single radio -- WiFi.begin()
  // firing while esp_wifi_scan_start() is still in-flight can abort or
  // corrupt the scan and leave the Wi-Fi stack flaky rather than just
  // failing cleanly. Skip this iteration's reconnect bookkeeping entirely
  // while a scan is running; wifiManagerLoop() calls this every loop()
  // iteration (well under a second later), so it picks back up normally as
  // soon as the scan completes -- at worst delaying a retry/fallback-AP
  // check by however long the scan takes, not skipping it.
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;

  const unsigned long now = millis();

  if (cfg.wifiSsid.isEmpty()) {
    ensureFallbackAp();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSinceMs = 0;
    wifiDisableFallbackAp();
    dropToStaOnly();
    return;
  }

  if (wifiDisconnectedSinceMs == 0) {
    wifiDisconnectedSinceMs = now;
  }

  if (now - wifiDisconnectedSinceMs >= WIFI_FALLBACK_AP_AFTER_MS) {
    ensureFallbackAp();
  }

  if (scanHoldUntilMs != 0 && (long)(scanHoldUntilMs - now) > 0) return;
  if (now - lastWifiAttemptMs < WIFI_RETRY_EVERY_MS) return;
  lastWifiAttemptMs = now;

  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
}

void wifiRetryNow() {
  wifiDisconnectedSinceMs = 0;
  lastWifiAttemptMs = 0;
  scanHoldUntilMs = 0;
}

void wifiForceDisconnect() {
  WiFi.disconnect(true);
  wifiDisconnectedSinceMs = millis();
  // Let connectWifi() call WiFi.begin() on the next loop() instead of waiting
  // out the retry throttle, so "Save and reconnect" reconnects right away.
  lastWifiAttemptMs = 0;
}

void printNetworkState(const char* reason) {
  dbgPrintln();
  dbgPrintln("---------------- Network State ----------------");
  dbgPrint("Reason: ");
  dbgPrintln(reason);
  const int wifiState = WiFi.status();
  if (wifiState == WL_CONNECTED) {
    dbgPrint("Wi-Fi: connected to '");
    dbgPrint(WiFi.SSID());
    dbgPrintln("'");
    dbgPrint("IP: ");
    dbgPrintln(WiFi.localIP());
    dbgPrint("Gateway: ");
    dbgPrintln(WiFi.gatewayIP());
    dbgPrint("RSSI: ");
    dbgPrint(WiFi.RSSI());
    dbgPrintln(" dBm");
    if (mdnsActive) {
      dbgPrint("mDNS: http://");
      dbgPrint(mdnsHostname);
      dbgPrintln(".local/");
    }
  } else {
    dbgPrint("Wi-Fi: disconnected (status=");
    dbgPrint(wifiState);
    dbgPrintln(")");
  }

  if (apModeActive) {
    dbgPrint("Fallback AP: active, SSID='");
    dbgPrint(wifiFallbackApSsid());
    dbgPrintln("'");
    dbgPrint("AP IP: ");
    dbgPrintln(WiFi.softAPIP());
  } else {
    dbgPrintln("Fallback AP: inactive");
  }

  dbgPrint("MQTT: ");
  dbgPrintln(mqttIsConnected() ? "connected" : "disconnected");
  dbgPrintln("-----------------------------------------------");
  dbgPrintln();
}

void wifiStartScan() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  WiFi.scanDelete();
  scanRetriesLeft = 2;
  scanHoldUntilMs = millis() + 20000;
  // Active scan (default), but with a longer per-channel dwell
  // (max_ms_per_chan) than the library default of 300ms -- 300ms only
  // reliably catches whichever APs happen to answer a probe request
  // immediately, missing weaker/farther ones that would otherwise respond
  // within a beacon interval or two. 600ms roughly doubles total scan time
  // (~7-8s across the 2.4GHz channel set) but this already runs async and
  // is polled from the web UI, so the extra time costs nothing but a longer
  // "Scanning..." label. Note this can't fully compensate for the device's
  // own fallback AP being active during the scan (WIFI_AP_STA, see
  // ensureFallbackAp()) -- a single radio interleaving station scanning
  // with AP beacon duty is inherently less thorough than a scan on a radio
  // doing nothing else, and there's no way around that while a browser is
  // relying on that same AP to reach this page.
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false, /*passive=*/false, /*max_ms_per_chan=*/600);
}

String wifiScanResultJson() {
  const int16_t n = WiFi.scanComplete();
  JsonDocument doc;

  if (n == WIFI_SCAN_RUNNING) {
    doc["status"] = "scanning";
  } else if (n == WIFI_SCAN_FAILED && scanRetriesLeft > 0) {
    // Transient failure (radio busy): retry transparently.
    scanRetriesLeft--;
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false, false, 600);
    doc["status"] = "scanning";
  } else if (n == WIFI_SCAN_FAILED) {
    scanHoldUntilMs = 0;
    doc["status"] = "failed";
  } else if (n < 0) {
    doc["status"] = "idle";
  } else {
    doc["status"] = "done";
    scanHoldUntilMs = 0;
    JsonArray arr = doc["networks"].to<JsonArray>();
    // n is small (real-world scans rarely exceed a few dozen APs), so this
    // repeated linear scan to pick strongest-first while skipping
    // already-listed SSIDs (mesh routers commonly broadcast the same name
    // from multiple APs) is cheap -- not worth a sort/dedup data structure
    // for a one-off web request.
    for (int rank = 0; rank < n; rank++) {
      int best = -1;
      int32_t bestRssi = INT32_MIN;
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;
        bool alreadyListed = false;
        for (JsonObject existing : arr) {
          if (existing["ssid"] == WiFi.SSID(i)) {
            alreadyListed = true;
            break;
          }
        }
        if (alreadyListed) continue;
        if (WiFi.RSSI(i) > bestRssi) {
          bestRssi = WiFi.RSSI(i);
          best = i;
        }
      }
      if (best < 0) break;
      JsonObject net = arr.add<JsonObject>();
      net["ssid"] = WiFi.SSID(best);
      net["rssi"] = WiFi.RSSI(best);
      net["secure"] = WiFi.encryptionType(best) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// Deliberately doesn't touch the fallback AP. If the browser reached this
// page via the AP (first-boot setup, no station network yet), tearing the
// AP down here would cut the browser's own connection before it can see the
// "connecting"/"failed" state below. connectWifi() already disables the AP
// itself the moment it observes WL_CONNECTED (see the early branch above),
// so a successful connect still ends up STA-only -- just not until the
// browser has had a chance to see the result.
void wifiConnectTo(const String& ssid, const String& password) {
  cfg.wifiSsid = ssid;
  cfg.wifiPassword = password;
  // Persists cfg.wifiSsid/wifiPassword too (rememberWifiNetwork() ->
  // saveConfig() writes the whole config), so no separate saveConfig() call
  // is needed here.
  rememberWifiNetwork(ssid, password);

  pendingConnectActive = true;
  pendingConnectStartedMs = millis();
  // Reset so connectWifi()'s own bookkeeping (fallback-AP timer, retry
  // throttle) restarts cleanly against this attempt rather than whatever was
  // left over from the previous SSID.
  wifiDisconnectedSinceMs = 0;

  // Same hazard connectWifi() guards against: WiFi.begin() while a scan
  // (wifiStartScan()) is still in-flight can corrupt the scan/radio state
  // instead of failing cleanly. A user can hit this by starting a scan and
  // then, before it finishes, clicking Connect on a *different*, always-
  // visible control (a saved network's own Connect button, not the one tied
  // to this scan's own results). cfg.wifiSsid/wifiPassword are already
  // saved above, so just let connectWifi()'s normal retry loop pick this up
  // -- lastWifiAttemptMs=0 makes it retry on the very next loop() iteration
  // once the scan completes, rather than waiting a full WIFI_RETRY_EVERY_MS.
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    lastWifiAttemptMs = 0;
    return;
  }

  lastWifiAttemptMs = millis();
  WiFi.begin(ssid.c_str(), password.c_str());
}

String wifiConnectStatusJson() {
  JsonDocument doc;
  if (!pendingConnectActive) {
    doc["status"] = "idle";
  } else if (WiFi.status() == WL_CONNECTED) {
    doc["status"] = "connected";
    doc["ip"] = WiFi.localIP().toString();
    pendingConnectActive = false;
  } else if (millis() - pendingConnectStartedMs > PENDING_CONNECT_TIMEOUT_MS) {
    doc["status"] = "failed";
    pendingConnectActive = false;
  } else {
    doc["status"] = "connecting";
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void wifiManagerSetup() {
  WiFi.mode(WIFI_AP_STA);
  connectWifi();
}

void wifiManagerLoop() {
  if (dnsServerActive) {
    dnsServer.processNextRequest();
  }

  connectWifi();

  const int wifiNow = WiFi.status();
  if (wifiNow != lastWifiStatus) {
    lastWifiStatus = wifiNow;
    if (wifiNow == WL_CONNECTED) {
      // LOG_ERROR here isn't about severity -- it's the one level guaranteed
      // to show regardless of the configured log level (see flushPendingLogLine:
      // a line is dropped only if it's MORE verbose than cfg.logLevel, and
      // nothing is more severe than ERROR). The IP is how you find the device
      // to reconfigure it, so it needs to survive even the quietest setting.
      dbgPrint("[wifi] connected, ip=");
      dbgPrintln(WiFi.localIP(), LogLevel::LOG_ERROR);
      startMdns();
      printNetworkState("wifi connected");
    } else {
      dbgPrintln("[wifi] disconnected", LogLevel::LOG_ERROR);
      stopMdns();
      printNetworkState("wifi disconnected");
    }
  }

  if (apModeActive != lastApModeState) {
    lastApModeState = apModeActive;
    printNetworkState(apModeActive ? "fallback AP enabled" : "fallback AP disabled");
  }
}
