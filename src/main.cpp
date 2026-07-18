#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
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

// MAX9814 electret mic amp - analog OUT. Power MAX9814 from 3.3V (not 5V) so
// its output never exceeds the ESP32 ADC's safe 0-3.3V range.
#define MIC_PIN     34

// MQ135 air quality sensor - analog AOUT. The module needs 5V for its heater,
// so AOUT can swing up to ~5V - that is NOT safe for the ESP32 ADC directly.
// Wire AOUT through a divider (e.g. 10k to AOUT, 20k to GND, tap the node into
// MQ135_PIN) so the pin only ever sees ~2/3 of the sensor's real output.
#define MQ135_PIN   35
#define MQ135_DIVIDER_RATIO 0.667f  // must match the resistor divider you wire

CRGB leds[NUM_LEDS];

bool getLedOn();
void notifySlaves(bool force = false);
void broadcastLedState(bool on, CRGB color, uint8_t bri, bool includeHttp);

WebServer server(80);

enum LedMode { OFF, ON, AUTO, SOUND };
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

// --- MAX9814 microphone (sound-reactive LEDs) ---
uint8_t soundLevel = 0;        // smoothed 0-255 overall level (status/UI)
uint16_t soundPeakToPeak = 0;  // last raw peak-to-peak ADC reading (diagnostic)
unsigned long lastSoundSlaveSync = 0;
const unsigned long soundSlaveSyncInterval = 50; // throttle slave pulse sync to 20Hz
#define SOUND_MIN_BRIGHTNESS 15 // idle glow floor so it's never pitch black between sounds

// Music engine: envelopes are normalized 0..1 with fast attack / slow release.
// Beat detection compares bass energy against its own ~1.5s running average -
// a spike well above that average is a beat (the standard trick WLED-SR and
// most ESP32 visualizer projects use; it self-adjusts to volume and genre).
uint8_t musicFx = 0;           // 0=Pulse (beat flash), 1=Waves (drifting blobs), 2=Spectrum (bass/treble color)
float bassEnv = 0, trebEnv = 0, totalEnv = 0;
float bassLongAvg = 1;         // long-running bass average for beat detection (raw RMS units)
float envMax = 40;             // adaptive normalization ceiling (auto-gain)
float beatPulse = 0;           // 1.0 at each detected beat, decays over ~150ms
bool beatNow = false;          // true only on the frame a beat was detected
unsigned long lastBeatMs = 0;
uint8_t beatHue = 0;           // advances on every beat -> color jumps with the music
uint16_t waveTime = 0;         // animation clock for the Waves effect
CRGB musicColor = CRGB::Black; // smoothed representative color (also sent to slaves)

// --- MQ135 air quality sensor ---
int mq135Raw = 0;          // last averaged raw ADC reading (0-4095, higher = worse air)
float mq135Voltage = 0;    // voltage measured at the ESP32 pin (post-divider)
unsigned long lastAqRead = 0;
const unsigned long aqInterval = 2000;
unsigned long mq135WarmupStart = 0;
bool mq135Ready = false;
const unsigned long MQ135_WARMUP_MS = 3UL * 60UL * 1000UL; // sensor needs a few min to stabilize each power-on

// Raw ADC thresholds - MQ135 output isn't linear/calibrated to real ppm without a
// proper burn-in + clean-air baseline, so this classifies on the raw reading.
// Watch the "aq" serial command after a 24-48h burn-in and tune these to your air.
#define AQ_THRESHOLD_MODERATE  1400
#define AQ_THRESHOLD_POOR      2000
#define AQ_THRESHOLD_HAZARDOUS 2800

bool aqAlertActive = false;
unsigned long lastAqAlertSent = 0;
const unsigned long AQ_ALERT_REPEAT_MS = 30UL * 60UL * 1000UL; // re-remind every 30 min while still poor

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

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);
  mq135WarmupStart = millis();
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

