#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "secrets.h"

#define LED_PIN    14
#define NUM_LEDS   122
#define UDP_PORT   4210

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

struct __attribute__((packed)) UdpLedPkt {
  uint8_t  version;
  uint8_t  on;
  uint8_t  r, g, b;
  uint8_t  brightness;
  uint16_t seq;
};

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

  int packetSize = udp.parsePacket();
  if (packetSize < (int)sizeof(UdpLedPkt)) return;

  UdpLedPkt pkt;
  udp.read((uint8_t*)&pkt, sizeof(pkt));

  if (pkt.version != 1) {
    Serial.printf("UDP: bad version %d\n", pkt.version);
    return;
  }

  pktCount++;
  ledOn = pkt.on;
  targetColor = CRGB(pkt.r, pkt.g, pkt.b);
  brightness = pkt.brightness;
  setLEDs();

  Serial.printf("UDP #%u: seq=%u on=%d RGB=%02X%02X%02X bri=%d\n",
    pktCount, pkt.seq, pkt.on, pkt.r, pkt.g, pkt.b, pkt.brightness);
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
