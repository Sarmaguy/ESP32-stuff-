#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "secrets.h"
#include "led_sync.h"

#define LED_PIN    14
#define NUM_LEDS   122
#define UDP_PORT   LED_SYNC_UDP_PORT

CRGB leds[NUM_LEDS];
WebServer server(80);
WiFiUDP udp;

bool ledOn = false;
CRGB targetColor = CRGB::White;
uint8_t brightness = 128;

unsigned long lastWiFiAttempt = 0;
unsigned long lastHeartbeat = 0;
unsigned long pktCount = 0;
bool netReady = false;

// rate-limit UDP logging: in music mode the master sends 20+ pkt/s
unsigned long lastUdpLog = 0;
unsigned long pktsSinceLog = 0;

void setLEDs() {
  if (ledOn) {
    fill_solid(leds, NUM_LEDS, targetColor);
    FastLED.setBrightness(brightness);
  } else {
    FastLED.clear();
  }
  FastLED.show();
}

void testLEDs() {
  Serial.println("LED test: red 1s");
  fill_solid(leds, NUM_LEDS, CRGB::Red);
  FastLED.setBrightness(64);
  FastLED.show();
  delay(1000);
  Serial.println("LED test: green 1s");
  fill_solid(leds, NUM_LEDS, CRGB::Green);
  FastLED.show();
  delay(1000);
  Serial.println("LED test: blue 1s");
  fill_solid(leds, NUM_LEDS, CRGB::Blue);
  FastLED.show();
  delay(1000);
  FastLED.clear();
  FastLED.show();
  Serial.println("LED test done");
}

void initServer() {
  server.on("/set", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"success\":false}");
      return;
    }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"success\":false}");
      return;
    }
    if (doc.containsKey("on")) ledOn = doc["on"];
    if (doc.containsKey("color")) {
      String c = doc["color"];
      if (c.startsWith("#")) {
        long hex = strtol(c.c_str() + 1, NULL, 16);
        targetColor = CRGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
      }
    }
    if (doc.containsKey("brightness")) {
      brightness = constrain((int)doc["brightness"], 0, 255);
    }
    setLEDs();
    server.send(200, "application/json", "{\"success\":true}");
  });
  server.on("/ping", []() {
    server.send(200, "text/plain", "pong");
  });
  server.on("/status", []() {
    StaticJsonDocument<192> doc;
    doc["on"] = ledOn;
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", targetColor.r, targetColor.g, targetColor.b);
    doc["color"] = hex;
    doc["brightness"] = brightness;
    doc["pkts"] = pktCount;
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000;
    String r;
    serializeJson(doc, r);
    server.send(200, "application/json", r);
  });
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 60; i++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Slave online, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi FAILED - will retry in loop");
  }
}

void initNetworking() {
  if (netReady) return;
  if (WiFi.status() != WL_CONNECTED) return;

  delay(300);

  initServer();
  Serial.println("HTTP server started");

  udp.begin(UDP_PORT);
  Serial.printf("UDP listening on port %d\n", UDP_PORT);

  netReady = true;
}

void processUdp() {
  if (!netReady) return;

  // Drain the whole queue and apply only the newest packet - the master sends
  // 20Hz in music mode (unicast + broadcast can double that), and processing
  // one packet per loop meant rendering stale frames and building up latency.
  bool got = false;
  UdpLedPkt last;
  IPAddress masterIp;
  int packetSize;
  while ((packetSize = udp.parsePacket()) > 0) {
    if (packetSize < (int)sizeof(UdpLedPkt)) continue;
    UdpLedPkt pkt;
    udp.read((uint8_t*)&pkt, sizeof(pkt));
    if (pkt.version != LED_SYNC_VERSION) continue;
    last = pkt;
    masterIp = udp.remoteIP();
    got = true;
    pktCount++;
    pktsSinceLog++;
  }
  if (!got) return;

  // Ack back so the master's slaveOnline[] tracking reflects reality
  UdpAckPkt ack = {LED_SYNC_ACK_VERSION, last.seq};
  udp.beginPacket(masterIp, UDP_PORT);
  udp.write((uint8_t*)&ack, sizeof(ack));
  udp.endPacket();

  CRGB c(last.r, last.g, last.b);
  bool changed = (bool)last.on != ledOn || c != targetColor || last.brightness != brightness;
  ledOn = last.on;
  targetColor = c;
  brightness = last.brightness;
  if (changed) setLEDs(); // skip the ~4ms show() when the state is identical

  if (millis() - lastUdpLog >= 1000) {
    Serial.printf("UDP: %lu pkt(s)/s, seq=%u on=%d RGB=%02X%02X%02X bri=%d\n",
      pktsSinceLog, last.seq, last.on, last.r, last.g, last.b, last.brightness);
    lastUdpLog = millis();
    pktsSinceLog = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("===========================");
  Serial.println("ESP32 SLAVE - LED Follower");
  Serial.println("  UDP + HTTP");
  Serial.println("===========================");
  Serial.printf("LED_PIN=%d NUM_LEDS=%d\n", LED_PIN, NUM_LEDS);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.clear();
  FastLED.show();

  testLEDs();

  startWiFi();
  initNetworking();

  ArduinoOTA.setHostname("esp32-slave");
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([]() { Serial.println("OTA: start"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: done"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA: error %d\n", e);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

void loop() {
  ArduinoOTA.handle();

  if (WiFi.status() == WL_CONNECTED) {
    initNetworking();
    if (netReady) {
      server.handleClient();
      processUdp();
    }
  } else {
    netReady = false;
    unsigned long n = millis();
    if (n - lastWiFiAttempt > 10000) {
      lastWiFiAttempt = n;
      Serial.println("Reconnecting WiFi...");
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
    }
  }

  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    Serial.printf("[HB] WiFi=%s IP=%s net=%s pkts=%u LED=%s\n",
      WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
      WiFi.localIP().toString().c_str(),
      netReady ? "OK" : "INIT",
      pktCount,
      ledOn ? "ON" : "OFF");
  }

  delay(10);
}
