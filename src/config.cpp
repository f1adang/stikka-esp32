#include "config.h"

#include <Preferences.h>

#include "logging.h"
#include "mqtt_bridge.h"

AppConfig cfg;

static Preferences prefs;

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::LOG_ERROR: return "ERROR";
    case LogLevel::LOG_WARN: return "WARN";
    case LogLevel::LOG_INFO: return "INFO";
    default: return "DEBUG";
  }
}

LogLevel logLevelFromString(const String& raw, LogLevel fallback) {
  String v = raw;
  v.toUpperCase();
  if (v == "ERROR") return LogLevel::LOG_ERROR;
  if (v == "WARN") return LogLevel::LOG_WARN;
  if (v == "INFO") return LogLevel::LOG_INFO;
  if (v == "DEBUG") return LogLevel::LOG_DEBUG;
  return fallback;
}

void loadConfig() {
  if (!prefs.begin("stikka", true)) {
    if (prefs.begin("stikka", false)) {
      prefs.end();
    }
    return;
  }
  cfg.wifiSsid = prefs.getString("wifiSsid", "");
  cfg.wifiPassword = prefs.getString("wifiPwd", "");
  cfg.mqttHost = prefs.getString("mqttHost", DEFAULT_MQTT_HOST);
  cfg.mqttPort = prefs.getUShort("mqttPort", DEFAULT_MQTT_PORT);
  cfg.mqttUseTls = prefs.getBool("mqttTls", DEFAULT_MQTT_USE_TLS);
  cfg.mqttTlsInsecure = prefs.getBool("mqttInsec", DEFAULT_MQTT_TLS_INSECURE);
  cfg.mqttCaCert = prefs.getString("mqttCa", "");
  cfg.mqttUser = prefs.getString("mqttUser", DEFAULT_MQTT_USER);
  cfg.mqttPassword = prefs.getString("mqttPwd", DEFAULT_MQTT_PASSWORD);
  cfg.serverUrl = prefs.getString("srvUrl", "");
  cfg.serverPairingCode = prefs.getString("srvCode", "");
  cfg.serverNodeId = prefs.getString("srvNodeId", "");
  cfg.serverNodeSecret = prefs.getString("srvSecret", "");
  cfg.serverWsUrl = prefs.getString("srvWsUrl", "");
  cfg.statusIntervalSec = prefs.getUShort("statInt", 30);
  cfg.printerName = prefs.getString("printer", "stikka-esp32");
  cfg.adminPassword = prefs.getString("adminPwd", "");
  cfg.location = prefs.getString("location", "");
  cfg.printerProtocol = prefs.getString("printProto", "");
  cfg.printerMethod = prefs.getString("printMeth", "");
  cfg.zplTargetHost = prefs.getString("zplHost", "");
  cfg.zplTargetPort = prefs.getUShort("zplPort", 9100);
  cfg.dpi = prefs.getInt("dpi", DEFAULT_DPI);
  cfg.labelWidth = prefs.getInt("labelW", 55);
  cfg.labelLength = prefs.getInt("labelL", 55);
  cfg.zplCompressionSupported = prefs.getBool("zplCompr", false);
  cfg.debugOutput = prefs.getBool("dbgOut", true);
  cfg.printerUartTxPin = prefs.getInt("puTx", 32);
  cfg.printerUartRxPin = prefs.getInt("puRx", 33);
  cfg.printerUartBaud = prefs.getUInt("puBaud", 9600);
  cfg.qlPrintheadPx = prefs.getInt("qlHeadPx", 720);
  cfg.qlInvalidateBytes = prefs.getInt("qlInval", 200);
  cfg.qlAutoCut = prefs.getBool("qlCut", true);
  cfg.qlFeedMarginDots = prefs.getInt("qlFeed", 35);
  cfg.qlRightMarginDots = prefs.getInt("qlRMarg", 0);
  cfg.seikoMaxDots = prefs.getInt("seikoDots", 576);
  cfg.seikoDensity = prefs.getInt("seikoDens", 0x00);
  cfg.seikoSpeed = prefs.getInt("seikoSpd", 0x02);
  cfg.logLevel = (LogLevel)prefs.getUChar("logLvl", (uint8_t)LogLevel::LOG_INFO);
  cfg.savedWifiNetworkCount = prefs.getInt("wNetCount", 0);
  if (cfg.savedWifiNetworkCount < 0) cfg.savedWifiNetworkCount = 0;
  if (cfg.savedWifiNetworkCount > MAX_SAVED_WIFI_NETWORKS) cfg.savedWifiNetworkCount = MAX_SAVED_WIFI_NETWORKS;
  for (int i = 0; i < MAX_SAVED_WIFI_NETWORKS; i++) {
    cfg.savedWifiNetworks[i].ssid = prefs.getString(("wNetS" + String(i)).c_str(), "");
    cfg.savedWifiNetworks[i].password = prefs.getString(("wNetP" + String(i)).c_str(), "");
  }
  prefs.end();
}