void sampleMic() {
  const int N = 128;
  static float dcEst = 2048; // running estimate of the mic's DC bias point
  static float lp = 0;       // one-pole low-pass state (persists across frames)
  float bassAcc = 0, trebAcc = 0, totAcc = 0;
  uint16_t minV = 4095, maxV = 0;

  // ~128 back-to-back reads take a few ms (effective rate ~20-30kHz). A crude
  // one-pole filter splits the signal: the low-passed part tracks bass, the
  // residual tracks treble - no FFT needed for a two-band ambient effect.
  for (int i = 0; i < N; i++) {
    uint16_t raw = analogRead(MIC_PIN);
    if (raw < minV) minV = raw;
    if (raw > maxV) maxV = raw;
    dcEst += (raw - dcEst) * 0.002f;
    float x = raw - dcEst;
    lp += (x - lp) * 0.06f;
    float hp = x - lp;
    bassAcc += lp * lp;
    trebAcc += hp * hp;
    totAcc  += x * x;
  }
  soundPeakToPeak = maxV - minV;

  float bassRms = sqrtf(bassAcc / N);
  float trebRms = sqrtf(trebAcc / N);
  float totRms  = sqrtf(totAcc / N);

  // Auto-gain: normalize against the loudest recent RMS, decaying slowly so
  // the scale re-tightens in quiet spells. Floor keeps silence from being
  // amplified into full-scale noise.
  if (totRms > envMax) envMax = totRms;
  else envMax *= 0.9995f;
  if (envMax < 40) envMax = 40;

  float bN = constrain(bassRms / envMax, 0.0f, 1.0f);
  float tN = constrain(trebRms / envMax, 0.0f, 1.0f);
  float aN = constrain(totRms  / envMax, 0.0f, 1.0f);
  bassEnv  = bN > bassEnv  ? bN : bassEnv  * 0.85f + bN * 0.15f;
  trebEnv  = tN > trebEnv  ? tN : trebEnv  * 0.85f + tN * 0.15f;
  totalEnv = aN > totalEnv ? aN : totalEnv * 0.85f + aN * 0.15f;
  soundLevel = (uint8_t)(totalEnv * 255.0f);

  // Beat = bass spike well above its own recent average, with a lockout so
  // one kick drum doesn't register as several beats.
  beatNow = false;
  unsigned long now = millis();
  if (bassRms > bassLongAvg * 1.6f && bassRms > 25 && now - lastBeatMs > 160) {
    beatNow = true;
    lastBeatMs = now;
    beatPulse = 1.0f;
    beatHue += 37; // odd step -> cycles the whole wheel without quick repeats
  }
  bassLongAvg += (bassRms - bassLongAvg) * 0.01f;

  beatPulse *= 0.93f;
  if (beatPulse < 0.01f) beatPulse = 0;
}

// All three effects are designed for *indirect* light (LEDs hidden behind
// furniture): no per-pixel meters, only whole-glow color/brightness dynamics
// and broad (tens of LEDs) gradients that read as glow moving along the shelf.
void updateSoundLEDs() {
  uint8_t floorBri = min((uint8_t)SOUND_MIN_BRIGHTNESS, brightness);
  uint8_t range = brightness - floorBri;
  uint8_t bri;

  switch (musicFx) {
    case 1: { // Waves: broad color blobs drift along the strip; music speeds
              // them up, beats surge the whole glow and shift the palette
      waveTime += 1 + (uint16_t)(totalEnv * 14) + (uint16_t)(beatPulse * 10);
      for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t n = inoise8(i * 6, waveTime); // large-scale blobs (~30-40 LEDs wide)
        leds[i] = CHSV(beatHue + (n >> 1), 255, 180 + (n >> 2));
      }
      float lvl = totalEnv * 0.6f + beatPulse * 0.4f;
      bri = floorBri + (uint8_t)(range * (0.35f + 0.65f * lvl));
      musicColor = leds[NUM_LEDS / 2];
      break;
    }
    case 2: { // Spectrum: the room's color IS the music's tone - bass glows
              // red, mids green, treble blue, all blended into one wash
      float midEnv = constrain(totalEnv - bassEnv * 0.5f - trebEnv * 0.5f, 0.0f, 1.0f);
      CRGB target((uint8_t)(bassEnv * 255), (uint8_t)(midEnv * 220), (uint8_t)(trebEnv * 255));
      nblend(musicColor, target, 60);
      fill_solid(leds, NUM_LEDS, musicColor);
      bri = floorBri + (uint8_t)(range * (totalEnv * 0.8f + beatPulse * 0.2f));
      break;
    }
    default: { // Pulse: every beat snaps to a new color and punches the
               // brightness, which then breathes back down until the next hit
      CRGB target = CHSV(beatHue, 240, 255);
      nblend(musicColor, target, beatNow ? 255 : 40);
      fill_gradient(leds, NUM_LEDS, CHSV(beatHue, 240, 255), CHSV(beatHue + 48, 240, 255));
      float lvl = beatPulse > totalEnv * 0.5f ? beatPulse : totalEnv * 0.5f;
      bri = floorBri + (uint8_t)(range * lvl);
      break;
    }
  }

  FastLED.setBrightness(bri);
  FastLED.show();

  unsigned long now = millis();
  if (now - lastSoundSlaveSync >= soundSlaveSyncInterval) {
    lastSoundSlaveSync = now;
    broadcastLedState(true, musicColor, bri, false);
  }
}

