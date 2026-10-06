#include "clicker.h"

#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>

namespace clicker {
namespace {

// Standard keyboard report: modifiers, reserved, 6 key slots (Report ID 1).
const uint8_t REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,  //   Usage Minimum (Left Control)
    0x29, 0xE7,  //   Usage Maximum (Right GUI)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)      -> modifier bits
    0x81, 0x02,  //   Input (Data, Variable, Absolute)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant)      -> reserved byte
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array)   -> up to 6 keys held
    0xC0,        // End Collection
};

struct KeyReport {
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keys[6];
} __attribute__((packed));

BLEServer* server = nullptr;
BLEHIDDevice* hid = nullptr;
BLECharacteristic* input = nullptr;
volatile State st = State::Off;
volatile bool stopping = false;  // stop() in progress: don't re-advertise on disconnect

class ServerCallbacks : public BLEServerCallbacks {
  // Runs in the BLE host task: keep it short (Serial is fine, no SPI/display).
  void onConnect(BLEServer*) override {
    st = State::Connected;
    Serial.printf("[%7lu] clicker: laptop connected\n", (unsigned long)millis());
  }
  void onDisconnect(BLEServer*) override {
    if (stopping) {  // stop() asked for this disconnect: just report it
      st = State::Advertising;
      return;
    }
    Serial.printf("[%7lu] clicker: laptop disconnected, advertising again\n", (unsigned long)millis());
    st = State::Advertising;
    BLEDevice::startAdvertising();  // BLE stops advertising while connected (G34b)
  }
};

void send(const KeyReport& r) {
  input->setValue((uint8_t*)&r, sizeof(r));
  input->notify();
}

bool built = false;  // the stack + HID service, built once per boot

// Build everything once. Tearing it down and rebuilding leaks ~7.6 KB per cycle:
// deinit() deletes the BLEServer, whose (default) destructor never frees its
// services/characteristics/descriptors (measured: free heap after "off" fell
// 128,556 -> 120,948 -> 113,344). deinit(true) is worse: it releases the
// controller's memory for good and the next init crashes. So: build once, and
// "off" = disconnect + stop advertising (nothing transmitted, nothing connects).
void build(const char* name) {
  BLEDevice::init(name);
  BLESecurity* sec = new BLESecurity();
  sec->setCapability(ESP_IO_CAP_NONE);            // no display/keypad pairing: "Just Works"
  sec->setAuthenticationMode(true, false, true);  // bonding, no MITM, secure connections

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  hid = new BLEHIDDevice(server);
  hid->manufacturer()->setValue("xteink3 project");
  hid->pnp(0x02, 0xE502, 0xA111, 0x0210);  // vendor/product/version (arbitrary, USB-IF source)
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
  input = hid->inputReport(1);
  hid->setBatteryLevel(100);
  hid->startServices();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);  // HID Keyboard
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  built = true;
}
}  // namespace

void start(const char* name) {
  if (st != State::Off) return;
  if (!built) build(name);
  st = State::Advertising;
  BLEDevice::startAdvertising();
}

// "Off" without tearing down (see build()): drop the laptop, stop advertising.
void stop() {
  if (st == State::Off) return;
  stopping = true;
  if (st == State::Connected && server) {
    server->disconnect(server->getConnId());
    const uint32_t t0 = millis();
    while (st == State::Connected && millis() - t0 < 1500) delay(10);  // onDisconnect flips it
    Serial.printf("[%7lu] clicker: disconnect %s after %lu ms\n", (unsigned long)millis(),
                  st == State::Connected ? "NOT confirmed" : "confirmed", (unsigned long)(millis() - t0));
  }
  BLEDevice::stopAdvertising();
  st = State::Off;
  stopping = false;
}

State state() { return st; }

const char* stateName() {
  switch (st) {
    case State::Off: return "off";
    case State::Advertising: return "waiting for laptop";
    case State::Connected: return "connected";
  }
  return "?";
}

bool tap(uint8_t keycode) {
  if (st != State::Connected || !input) {
    Serial.printf("[%7lu] clicker: key 0x%02X NOT sent (%s)\n", (unsigned long)millis(), keycode, stateName());
    return false;
  }
  KeyReport r = {};
  r.keys[0] = keycode;
  send(r);   // key down
  delay(15);
  r.keys[0] = 0;
  send(r);   // key up
  Serial.printf("[%7lu] clicker: key 0x%02X sent\n", (unsigned long)millis(), keycode);
  return true;
}

}  // namespace clicker
