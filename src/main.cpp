#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

// drop, never block: monitor open + Mac asleep means CDC never drains and a
// blocking write stalls pod input
#define LOG(...)                                                          \
  do {                                                                    \
    if (!TinyUSBDevice.suspended() && Serial.availableForWrite() >= 64) { \
      Serial.printf("%lu ", (unsigned long)millis());                     \
      Serial.printf(__VA_ARGS__);                                         \
    }                                                                     \
  } while (0)

enum ReportId : uint8_t {
  REPORT_ID_KEYBOARD = 1,
  REPORT_ID_CONSUMER,
};

enum class DialMode : uint8_t {
  Brightness,
  Track,
};

// turn within this after a tap for brightness/track, else mute/play-pause fires
constexpr uint32_t TAP_WINDOW_MS = 500;
// each turn in brightness/track mode extends it by this much
constexpr uint32_t MODE_HOLD_MS = 1000;
// Shift+Option with volume/brightness = macOS quarter steps (64 per range, not 16)
constexpr bool FINE_STEPS = true;

// the pod sends plain media keys for its gestures, e.g. single tap = Mute
constexpr uint8_t TOP_SINGLE = HID_USAGE_CONSUMER_MUTE;
constexpr uint8_t TOP_DOUBLE = HID_USAGE_CONSUMER_PLAY_PAUSE;
constexpr uint8_t TOP_TRIPLE = HID_USAGE_CONSUMER_SCAN_NEXT;
constexpr uint8_t DIAL_RIGHT = HID_USAGE_CONSUMER_VOLUME_INCREMENT;
constexpr uint8_t DIAL_LEFT = HID_USAGE_CONSUMER_VOLUME_DECREMENT;

uint8_t const usbReportDescriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

Adafruit_USBD_HID usbHid(usbReportDescriptor, sizeof(usbReportDescriptor),
                         HID_ITF_PROTOCOL_NONE, 2, false);
BLEClientService hidService(UUID16_SVC_HUMAN_INTERFACE_DEVICE);
BLEClientCharacteristic reports[] = {
    BLEClientCharacteristic(UUID16_CHR_REPORT),
    BLEClientCharacteristic(UUID16_CHR_REPORT),
    BLEClientCharacteristic(UUID16_CHR_REPORT),
    BLEClientCharacteristic(UUID16_CHR_REPORT),
    BLEClientCharacteristic(UUID16_CHR_REPORT),
    BLEClientCharacteristic(UUID16_CHR_REPORT),
};

DialMode dialMode = DialMode::Brightness;
bool consumerHeld = false;
bool keyboardHeld = false;
uint32_t modeDeadline = 0;
uint32_t releaseDeadline = 0;
bool inputPressed = false;
bool modeUsed = false;
ble_gap_addr_t podAddress = {};
bool podIdentified = false;

// keyboard + consumer share one IN endpoint; a back-to-back report waits for
// the host's next poll (2 ms)
bool usbReady() {
  for (uint8_t i = 0; i < 10 && !usbHid.ready(); ++i) delay(1);
  return usbHid.ready();
}

void sendConsumer(uint16_t usage, bool fine = false) {
  // keyboard report is absolute state: also clears modifiers left over from a
  // previous fine step or the lock shortcut
  if (fine || keyboardHeld) {
    if (!usbReady()) return;
    uint8_t noKeys[6] = {};
    usbHid.keyboardReport(
        REPORT_ID_KEYBOARD,
        fine ? KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_LEFTALT : 0,
        noKeys);
    keyboardHeld = fine;
  }
  // bail with modifiers down is fine, loop() releases them
  if (!usbReady()) return;
  LOG("send 0x%02X%s\n", usage, fine ? " fine" : "");
  usbHid.sendReport16(REPORT_ID_CONSUMER, usage);
  consumerHeld = true;
  releaseDeadline = millis() + 5;
}

void lockMac() {
  if (!usbReady()) return;
  LOG("send lock\n");
  uint8_t keys[6] = {HID_KEY_Q};
  usbHid.keyboardReport(REPORT_ID_KEYBOARD,
                        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTGUI,
                        keys);
  keyboardHeld = true;
  releaseDeadline = millis() + 5;
}

void handleInput(uint8_t usage) {
  if (usage == 0) {
    inputPressed = false;
    return;
  }
  if (inputPressed) return;
  inputPressed = true;

  // input that wakes the Mac isn't applied
  if (TinyUSBDevice.suspended()) {
    TinyUSBDevice.remoteWakeup();
    return;
  }

  if (usage == TOP_SINGLE || usage == TOP_DOUBLE) {
    dialMode = usage == TOP_SINGLE ? DialMode::Brightness : DialMode::Track;
    modeDeadline = millis() + TAP_WINDOW_MS;
    modeUsed = false;
    return;
  }
  if (usage == TOP_TRIPLE) {
    modeDeadline = 0;
    lockMac();
    return;
  }
  if (usage != DIAL_RIGHT && usage != DIAL_LEFT) return;

  const bool right = usage == DIAL_RIGHT;
  if (modeDeadline && static_cast<int32_t>(modeDeadline - millis()) > 0) {
    modeUsed = true;
    modeDeadline = millis() + MODE_HOLD_MS;
    if (dialMode == DialMode::Brightness) {
      sendConsumer(right ? HID_USAGE_CONSUMER_BRIGHTNESS_INCREMENT
                         : HID_USAGE_CONSUMER_BRIGHTNESS_DECREMENT,
                   FINE_STEPS);
    } else {
      sendConsumer(right ? HID_USAGE_CONSUMER_SCAN_NEXT
                         : HID_USAGE_CONSUMER_SCAN_PREVIOUS);
    }
    return;
  }

  modeDeadline = 0;
  sendConsumer(right ? HID_USAGE_CONSUMER_VOLUME_INCREMENT
                     : HID_USAGE_CONSUMER_VOLUME_DECREMENT,
               FINE_STEPS);
}

