#include "target.h"

#include <usb/usb_host.h>

#include <cstring>

#include "../logging.h"

// "usb_host" transport method: this board's ESP32-S2/S3 acts as a genuine
// USB host, enumerating whatever's plugged into the native USB port, claims
// it if it's a USB Printer-class device (bInterfaceClass 0x07), and forwards
// bytes to its bulk OUT endpoint as real USB bulk transfers. Ported from a
// standalone test sketch that validated this approach against a real
// Brother QL-720NW before this was integrated here (device enumeration/
// claim/transfer sequence, and the need for -D ARDUINO_USB_CDC_ON_BOOT=0 in
// platformio.ini -- without it the board's default native-USB-CDC auto-init
// fights usb_host_install() for the same OTG peripheral and bootloops).
// Always compiled in -- usb_host_install()/usb_host_client_register() below
// only ever run when target.cpp's dispatcher calls usbHostTargetSetup(),
// i.e. only when cfg.printerMethod=="usb_host".
//
// Requires a board whose USB-C/USB port circuitry can actually source VBUS
// to a downstream device in host mode -- confirmed for esp32-s2-saola-1 via
// its schematic (Espressif ESP32-S2-SAOLA-1_V1.1_schematics.pdf); some
// compact S2/S3 boards are wired power-in-only and can't (e.g. UM TinyS2,
// ruled out this way before this board was chosen).

static usb_host_client_handle_t clientHandle;
static usb_device_handle_t deviceHandle;

static bool printerInterfaceFound = false;
static bool printerClaimed = false;

static usb_transfer_t* printerOut = nullptr;
static volatile bool transferDone = false;
static volatile bool transferOk = false;
static volatile int transferStatus = 0;  // usb_transfer_status_t of the last completed transfer
// True from a successful submit until its completion callback fires. Normally
// only ever true inside usbHostTargetSend(), but a job that gives up on a
// transfer the endpoint reset couldn't reclaim leaves it set -- the next job
// has to get that transfer back before it can reuse printerOut.
static bool transferInFlight = false;

// Interface number currently claimed via usb_host_interface_claim() (primary
// printer-class path or the fallback path below) -- needed by DEV_GONE
// handling to release it before closing the device. Without that release,
// the Host Library considers the device still in use by this client and
// won't fully free it: usb_host_device_addr_list_fill() keeps reporting the
// same stale address, and any usb_host_device_open() attempt on it fails
// with ESP_ERR_INVALID_STATE (0x103/259) -- permanently, not just for a
// short race window -- until a physical replug or reboot.
static uint8_t claimedInterfaceNumber = 0;

// Fallback path for a bulk-OUT-capable USB device that doesn't identify as a
// USB Printer-class interface (bInterfaceClass 0x07) -- confirmed hardware
// validation of the printer-class scan below only covers a Brother QL-720NW.
// Small integrated label printers (Seiko SLP-series among them) commonly
// ship a vendor-specific class and a proprietary desktop driver instead of
// conforming to the Printer Class spec, so the primary scan may find nothing
// to claim on such a device. This tracks the first bulk-OUT endpoint seen on
// *any* interface while walking the descriptor, so clientEventCallback() can
// fall back to claiming it if the printer-class scan comes up empty.
// Unvalidated against real Seiko hardware -- needs confirmation once a
// device is available; see stikka-esp32/README.md.
static bool fallbackEndpointFound = false;
static uint8_t fallbackInterfaceNumber = 0;
static uint8_t fallbackAltSetting = 0;
static uint8_t fallbackEndpointAddress = 0;
static uint16_t fallbackMaxPacketSize = 0;
static uint8_t currentInterfaceNumber = 0;
static uint8_t currentAltSetting = 0;

// USB-IF Printer class code. Hardcoded rather than relying on a
// USB_CLASS_PRINTER macro -- not confirmed provided by the usb_host headers.
static const uint8_t USB_CLASS_PRINTER_ = 0x07;

