// ESP32-S3: minimal WiFi AP + status page.
// Proves the toolchain before touching camera or UART.

#include <WiFi.h>
#include <WebServer.h>

const char *AP_SSID = "CARLAB";
const char *AP_PASS = "robotcar";      // must be 8+ chars
const uint8_t AP_CHANNEL = 6;

WebServer server(80);

void handleRoot() {
  String html = "<html><body style='font-family:sans-serif'>";
  html += "<h2>carlab esp32-s3</h2>";
  html += "<p>uptime: " + String(millis() / 1000) + "s</p>";
  html += "<p>free heap: " + String(ESP.getFreeHeap()) + "</p>";
  html += "<p>psram: " + String(ESP.getPsramSize()) + "</p>";
  html += "<p>clients: " + String(WiFi.softAPgetStationNum()) + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nbooting");

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  Serial.printf("softAP %s -> %s\n", AP_SSID, ok ? "up" : "FAILED");
  Serial.print("ip: ");
  Serial.println(WiFi.softAPIP());
  Serial.printf("psram: %u bytes\n", ESP.getPsramSize());

  server.on("/", handleRoot);
  server.begin();
  Serial.println("http server on :80");
}

void loop() {
  server.handleClient();
  static uint32_t t = 0;
  if (millis() - t > 1000) {
    t = millis();
    Serial.printf("alive %lus  ap=%s  clients=%u  heap=%u\n",
                  millis()/1000, WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum(), ESP.getFreeHeap());
  }
}