bool setupWizardComplete() {
  return !cfg.printerProtocol.isEmpty() && !cfg.printerMethod.isEmpty() && !cfg.wifiSsid.isEmpty();
}

void saveConfig() {
  prefs.begin("stikka", false);
  prefs.putString("wifiSsid", cfg.wifiSsid);
  prefs.putString("wifiPwd", cfg.wifiPassword);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putBool("mqttTls", cfg.mqttUseTls);
  prefs.putBool("mqttInsec", cfg.mqttTlsInsecure);
  prefs.putString("mqttCa", cfg.mqttCaCert);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPwd", cfg.mqttPassword);
  prefs.putString("srvUrl", cfg.serverUrl);
  prefs.putString("srvCode", cfg.serverPairingCode);
  prefs.putString("srvNodeId", cfg.serverNodeId);
  prefs.putString("srvSecret", cfg.serverNodeSecret);
  prefs.putString("srvWsUrl", cfg.serverWsUrl);
  prefs.putUShort("statInt", cfg.statusIntervalSec);
  prefs.putString("printer", cfg.printerName);
  prefs.putString("adminPwd", cfg.adminPassword);
  prefs.putString("location", cfg.location);
  prefs.putString("printProto", cfg.printerProtocol);
  prefs.putString("printMeth", cfg.printerMethod);
  prefs.putString("zplHost", cfg.zplTargetHost);
  prefs.putUShort("zplPort", cfg.zplTargetPort);
  prefs.putInt("dpi", cfg.dpi);
  prefs.putInt("labelW", cfg.labelWidth);
  prefs.putInt("labelL", cfg.labelLength);
  prefs.putBool("zplCompr", cfg.zplCompressionSupported);
  prefs.putBool("dbgOut", cfg.debugOutput);
  prefs.putInt("puTx", cfg.printerUartTxPin);
  prefs.putInt("puRx", cfg.printerUartRxPin);
  prefs.putUInt("puBaud", cfg.printerUartBaud);
  prefs.putInt("qlHeadPx", cfg.qlPrintheadPx);
  prefs.putInt("qlInval", cfg.qlInvalidateBytes);
  prefs.putBool("qlCut", cfg.qlAutoCut);
  prefs.putInt("qlFeed", cfg.qlFeedMarginDots);
  prefs.putInt("qlRMarg", cfg.qlRightMarginDots);
  prefs.putInt("seikoDots", cfg.seikoMaxDots);
  prefs.putInt("seikoDens", cfg.seikoDensity);
  prefs.putInt("seikoSpd", cfg.seikoSpeed);
  prefs.putUChar("logLvl", (uint8_t)cfg.logLevel);
  prefs.putInt("wNetCount", cfg.savedWifiNetworkCount);
  for (int i = 0; i < MAX_SAVED_WIFI_NETWORKS; i++) {
    prefs.putString(("wNetS" + String(i)).c_str(), cfg.savedWifiNetworks[i].ssid);
    prefs.putString(("wNetP" + String(i)).c_str(), cfg.savedWifiNetworks[i].password);
  }
  prefs.end();
}

void rememberWifiNetwork(const String& ssid, const String& password) {
  if (ssid.isEmpty()) return;

  for (int i = 0; i < cfg.savedWifiNetworkCount; i++) {
    if (cfg.savedWifiNetworks[i].ssid == ssid) {
      cfg.savedWifiNetworks[i].password = password;
      saveConfig();
      return;
    }
  }

  if (cfg.savedWifiNetworkCount >= MAX_SAVED_WIFI_NETWORKS) {
    for (int i = 1; i < MAX_SAVED_WIFI_NETWORKS; i++) {
      cfg.savedWifiNetworks[i - 1] = cfg.savedWifiNetworks[i];
    }
    cfg.savedWifiNetworkCount = MAX_SAVED_WIFI_NETWORKS - 1;
  }
  cfg.savedWifiNetworks[cfg.savedWifiNetworkCount].ssid = ssid;
  cfg.savedWifiNetworks[cfg.savedWifiNetworkCount].password = password;
  cfg.savedWifiNetworkCount++;
  saveConfig();
}

void forgetWifiNetwork(const String& ssid) {
  for (int i = 0; i < cfg.savedWifiNetworkCount; i++) {
    if (cfg.savedWifiNetworks[i].ssid == ssid) {
      for (int j = i + 1; j < cfg.savedWifiNetworkCount; j++) {
        cfg.savedWifiNetworks[j - 1] = cfg.savedWifiNetworks[j];
      }
      cfg.savedWifiNetworkCount--;
      cfg.savedWifiNetworks[cfg.savedWifiNetworkCount].ssid = "";
      cfg.savedWifiNetworks[cfg.savedWifiNetworkCount].password = "";
      saveConfig();
      return;
    }
  }
}

