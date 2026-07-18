#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ArduinoOTA.h>
#include "secrets.h"

// HLK-LD2410B OUT pin (open-drain, LOW=no one, HIGH=presence)
#define LD2410_PIN  32
#define LED_PIN     14
#define NUM_LEDS    100
#define DHTPIN      4
#define DHTTYPE     DHT22

CRGB leds[NUM_LEDS];

bool getLedOn();
void notifySlaves(bool force = false);

WebServer server(80);

enum LedMode { OFF, ON, AUTO };
LedMode ledMode = AUTO;

CRGB targetColor = CRGB::White;
uint8_t brightness = 128;

DHT dht(DHTPIN, DHTTYPE);

float currentTemperature = 0;
float currentHumidity = 0;
float tempOffset = -2.0;
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 10000;

bool motionDetected = false;
unsigned long lastMotionTime = 0;
unsigned long motionTimeout = 30000;

// --- Slave configuration ---
#define NUM_SLAVES 2
const char* slaveIPs[NUM_SLAVES] = {"192.168.1.101", "192.168.1.102"};
const int slavePort = 80;
bool slaveOnline[NUM_SLAVES] = {false, false};
unsigned long lastSlaveSync = 0;
const unsigned long slaveSyncInterval = 3000;

// Cache last sent state to avoid flooding slaves
bool lastSentLedOn = false;
CRGB lastSentColor = CRGB::Black;
uint8_t lastSentBrightness = 0;

// --- UDP broadcast (fast path for real-time sync) ---
#define UDP_PORT 4210
WiFiUDP udp;
uint16_t udpSeq = 0;

struct __attribute__((packed)) UdpLedPkt {
  uint8_t  version;    // = 1
  uint8_t  on;         // 1=on, 0=off
  uint8_t  r, g, b;
  uint8_t  brightness;
  uint16_t seq;
};

void setupPins() {
  pinMode(LD2410_PIN, INPUT_PULLUP);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.clear();
  FastLED.show();
  dht.begin();
}

void readMotionSensor() {
  bool reading = digitalRead(LD2410_PIN);

  if (reading == HIGH) {
    if (!motionDetected) {
      Serial.println("[LD2410] Presence detected!");
    }
    motionDetected = true;
    lastMotionTime = millis();
  } else {
    if (motionDetected) {
      Serial.println("[LD2410] No presence");
    }
    motionDetected = false;
  }
}

void updateLEDs() {
  bool shouldBeOn = false;

  switch (ledMode) {
    case ON:
      shouldBeOn = true;
      break;
    case OFF:
      shouldBeOn = false;
      break;
    case AUTO:
      shouldBeOn = (millis() - lastMotionTime < motionTimeout);
      break;
  }

  if (shouldBeOn) {
    fill_solid(leds, NUM_LEDS, targetColor);
    FastLED.setBrightness(brightness);
  } else {
    FastLED.clear();
  }

  FastLED.show();

  notifySlaves();
}

bool getLedOn() {
  if (ledMode == ON) return true;
  if (ledMode == OFF) return false;
  return (millis() - lastMotionTime < motionTimeout);
}

void sendUdpPkt(IPAddress ip) {
  bool ledOn = getLedOn();
  UdpLedPkt pkt;
  pkt.version = 1;
  pkt.on = ledOn ? 1 : 0;
  pkt.r = targetColor.r;
  pkt.g = targetColor.g;
  pkt.b = targetColor.b;
  pkt.brightness = brightness;
  pkt.seq = udpSeq++;

  udp.beginPacket(ip, UDP_PORT);
  udp.write((uint8_t*)&pkt, sizeof(pkt));
  udp.endPacket();
}

void notifySlaves(bool force) {
  bool ledOn = getLedOn();
  if (!force && ledOn == lastSentLedOn && targetColor == lastSentColor && brightness == lastSentBrightness) return;

  lastSentLedOn = ledOn;
  lastSentColor = targetColor;
  lastSentBrightness = brightness;

  // 1. UDP unicast to each configured slave (reliable)
  for (int i = 0; i < NUM_SLAVES; i++) {
    IPAddress ip;
    if (ip.fromString(slaveIPs[i])) {
      sendUdpPkt(ip);
      slaveOnline[i] = true;
    } else {
      slaveOnline[i] = false;
    }
  }

  // 2. UDP broadcast (auto-discovery, may be blocked by some routers)
  IPAddress bcast = WiFi.broadcastIP();
  if (bcast != INADDR_NONE) sendUdpPkt(bcast);

  // HTTP — slow fallback, only for periodic sync
  if (!force) return;

  StaticJsonDocument<128> doc;
  doc["on"] = ledOn;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", targetColor.r, targetColor.g, targetColor.b);
  doc["color"] = hex;
  doc["brightness"] = brightness;

  String payload;
  serializeJson(doc, payload);

  for (int i = 0; i < NUM_SLAVES; i++) {
    WiFiClient client;
    if (client.connect(slaveIPs[i], slavePort, 500)) {
      client.printf("POST /set HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
        slaveIPs[i], payload.length(), payload.c_str());
      client.flush();
      client.stop();
    }
  }
}

