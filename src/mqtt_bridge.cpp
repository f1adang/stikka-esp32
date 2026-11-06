#ifndef STIKKA_SERVER  // replaced by server_bridge.cpp in the stikka-server build
#include "mqtt_bridge.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <espMqttClient.h>
#include <mbedtls/base64.h>

#include <new>
#include <utility>

#include "config.h"
#include "logging.h"
#include "status_led.h"
#include "targets/target.h"

// Two concrete client instances (transport is baked into the type, unlike
// PubSubClient's swappable Client&) -- activeMqtt points at whichever one is
// actually in use for the current cfg.mqttUseTls setting. The other instance
// is constructed but never connected. Both run with UseInternalTask::NO so
// every callback (onMqttMessage/onMqttConnect/onMqttDisconnect) still fires
// synchronously from mqttBridgeLoop() on the main Arduino thread, same as
// PubSubClient's mqtt.loop() did -- the rest of this codebase (logging ring
// buffer, target send/stream functions, status LED) was never written to be
// thread-safe, so this avoids introducing a second concurrent caller of any
// of that from an internal FreeRTOS task.
static espMqttClient mqttPlain(espMqttClientTypes::UseInternalTask::NO);
static espMqttClientSecure mqttSecure(espMqttClientTypes::UseInternalTask::NO);
static MqttClient* activeMqtt = nullptr;

// setServer()/setClientId() only store the raw pointer they're given (no
// internal copy), and the connecting/connected state machine keeps reading
// through it long after connectMqtt() returns -- these have to be static
// storage, not connectMqtt()-local Strings, or the pointer goes stale mid-
// handshake. connectMqtt() only reassigns them once activeMqtt is fully
// State::disconnected (see its guard below), so there's no in-flight use at
// the moment they're overwritten.
static String mqttHostNormalized;
static String mqttClientIdStr;

static unsigned long lastMqttAttemptMs = 0;
static unsigned long lastStatusMs = 0;
static int lastMqttFailReason = -1; // last DisconnectReason logged as an error (-1 = none yet), so retries every 5s during an outage don't flood the log ring buffer

// Raw incoming-MQTT-payload reassembly. espMqttClient streams a PUBLISH's
// payload through onMqttMessage() in pieces sized by its small fixed
// internal read buffer (EMC_RX_BUFFER_SIZE, ~1.4KB) regardless of the
// message's total size -- see onMqttMessage() below -- rather than handing
// PubSubClient's old single complete-message callback. This accumulates
// those pieces back into one String before handing off to the exact same
// JSON parsing this file already had.
static String mqttMsgBuffer;
static size_t mqttMsgTotal = 0;

String normalizeMqttHost(const String& raw) {
  String host = raw;
  host.trim();

  if (host.startsWith("mqtt://")) host = host.substring(7);
  if (host.startsWith("mqtts://")) host = host.substring(8);
  if (host.startsWith("ws://")) host = host.substring(5);
  if (host.startsWith("wss://")) host = host.substring(6);

  const int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  return host;
}

String commandTopic() {
  return String("/") + cfg.printerName + "/command/";
}

String statusTopic() {
  return String("/") + cfg.printerName + "/status/";
}

bool mqttIsConnected() {
  return activeMqtt && activeMqtt->connected();
}

static String extractJsonStringField(const String& json, const char* key) {
  const String marker = String("\"") + key + "\":\"";
  const int start = json.indexOf(marker);
  if (start < 0) return "";

  int i = start + marker.length();
  String out;
  // Reserve the whole remaining span up front: escape sequences only ever
  // shrink the decoded length, never grow it, so this is a safe upper
  // bound. Growing this incrementally via out += c in the loop below used
  // to silently truncate large payloads (String::operator+= swallows a
  // failed realloc with no error) once the heap got fragmented enough that
  // no single bigger contiguous block was available -- exactly the kind of
  // thing a 65535-byte MQTT buffer plus WiFi/TLS overhead can cause.
  out.reserve((size_t)(json.length() - i));
  while (i < (int)json.length()) {
    const char c = json[i++];
    if (c == '\\') {
      if (i >= (int)json.length()) break;
      const char esc = json[i++];
      switch (esc) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        default: out += esc; break; // \uXXXX not expected in ZPL/base64 payloads
      }
      continue;
    }
    if (c == '"') {
      return out;
    }
    out += c;
  }

  return "";
}

