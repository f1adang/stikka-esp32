#pragma once

#include <Arduino.h>

// Strips any mqtt(s)://ws(s):// scheme prefix and trailing path off a
// user-entered broker host string. Shared by connectMqtt() and the web UI's
// /save handler so both accept the same loose input format.
#ifndef STIKKA_SERVER
String normalizeMqttHost(const String& raw);
#endif

#ifndef STIKKA_SERVER
String commandTopic();
String statusTopic();
#else
// stikka-server build: one-line human-readable link state for the web UI
// ("paired as X, connected" / "enrolling..." / last error).
String serverBridgeStatus();
#endif

bool mqttIsConnected();

// Registers the MQTT client callbacks. Call once from setup().
void mqttBridgeSetup();

// Connects (retrying every 5s), subscribes, and services mqtt.loop() plus
// the periodic status publish. Call once per loop() iteration.
void mqttBridgeLoop();

void publishStatus(const char* phase = "ready", const char* lastError = "");

// Applies changed name/broker settings without touching Wi-Fi: drops the
// current MQTT connection (it re-establishes within a few seconds with the new
// client id/topics), or in the server build just pushes a fresh status.
void mqttBridgeReconnect();
