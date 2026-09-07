// First-run setup: AP + captive portal to enter WiFi, server URL and token.
// Also accepts the same settings as JSON lines over USB serial at any time:
//   {"ssid":"..","pass":"..","url":"ws://..","token":"..","name":".."}
#pragma once
#include <Arduino.h>

namespace provisioning {
void startPortal();          // blocks until saved (then reboots) or timeout
void pollSerial();           // call from loop(); applies JSON config lines from USB serial
String apName();
}
