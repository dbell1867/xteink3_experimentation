// Presentation clicker: the X3 as a Bluetooth LE keyboard (HID over GATT).
// Built on the core's BLE library (NimBLE backend), "Just Works" pairing with
// bonding like a commercial clicker. The radio is only on between start() and
// stop() (skill G35: BLE costs while ON, connected or not).
//
// reveal.js keys: Page Down / Page Up = next / previous step in LINEAR order
// (fragments and vertical sub-slides included; Right/Left skip sub-slides).
#pragma once
#include <Arduino.h>

namespace clicker {

enum class State : uint8_t { Off, Advertising, Connected };

// HID key codes (USB HID Usage Tables, keyboard page)
constexpr uint8_t KEY_NEXT = 0x4E;   // Page Down
constexpr uint8_t KEY_PREV = 0x4B;   // Page Up
constexpr uint8_t KEY_FIRST = 0x4A;  // Home -> reveal.js first slide

void start(const char* name);  // first call builds the BLE stack (~82 KB heap, kept until reboot); advertise
void stop();                   // disconnect + stop advertising: nothing transmitted, nothing can connect
State state();
const char* stateName();

// Press and release one key. Returns false (and sends nothing) if no host is connected.
bool tap(uint8_t keycode);

}  // namespace clicker