static void printerTransferCb(usb_transfer_t* transfer) {
  if (transfer->device_handle != deviceHandle) return;
  const bool isIn = transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
  if (isIn) return;  // this firmware doesn't read the printer's status back
  transferInFlight = false;
  transferStatus = (int)transfer->status;
  transferOk = (transfer->status == USB_TRANSFER_STATUS_COMPLETED);
  transferDone = true;
}

// Walks one USB config descriptor's TLV list looking for a Printer-class
// interface (subclass 1 = USB Printer Class Spec 1.1; protocol 1 unidirectional
// or 2 bidirectional -- 1284.4 bidirectional, protocol 3, isn't handled) and,
// once found, its bulk OUT endpoint.
static void scanConfigDescriptor(const usb_config_desc_t* configDesc) {
  const uint8_t* p = &configDesc->val[0];
  uint8_t bLength;
  bool inPrinterInterface = false;

  for (int i = 0; i < configDesc->wTotalLength; i += bLength, p += bLength) {
    bLength = *p;
    if (i + bLength > configDesc->wTotalLength) break;
    const uint8_t bDescriptorType = *(p + 1);

    if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* intf = reinterpret_cast<const usb_intf_desc_t*>(p);
      currentInterfaceNumber = intf->bInterfaceNumber;
      currentAltSetting = intf->bAlternateSetting;
      dbgPrint("[target] usb_host: interface ");
      dbgPrint(intf->bInterfaceNumber);
      dbgPrint(" class=0x");
      dbgPrint(String(intf->bInterfaceClass, HEX));
      dbgPrint(" subclass=");
      dbgPrint(intf->bInterfaceSubClass);
      dbgPrint(" protocol=");
      dbgPrintln(intf->bInterfaceProtocol, LogLevel::LOG_DEBUG);
      inPrinterInterface = (intf->bInterfaceClass == USB_CLASS_PRINTER_) &&
                           (intf->bInterfaceSubClass == 1) &&
                           (intf->bInterfaceProtocol == 1 || intf->bInterfaceProtocol == 2);
      if (inPrinterInterface && !printerInterfaceFound) {
        printerInterfaceFound = true;
        const esp_err_t err = usb_host_interface_claim(clientHandle, deviceHandle,
            intf->bInterfaceNumber, intf->bAlternateSetting);
        if (err == ESP_OK) {
          printerClaimed = true;
          claimedInterfaceNumber = intf->bInterfaceNumber;
          dbgPrintln("[target] usb_host: printer interface claimed", LogLevel::LOG_INFO);
        } else {
          dbgPrint("[target] usb_host: usb_host_interface_claim failed: ");
          dbgPrintln(err, LogLevel::LOG_ERROR);
        }
      }
    } else if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t* ep = reinterpret_cast<const usb_ep_desc_t*>(p);
      const bool isBulk = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK;
      const bool isOut = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
      if (inPrinterInterface && printerClaimed) {
        if (isBulk && isOut && printerOut == nullptr) {
          const int packetsPerTransfer = 8;
          const esp_err_t err = usb_host_transfer_alloc(ep->wMaxPacketSize * packetsPerTransfer, 0, &printerOut);
          if (err == ESP_OK) {
            printerOut->device_handle = deviceHandle;
            printerOut->bEndpointAddress = ep->bEndpointAddress;
            printerOut->callback = printerTransferCb;
            printerOut->context = nullptr;
            dbgPrint("[target] usb_host: bulk OUT endpoint ready, buffer=");
            dbgPrintln(printerOut->data_buffer_size, LogLevel::LOG_INFO);
          } else {
            dbgPrint("[target] usb_host: usb_host_transfer_alloc failed: ");
            dbgPrintln(err, LogLevel::LOG_ERROR);
          }
        }
      } else if (isBulk && isOut && !fallbackEndpointFound) {
        fallbackEndpointFound = true;
        fallbackInterfaceNumber = currentInterfaceNumber;
        fallbackAltSetting = currentAltSetting;
        fallbackEndpointAddress = ep->bEndpointAddress;
        fallbackMaxPacketSize = ep->wMaxPacketSize;
        dbgPrint("[target] usb_host: fallback bulk-OUT candidate on interface ");
        dbgPrintln(currentInterfaceNumber, LogLevel::LOG_DEBUG);
      }
    }
  }
}