static bool decodeBase64Payload(const String& in, std::unique_ptr<uint8_t[]>& out, size_t& outLen, String& err) {
  size_t needed = 0;
  int rc = mbedtls_base64_decode(nullptr, 0, &needed,
                                 reinterpret_cast<const unsigned char*>(in.c_str()),
                                 in.length());
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
    err = "invalid base64 payload";
    return false;
  }

  // Plain `new` aborts the whole firmware (no catchable exception, no
  // graceful nullptr) if this allocation fails -- unlike every other
  // allocation in this file (String::reserve(), the nothrow news elsewhere),
  // this one wasn't checked. That silently turned an out-of-memory condition
  // into an unexplained reboot instead of a clean "failed" job status.
  out.reset(new (std::nothrow) uint8_t[needed]);
  if (!out) {
    err = "esp32 out of memory decoding base64 (" + String((unsigned long)needed) +
          " bytes, freeHeap=" + String((unsigned long)ESP.getFreeHeap()) +
          " maxAlloc=" + String((unsigned long)ESP.getMaxAllocHeap()) + ")";
    return false;
  }
  rc = mbedtls_base64_decode(out.get(), needed, &outLen,
                             reinterpret_cast<const unsigned char*>(in.c_str()),
                             in.length());
  if (rc != 0) {
    err = "base64 decode failed";
    return false;
  }

  return true;
}

static String buildStatusJson(const char* phase, const char* lastError) {
  JsonDocument doc;
  doc["printer_name"] = cfg.printerName;
  doc["name"] = cfg.printerName;
  doc["online"] = true;
  doc["busy"] = String(phase) == "printing";
  doc["phase"] = phase;
  doc["type"] = cfg.printerProtocol;
  doc["serial"] = String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["location"] = cfg.location;
  doc["dpi"] = cfg.dpi;
  doc["last_error"] = lastError;

  JsonObject label = doc["label"].to<JsonObject>();
  label["width"] = cfg.labelWidth;
  label["length"] = cfg.labelLength;
  label["isRound"] = false;
  label["verticalOffset"] = 0;
  label["cut"] = false;

  JsonObject capabilities = doc["capabilities"].to<JsonObject>();
  capabilities["type"] = cfg.printerProtocol;
  capabilities["dpi"] = cfg.dpi;
  capabilities["zplCompression"] = cfg.zplCompressionSupported;
  // Brother QL raster protocol knobs -- the frontend now builds the whole
  // raster byte stream itself (zpl-image.ts), so these are reported here for
  // it to consume instead of being used internally by firmware (see the
  // former ql_raster.cpp, removed when this moved client-side).
  capabilities["qlPrintheadPx"] = cfg.qlPrintheadPx;
  capabilities["qlInvalidateBytes"] = cfg.qlInvalidateBytes;
  capabilities["qlAutoCut"] = cfg.qlAutoCut;
  capabilities["qlFeedMarginDots"] = cfg.qlFeedMarginDots;
  capabilities["qlRightMarginDots"] = cfg.qlRightMarginDots;
  // Seiko SLP raster protocol knobs -- same rationale as the ql* ones above.
  capabilities["seikoMaxDots"] = cfg.seikoMaxDots;
  capabilities["seikoDensity"] = cfg.seikoDensity;
  capabilities["seikoSpeed"] = cfg.seikoSpeed;
  JsonObject capLabel = capabilities["label"].to<JsonObject>();
  capLabel["width"] = cfg.labelWidth;
  capLabel["length"] = cfg.labelLength;
  capLabel["isRound"] = false;
  capLabel["verticalOffset"] = 0;
  capLabel["cut"] = false;

  String out;
  serializeJson(doc, out);
  return out;
}