void reportCallback(BLEClientCharacteristic*, uint8_t* data, uint16_t length) {
  if (!length) return;
  LOG("report 0x%02X (%u bytes)\n", data[0], length);
  handleInput(data[0]);
}

void discoverReports(uint16_t connectionHandle) {
  BLEClientCharacteristic* reportPointers[6];
  for (size_t i = 0; i < 6; ++i) reportPointers[i] = &reports[i];

  const uint8_t found = Bluefruit.Discovery.discoverCharacteristic(
      connectionHandle, reportPointers, 6);
  if (!found) {
    LOG("no report characteristics, disconnecting\n");
    Bluefruit.disconnect(connectionHandle);
    return;
  }
  for (size_t i = 0; i < found; ++i) reports[i].enableNotify();
  LOG("ready, %u report characteristics\n", found);
}

void securedCallback(uint16_t connectionHandle) {
  BLEConnection* connection = Bluefruit.Connection(connectionHandle);
  if (!connection->secured()) {
    Bluefruit.Security._authenticate(connectionHandle);
    return;
  }
  LOG("secured\n");
  if (!hidService.discovered() && !hidService.discover(connectionHandle)) {
    LOG("HID service not found, disconnecting\n");
    Bluefruit.disconnect(connectionHandle);
    return;
  }
  discoverReports(connectionHandle);
}

void connectCallback(uint16_t connectionHandle) {
  LOG("connected\n");
  BLEConnection* connection = Bluefruit.Connection(connectionHandle);
  if (!connection->bonded()) {
    Bluefruit.Security._authenticate(connectionHandle);
  } else if (connection->secured()) {
    securedCallback(connectionHandle);
  } else {
    connection->requestPairing();
  }
}

// don't clear pending USB releases here, the key would stay held on the Mac
void disconnectCallback(uint16_t, uint8_t reason) {
  LOG("disconnected, reason 0x%02X\n", reason);
  inputPressed = false;
}

void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t name[32] = {};
  uint8_t nameLength = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, name, sizeof(name) - 1);
  if (!nameLength) {
    nameLength = Bluefruit.Scanner.parseReportByType(
        report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, name, sizeof(name) - 1);
  }
  if (nameLength && strcmp(reinterpret_cast<char*>(name),
                           "Razer Wireless Control Pod") == 0) {
    podAddress = report->peer_addr;
    podIdentified = true;
  }

  const bool isPod = podIdentified &&
      report->peer_addr.addr_type == podAddress.addr_type &&
      memcmp(report->peer_addr.addr, podAddress.addr, sizeof(podAddress.addr)) == 0;
  if (isPod && report->type.connectable &&
      Bluefruit.Scanner.checkReportForUuid(
          report, UUID16_SVC_HUMAN_INTERFACE_DEVICE)) {
    LOG("pod found, connecting\n");
    if (Bluefruit.Central.connect(report)) return;
    LOG("connect failed\n");
  }
  Bluefruit.Scanner.resume();
}

void setup() {
  Serial.begin(115200);
  usbHid.begin();

  Bluefruit.begin(0, 1);
  Bluefruit.setName("XIAO Razer Bridge");
  Bluefruit.Security.setIOCaps(false, false, false);
  Bluefruit.Security.setSecuredCallback(securedCallback);

  hidService.begin();
  for (auto& report : reports) {
    report.setNotifyCallback(reportCallback);
    report.begin();
  }

  Bluefruit.Central.setConnectCallback(connectCallback);
  Bluefruit.Central.setDisconnectCallback(disconnectCallback);
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  // window == interval: scan nonstop so the pod's found fast after idle; USB
  // power makes it free
  Bluefruit.Scanner.setInterval(160, 160);
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);
}

void loop() {
  const uint32_t now = millis();
  // one report per pass, key up before modifiers, so the Mac never sees the
  // key unmodified
  if ((consumerHeld || keyboardHeld) &&
      static_cast<int32_t>(now - releaseDeadline) >= 0 && usbHid.ready()) {
    if (consumerHeld) {
      usbHid.sendReport16(REPORT_ID_CONSUMER, 0);
      consumerHeld = false;
    } else {
      usbHid.keyboardRelease(REPORT_ID_KEYBOARD);
      keyboardHeld = false;
    }
  }

  if (modeDeadline && static_cast<int32_t>(now - modeDeadline) >= 0) {
    if (!modeUsed) {
      sendConsumer(dialMode == DialMode::Brightness
                       ? HID_USAGE_CONSUMER_MUTE
                       : HID_USAGE_CONSUMER_PLAY_PAUSE);
    }
    modeDeadline = 0;
  }
  delay(1);
}