// Opens the device at `address` (if not already open under this client) and
// attempts to claim a USB Printer-class interface on it, same descriptor
// scan/claim/fallback sequence as always ran inline in clientEventCallback's
// USB_HOST_CLIENT_EVENT_NEW_DEV case. Extracted so attemptUsbReconnect() below
// can run the exact same logic against a device this client never got a
// NEW_DEV notification for.
static void openAndClaimDevice(uint8_t address) {
  const esp_err_t openErr = usb_host_device_open(clientHandle, address, &deviceHandle);
  if (openErr != ESP_OK) {
    dbgPrint("[target] usb_host: usb_host_device_open failed: ");
    dbgPrintln(openErr, LogLevel::LOG_ERROR);
    return;
  }

  const usb_config_desc_t* configDesc;
  const esp_err_t descErr = usb_host_get_active_config_descriptor(deviceHandle, &configDesc);
  if (descErr != ESP_OK) {
    dbgPrint("[target] usb_host: usb_host_get_active_config_descriptor failed: ");
    dbgPrintln(descErr, LogLevel::LOG_ERROR);
    return;
  }

  scanConfigDescriptor(configDesc);
  if (!printerInterfaceFound) {
    dbgPrintln("[target] usb_host: no printer-class interface found on this device", LogLevel::LOG_WARN);
    if (fallbackEndpointFound) {
      dbgPrint("[target] usb_host: falling back to first bulk-OUT endpoint on interface ");
      dbgPrint(fallbackInterfaceNumber);
      dbgPrintln(" (unvalidated device path -- not USB Printer-class)", LogLevel::LOG_WARN);
      const esp_err_t claimErr = usb_host_interface_claim(clientHandle, deviceHandle,
          fallbackInterfaceNumber, fallbackAltSetting);
      if (claimErr == ESP_OK) {
        printerClaimed = true;
        printerInterfaceFound = true;
        claimedInterfaceNumber = fallbackInterfaceNumber;
        const int packetsPerTransfer = 8;
        const esp_err_t allocErr = usb_host_transfer_alloc(fallbackMaxPacketSize * packetsPerTransfer, 0, &printerOut);
        if (allocErr == ESP_OK) {
          printerOut->device_handle = deviceHandle;
          printerOut->bEndpointAddress = fallbackEndpointAddress;
          printerOut->callback = printerTransferCb;
          printerOut->context = nullptr;
          dbgPrint("[target] usb_host: fallback bulk OUT endpoint ready, buffer=");
          dbgPrintln(printerOut->data_buffer_size, LogLevel::LOG_INFO);
        } else {
          dbgPrint("[target] usb_host: fallback usb_host_transfer_alloc failed: ");
          dbgPrintln(allocErr, LogLevel::LOG_ERROR);
        }
      } else {
        dbgPrint("[target] usb_host: fallback usb_host_interface_claim failed: ");
        dbgPrintln(claimErr, LogLevel::LOG_ERROR);
      }
    }
  }
}