void updateLEDs() {
  if (ledMode == SOUND) {
    updateSoundLEDs();
    return;
  }

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
    default:
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
  if (ledMode == SOUND) return true; // always at least a dim idle glow
  return (millis() - lastMotionTime < motionTimeout);
}

void sendNtfyNotification(const String& title, const String& message, const String& priority, const String& tags) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation - acceptable for this hobby use case
  client.setTimeout(5000);

  HTTPClient http;
  http.setConnectTimeout(5000);
  String url = String("https://ntfy.sh/") + ntfyTopic;
  if (!http.begin(client, url)) return;

  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  http.addHeader("Tags", tags);
  int code = http.POST(message);
  Serial.printf("[ntfy] POST %s -> %d\n", url.c_str(), code);
  http.end();
}

void readMQ135() {
  const int SAMPLES = 10;
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(MQ135_PIN);
    delay(2);
  }
  mq135Raw = sum / SAMPLES;
  mq135Voltage = mq135Raw * (3.3f / 4095.0f) / MQ135_DIVIDER_RATIO;
}

const char* airQualityLabel(int raw) {
  if (raw < AQ_THRESHOLD_MODERATE) return "Good";
  if (raw < AQ_THRESHOLD_POOR) return "Moderate";
  if (raw < AQ_THRESHOLD_HAZARDOUS) return "Poor";
  return "Hazardous";
}

uint8_t airQualityPercent(int raw) {
  return (uint8_t)constrain((long)map(raw, 0, 4095, 0, 100), 0L, 100L);
}

void checkAirQualityAlert(int raw) {
  if (!mq135Ready) return;

  bool isPoor = raw >= AQ_THRESHOLD_POOR;
  unsigned long now = millis();

  if (isPoor) {
    if (!aqAlertActive || (now - lastAqAlertSent >= AQ_ALERT_REPEAT_MS)) {
      bool hazardous = raw >= AQ_THRESHOLD_HAZARDOUS;
      String label = hazardous ? "HAZARDOUS" : "POOR";
      String msg = "Air quality is " + label + " (index " + String(airQualityPercent(raw)) + "%). Consider ventilating the room.";
      sendNtfyNotification("Air Quality Alert", msg, hazardous ? "urgent" : "high", "warning");
      aqAlertActive = true;
      lastAqAlertSent = now;
    }
  } else if (aqAlertActive && raw < AQ_THRESHOLD_MODERATE) {
    sendNtfyNotification("Air Quality Normal", "Air quality has returned to normal levels.", "default", "white_check_mark");
    aqAlertActive = false;
  }
}

void sendUdpPkt(IPAddress ip, bool on, CRGB color, uint8_t bri) {
  UdpLedPkt pkt;
  pkt.version = 1;
  pkt.on = on ? 1 : 0;
  pkt.r = color.r;
  pkt.g = color.g;
  pkt.b = color.b;
  pkt.brightness = bri;
  pkt.seq = udpSeq++;

  udp.beginPacket(ip, UDP_PORT);
  udp.write((uint8_t*)&pkt, sizeof(pkt));
  udp.endPacket();
}