const char MAIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ESP32 LED Controller</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;justify-content:center;align-items:center;padding:20px}
.container{max-width:420px;width:100%;background:#16213e;border-radius:16px;padding:24px;box-shadow:0 8px 32px rgba(0,0,0,0.3)}
h1{text-align:center;color:#0f3460;margin-bottom:20px;font-size:1.4em}
.preview{width:100%;height:36px;border-radius:10px;margin-bottom:16px;transition:background .3s;background:#1a1a2e}
.status-row{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:14px}
.status-row div{flex:1;min-width:80px;padding:8px;background:#1a1a2e;border-radius:8px;font-size:0.8em;text-align:center}
.status-row div span{color:#888;display:block;font-size:0.75em;margin-bottom:2px}
.status-row div b{font-size:1.1em}
.mode-btns{display:flex;gap:8px;margin-bottom:16px}
.mode-btns button{flex:1;padding:10px;border:none;border-radius:10px;font-size:1em;cursor:pointer;color:#fff;transition:.2s}
.btn-off{background:#e74c3c}
.btn-on{background:#2ecc71}
.btn-auto{background:#3498db}
.mode-btns button.active{transform:scale(0.95);box-shadow:inset 0 0 10px rgba(0,0,0,0.4)}
.ctrl{margin-bottom:14px}
.ctrl label{display:block;margin-bottom:6px;color:#aaa;font-size:0.85em}
.row{display:flex;align-items:center;gap:12px}
.row input[type=color]{width:54px;height:54px;border:none;border-radius:10px;cursor:pointer;background:0 0;padding:0}
.row input[type=range]{flex:1;height:6px;-webkit-appearance:none;appearance:none;background:#0f3460;border-radius:3px;outline:0}
.row input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:22px;height:22px;border-radius:50%;background:#e94560;cursor:pointer}
.bright-val{min-width:36px;text-align:center;font-weight:700;color:#e94560}
.tout-row{display:flex;align-items:center;gap:8px}
.tout-row input[type=number]{width:80px;padding:8px;border:none;border-radius:8px;background:#1a1a2e;color:#eee;font-size:1em;text-align:center}
.tout-row span{color:#aaa}
</style>
</head>
<body>
<div class="container">
<h1>LED Controller</h1>
<div class="preview" id="preview"></div>
<div class="status-row">
<div><span>LED</span><b id="sLed">...</b></div>
<div><span>Mode</span><b id="sMode">...</b></div>
<div><span>Presence</span><b id="sMotion">...</b></div>
<div><span>Temp</span><b id="sTemp">--.-</b></div>
<div><span>Humidity</span><b id="sHum">--.-</b></div>
</div>
<div class="mode-btns">
<button class="btn-off" id="bOff" onclick="setMode('off')">Off</button>
<button class="btn-on" id="bOn" onclick="setMode('on')">On</button>
<button class="btn-auto" id="bAuto" onclick="setMode('auto')">Auto</button>
</div>
<div class="ctrl">
<label>Color</label>
<div class="row">
<input type="color" id="colorPick" value="#ffffff" oninput="setColor(this.value)">
<input type="range" id="brightSlider" min="1" max="255" value="128" oninput="setBright(+this.value)">
<span class="bright-val" id="bVal">128</span>
</div>
</div>
<div class="ctrl">
<label>Motion timeout (seconds)</label>
<div class="tout-row">
<input type="number" id="toutInput" value="30" min="1" max="3600" onchange="setTout(+this.value)">
<span>sec</span>
</div>
</div>
<div class="ctrl">
<label>Temp offset (°C)</label>
<div class="tout-row">
<input type="number" id="toffInput" value="-2" step="0.5" min="-10" max="10" onchange="setToff(+this.value)">
<span>°C</span>
</div>
</div>
</div>
<script>
let toutTimer;

function setMode(m){fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setColor(c){fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({color:c})}).then(r=>r.json()).then(d=>{if(d.success){document.getElementById('preview').style.background=c;getStatus()}})}
function setBright(v){document.getElementById('bVal').textContent=v;clearTimeout(toutTimer);toutTimer=setTimeout(()=>{fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})},150)}
function setTout(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timeout:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setToff(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({toff:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}

function getStatus(){
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('sLed').textContent=d.ledOn?'ON':'OFF'
document.getElementById('sMode').textContent=d.mode.toUpperCase()
document.getElementById('sMotion').textContent=d.motion?'Yes':'No'
if(typeof d.temp!=='undefined')document.getElementById('sTemp').textContent=d.temp.toFixed(1)
if(typeof d.hum!=='undefined')document.getElementById('sHum').textContent=d.hum.toFixed(1)
document.getElementById('preview').style.background=d.ledOn?d.color:'#1a1a2e'
if(document.activeElement!==document.getElementById('colorPick'))document.getElementById('colorPick').value=d.color
if(document.activeElement!==document.getElementById('brightSlider')){document.getElementById('brightSlider').value=d.brightness;document.getElementById('bVal').textContent=d.brightness}
if(document.activeElement!==document.getElementById('toutInput'))document.getElementById('toutInput').value=d.timeout
if(document.activeElement!==document.getElementById('toffInput'))document.getElementById('toffInput').value=d.toff
document.querySelectorAll('.mode-btns button').forEach(b=>b.classList.remove('active'))
if(d.mode==='off')document.getElementById('bOff').classList.add('active')
else if(d.mode==='on')document.getElementById('bOn').classList.add('active')
else if(d.mode==='auto')document.getElementById('bAuto').classList.add('active')
}).catch(()=>{})
}

getStatus()
setInterval(getStatus,2000)
</script>
</body>
</html>
)=====";

void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}

void handleStatus() {
  StaticJsonDocument<256> doc;

  bool ledOn = getLedOn();
  doc["ledOn"] = ledOn;
  doc["mode"] = ledMode == OFF ? "off" : (ledMode == ON ? "on" : "auto");

  // Read OUT pin live for status
  doc["motion"] = digitalRead(LD2410_PIN) == HIGH;
  doc["temp"] = currentTemperature;
  doc["hum"] = currentHumidity;
  doc["toff"] = tempOffset;

  char hexColor[8];
  snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", targetColor.r, targetColor.g, targetColor.b);
  doc["color"] = hexColor;
  doc["brightness"] = brightness;
  doc["timeout"] = motionTimeout / 1000;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetMode() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  String mode = doc["mode"];
  if (mode == "off") ledMode = OFF;
  else if (mode == "on") ledMode = ON;
  else if (mode == "auto") ledMode = AUTO;

  server.send(200, "application/json", "{\"success\":true}");
}

void handleSetLed() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  if (doc.containsKey("color")) {
    String c = doc["color"];
    if (c.startsWith("#")) {
      long hex = strtol(c.c_str() + 1, NULL, 16);
      targetColor = CRGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
    }
  }
  if (doc.containsKey("brightness")) {
    brightness = constrain((int)doc["brightness"], 0, 255);
    FastLED.setBrightness(brightness);
  }

  server.send(200, "application/json", "{\"success\":true}");
}

void handleSetSettings() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  if (doc.containsKey("timeout")) {
    motionTimeout = (unsigned long)doc["timeout"] * 1000;
  }
  if (doc.containsKey("toff")) {
    tempOffset = doc["toff"];
  }

  server.send(200, "application/json", "{\"success\":true}");
}

void handleNotFound() {
  String msg = "File Not Found\nURI: " + server.uri();
  server.send(404, "text/plain", msg);
}

unsigned long lastWiFiAttempt = 0;
unsigned long lastSerialPrint = 0;
const unsigned long serialPrintInterval = 2000;

void setupServer() {
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/mode", HTTP_POST, handleSetMode);
  server.on("/api/led", HTTP_POST, handleSetLed);
  server.on("/api/settings", HTTP_POST, handleSetSettings);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Server started");
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
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed, will retry in loop");
  }
}

// ============================================================
// DIAGNOSTIC SERIAL COMMANDS
// ============================================================
void processSerialCommands() {
  static String cmdBuffer;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuffer.length() == 0) continue;
      cmdBuffer.trim();
      cmdBuffer.toLowerCase();

      if (cmdBuffer == "help" || cmdBuffer == "h") {
        Serial.println("\n=== MASTER Commands ===");
        Serial.println("  help, h    - Show this help");
        Serial.println("  sensor, s  - Read LD2410 OUT pin");
        Serial.println("  pin        - Read raw pin voltage state");
        Serial.println("  blink      - Blink LED strip 3x (test LEDs)");
        Serial.println("  slaves     - Show slave connection status");
        Serial.println("  status     - Show all status");
        Serial.println("=================================\n");
      } else if (cmdBuffer == "sensor" || cmdBuffer == "s") {
        int val = digitalRead(LD2410_PIN);
        Serial.printf("LD2410B OUT pin (GPIO%d): %s\n", LD2410_PIN, val == HIGH ? "HIGH - presence detected" : "LOW - no presence");
        Serial.printf("Internal pull-up: enabled\n");
        Serial.println("Wave hand in front of sensor - value should change.");
      } else if (cmdBuffer == "pin") {
        int val = digitalRead(LD2410_PIN);
        Serial.printf("GPIO%d = %d (%s)\n", LD2410_PIN, val, val == HIGH ? "HIGH" : "LOW");
      } else if (cmdBuffer == "blink") {
        Serial.println("Blinking LEDs 3x...");
        for (int i = 0; i < 3; i++) {
          fill_solid(leds, NUM_LEDS, CRGB::White);
          FastLED.setBrightness(64);
          FastLED.show();
          delay(300);
          FastLED.clear();
          FastLED.show();
          delay(200);
        }
        Serial.println("LED blink test done.");
      } else if (cmdBuffer == "slaves") {
        for (int i = 0; i < NUM_SLAVES; i++) {
          Serial.printf("Slave %d (%s): %s\n", i + 1, slaveIPs[i],
            slaveOnline[i] ? "ONLINE" : "OFFLINE");
        }
      } else if (cmdBuffer == "status") {
        int val = digitalRead(LD2410_PIN);
        Serial.printf("OUT pin (GPIO%d): %s\n", LD2410_PIN, val == HIGH ? "PRESENCE" : "NONE");
        Serial.printf("motionDetected: %s\n", motionDetected ? "true" : "false");
        Serial.printf("LED mode: %s, LEDs: %s\n",
          ledMode == OFF ? "OFF" : (ledMode == ON ? "ON" : "AUTO"),
          getLedOn() ? "ON" : "OFF");
        Serial.printf("Temp: %.1f C, Humidity: %.1f %%\n", currentTemperature, currentHumidity);
      } else {
        Serial.printf("Unknown '%s'. Type 'help'.\n", cmdBuffer.c_str());
      }

      cmdBuffer = "";
    } else {
      cmdBuffer += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println("ESP32 MASTER - LED Controller + LD2410B");
  Serial.println("============================================");
  Serial.println();
  Serial.printf("Slaves: %d configured\n", NUM_SLAVES);
  for (int i = 0; i < NUM_SLAVES; i++) Serial.printf("  Slave %d: %s\n", i + 1, slaveIPs[i]);
  Serial.println();
  Serial.println("Type 'help' for diagnostic commands.");
  Serial.println();

  setupPins();
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  setupServer();
  startWiFi();

  ArduinoOTA.setHostname("esp32-master");
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
    server.handleClient();
  } else {
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

  readMotionSensor();

  if (millis() - lastSerialPrint >= serialPrintInterval) {
    lastSerialPrint = millis();
    int val = digitalRead(LD2410_PIN);
    Serial.printf("[%lu] OUT=%d %s | LED=%s | S1=%d S2=%d\n",
      millis(), val, val == HIGH ? "PRESENCE" : "NONE",
      getLedOn() ? "ON" : "OFF",
      slaveOnline[0], slaveOnline[1]);
  }

  if (millis() - lastSlaveSync >= slaveSyncInterval) {
    lastSlaveSync = millis();
    notifySlaves(true);
  }

  processSerialCommands();

  unsigned long m = millis();
  if (m - lastSensorRead >= sensorInterval) {
    lastSensorRead = m;
    float tSum = 0, hSum = 0;
    int valid = 0;
    for (int i = 0; i < 3; i++) {
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      if (!isnan(t) && !isnan(h) && t > 0 && t < 50 && h > 0 && h <= 100) {
        tSum += t;
        hSum += h;
        valid++;
      }
      delay(100);
    }
    if (valid > 0) {
      currentTemperature = tSum / valid + tempOffset;
      currentHumidity = hSum / valid;
    }
  }

  updateLEDs();
  delay(10);
}