static void clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg) {
  if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    dbgPrintln("[target] usb_host: device gone", LogLevel::LOG_WARN);
    if (printerOut != nullptr) {
      usb_host_transfer_free(printerOut);
      printerOut = nullptr;
    }
    transferInFlight = false;
    if (deviceHandle != nullptr) {
      // The client contract (usb_host_device_close()'s doc comment) requires
      // releasing any claimed interface before closing the device -- skip
      // this and the Host Library keeps the device object alive waiting for
      // a release that never comes, so it never gets freed even though it's
      // physically gone. That's what was causing the *permanent* (not just a
      // brief race) "usb_host_device_open failed: 259" (ESP_ERR_INVALID_STATE)
      // on every later reconnect attempt at the same address.
      if (printerClaimed) {
        const esp_err_t relErr = usb_host_interface_release(clientHandle, deviceHandle, claimedInterfaceNumber);
        dbgPrint("[target] usb_host: interface release on device-gone: ");
        dbgPrintln(relErr, LogLevel::LOG_WARN);
      }
      const esp_err_t closeErr = usb_host_device_close(clientHandle, deviceHandle);
      dbgPrint("[target] usb_host: device close on device-gone: ");
      dbgPrintln(closeErr, LogLevel::LOG_WARN);
      deviceHandle = nullptr;
    }
    printerClaimed = false;
    printerInterfaceFound = false;
    fallbackEndpointFound = false;
    return;
  }
  if (eventMsg->event != USB_HOST_CLIENT_EVENT_NEW_DEV) return;

  dbgPrint("[target] usb_host: new device address=");
  dbgPrintln(eventMsg->new_dev.address, LogLevel::LOG_INFO);
  openAndClaimDevice(eventMsg->new_dev.address);
}

void usbHostTargetSetup() {
  const usb_host_config_t hostConfig = {
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  const esp_err_t installErr = usb_host_install(&hostConfig);
  dbgPrint("[target] usb_host: usb_host_install: ");
  dbgPrintln(installErr, LogLevel::LOG_INFO);

  const usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {
      .client_event_callback = clientEventCallback,
      .callback_arg = nullptr,
    },
  };
  const esp_err_t clientErr = usb_host_client_register(&clientConfig, &clientHandle);
  dbgPrint("[target] usb_host: usb_host_client_register: ");
  dbgPrintln(clientErr, LogLevel::LOG_INFO);

  // Baseline reading, before WiFi/MQTT/TLS are up -- isolates how much the
  // USB host library itself costs vs. everything else that later competes
  // with it for heap.
  dbgPrint("[target] usb_host: post-init freeHeap=");
  dbgPrint((unsigned long)ESP.getFreeHeap());
  dbgPrint(" maxAlloc=");
  dbgPrintln((unsigned long)ESP.getMaxAllocHeap(), LogLevel::LOG_INFO);
}

void usbHostTargetLoop() {
  uint32_t eventFlags;
  usb_host_lib_handle_events(0, &eventFlags);
  usb_host_client_handle_events(clientHandle, 0);
}

// Blocks (pumping USB events) until the in-flight transfer's completion
// callback fires or the timeout elapses. Returns whether the callback fired;
// transferOk says whether it succeeded. Per the USB host library's own
// contract: never resubmit or modify a transfer before its prior completion
// callback has fired.
static bool waitForTransferDone(unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (!transferDone) {
    usbHostTargetLoop();
    if (millis() - start > timeoutMs) return false;
  }
  return true;
}

// How long one bulk chunk may sit NAKed before it's treated as stuck. A
// printer that's mid-feed or mid-cut legitimately NAKs for a while, so this
// is deliberately generous.
static const unsigned long USB_CHUNK_TIMEOUT_MS = 5000;