void printRuntimeSettings(const char* reason) {
  dbgPrintln();
  dbgPrintln("================ Stikka ESP32 Settings ================");
  dbgPrint("Reason: ");
  dbgPrintln(reason);
  dbgPrint("Printer name: ");
  dbgPrintln(cfg.printerName);
  dbgPrint("Printer protocol: ");
  dbgPrintln(cfg.printerProtocol.isEmpty() ? String("<not set -- setup wizard incomplete>") : cfg.printerProtocol);
  dbgPrint("Location: ");
  dbgPrintln(cfg.location.isEmpty() ? String("<not set>") : cfg.location);
  if (cfg.printerMethod == "network") {
    dbgPrint("Target (network): ");
    dbgPrint(cfg.zplTargetHost);
    dbgPrint(":");
    dbgPrintln(cfg.zplTargetPort);
  } else if (cfg.printerMethod == "serial") {
    dbgPrint("Target (serial UART) TX/RX/baud: ");
    dbgPrint(cfg.printerUartTxPin);
    dbgPrint("/");
    dbgPrint(cfg.printerUartRxPin);
    dbgPrint("/");
    dbgPrintln(cfg.printerUartBaud);
  } else if (cfg.printerMethod == "usb_host") {
    dbgPrintln("Target (usb_host): auto-enumerates connected USB printer");
  } else {
    dbgPrintln("Target: <not set -- setup wizard incomplete>");
  }
  if (cfg.printerProtocol == "ql") {
    dbgPrint("QL printhead px / invalidate bytes: ");
    dbgPrint(cfg.qlPrintheadPx);
    dbgPrint(" / ");
    dbgPrintln(cfg.qlInvalidateBytes);
    dbgPrint("QL auto-cut / feed margin / right margin: ");
    dbgPrint(cfg.qlAutoCut ? "on" : "off");
    dbgPrint(" / ");
    dbgPrint(cfg.qlFeedMarginDots);
    dbgPrint(" / ");
    dbgPrintln(cfg.qlRightMarginDots);
  }
  if (cfg.printerProtocol == "seiko") {
    dbgPrint("Seiko max dots / density / speed: ");
    dbgPrint(cfg.seikoMaxDots);
    dbgPrint(" / ");
    dbgPrint(cfg.seikoDensity);
    dbgPrint(" / ");
    dbgPrintln(cfg.seikoSpeed);
  }
  dbgPrint("Label (mm): ");
  dbgPrint(cfg.labelWidth);
  dbgPrint("x");
  dbgPrint(cfg.labelLength);
  dbgPrint(" @ ");
  dbgPrint(cfg.dpi);
  dbgPrintln(" DPI");
  dbgPrint("ZPL compression (:Z64:/:B64:) supported: ");
  dbgPrintln(cfg.zplCompressionSupported ? "yes" : "no");
  dbgPrint("Wi-Fi SSID: ");
  dbgPrintln(cfg.wifiSsid.isEmpty() ? String("<not configured>") : cfg.wifiSsid);
  dbgPrint("Wi-Fi password set: ");
  dbgPrintln(cfg.wifiPassword.isEmpty() ? "no" : "yes");
  dbgPrint("Saved Wi-Fi networks: ");
  dbgPrint(cfg.savedWifiNetworkCount);
  dbgPrint("/");
  dbgPrintln(MAX_SAVED_WIFI_NETWORKS);
#ifdef STIKKA_SERVER
  dbgPrint("Server: ");
  dbgPrintln(cfg.serverUrl.isEmpty() ? String("<not configured>") : cfg.serverUrl);
  dbgPrint("Node id: ");
  dbgPrintln(cfg.serverNodeId.isEmpty() ? String("<not paired>") : cfg.serverNodeId);
  dbgPrint("Pairing code pending: ");
  dbgPrintln(cfg.serverPairingCode.isEmpty() ? "no" : "yes");
#else
  dbgPrint("MQTT broker: ");
  if (cfg.mqttHost.isEmpty()) {
    dbgPrintln("<not configured>");
  } else {
    dbgPrint(cfg.mqttHost);
    dbgPrint(":");
    dbgPrintln(cfg.mqttPort);
  }
  dbgPrint("MQTT user: ");
  dbgPrintln(cfg.mqttUser.isEmpty() ? String("<none>") : cfg.mqttUser);
  dbgPrint("MQTT TLS: ");
  dbgPrintln(cfg.mqttUseTls ? "enabled" : "disabled");
  dbgPrint("MQTT TLS insecure: ");
  dbgPrintln(cfg.mqttTlsInsecure ? "enabled" : "disabled");
  dbgPrint("MQTT CA cert: ");
  dbgPrintln(cfg.mqttCaCert.isEmpty() ? "<none>" : "configured");
  dbgPrint("Command topic: ");
  dbgPrintln(commandTopic());
  dbgPrint("Status topic: ");
  dbgPrintln(statusTopic());
#endif
  dbgPrint("Debug output: ");
  dbgPrintln(cfg.debugOutput ? "enabled" : "disabled");
  dbgPrintln("========================================================");
  dbgPrintln();
}