// Pushes an explicit LED state to all slaves (UDP unicast + broadcast, optional
// HTTP fallback). Used both by the normal notifySlaves() path and directly by
// sound-reactive mode, which needs to push a fast-changing pulse that doesn't
// fit the single "on/color/brightness" state notifySlaves() tracks.
void broadcastLedState(bool on, CRGB color, uint8_t bri, bool includeHttp) {
  lastSentLedOn = on;
  lastSentColor = color;
  lastSentBrightness = bri;

  // 1. UDP unicast to each configured slave (reliable)
  for (int i = 0; i < NUM_SLAVES; i++) {
    IPAddress ip;
    if (ip.fromString(slaveIPs[i])) {
      sendUdpPkt(ip, on, color, bri);
      slaveOnline[i] = true;
    } else {
      slaveOnline[i] = false;
    }
  }

  // 2. UDP broadcast (auto-discovery, may be blocked by some routers)
  IPAddress bcast = WiFi.broadcastIP();
  if (bcast != INADDR_NONE) sendUdpPkt(bcast, on, color, bri);

  // HTTP — slow fallback, only for periodic sync
  if (!includeHttp) return;

  StaticJsonDocument<128> doc;
  doc["on"] = on;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", color.r, color.g, color.b);
  doc["color"] = hex;
  doc["brightness"] = bri;

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

void notifySlaves(bool force) {
  bool ledOn = getLedOn();
  if (!force && ledOn == lastSentLedOn && targetColor == lastSentColor && brightness == lastSentBrightness) return;

  broadcastLedState(ledOn, targetColor, brightness, force);
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
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;justify-content:center;align-items:center;padding:20px;touch-action:manipulation}
.container{max-width:420px;width:100%;background:#16213e;border-radius:16px;padding:24px;box-shadow:0 8px 32px rgba(0,0,0,0.3)}
h1{text-align:center;color:#0f3460;margin-bottom:20px;font-size:1.4em}
.preview{width:100%;height:36px;border-radius:10px;margin-bottom:16px;transition:background .3s;background:#1a1a2e}
.status-row{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:14px}
.status-row div{flex:1;min-width:80px;padding:8px;background:#1a1a2e;border-radius:8px;font-size:0.8em;text-align:center}
.status-row div span{color:#888;display:block;font-size:0.75em;margin-bottom:2px}
.status-row div b{font-size:1.1em}
.mode-btns{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:16px}
.mode-btns button{flex:1 1 40%;padding:10px;border:none;border-radius:10px;font-size:1em;cursor:pointer;color:#fff;transition:.2s}
.btn-off{background:#e74c3c}
.btn-on{background:#2ecc71}
.btn-auto{background:#3498db}
.btn-sound{background:#9b59b6}
.mode-btns button.active{transform:scale(0.95);box-shadow:inset 0 0 10px rgba(0,0,0,0.4)}
.fx-btns{display:flex;gap:8px}
.fx-btns button{flex:1;padding:9px;border:none;border-radius:10px;font-size:0.9em;cursor:pointer;color:#fff;background:#34495e;transition:.2s}
.fx-btns button.active{background:#9b59b6;transform:scale(0.95);box-shadow:inset 0 0 10px rgba(0,0,0,0.4)}
.aq-good{color:#2ecc71}
.aq-moderate{color:#f1c40f}
.aq-poor{color:#e67e22}
.aq-hazardous{color:#e74c3c}
.ctrl{margin-bottom:16px}
.ctrl label{display:block;margin-bottom:8px;color:#aaa;font-size:0.85em}
.swatches{display:grid;grid-template-columns:repeat(7,1fr);gap:8px;margin-bottom:14px}
.swatches button{aspect-ratio:1;width:100%;border:2px solid rgba(255,255,255,0.15);border-radius:50%;cursor:pointer;padding:0;transition:.15s}
.swatches button.sel{border-color:#fff;transform:scale(1.12);box-shadow:0 0 8px rgba(255,255,255,0.4)}
.slide-row{display:flex;align-items:center;gap:12px;margin-bottom:12px}
.slide-row:last-child{margin-bottom:0}
input[type=range]{flex:1;height:16px;-webkit-appearance:none;appearance:none;background:#0f3460;border-radius:8px;outline:0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:30px;height:30px;border-radius:50%;background:#fff;border:3px solid #e94560;cursor:pointer;box-shadow:0 2px 6px rgba(0,0,0,0.5)}
input[type=range]::-moz-range-thumb{width:24px;height:24px;border-radius:50%;background:#fff;border:3px solid #e94560;cursor:pointer}
#hueSlider{background:linear-gradient(to right,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}
.sl-lab{min-width:34px;color:#888;font-size:0.75em}
.bright-val{min-width:36px;text-align:center;font-weight:700;color:#e94560}
.tout-row{display:flex;align-items:center;gap:8px}
.tout-row input[type=number]{width:90px;padding:10px;border:none;border-radius:8px;background:#1a1a2e;color:#eee;font-size:16px;text-align:center}
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
<div><span>Sound</span><b id="sSound">--</b></div>
<div><span>Air Quality</span><b id="sAq">--</b></div>
</div>
<div class="mode-btns">
<button class="btn-off" id="bOff" onclick="setMode('off')">Off</button>
<button class="btn-on" id="bOn" onclick="setMode('on')">On</button>
<button class="btn-auto" id="bAuto" onclick="setMode('auto')">Auto</button>
<button class="btn-sound" id="bSound" onclick="setMode('sound')">Music</button>
</div>
<div class="ctrl" id="fxCtrl" style="display:none">
<label>Music effect</label>
<div class="fx-btns">
<button id="fx0" onclick="setFx(0)">Pulse</button>
<button id="fx1" onclick="setFx(1)">Waves</button>
<button id="fx2" onclick="setFx(2)">Spectrum</button>
</div>
</div>
<div class="ctrl">
<label>Color</label>
<div class="swatches" id="swatches"></div>
<div class="slide-row"><span class="sl-lab">Hue</span><input type="range" id="hueSlider" min="0" max="360" value="0" oninput="onHS()"></div>
<div class="slide-row"><span class="sl-lab">Vivid</span><input type="range" id="satSlider" min="0" max="100" value="0" oninput="onHS()"></div>
</div>
<div class="ctrl">
<label>Brightness</label>
<div class="slide-row"><input type="range" id="brightSlider" min="1" max="255" value="128" oninput="setBright(+this.value)"><span class="bright-val" id="bVal">128</span></div>
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
let toutTimer,colTimer;

const SW=['#FFB46B','#FFFFFF','#FF0000','#FF6A00','#FFC800','#80FF00','#00FF00','#00FFB0','#00FFFF','#0080FF','#0000FF','#8000FF','#FF00FF','#FF0060'];
const swDiv=document.getElementById('swatches');
SW.forEach(c=>{const b=document.createElement('button');b.style.background=c;b.dataset.c=c;b.onclick=()=>{const[h,s]=hex2hs(c);setHS(h,s);sendColor(c)};swDiv.appendChild(b)});

function hs2hex(h,s){s/=100;const f=n=>{const k=(n+h/60)%6;const v=1-s*Math.max(0,Math.min(k,4-k,1));return Math.round(v*255).toString(16).padStart(2,'0')};return('#'+f(5)+f(3)+f(1)).toUpperCase()}
function hex2hs(x){const r=parseInt(x.substr(1,2),16)/255,g=parseInt(x.substr(3,2),16)/255,b=parseInt(x.substr(5,2),16)/255;const mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;let h=0;if(d){if(mx===r)h=((g-b)/d)%6;else if(mx===g)h=(b-r)/d+2;else h=(r-g)/d+4;h*=60;if(h<0)h+=360}return[Math.round(h),mx?Math.round(d/mx*100):0]}
function setHS(h,s){document.getElementById('hueSlider').value=h;document.getElementById('satSlider').value=s;paintHS(h,s)}
function paintHS(h,s){const full=hs2hex(h,100),cur=hs2hex(h,s);document.getElementById('satSlider').style.background='linear-gradient(to right,#fff,'+full+')';document.getElementById('preview').style.background=cur;document.querySelectorAll('.swatches button').forEach(b=>b.classList.toggle('sel',b.dataset.c===cur))}
function onHS(){const h=+document.getElementById('hueSlider').value,s=+document.getElementById('satSlider').value;paintHS(h,s);clearTimeout(colTimer);colTimer=setTimeout(()=>sendColor(hs2hex(h,s)),120)}
function sendColor(c){fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({color:c})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}

function setMode(m){fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setBright(v){document.getElementById('bVal').textContent=v;clearTimeout(toutTimer);toutTimer=setTimeout(()=>{fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})},150)}
function setTout(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timeout:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setToff(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({toff:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setFx(n){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({musicFx:n})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}

function getStatus(){
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('sLed').textContent=d.ledOn?'ON':'OFF'
document.getElementById('sMode').textContent=d.mode.toUpperCase()
document.getElementById('sMotion').textContent=d.motion?'Yes':'No'
if(typeof d.temp!=='undefined')document.getElementById('sTemp').textContent=d.temp.toFixed(1)
if(typeof d.hum!=='undefined')document.getElementById('sHum').textContent=d.hum.toFixed(1)
if(typeof d.soundLevel!=='undefined')document.getElementById('sSound').textContent=d.soundLevel
if(typeof d.aqPercent!=='undefined'){
const aq=document.getElementById('sAq')
aq.textContent=(d.aqReady?d.aqLabel:'Warming up')+' ('+d.aqPercent+'%)'
aq.className=d.aqReady?('aq-'+d.aqLabel.toLowerCase()):''
}
document.getElementById('preview').style.background=d.ledOn?d.color:'#1a1a2e'
const hs=document.getElementById('hueSlider'),ss=document.getElementById('satSlider')
if(document.activeElement!==hs&&document.activeElement!==ss){const[h,s]=hex2hs(d.color);hs.value=h;ss.value=s;const full=hs2hex(h,100);ss.style.background='linear-gradient(to right,#fff,'+full+')';document.querySelectorAll('.swatches button').forEach(b=>b.classList.toggle('sel',b.dataset.c===d.color.toUpperCase()))}
if(document.activeElement!==document.getElementById('brightSlider')){document.getElementById('brightSlider').value=d.brightness;document.getElementById('bVal').textContent=d.brightness}
if(document.activeElement!==document.getElementById('toutInput'))document.getElementById('toutInput').value=d.timeout
if(document.activeElement!==document.getElementById('toffInput'))document.getElementById('toffInput').value=d.toff
document.querySelectorAll('.mode-btns button').forEach(b=>b.classList.remove('active'))
if(d.mode==='off')document.getElementById('bOff').classList.add('active')
else if(d.mode==='on')document.getElementById('bOn').classList.add('active')
else if(d.mode==='auto')document.getElementById('bAuto').classList.add('active')
else if(d.mode==='sound')document.getElementById('bSound').classList.add('active')
document.getElementById('fxCtrl').style.display=d.mode==='sound'?'':'none'
if(typeof d.musicFx!=='undefined')[0,1,2].forEach(n=>document.getElementById('fx'+n).classList.toggle('active',d.musicFx===n))
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
  StaticJsonDocument<512> doc;

  bool ledOn = getLedOn();
  doc["ledOn"] = ledOn;
  const char* modeStr = "auto";
  if (ledMode == OFF) modeStr = "off";
  else if (ledMode == ON) modeStr = "on";
  else if (ledMode == SOUND) modeStr = "sound";
  doc["mode"] = modeStr;

  // Read OUT pin live for status
  doc["motion"] = digitalRead(LD2410_PIN) == HIGH;
  doc["temp"] = currentTemperature;
  doc["hum"] = currentHumidity;
  doc["toff"] = tempOffset;
  doc["soundLevel"] = soundLevel;
  doc["musicFx"] = musicFx;
  doc["aq"] = mq135Raw;
  doc["aqPercent"] = airQualityPercent(mq135Raw);
  doc["aqLabel"] = airQualityLabel(mq135Raw);
  doc["aqReady"] = mq135Ready;

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
  else if (mode == "sound") ledMode = SOUND;

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
  if (doc.containsKey("musicFx")) {
    musicFx = constrain((int)doc["musicFx"], 0, 2);
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
        Serial.println("  mic        - Read MAX9814 mic level");
        Serial.println("  aq         - Read MQ135 air quality sensor");
        Serial.println("  testnotify - Send a test ntfy.sh notification");
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
        const char* modeName = ledMode == OFF ? "OFF" : (ledMode == ON ? "ON" : (ledMode == SOUND ? "SOUND" : "AUTO"));
        Serial.printf("LED mode: %s, LEDs: %s\n", modeName, getLedOn() ? "ON" : "OFF");
        Serial.printf("Temp: %.1f C, Humidity: %.1f %%\n", currentTemperature, currentHumidity);
        Serial.printf("Sound level: %d/255 (p2p=%d, bass=%.2f treb=%.2f, ceiling=%.0f)\n",
          soundLevel, soundPeakToPeak, bassEnv, trebEnv, envMax);
        Serial.printf("Air quality: raw=%d volt=%.2fV %d%% [%s] %s\n", mq135Raw, mq135Voltage,
          airQualityPercent(mq135Raw), airQualityLabel(mq135Raw), mq135Ready ? "" : "(warming up)");
      } else if (cmdBuffer == "mic") {
        Serial.printf("Mic level: %d/255  peak-to-peak=%d  auto-gain ceiling=%.0f\n", soundLevel, soundPeakToPeak, envMax);
        Serial.printf("Envelopes: bass=%.2f treble=%.2f total=%.2f | beatPulse=%.2f lastBeat=%lums ago\n",
          bassEnv, trebEnv, totalEnv, beatPulse, millis() - lastBeatMs);
        Serial.printf("Music effect: %d (0=Pulse 1=Waves 2=Spectrum)\n", musicFx);
        Serial.println("Make noise near the mic and run this again - level/peak-to-peak should rise.");
      } else if (cmdBuffer == "aq" || cmdBuffer == "air") {
        Serial.printf("MQ135 raw=%d  voltage=%.2fV  index=%d%%  [%s]\n", mq135Raw, mq135Voltage,
          airQualityPercent(mq135Raw), airQualityLabel(mq135Raw));
        if (!mq135Ready) {
          Serial.printf("Still warming up (%lu s remaining)\n", (MQ135_WARMUP_MS - (millis() - mq135WarmupStart)) / 1000);
        }
        Serial.printf("Thresholds (raw): moderate>=%d poor>=%d hazardous>=%d\n", AQ_THRESHOLD_MODERATE, AQ_THRESHOLD_POOR, AQ_THRESHOLD_HAZARDOUS);
      } else if (cmdBuffer == "testnotify") {
        Serial.println("Sending test notification via ntfy.sh...");
        sendNtfyNotification("Test Notification", "This is a test alert from your ESP32.", "default", "bell");
        Serial.println("Sent (check the ntfy app / topic).");
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

  // Sound mode syncs slaves itself (fast pulse); skip the periodic full-state
  // sync then so it doesn't fight with that and briefly flash stale color.
  if (ledMode != SOUND && millis() - lastSlaveSync >= slaveSyncInterval) {
    lastSlaveSync = millis();
    notifySlaves(true);
  }

  processSerialCommands();

  sampleMic();

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

  if (!mq135Ready && m - mq135WarmupStart >= MQ135_WARMUP_MS) {
    mq135Ready = true;
    Serial.println("[MQ135] Warm-up complete, readings now considered valid.");
  }

  if (m - lastAqRead >= aqInterval) {
    lastAqRead = m;
    readMQ135();
    checkAirQualityAlert(mq135Raw);
  }

  updateLEDs();
  delay(10);
}