// Recovers the bulk OUT endpoint after a submit failure or a stalled/timed-out
// transfer, so usbHostTargetSend() can retry instead of failing the whole job
// (and every job after it, until a physical replug) on a single glitch. Per
// usb_host.h's documented contract: a transfer error implicitly halts the
// endpoint, and it must be flushed then cleared before it'll accept transfers
// again -- usb_host_endpoint_halt() is called too (best-effort) to force that
// halted state in the timeout case, where the transfer's completion callback
// never fired so the library may not have auto-halted it yet.
static void resetUsbEndpoint() {
  if (deviceHandle == nullptr || printerOut == nullptr) return;
  const uint8_t ep = printerOut->bEndpointAddress;
  const esp_err_t haltErr = usb_host_endpoint_halt(deviceHandle, ep);
  const esp_err_t flushErr = usb_host_endpoint_flush(deviceHandle, ep);
  // Pump events before clearing: per usb_host.h, clearing a halt resumes any
  // transfer still queued on the endpoint, so the flushed transfer must have
  // been handed back first or the clear just restarts the stuck transfer.
  for (int i = 0; i < 20 && !transferDone; i++) {
    usbHostTargetLoop();
    delay(5);
  }
  const esp_err_t clearErr = usb_host_endpoint_clear(deviceHandle, ep);
  dbgPrint("[target] usb_host: endpoint reset after transfer error, halt/flush/clear: 0x");
  dbgPrint(String(haltErr, HEX));
  dbgPrint("/0x");
  dbgPrint(String(flushErr, HEX));
  dbgPrint("/0x");
  dbgPrint(String(clearErr, HEX));
  dbgPrint(", transfer returned: ");
  dbgPrintln(transferDone ? "yes" : "no", LogLevel::LOG_WARN);
}

// Called when a send is attempted with no printer currently claimed. Pumps
// pending USB host events first (targetLoop() already does this every
// loop() iteration, but a job can land in the same tick a device reappears)
// and, if that doesn't produce a claimed printer, proactively lists already-
// attached device addresses and tries each one. That second step matters
// because a NEW_DEV client event only fires for devices the Host Library
// enumerates *after* this client registered -- a printer that was already
// plugged in (or that reappeared while this client's event queue was full)
// would otherwise never get (re-)claimed until a physical unplug/replug.
static bool attemptUsbReconnect() {
  for (int i = 0; i < 5 && !printerClaimed; i++) {
    usbHostTargetLoop();
  }
  if (printerClaimed) return true;

  // A device that just went through USB_HOST_CLIENT_EVENT_DEV_GONE (or is
  // mid-bus-reset while re-enumerating) can sit in a transient state for a
  // short window where usb_host_device_addr_list_fill() still reports it but
  // usb_host_device_open() rejects it with ESP_ERR_INVALID_STATE -- observed
  // in practice as "usb_host_device_open failed: 259" immediately after a
  // "device gone" log, i.e. the Host Library hadn't finished tearing the old
  // device object down yet. Retry the whole probe a few times with a short
  // delay (pumping events in between, in case the retry gap is what it takes
  // for a queued NEW_DEV to land) rather than failing on the first race.
  const int maxAttempts = 5;
  const unsigned long retryDelayMs = 150;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      delay(retryDelayMs);
      usbHostTargetLoop();
      if (printerClaimed) return true;
    }

    uint8_t addrList[8];
    int numDevices = 0;
    const esp_err_t fillErr = usb_host_device_addr_list_fill(8, addrList, &numDevices);
    if (fillErr != ESP_OK) {
      dbgPrint("[target] usb_host: reconnect: usb_host_device_addr_list_fill failed: ");
      dbgPrintln(fillErr, LogLevel::LOG_ERROR);
      continue;
    }
    if (numDevices == 0) {
      dbgPrintln("[target] usb_host: reconnect: no attached devices yet", LogLevel::LOG_WARN);
      continue;
    }

    dbgPrint("[target] usb_host: reconnect: probing ");
    dbgPrint(numDevices);
    dbgPrintln(" attached device(s)", LogLevel::LOG_INFO);

    for (int i = 0; i < numDevices && !printerClaimed; i++) {
      openAndClaimDevice(addrList[i]);
    }
    if (printerClaimed) return true;
  }
  dbgPrint("[target] usb_host: reconnect: giving up after ");
  dbgPrint(maxAttempts);
  dbgPrintln(" attempts", LogLevel::LOG_ERROR);
  return false;
}

