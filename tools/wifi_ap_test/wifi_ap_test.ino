// Minimal ESP32-C3 softAP test.
//
// Purpose: split "this board cannot radiate an access point at all" from "the HomeKey
// firmware's AP path is wrong". The HomeKey build configures the AP correctly at the
// driver level (mode=AP, correct SSID/channel/auth read back) yet no client can see it,
// so this sketch does the smallest possible version of the same thing with nothing else
// running.
//
// It also scans BEFORE starting the AP. A scan is the one receive-path check that is
// valid on a single-radio device (a station cannot hear its own beacon, but it can hear
// every other access point), so:
//   - a scan that finds the surrounding networks proves the RF receive path works;
//   - a scan that finds nothing while a phone or laptop sees several networks points at
//     the board's RF rather than at any firmware.
//
// The AP is then started with the exact sequence the HomeKey firmware uses - WIFI_OFF,
// a short settle, WIFI_AP, softAP on channel 11 with WPA2-PSK/CCMP - so a difference in
// outcome between this and the app is a difference caused by the app, not by the order
// of the calls.
//
// Build/upload with the same board settings the working PN532 sketch uses:
//   FQBN=esp32:esp32:esp32c3:CDCOnBoot=cdc
//   arduino-cli compile --fqbn "$FQBN" tools/wifi_ap_test
//   arduino-cli upload  -p /dev/cu.usbmodem1201 --fqbn "$FQBN" tools/wifi_ap_test
//
// If APTEST appears, the board is fine and the fault is in the HomeKey AP path.
// If APTEST does not appear, the fault is the board's Wi-Fi (nothing in the firmware
// can fix that).
//
// OUTCOME on the first board tested (ESP32-C3 Super Mini, 48:F6:EE:13:D6:B0):
//   - the scan found 12 networks, so the receive path and the antenna are healthy;
//   - APTEST at the driver's default 20 dBm was invisible to a laptop that had first been
//     validated as a working client: it reports "failed to join" for a nearby network when
//     given a deliberately wrong password, but "could not find" for this one;
//   - the same AP with the transmit power capped to 8 dBm was visible and joinable.
// The same result came from ESP-IDF's own softAP path with none of the Arduino layer
// present, so on that board the limit is the power the board can deliver during a TX
// burst, not any firmware. main/main.cpp caps the setup AP to 8 dBm for that reason;
// raising it back needs a better supply, not a code change.

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate

  Serial.println();
  Serial.println("=== minimal softAP test ===");

  // [1] Receive-path check. Kept inline: the Arduino preprocessor mis-places the
  // auto-generated prototype for a helper declared above setup(), and this is the only
  // function in the sketch.
  Serial.println("[1] station scan (receive path)");
  WiFi.mode(WIFI_STA);
  delay(200);
  const int found = WiFi.scanNetworks();
  Serial.printf("scanNetworks() -> %d\n", found);
  for (int i = 0; i < found; i++) {
    Serial.printf("  #%d ch=%d rssi=%d \"%s\"\n", i, WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.SSID(i).c_str());
  }
  WiFi.scanDelete();

  Serial.println("[2] starting AP");
  WiFi.mode(WIFI_OFF);
  delay(100);
  Serial.printf("mode(WIFI_AP) -> %d\n", static_cast<int>(WiFi.mode(WIFI_AP)));
  const bool ok = WiFi.softAP("APTEST", "testtest123", 11, false, 2, false,
                              WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);

  Serial.printf("softAP=%d ip=%s mac=%s getMode=%d\n", ok,
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(), static_cast<int>(WiFi.getMode()));
  Serial.println(ok ? "Look for SSID \"APTEST\" in your Wi-Fi list."
                    : "softAP() failed.");
}

void loop() {
  // Report state continuously, so a client joining (or the AP silently dropping) is
  // visible in the serial log rather than only at startup.
  static uint32_t last = 0;
  if (millis() - last >= 3000) {
    last = millis();
    Serial.printf("stations=%u mode=%d\n", WiFi.softAPgetStationNum(),
                  static_cast<int>(WiFi.getMode()));
  }
  delay(100);
}
