#pragma once

#include <Arduino.h>

// WiFi.mode(WIFI_AP_STA) + first connect attempt. Call once from setup().
void wifiManagerSetup();

// DNS captive-portal servicing, station reconnect/retry, fallback-AP
// enter/exit, and state-change diagnostics. Call once per loop() iteration.
void wifiManagerLoop();

bool wifiIsConnected();
bool wifiApModeActive();
String wifiFallbackApSsid();
const char* wifiApPassword();

// The mDNS hostname (without ".local") currently registered, derived from
// cfg.printerName -- empty if the responder isn't running (not yet
// connected to a station network, or it failed to start). See
// startMdns()/wifiManagerLoop() in wifi_manager.cpp.
String wifiMdnsHostname();

// Hostname (without ".local") that cfg.printerName maps to.
String sanitizeMdnsHostname(const String& raw);

// Torn down explicitly (rather than left to the next wifiManagerLoop()
// retry) when the web UI saves new Wi-Fi/MQTT settings, so the old
// connection doesn't linger with stale credentials.
void wifiDisableFallbackAp();
void wifiForceDisconnect();

// Makes the next loop() call WiFi.begin() with the current cfg credentials
// without tearing down the radio or the fallback AP.
void wifiRetryNow();

// Re-registers the mDNS hostname from cfg.printerName (no-op if not connected).
void wifiRestartMdns();

void printNetworkState(const char* reason);

// Kicks off an async WiFi.scanNetworks() -- no-op if a scan is already in
// progress. Non-blocking: results are collected on subsequent
// wifiScanResultJson() calls (the web UI's /wifi-scan/start + poll
// /wifi-scan/result endpoints, see web_ui.cpp), not returned here.
void wifiStartScan();

// {"status":"idle|scanning|failed|done","networks":[{"ssid","rssi","secure"},...]}
// -- networks are de-duplicated by SSID (keeping the strongest RSSI) and
// sorted strongest-first, skipping hidden (empty-SSID) networks. Only
// present when status=="done".
String wifiScanResultJson();

// Persists ssid/password as the station credentials and immediately attempts
// to connect, bypassing connectWifi()'s normal WIFI_RETRY_EVERY_MS throttle.
// Used by the web UI's "Connect" button next to the WLAN scan results
// (POST /wifi-scan/connect, see web_ui.cpp) so picking a scanned network
// actually joins it instead of only pre-filling the SSID field.
void wifiConnectTo(const String& ssid, const String& password);

// Polled by the web UI while a wifiConnectTo() attempt is in flight.
// {"status":"idle|connecting|connected|failed","ip":"..."} -- "ip" present
// only when connected. See wifi_manager.cpp for the exact "failed" cutoff.
String wifiConnectStatusJson();