// The USB device/interface claim persists across jobs (torn down only if the
// device itself disconnects, see USB_HOST_CLIENT_EVENT_DEV_GONE above), so
// there's nothing to release once a send completes -- unlike the network
// target's TCP socket.
bool usbHostTargetSend(const uint8_t* data, size_t len, String& err) {
  if (len == 0) {
    err = "empty payload";
    return false;
  }
  if (!printerClaimed || printerOut == nullptr) {
    if (!attemptUsbReconnect()) {
      err = "no USB printer enumerated yet";
      return false;
    }
  }

  if (transferInFlight) {
    dbgPrintln("[target] usb_host: previous job's transfer still in flight, resetting endpoint", LogLevel::LOG_WARN);
    if (!waitForTransferDone(500)) {
      resetUsbEndpoint();
      if (transferInFlight && !waitForTransferDone(1000)) {
        err = "previous usb transfer still stuck; power-cycle the printer";
        return false;
      }
    }
  }

  size_t sent = 0;
  // One retry per chunk: if a chunk fails, reset the endpoint (halt/flush/
  // clear) and resubmit whatever the device hasn't accepted yet before giving
  // up on the whole job. Reset to 1 after every successful chunk.
  int retriesLeft = 1;
  while (sent < len) {
    if (!printerClaimed || printerOut == nullptr) {
      err = "usb printer disconnected mid-transfer";
      return false;
    }
    const size_t chunk = min(len - sent, (size_t)printerOut->data_buffer_size);
    memcpy(printerOut->data_buffer, data + sent, chunk);
    printerOut->num_bytes = chunk;
    transferDone = false;
    transferOk = false;
    const esp_err_t submitErr = usb_host_transfer_submit(printerOut);
    if (submitErr == ESP_OK) transferInFlight = true;
    if (submitErr == ESP_OK && waitForTransferDone(USB_CHUNK_TIMEOUT_MS) && transferOk) {
      sent += chunk;
      retriesLeft = 1;
      continue;
    }

    const bool submitted = (submitErr == ESP_OK);
    if (!submitted) {
      err = "usb_host_transfer_submit failed: 0x" + String(submitErr, HEX);
    } else if (!transferDone) {
      err = "usb bulk transfer timed out (printer not accepting data)";
    } else {
      err = "usb bulk transfer failed, status=" + String(transferStatus);
    }
    dbgPrint("[target] usb_host: ");
    dbgPrint(err);
    dbgPrint(" at byte ");
    dbgPrint((unsigned long)sent);
    dbgPrint("/");
    dbgPrintln((unsigned long)len, LogLevel::LOG_WARN);

    if (!printerClaimed || printerOut == nullptr) {
      err = "usb printer disconnected mid-transfer";
      return false;
    }
    resetUsbEndpoint();

    if (submitted) {
      // The flush above hands a stuck transfer back through printerTransferCb,
      // but only once events are pumped -- until then the library still marks
      // it in flight and any resubmit fails with ESP_ERR_NOT_FINISHED (0x10c).
      if (!transferDone && !waitForTransferDone(500)) {
        err = "usb transfer stuck in flight after endpoint reset";
        return false;
      }
      // Packets the device ACKed before the stall must not be resent: a
      // duplicated slice of a raster stream corrupts the whole job.
      const size_t accepted = min((size_t)printerOut->actual_num_bytes, chunk);
      if (accepted > 0) {
        dbgPrint("[target] usb_host: device had accepted ");
        dbgPrint((unsigned long)accepted);
        dbgPrintln(" bytes of the failed chunk", LogLevel::LOG_WARN);
      }
      sent += accepted;
    }

    if (retriesLeft-- <= 0) return false;
    dbgPrintln("[target] usb_host: retrying after endpoint reset", LogLevel::LOG_WARN);
  }
  return true;
}

bool usbHostTargetSendString(const String& body, String& err) {
  if (body.length() == 0) {
    err = "empty zpl payload";
    return false;
  }
  return usbHostTargetSend(reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), err);
}