void publishStatus(const char* phase, const char* lastError) {
  if (!activeMqtt || !activeMqtt->connected()) return;
  markLedEvent(LedEventType::tx);
  const String payload = buildStatusJson(phase, lastError);
  dbgPrint("[mqtt] publish status -> ");
  dbgPrintln(statusTopic());
  dbgPrint("[mqtt] payload bytes: ");
  dbgPrintln(payload.length());
  dbgPrint("[mqtt] payload: ");
  dbgPrintln(shortenForLog(payload));
  const uint16_t packetId = activeMqtt->publish(statusTopic().c_str(), 0, true, payload.c_str());
  if (packetId == 0) {
    dbgPrintln("[mqtt] publish status failed (not connected or out of memory)", LogLevel::LOG_WARN);
  }
}

static void publishJobStatus(const char* jobId, const char* status, const char* message) {
  if (!activeMqtt || !activeMqtt->connected()) return;
  markLedEvent(LedEventType::tx);

  JsonDocument doc;
  doc["printer_name"] = cfg.printerName;
  doc["job_id"] = jobId;
  doc["status"] = status;
  doc["message"] = message;

  String payload;
  serializeJson(doc, payload);
  dbgPrint("[mqtt] publish job status -> ");
  dbgPrintln(statusTopic());
  dbgPrint("[mqtt] payload bytes: ");
  dbgPrintln(payload.length());
  dbgPrint("[mqtt] payload: ");
  dbgPrintln(shortenForLog(payload));
  const uint16_t packetId = activeMqtt->publish(statusTopic().c_str(), 0, false, payload.c_str());
  if (packetId == 0) {
    dbgPrintln("[mqtt] publish job status failed (not connected or out of memory)", LogLevel::LOG_WARN);
  }
}

// espMqttClient calls this once per internal-buffer-sized piece of a single
// PUBLISH's payload (see EMC_RX_BUFFER_SIZE, ~1.4KB, independent of the
// message's total size) rather than PubSubClient's old one-shot complete-
// message callback -- index==0 starts a message, index+len==total finishes
// it. This reassembles the raw payload text back into one String (exactly
// what `payload`/`length` used to hand over directly) before falling through
// to the same JSON parsing this file already had.
static void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                           const char* topic, const uint8_t* payload,
                           size_t len, size_t index, size_t total) {
  (void)properties;
  String incomingTopic(topic);
  if (incomingTopic != commandTopic()) return;

  if (index == 0) {
    markLedEvent(LedEventType::rx);
    mqttMsgBuffer = "";
    mqttMsgTotal = 0;
    if (!mqttMsgBuffer.reserve(total + 1)) {
      dbgPrint("[mqtt] out of memory reserving ");
      dbgPrint((unsigned long)(total + 1));
      dbgPrintln(" bytes for incoming message", LogLevel::LOG_ERROR);
      publishJobStatus("", "failed", "esp32 out of memory for incoming message");
      publishStatus("error", "out of memory");
      return;
    }
    mqttMsgTotal = total;
  }

  if (mqttMsgTotal == 0) return; // no message in progress -- reservation above failed

  for (size_t i = 0; i < len; i++) mqttMsgBuffer += (char)payload[i];

  if (index + len < mqttMsgTotal) return; // wait for the rest of this message

  String msg = std::move(mqttMsgBuffer);
  mqttMsgTotal = 0;

  dbgPrint("[mqtt] recv <- ");
  dbgPrintln(incomingTopic);
  dbgPrint("[mqtt] payload bytes: ");
  dbgPrintln(msg.length());
  dbgPrint("[mqtt] payload: ");
  dbgPrintln(shortenForLog(msg));

  // The "payload" field can be tens of KB (a full ZPL/image body). Filter it out of the
  // parse so ArduinoJson never has to duplicate it into the JsonDocument's memory pool --
  // it's pulled out separately below via extractJsonStringField(). Without this filter,
  // deserializeJson() ran out of heap on payloads well under the 65535-byte MQTT ceiling.
  JsonDocument filter;
  filter["job_id"] = true;
  filter["payload_type"] = true;
  filter["payload_encoding"] = true;

  JsonDocument doc;
  const auto err = deserializeJson(doc, msg, DeserializationOption::Filter(filter));
  if (err) {
    dbgPrint("[mqtt] JSON parse error: ");
    dbgPrintln(err.c_str(), LogLevel::LOG_ERROR);
    publishJobStatus("", "failed", err.c_str());
    return;
  }

  const char* jobId = doc["job_id"] | "";
  const char* payloadType = doc["payload_type"] | "";
  const char* payloadEncoding = doc["payload_encoding"] | "";
  // "payload" is filtered out of doc above, so it must come from the raw string.
  // Non-const so it can be std::move()'d below instead of deep-copied -- for large
  // (tens-of-KB) bodies, a second copy on top of msg/body was enough to blow the heap,
  // and Arduino String's operator= silently leaves the destination empty on malloc failure.
  String body = extractJsonStringField(msg, "payload");

  dbgPrint("[mqtt] parsed payload bytes=");
  dbgPrintln(body.length());

  dbgPrint("[mqtt] command type=");
  dbgPrint(payloadType);
  dbgPrint(", encoding=");
  dbgPrintln(payloadEncoding);

  publishStatus("printing", "");
  publishJobStatus(jobId, "accepted", "job accepted");

  if (String(payloadType) == "zpl") {
    if (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") {
      dbgPrintln("[zpl] rejected: this printer is configured for pre-rasterized jobs (" + cfg.printerProtocol + " raster protocol), not raw ZPL", LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "this printer only accepts pre-rasterized jobs, no raw ZPL");
      publishStatus("error", "zpl payload_type not supported by this printer");
      return;
    }
    dbgPrint("[zpl] command received, encoding=");
    dbgPrintln(payloadEncoding);
    String zplBody;
    bool zplIsBase64 = false;
    if (String(payloadEncoding) == "utf8" || String(payloadEncoding) == "base64_utf8") {
      dbgPrintln("[zpl] job start jobId=" + String(jobId), LogLevel::LOG_INFO);
      zplBody = std::move(body);
      zplIsBase64 = String(payloadEncoding) == "base64_utf8";
    } else {
      dbgPrintln("[zpl] unsupported payload_encoding: " + String(payloadEncoding), LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "payload_encoding must be utf8 or base64_utf8 for zpl");
      publishStatus("error", "payload_encoding must be utf8 or base64_utf8 for zpl");
      return;
    }

    String sendErr;
    bool ok = false;
    if (zplIsBase64) {
      dbgPrint("[zpl] decoding base64 bytes=");
      dbgPrintln(zplBody.length());
      std::unique_ptr<uint8_t[]> bytes;
      size_t decodedLen = 0;
      String decodeErr;
      if (!decodeBase64Payload(zplBody, bytes, decodedLen, decodeErr)) {
        dbgPrintln("[zpl] decode failed: " + decodeErr, LogLevel::LOG_ERROR);
        publishJobStatus(jobId, "failed", decodeErr.c_str());
        publishStatus("error", decodeErr.c_str());
        return;
      }
      dbgPrint("[zpl] sending decoded bytes=");
      dbgPrintln(decodedLen);
      dbgPrintln("[zpl] ---- ZPL BODY (head/tail) ----");
      dbgPrintHeadTailBytes(bytes.get(), decodedLen);
      dbgPrintln("[zpl] ---- ZPL BODY END ----");
      ok = targetSend(bytes.get(), decodedLen, sendErr);
    } else {
      dbgPrint("[zpl] sending utf8 bytes=");
      dbgPrintln(zplBody.length());
      dbgPrintln("[zpl] ---- ZPL BODY (head/tail) ----");
      dbgPrintHeadTail(zplBody);
      dbgPrintln("[zpl] ---- ZPL BODY END ----");
      ok = targetSendString(zplBody, sendErr);
    }
    if (!ok) {
      dbgPrint("[zpl] send failed: ");
      dbgPrintln(sendErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", sendErr.c_str());
      publishStatus("error", sendErr.c_str());
      return;
    }

    dbgPrintln("[zpl] job sent to target successfully", LogLevel::LOG_INFO);
    publishJobStatus(jobId, "done", "zpl sent");
    publishStatus("ready", "");
    return;
  }

  if (String(payloadType) == "image") {
    if (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") {
      dbgPrintln("[image] rejected: this printer is configured for " + cfg.printerProtocol + "_raster jobs (frontend pre-rasterizes client-side)", LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "this printer only accepts pre-rasterized jobs (frontend-rasterized), not raw image");
      publishStatus("error", "image payload_type not supported by this printer");
      return;
    }
    dbgPrint("[image] command received, encoding=");
    dbgPrintln(payloadEncoding);
    dbgPrintln("[image] job start jobId=" + String(jobId), LogLevel::LOG_INFO);
    String encoded = std::move(body);
    if (String(payloadEncoding) == "data_url") {
      const int comma = encoded.indexOf(',');
      if (comma < 0) {
        dbgPrintln("[image] invalid data_url payload", LogLevel::LOG_ERROR);
        publishJobStatus(jobId, "failed", "invalid data_url payload");
        publishStatus("error", "invalid data_url payload");
        return;
      }
      encoded = encoded.substring(comma + 1);
    } else if (String(payloadEncoding) != "base64_png") {
      dbgPrintln("[image] unsupported payload_encoding: " + String(payloadEncoding), LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "unsupported image payload_encoding");
      publishStatus("error", "unsupported image payload_encoding");
      return;
    }

    std::unique_ptr<uint8_t[]> bytes;
    size_t decodedLen = 0;
    String decodeErr;
    dbgPrint("[image] decoding base64 bytes=");
    dbgPrintln(encoded.length());
    if (!decodeBase64Payload(encoded, bytes, decodedLen, decodeErr)) {
      dbgPrint("[image] decode failed: ");
      dbgPrintln(decodeErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", decodeErr.c_str());
      publishStatus("error", decodeErr.c_str());
      return;
    }

    dbgPrint("[image] decoded bytes=");
    dbgPrintln(decodedLen);
    String sendErr;
    dbgPrintln("[image] sending decoded image to target");
    if (!targetSend(bytes.get(), decodedLen, sendErr)) {
      dbgPrint("[image] send failed: ");
      dbgPrintln(sendErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", sendErr.c_str());
      publishStatus("error", sendErr.c_str());
      return;
    }

    dbgPrintln("[image] job sent to target successfully", LogLevel::LOG_INFO);
    publishJobStatus(jobId, "done", "image bytes sent");
    publishStatus("ready", "");
    return;
  }

  if (String(payloadType) == "ql_raster") {
    if (cfg.printerProtocol != "ql") {
      dbgPrintln("[ql] rejected: this printer is not configured for ql_raster jobs", LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "this printer does not support ql_raster jobs");
      publishStatus("error", "ql_raster payload_type not supported by this printer");
      return;
    }
    dbgPrint("[ql] command received, encoding=");
    dbgPrintln(payloadEncoding);

    if (String(payloadEncoding) != "base64_bytes") {
      dbgPrintln("[ql] unsupported payload_encoding: " + String(payloadEncoding), LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "unsupported ql_raster payload_encoding");
      publishStatus("error", "unsupported ql_raster payload_encoding");
      return;
    }

    dbgPrintln("[ql] job start jobId=" + String(jobId), LogLevel::LOG_INFO);
    std::unique_ptr<uint8_t[]> bytes;
    size_t decodedLen = 0;
    String decodeErr;
    if (!decodeBase64Payload(body, bytes, decodedLen, decodeErr)) {
      dbgPrintln("[ql] decode failed: " + decodeErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", decodeErr.c_str());
      publishStatus("error", decodeErr.c_str());
      return;
    }

    String sendErr;
    if (!targetSend(bytes.get(), decodedLen, sendErr)) {
      dbgPrintln("[ql] send failed: " + sendErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", sendErr.c_str());
      publishStatus("error", sendErr.c_str());
      return;
    }

    dbgPrintln("[ql] job sent to target successfully", LogLevel::LOG_INFO);
    publishJobStatus(jobId, "done", "ql raster bytes sent");
    publishStatus("ready", "");
    return;
  }

  if (String(payloadType) == "seiko_raster") {
    if (cfg.printerProtocol != "seiko") {
      dbgPrintln("[seiko] rejected: this printer is not configured for seiko_raster jobs", LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "this printer does not support seiko_raster jobs");
      publishStatus("error", "seiko_raster payload_type not supported by this printer");
      return;
    }
    dbgPrint("[seiko] command received, encoding=");
    dbgPrintln(payloadEncoding);

    if (String(payloadEncoding) != "base64_bytes") {
      dbgPrintln("[seiko] unsupported payload_encoding: " + String(payloadEncoding), LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", "unsupported seiko_raster payload_encoding");
      publishStatus("error", "unsupported seiko_raster payload_encoding");
      return;
    }

    dbgPrintln("[seiko] job start jobId=" + String(jobId), LogLevel::LOG_INFO);
    std::unique_ptr<uint8_t[]> bytes;
    size_t decodedLen = 0;
    String decodeErr;
    if (!decodeBase64Payload(body, bytes, decodedLen, decodeErr)) {
      dbgPrintln("[seiko] decode failed: " + decodeErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", decodeErr.c_str());
      publishStatus("error", decodeErr.c_str());
      return;
    }

    String sendErr;
    if (!targetSend(bytes.get(), decodedLen, sendErr)) {
      dbgPrintln("[seiko] send failed: " + sendErr, LogLevel::LOG_ERROR);
      publishJobStatus(jobId, "failed", sendErr.c_str());
      publishStatus("error", sendErr.c_str());
      return;
    }

    dbgPrintln("[seiko] job sent to target successfully", LogLevel::LOG_INFO);
    publishJobStatus(jobId, "done", "seiko raster bytes sent");
    publishStatus("ready", "");
    return;
  }

  dbgPrintln("[mqtt] unsupported payload_type: " + String(payloadType), LogLevel::LOG_ERROR);
  publishJobStatus(jobId, "failed", "unsupported payload_type");
  publishStatus("error", "unsupported payload_type");
}

// Fired once the whole plain/secure CONNACK round-trip completes -- unlike
// PubSubClient's blocking mqtt.connect(), espMqttClient's connect() only
// queues the CONNECT packet and returns immediately, so subscribe()/the
// "ready" status publish have to happen here instead of right after
// connect() (subscribing before the broker has actually acked the
// connection would just silently fail: subscribe() no-ops unless
// _state==connected).
static void onMqttConnect(bool sessionPresent) {
  (void)sessionPresent;
  lastMqttFailReason = -1;
  dbgPrintln("[mqtt] connected", LogLevel::LOG_INFO);
  if (activeMqtt->subscribe(commandTopic().c_str(), 1) != 0) {
    dbgPrint("[mqtt] subscribed to ");
    dbgPrintln(commandTopic(), LogLevel::LOG_INFO);
  } else {
    dbgPrint("[mqtt] subscribe failed for ");
    dbgPrintln(commandTopic(), LogLevel::LOG_ERROR);
  }
  publishStatus("ready", "");
}

static void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
  const int reasonCode = static_cast<int>(reason);
  // Retried every 5s by connectMqtt() -- only log a given failure reason
  // once, otherwise a sustained broker outage floods the ring buffer with
  // identical ERROR lines and evicts everything else within minutes.
  if (reasonCode != lastMqttFailReason) {
    dbgPrint("[mqtt] disconnected, reason=");
    dbgPrintln(espMqttClientTypes::disconnectReasonToString(reason), LogLevel::LOG_ERROR);
    lastMqttFailReason = reasonCode;
  }
}

template <typename T>
static void configureCommonMqttSettings(T& client) {
  client.setServer(mqttHostNormalized.c_str(), cfg.mqttPort);
  client.setClientId(mqttClientIdStr.c_str());
  client.setCleanSession(true);
  if (!cfg.mqttUser.isEmpty()) {
    client.setCredentials(cfg.mqttUser.c_str(), cfg.mqttPassword.c_str());
  }
}

static void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.mqttHost.isEmpty()) return;
  // Covers "already connected" (old mqtt.connected() guard) and "mid-
  // handshake" (connectingTcp1/2, connectingMqtt) -- the latter matters
  // because mqttHostNormalized/mqttClientIdStr below are reassigned here,
  // and the state machine keeps reading through those pointers well past
  // this function returning.
  if (activeMqtt && !activeMqtt->disconnected()) return;

  const unsigned long now = millis();
  if (now - lastMqttAttemptMs < 5000) return;
  lastMqttAttemptMs = now;

  mqttHostNormalized = normalizeMqttHost(cfg.mqttHost);
  if (mqttHostNormalized.isEmpty()) return;
  mqttClientIdStr = cfg.printerName + "-bridge";

  dbgPrint("[mqtt] connecting to ");
  dbgPrint(mqttHostNormalized);
  dbgPrint(":");
  dbgPrint(cfg.mqttPort);
  dbgPrint(" tls=");
  dbgPrintln(cfg.mqttUseTls ? "on" : "off", LogLevel::LOG_INFO);

  if (cfg.mqttUseTls) {
    configureCommonMqttSettings(mqttSecure);
    if (cfg.mqttTlsInsecure || cfg.mqttCaCert.isEmpty()) {
      mqttSecure.setInsecure();
      dbgPrintln("[mqtt] tls insecure mode enabled", LogLevel::LOG_WARN);
    } else {
      mqttSecure.setCACert(cfg.mqttCaCert.c_str());
      dbgPrintln("[mqtt] tls CA certificate configured", LogLevel::LOG_INFO);
    }
    activeMqtt = &mqttSecure;
  } else {
    configureCommonMqttSettings(mqttPlain);
    activeMqtt = &mqttPlain;
  }

  if (!activeMqtt->connect()) {
    dbgPrintln("[mqtt] connect() failed to queue CONNECT packet (out of memory)", LogLevel::LOG_ERROR);
    return;
  }
  dbgPrintln("[mqtt] connecting...", LogLevel::LOG_INFO);
}

void mqttBridgeLoop() {
  connectMqtt();
  // Both clients need pumping every iteration regardless of which is
  // "active" -- the inactive one only ever sits idle in State::disconnected,
  // but the active one's whole state machine (including noticing a dropped
  // TCP connection and transitioning back to disconnected so connectMqtt()
  // can retry) is driven exclusively by these loop() calls.
  mqttPlain.loop();
  mqttSecure.loop();
  if (!activeMqtt || !activeMqtt->connected()) return;

  const unsigned long now = millis();
  const unsigned long intervalMs = (unsigned long)cfg.statusIntervalSec * 1000UL;
  if (now - lastStatusMs > intervalMs) {
    publishStatus("ready", "");
    lastStatusMs = now;
  }
}

void mqttBridgeSetup() {
  mqttPlain.onConnect(onMqttConnect);
  mqttPlain.onDisconnect(onMqttDisconnect);
  mqttPlain.onMessage(onMqttMessage);
  mqttSecure.onConnect(onMqttConnect);
  mqttSecure.onDisconnect(onMqttDisconnect);
  mqttSecure.onMessage(onMqttMessage);
}

#endif  // !STIKKA_SERVER
