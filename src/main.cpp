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
#include <Preferences.h>
#include <time.h>
#include <esp_wifi.h>
#include <algorithm>
#include "secrets.h"
#include "led_sync.h"
#include "node_health.h"

// HLK-LD2410B OUT pin (open-drain, LOW=no one, HIGH=presence)
// Its built-in Bluetooth must stay OFF (done 2026-10-29). It sits a few cm from
// this ESP's antenna, and its 2.4 GHz bursts were destroying full-size Wi-Fi
// frames: web page and OTA stalled while small packets still got through.
// See CLAUDE.md "Hardware documentation" for how to turn it off again.
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
void syncStrips(bool force, bool includeHttp = false);
uint8_t musicBriFor(int strip);
void sanitizeAqThresholds();

WebServer server(80);

enum LedMode { OFF, ON, AUTO, SOUND };
LedMode ledMode = AUTO;

// Per-strip color/brightness live in stripColor[]/stripBri[] further down -
// they need NUM_SLAVES, which isn't defined until the slave config block.

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
float musicLevel = 0;          // 0..1 glow level for the current frame; each strip scales its own brightness by this

// --- MQ135 air quality sensor ---
int mq135Raw = 0;          // last averaged raw ADC reading (0-4095, higher = worse air)
float mq135Voltage = 0;    // voltage measured at the ESP32 pin (post-divider)
unsigned long lastAqRead = 0;
const unsigned long aqInterval = 2000;
unsigned long mq135WarmupStart = 0;
bool mq135Ready = false;
const unsigned long MQ135_WARMUP_MS = 3UL * 60UL * 1000UL; // sensor needs a few min to stabilize each power-on

// Levels are set RELATIVE to this room's own clean-air baseline, not as raw ADC
// values. The MQ135 is uncalibrated and its baseline drifts with temperature,
// humidity and age far more than any useful threshold is wide: ~60 on
// 2026-07-26, ~110-140 mid-October, 206-541 (median 321) by 2026-10-29. Every
// fixed threshold went stale within days - the last set (220/221/290) sat below
// the median and fired all day.
//
// So the baseline is learned: the 10th percentile of the 5-minute averages in
// the 24h history (aqDayAvg), recomputed every minute - "the cleanest the air
// got today". The three levels are percentages above it (NVS, editable from the
// web UI and "aqset"). Percent rather than fixed offsets because the response
// scales with the baseline: +55 was big at a baseline of 60, small at 230.
#define AQ_DEFAULT_PCT_MODERATE  30
#define AQ_DEFAULT_PCT_POOR      60
#define AQ_DEFAULT_PCT_HAZARDOUS 100
#define AQ_BASE_PERCENTILE       10
#define AQ_BASE_MIN_BUCKETS      24 // 2h of history before alerts trust the baseline

int aqPctModerate  = AQ_DEFAULT_PCT_MODERATE;
int aqPctPoor      = AQ_DEFAULT_PCT_POOR;
int aqPctHazardous = AQ_DEFAULT_PCT_HAZARDOUS;
int aqBaseline = 0;        // learned clean-air level (raw); 0 = nothing to learn from yet
bool aqBaseTrusted = false; // enough history for alerts (AQ_BASE_MIN_BUCKETS)
float aqSmooth = 0;        // ~1 min EMA of the raw reading; alerts decide on this, not single samples

// Effective raw levels = baseline * (1 + pct/100), recomputed with the baseline.
// Everything that classifies (label, bar, graph bands) reads these.
int aqThreshModerate  = 0;
int aqThreshPoor      = 0;
int aqThreshHazardous = 0;

// ~10 minutes of history (one slot per minute) so the UI can say whether the air
// is getting better or worse, which a single instantaneous number never shows.
#define AQ_HIST_SLOTS 10
int aqHist[AQ_HIST_SLOTS];
uint8_t aqHistCount = 0;   // filled slots, saturates at AQ_HIST_SLOTS
uint8_t aqHistIdx = 0;     // next slot to write = oldest sample once wrapped
unsigned long lastAqHist = 0;
const unsigned long AQ_HIST_INTERVAL = 60000;

// --- 24h air quality history (backs the web UI's graph) ---
// One bucket per 5 minutes, 288 buckets = exactly 24h (576 bytes per array).
// Each bucket keeps the mean of the ~150 raw reads that land in it *and* their
// peak, because a 5-minute mean on its own smooths away precisely the short
// spikes (cooking, aerosols, breathing on the sensor) the graph exists to show.
#define AQ_DAY_SLOTS    288
#define AQ_DAY_BUCKET_S 300
#define AQ_DAY_EMPTY    -1

int16_t aqDayAvg[AQ_DAY_SLOTS];
int16_t aqDayPeak[AQ_DAY_SLOTS];
uint16_t aqDayNewestIdx = 0;     // slot holding the bucket currently being filled
uint32_t aqDayNewestBucket = 0;  // that bucket's number (epoch / AQ_DAY_BUCKET_S)
bool aqDayHasData = false;
uint32_t aqBucketSum = 0;        // running mean + peak of the open bucket. Both are
uint16_t aqBucketN = 0;          // rewritten into the slot on every sample, so the
int16_t aqBucketPeak = 0;        // newest point on the graph is always live.

// Buckets are keyed to wall-clock time, which is what lets the ring survive a
// reboot: without a clock there is no way to know how long the node was off, so
// no way to place the restored samples. Falls back to a monotonic count when NTP
// never lands - buckets still advance correctly, the UI just labels the x-axis
// relative ("-6h") instead of as clock times, and nothing is persisted.
bool aqClockSynced = false;
bool aqDayPending = false;       // loaded from NVS, not yet realigned against the clock
uint32_t aqDayStoredBucket = 0;
uint16_t aqDayStoredIdx = 0;
unsigned long lastAqDaySave = 0;
// ~2.3KB of blob per write, so this is deliberately not per bucket. A reboot
// loses at most this much history (OTA flushes on its way out, see onStart).
const unsigned long AQ_DAY_SAVE_INTERVAL = 30UL * 60UL * 1000UL;

// Alerting works in episodes so one stretch of bad air is one notification, not
// one per sample or per 30 minutes (the old behaviour, which with the stale
// thresholds above meant ~48 alerts a day). An episode starts after Poor has
// held for AQ_ALERT_HOLD_MS, may escalate once to Hazardous, reminds every
// AQ_ALERT_REMIND_MS while still Poor, and only ends after AQ_CLEAR_HOLD_MS below
// Moderate - so it can't flap. A new one can't alert within AQ_ALERT_COOLDOWN_MS
// of the last ending.
const unsigned long AQ_ALERT_HOLD_MS     = 5UL * 60UL * 1000UL;
const unsigned long AQ_CLEAR_HOLD_MS     = 10UL * 60UL * 1000UL;
const unsigned long AQ_ALERT_REMIND_MS   = 3UL * 60UL * 60UL * 1000UL;
const unsigned long AQ_ALERT_COOLDOWN_MS = 60UL * 60UL * 1000UL;
bool aqAlertActive = false;        // inside an alerted episode
bool aqEpisodeHazSent = false;
unsigned long aqPoorSince = 0;     // smoothed >= Poor since (0 = not)
unsigned long aqHazSince = 0;      // smoothed >= Hazardous since, within an episode
unsigned long aqClearSince = 0;    // smoothed < Moderate since, within an episode
unsigned long lastAqAlertSent = 0;
unsigned long lastAqEpisodeEnd = 0;

// --- Slave configuration ---
// Leave an entry as "" if that slave doesn't exist yet - blank slots are
// skipped everywhere. Give slaves DHCP reservations in the router so these
// don't go stale (they were 192.168.1.x once while the LAN is 192.168.0.x,
// which silently killed unicast sync and blocked the loop on HTTP timeouts).
#define NUM_SLAVES 2
const char* slaveIPs[NUM_SLAVES] = {"192.168.0.65", "192.168.0.71"};
const int slavePort = 80;
// Liveness is tracked from UDP acks the slaves send back (see led_sync.h),
// not from whether the IP string parses like before.
bool slaveOnline[NUM_SLAVES] = {false, false};
unsigned long slaveLastSeen[NUM_SLAVES] = {0, 0};
const unsigned long SLAVE_ONLINE_TIMEOUT_MS = 15000;
unsigned long lastSlaveSync = 0;
const unsigned long slaveSyncInterval = 3000;

// --- Per-strip state ---
// Strip 0 is this master's own strip; 1..NUM_SLAVES map to slaveIPs[] in order.
// Each strip carries its own enable, color and brightness; the LED *mode*
// (off/on/auto/music) and the motion timeout stay global.
#define NUM_STRIPS (NUM_SLAVES + 1)
bool stripEnabled[NUM_STRIPS];   // all filled by loadSettings()
CRGB stripColor[NUM_STRIPS];
uint8_t stripBri[NUM_STRIPS];
const char* stripNames[NUM_STRIPS] = {"Main", "Strip 2", "Strip 3"};

// True only when every strip would show exactly the same thing. The UDP
// broadcast can't address individual strips, so it's only safe to send while
// this holds (see broadcastLedState).
bool stripsUniform() {
  for (int i = 0; i < NUM_STRIPS; i++) {
    if (!stripEnabled[i]) return false;
    if (stripColor[i] != stripColor[0] || stripBri[i] != stripBri[0]) return false;
  }
  return true;
}

// Cache last sent state per slave to avoid flooding them. Indexed by slave, so
// [i] corresponds to strip i+1.
bool lastSentOn[NUM_SLAVES] = {false, false};
CRGB lastSentColor[NUM_SLAVES] = {CRGB::Black, CRGB::Black};
uint8_t lastSentBri[NUM_SLAVES] = {0, 0};

// --- UDP (fast path for real-time sync + ack receive) ---
#define UDP_PORT LED_SYNC_UDP_PORT
WiFiUDP udp;
uint16_t udpSeq = 0;

// --- Persistent settings (NVS) ---
Preferences prefs;
bool settingsDirty = false;
unsigned long lastSettingsChange = 0;
const unsigned long SETTINGS_SAVE_DEBOUNCE_MS = 3000; // batch rapid slider drags into one flash write

// --- Daily schedule ---
// Every day from this local hour the mode goes back to AUTO, so a manual
// ON/OFF/music choice made during the day doesn't carry into the evening.
#define AUTO_MODE_HOUR 17
// Local time exists for this schedule only - the 24h graph stays on UTC epoch
// seconds and the browser localizes it. POSIX rule for Europe/Zagreb, so the
// CET/CEST switch is handled without anyone touching it.
#define LOCAL_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
uint32_t lastAutoDay = 0; // local yyyymmdd the reset last ran (NVS "autod")

// --- Wi-Fi link knobs (POST /api/wifi), runtime only ---
// Defaults match startWiFi(). Added 2026-10-27 to chase full-size frames being
// lost on this node while small ones get through (see CLAUDE.md).
bool wifiSleepOn = false;
uint8_t wifiProto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
unsigned long wifiRejoinAt = 0; // millis() to re-associate after a protocol change; 0 = none

const char* wifiProtoName(uint8_t p) {
  if (p == WIFI_PROTOCOL_11B) return "b";
  if (p == (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)) return "bg";
  return "bgn";
}

void setupPins() {
  pinMode(LD2410_PIN, INPUT_PULLUP);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(stripBri[0]);
  FastLED.clear();
  FastLED.show();
  dht.begin();

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);
  mq135WarmupStart = millis();
}

void loadSettings() {
  prefs.begin("ledctl", false);
  uint8_t m = prefs.getUChar("mode", AUTO);
  ledMode = m <= SOUND ? (LedMode)m : AUTO;
  motionTimeout = prefs.getUInt("tout", 30000);
  tempOffset = prefs.getFloat("toff", -2.0f);
  musicFx = prefs.getUChar("fx", 0);
  if (musicFx > 2) musicFx = 0;
  aqPctModerate  = prefs.getInt("aqmP", AQ_DEFAULT_PCT_MODERATE);
  aqPctPoor      = prefs.getInt("aqpP", AQ_DEFAULT_PCT_POOR);
  aqPctHazardous = prefs.getInt("aqhP", AQ_DEFAULT_PCT_HAZARDOUS);
  sanitizeAqThresholds();
  // Raw-value thresholds from before the learned baseline mean nothing now.
  for (const char* k : {"aqm", "aqp", "aqh"}) if (prefs.isKey(k)) prefs.remove(k);
  lastAutoDay = prefs.getUInt("autod", 0);
  uint8_t stripMask = prefs.getUChar("strips", 0xFF); // 0xFF = every strip on
  // Pre-per-strip builds stored one shared "color"/"bri"; fall back to those so
  // an upgrade keeps the look it had instead of snapping every strip to white.
  uint32_t legacyColor = prefs.getUInt("color", 0xFFFFFF);
  uint8_t legacyBri = prefs.getUChar("bri", 128);
  char key[8];
  for (int i = 0; i < NUM_STRIPS; i++) {
    stripEnabled[i] = stripMask & (1 << i);
    snprintf(key, sizeof(key), "c%d", i);
    uint32_t c = prefs.getUInt(key, legacyColor);
    stripColor[i] = CRGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    snprintf(key, sizeof(key), "b%d", i);
    stripBri[i] = prefs.getUChar(key, legacyBri);
    if (stripBri[i] < 1) stripBri[i] = 1; // 0 would look like a dead strip
  }
}

void markSettingsDirty() {
  settingsDirty = true;
  lastSettingsChange = millis();
}

// NVS dedupes unchanged values internally, so this only wears flash when
// something actually changed.
void persistSettings() {
  if (!settingsDirty || millis() - lastSettingsChange < SETTINGS_SAVE_DEBOUNCE_MS) return;
  settingsDirty = false;
  prefs.putUChar("mode", (uint8_t)ledMode);
  char key[8];
  for (int i = 0; i < NUM_STRIPS; i++) {
    snprintf(key, sizeof(key), "c%d", i);
    prefs.putUInt(key, ((uint32_t)stripColor[i].r << 16) | ((uint32_t)stripColor[i].g << 8) | stripColor[i].b);
    snprintf(key, sizeof(key), "b%d", i);
    prefs.putUChar(key, stripBri[i]);
  }
  prefs.putUInt("tout", motionTimeout);
  prefs.putFloat("toff", tempOffset);
  prefs.putUChar("fx", musicFx);
  prefs.putInt("aqmP", aqPctModerate);
  prefs.putInt("aqpP", aqPctPoor);
  prefs.putInt("aqhP", aqPctHazardous);
  uint8_t stripMask = 0;
  for (int i = 0; i < NUM_STRIPS; i++) if (stripEnabled[i]) stripMask |= (1 << i);
  prefs.putUChar("strips", stripMask);
  Serial.println("[NVS] settings saved");
}

// Puts the mode back to AUTO once per local day at AUTO_MODE_HOUR. If the node
// was off or rebooting right then it catches up when it's back (until
// midnight), but only once per day, so a mode picked by hand afterwards
// sticks. The day is saved to NVS so a reboot doesn't fire it a second time.
void checkDailyAuto() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  time_t now = time(nullptr);
  if (now < 1700000000) return; // no NTP yet, so no idea what the local time is
  struct tm lt;
  localtime_r(&now, &lt);
  uint32_t today = (lt.tm_year + 1900) * 10000UL + (lt.tm_mon + 1) * 100UL + lt.tm_mday;

  if (lastAutoDay == 0) {
    // Nothing stored (first boot with this feature, or NVS was wiped). Already
    // past the hour: count today as done rather than override a mode set by
    // hand. Before it: arm with a non-date so today's run happens normally.
    lastAutoDay = lt.tm_hour >= AUTO_MODE_HOUR ? today : 1;
    prefs.putUInt("autod", lastAutoDay);
  }
  if (lt.tm_hour < AUTO_MODE_HOUR || today == lastAutoDay) return;

  lastAutoDay = today;
  prefs.putUInt("autod", today);
  if (ledMode == AUTO) return;

  ledMode = AUTO;
  markSettingsDirty();
  Serial.printf("[SCHED] %02d:%02d local - mode reset to AUTO\n", lt.tm_hour, lt.tm_min);
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
// Music drives one shared color for every strip (the effect *is* the color, so
// per-strip color would be meaningless here), but each strip still scales that
// against its own brightness setting as a ceiling.
uint8_t musicBriFor(int strip) {
  uint8_t ceiling = stripBri[strip];
  uint8_t floorBri = min((uint8_t)SOUND_MIN_BRIGHTNESS, ceiling);
  return floorBri + (uint8_t)((ceiling - floorBri) * musicLevel);
}

void updateSoundLEDs() {
  switch (musicFx) {
    case 1: { // Waves: broad color blobs drift along the strip; music speeds
              // them up, beats surge the whole glow and shift the palette
      waveTime += 1 + (uint16_t)(totalEnv * 14) + (uint16_t)(beatPulse * 10);
      for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t n = inoise8(i * 6, waveTime); // large-scale blobs (~30-40 LEDs wide)
        leds[i] = CHSV(beatHue + (n >> 1), 255, 180 + (n >> 2));
      }
      float lvl = totalEnv * 0.6f + beatPulse * 0.4f;
      musicLevel = 0.35f + 0.65f * lvl;
      musicColor = leds[NUM_LEDS / 2];
      break;
    }
    case 2: { // Spectrum: the room's color IS the music's tone - bass glows
              // red, mids green, treble blue, all blended into one wash
      float midEnv = constrain(totalEnv - bassEnv * 0.5f - trebEnv * 0.5f, 0.0f, 1.0f);
      CRGB target((uint8_t)(bassEnv * 255), (uint8_t)(midEnv * 220), (uint8_t)(trebEnv * 255));
      nblend(musicColor, target, 60);
      fill_solid(leds, NUM_LEDS, musicColor);
      musicLevel = totalEnv * 0.8f + beatPulse * 0.2f;
      break;
    }
    default: { // Pulse: every beat snaps to a new color and punches the
               // brightness, which then breathes back down until the next hit
      CRGB target = CHSV(beatHue, 240, 255);
      nblend(musicColor, target, beatNow ? 255 : 40);
      fill_gradient(leds, NUM_LEDS, CHSV(beatHue, 240, 255), CHSV(beatHue + 48, 240, 255));
      musicLevel = beatPulse > totalEnv * 0.5f ? beatPulse : totalEnv * 0.5f;
      break;
    }
  }
  musicLevel = constrain(musicLevel, 0.0f, 1.0f);

  // musicColor/musicLevel are still computed when strip 0 is disabled - the
  // slaves need them; brightness 0 is what blanks the local strip.
  FastLED.setBrightness(stripEnabled[0] ? musicBriFor(0) : 0);
  FastLED.show();

  unsigned long now = millis();
  if (now - lastSoundSlaveSync >= soundSlaveSyncInterval) {
    lastSoundSlaveSync = now;
    syncStrips(true);
  }
}

void updateLEDs() {
  // Statics track what's physically on the strip so we only push a frame when
  // something changed - a full show() costs ~3ms and was running every loop.
  static int lastShownMode = -1;
  static bool lastShownOn = false;
  static CRGB lastShownColor = CRGB::Black;
  static uint8_t lastShownBri = 0;

  if (ledMode == SOUND) {
    lastShownMode = SOUND; // force a redraw when leaving sound mode
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

  // shouldBeOn stays the *logical* state (what syncStrips pushes to slaves);
  // localOn is what this ESP32's own strip actually renders.
  bool localOn = shouldBeOn && stripEnabled[0];

  bool changed = (int)ledMode != lastShownMode || localOn != lastShownOn ||
                 (localOn && (stripColor[0] != lastShownColor || stripBri[0] != lastShownBri));

  if (changed) {
    if (localOn) {
      fill_solid(leds, NUM_LEDS, stripColor[0]);
      FastLED.setBrightness(stripBri[0]);
    } else {
      FastLED.clear();
    }
    FastLED.show();

    lastShownMode = (int)ledMode;
    lastShownOn = localOn;
    lastShownColor = stripColor[0];
    lastShownBri = stripBri[0];
  }

  syncStrips(false);
}

bool getLedOn() {
  if (ledMode == ON) return true;
  if (ledMode == OFF) return false;
  if (ledMode == SOUND) return true; // always at least a dim idle glow
  return (millis() - lastMotionTime < motionTimeout);
}

// TLS + HTTP to ntfy.sh takes seconds; doing it inline froze LEDs, the web UI
// and OTA for the duration. It now runs in a one-shot background task so the
// loop never blocks. One send in flight at a time - a dropped alert repeats
// via the 30-min reminder anyway.
struct NtfyMsg {
  String title, message, priority, tags;
};
volatile bool ntfyInFlight = false;

void ntfyTask(void* param) {
  NtfyMsg* m = (NtfyMsg*)param;

  // The braces matter: vTaskDelete(NULL) never returns, so anything still in
  // scope at that point never has its destructor run. With the client, http and
  // url declared at function scope, every notification leaked the whole TLS
  // session (~35KB of mbedTLS buffers) plus a socket - ntfy.sh answers
  // keep-alive, so http.end() deliberately left it open for reuse. A few alerts
  // and the heap was too far gone to serve the web page.
  {
    WiFiClientSecure client;
    client.setInsecure(); // skip TLS cert validation - acceptable for this hobby use case
    client.setTimeout(5000);

    HTTPClient http;
    http.setReuse(false); // one-shot: close on end() rather than hold the session
    http.setConnectTimeout(5000);
    String url = String("https://ntfy.sh/") + ntfyTopic;
    if (http.begin(client, url)) {
      http.addHeader("Title", m->title);
      http.addHeader("Priority", m->priority);
      http.addHeader("Tags", m->tags);
      int code = http.POST(m->message);
      Serial.printf("[ntfy] POST %s -> %d (heap %u, largest block %u)\n", url.c_str(), code,
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      http.end();
    }
  }

  delete m;
  ntfyInFlight = false;
  vTaskDelete(NULL);
}

void sendNtfyNotification(const String& title, const String& message, const String& priority, const String& tags) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (ntfyInFlight) {
    Serial.println("[ntfy] send already in flight, dropped");
    return;
  }

  NtfyMsg* m = new NtfyMsg{title, message, priority, tags};
  ntfyInFlight = true;
  // 10KB stack: TLS handshake is stack-hungry
  if (xTaskCreate(ntfyTask, "ntfy", 10240, m, 1, NULL) != pdPASS) {
    delete m;
    ntfyInFlight = false;
    Serial.println("[ntfy] task create failed");
  }
}

void readMQ135() {
  const int SAMPLES = 10;
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(MQ135_PIN); // back-to-back reads still average noise fine; no need to block
  }
  mq135Raw = sum / SAMPLES;
  mq135Voltage = mq135Raw * (3.3f / 4095.0f) / MQ135_DIVIDER_RATIO;
}

const char* airQualityLabel(int raw) {
  if (raw < aqThreshModerate) return "Good";
  if (raw < aqThreshPoor) return "Moderate";
  if (raw < aqThreshHazardous) return "Poor";
  return "Hazardous";
}

// Turns baseline + percentages into the raw levels everything classifies
// against. With no history at all yet, the smoothed reading stands in as a
// provisional baseline so the bar and label stay sensible; alerts wait for
// aqBaseTrusted regardless.
void aqApplyLevels() {
  int base = aqBaseline > 0 ? aqBaseline : aqSmooth > 0 ? (int)aqSmooth : mq135Raw > 0 ? mq135Raw : 100;
  aqThreshModerate  = constrain(base + base * aqPctModerate / 100, 1, 4093);
  aqThreshPoor      = constrain(base + base * aqPctPoor / 100, aqThreshModerate + 1, 4094);
  aqThreshHazardous = constrain(base + base * aqPctHazardous / 100, aqThreshPoor + 1, 4095);
}

// The bands only mean anything if they stay ordered - a UI or serial edit that
// crosses two of them over would make airQualityLabel() skip a band entirely.
void sanitizeAqThresholds() {
  aqPctModerate  = constrain(aqPctModerate, 5, 300);
  aqPctPoor      = constrain(aqPctPoor, aqPctModerate + 5, 400);
  aqPctHazardous = constrain(aqPctHazardous, aqPctPoor + 5, 500);
  aqApplyLevels();
}

// Re-learns the clean-air baseline from the 24h ring: the AQ_BASE_PERCENTILE-th
// lowest 5-minute average. A percentile rather than the minimum so one odd
// bucket can't drag it down. Once a minute; 288 values is microseconds.
void aqUpdateBaseline() {
  int16_t v[AQ_DAY_SLOTS];
  int n = 0;
  if (aqDayHasData) {
    for (int i = 0; i < AQ_DAY_SLOTS; i++) if (aqDayAvg[i] >= 0) v[n++] = aqDayAvg[i];
  }
  if (n > 0) {
    int k = n * AQ_BASE_PERCENTILE / 100;
    std::nth_element(v, v + k, v + n);
    aqBaseline = v[k];
  }
  aqBaseTrusted = n >= AQ_BASE_MIN_BUCKETS;
  aqApplyLevels();
}

// Where the reading sits across the whole Good..Hazardous span, so the number
// next to the label actually tracks the label (the old version mapped the raw
// value onto the full 0-4095 ADC range and read "Moderate (2%)").
uint8_t airQualityPercent(int raw) {
  int top = aqThreshHazardous + (aqThreshHazardous - aqThreshPoor); // headroom above "hazardous"
  if (top < 1) top = 1;
  return (uint8_t)constrain((long)map(raw, 0, top, 0, 100), 0L, 100L);
}

void sendAqAlert(const char* what, int level, const char* priority) {
  int pct = aqBaseline > 0 ? (level - aqBaseline) * 100 / aqBaseline : 0;
  String msg = String("Air quality ") + what + ": " + pct + "% above this room's clean-air level (reading " +
    level + ", baseline " + aqBaseline + "). Consider ventilating.";
  sendNtfyNotification("Air Quality Alert", msg, priority, "warning");
}

// Episode state machine - see the AQ_ALERT_* constants for the rules. Runs on
// every warm sample (2s) and decides on aqSmooth, never on a single reading.
void checkAirQualityAlert(int raw) {
  if (!mq135Ready) return;
  unsigned long now = millis();
  // ~1 minute EMA at one sample per aqInterval. Seeded with the first warm read.
  aqSmooth = aqSmooth <= 0 ? raw : aqSmooth + (raw - aqSmooth) * (aqInterval / 60000.0f);
  if (!aqBaseTrusted) return; // still learning what "clean" means in this room

  int s = (int)aqSmooth;
  aqPoorSince = s >= aqThreshPoor ? (aqPoorSince ? aqPoorSince : now) : 0;
  bool poorHeld = aqPoorSince && now - aqPoorSince >= AQ_ALERT_HOLD_MS;

  if (!aqAlertActive) {
    if (!poorHeld) return;
    if (lastAqEpisodeEnd && now - lastAqEpisodeEnd < AQ_ALERT_COOLDOWN_MS) return;
    bool haz = s >= aqThreshHazardous;
    aqAlertActive = true;
    aqEpisodeHazSent = haz;
    aqHazSince = aqClearSince = 0;
    lastAqAlertSent = now;
    sendAqAlert(haz ? "HAZARDOUS" : "POOR", s, haz ? "urgent" : "high");
    return;
  }

  // Inside an episode: one escalation, then sparse reminders.
  aqHazSince = s >= aqThreshHazardous ? (aqHazSince ? aqHazSince : now) : 0;
  if (!aqEpisodeHazSent && aqHazSince && now - aqHazSince >= AQ_ALERT_HOLD_MS) {
    aqEpisodeHazSent = true;
    lastAqAlertSent = now;
    sendAqAlert("now HAZARDOUS", s, "urgent");
  } else if (poorHeld && now - lastAqAlertSent >= AQ_ALERT_REMIND_MS) {
    lastAqAlertSent = now;
    sendAqAlert("still POOR", s, "default");
  }

  aqClearSince = s < aqThreshModerate ? (aqClearSince ? aqClearSince : now) : 0;
  if (aqClearSince && now - aqClearSince >= AQ_CLEAR_HOLD_MS) {
    aqAlertActive = false;
    aqPoorSince = 0;
    lastAqEpisodeEnd = now;
    // "low" = listed in the app without a sound; the all-clear isn't urgent.
    sendNtfyNotification("Air Quality Normal", "Air quality is back to this room's normal level.", "low", "white_check_mark");
  }
}

// ============================================================
// 24h AIR QUALITY HISTORY
// ============================================================

// Wall clock once NTP has landed, otherwise a monotonic seconds count. Both
// bucket identically; only what the UI can label the x-axis with differs.
uint32_t aqNowSeconds() {
  time_t t = time(nullptr);
  if (t > 1700000000) { aqClockSynced = true; return (uint32_t)t; } // anything past 2023 means NTP replied
  return (uint32_t)(millis() / 1000);
}

void aqDayClear() {
  for (int i = 0; i < AQ_DAY_SLOTS; i++) {
    aqDayAvg[i] = AQ_DAY_EMPTY;
    aqDayPeak[i] = AQ_DAY_EMPTY;
  }
  aqDayNewestIdx = 0;
  aqBucketSum = 0; aqBucketN = 0; aqBucketPeak = 0;
  aqDayHasData = false;
}

// Walk the ring forward over time that produced no samples, leaving those slots
// empty so the graph draws a break instead of interpolating across the gap.
void aqDayAdvance(uint32_t buckets) {
  for (uint32_t k = 0; k < buckets; k++) {
    aqDayNewestIdx = (aqDayNewestIdx + 1) % AQ_DAY_SLOTS;
    aqDayAvg[aqDayNewestIdx] = AQ_DAY_EMPTY;
    aqDayPeak[aqDayNewestIdx] = AQ_DAY_EMPTY;
  }
}

void aqDayRecord(int raw) {
  uint32_t bucket = aqNowSeconds() / AQ_DAY_BUCKET_S;

  if (!aqDayHasData) {
    aqDayNewestBucket = bucket;
    aqDayHasData = true;
  } else if (bucket > aqDayNewestBucket) {
    uint32_t skip = bucket - aqDayNewestBucket;
    if (skip >= AQ_DAY_SLOTS) {
      aqDayClear();       // everything on hand is older than the window
      aqDayHasData = true;
    } else {
      aqDayAdvance(skip);
    }
    aqDayNewestBucket = bucket;
    aqBucketSum = 0; aqBucketN = 0; aqBucketPeak = 0;
  } else if (bucket < aqDayNewestBucket) {
    // Clock went backwards - in practice only when NTP lands after history
    // already started on the millis() fallback. Those samples can't be placed.
    aqDayClear();
    aqDayNewestBucket = bucket;
    aqDayHasData = true;
  }

  aqBucketSum += raw;
  aqBucketN++;
  if (raw > aqBucketPeak) aqBucketPeak = raw;
  aqDayAvg[aqDayNewestIdx] = (int16_t)(aqBucketSum / aqBucketN);
  aqDayPeak[aqDayNewestIdx] = aqBucketPeak;
}

void aqDaySave() {
  if (!aqClockSynced || !aqDayHasData || aqDayPending) return; // relative timelines can't be resumed
  // Say so if this ever fails: a full NVS would otherwise just quietly lose the
  // history at every reboot with nothing on the wire to explain it.
  if (prefs.putBytes("aqda", aqDayAvg, sizeof(aqDayAvg)) != sizeof(aqDayAvg) ||
      prefs.putBytes("aqdp", aqDayPeak, sizeof(aqDayPeak)) != sizeof(aqDayPeak)) {
    Serial.println("[AQ] NVS write of the 24h history failed");
    return;
  }
  prefs.putUInt("aqdb", aqDayNewestBucket);
  prefs.putUShort("aqdi", aqDayNewestIdx);
}

void aqDayLoad() {
  aqDayClear();
  if (prefs.getBytesLength("aqda") != sizeof(aqDayAvg)) return;
  prefs.getBytes("aqda", aqDayAvg, sizeof(aqDayAvg));
  if (prefs.getBytesLength("aqdp") == sizeof(aqDayPeak)) prefs.getBytes("aqdp", aqDayPeak, sizeof(aqDayPeak));
  aqDayStoredBucket = prefs.getUInt("aqdb", 0);
  aqDayStoredIdx = prefs.getUShort("aqdi", 0);
  // Left unusable (aqDayHasData still false) until the clock says how much time
  // passed while we were off - see aqDayResume().
  aqDayPending = aqDayStoredBucket > 0;
  if (!aqDayPending) aqDayClear(); // untimestamped samples can't be placed; don't let them linger
}

// Called every loop until it resolves. NTP needs a few seconds after the WiFi
// association, and history only starts after the 3 minute warm-up anyway.
void aqDayResume() {
  if (!aqDayPending) return;

  uint32_t bucket = aqNowSeconds() / AQ_DAY_BUCKET_S;
  if (!aqClockSynced) {
    if (millis() > 90000) {
      aqDayPending = false;
      aqDayClear();
      Serial.println("[AQ] no NTP - 24h history restarts from boot and won't be saved");
    }
    return;
  }

  aqDayPending = false;
  if (bucket < aqDayStoredBucket || bucket - aqDayStoredBucket >= AQ_DAY_SLOTS) {
    aqDayClear();
    Serial.println("[AQ] stored 24h history is older than the window, starting fresh");
    return;
  }

  aqDayNewestIdx = aqDayStoredIdx % AQ_DAY_SLOTS;
  aqDayNewestBucket = aqDayStoredBucket;
  aqDayHasData = true;
  uint32_t off = bucket - aqDayStoredBucket;
  aqDayAdvance(off);
  aqDayNewestBucket = bucket;
  aqBucketSum = 0; aqBucketN = 0; aqBucketPeak = 0;
  Serial.printf("[AQ] restored 24h history (%lu min gap while off)\n",
    (unsigned long)(off * AQ_DAY_BUCKET_S / 60));
  // Learn the baseline from it now rather than at the first post-warm-up
  // minute tick, so the bar and graph have real levels from a few seconds in.
  aqUpdateBaseline();
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

// Reads slave ack packets (see led_sync.h) and refreshes the online table.
// This is what makes slaveOnline[] mean "actually responding" rather than
// "IP string parses".
void processSlaveAcks() {
  int sz;
  while ((sz = udp.parsePacket()) > 0) {
    if (sz < (int)sizeof(UdpAckPkt)) continue;
    UdpAckPkt ack;
    udp.read((uint8_t*)&ack, sizeof(ack));
    if (ack.version != LED_SYNC_ACK_VERSION) continue;

    IPAddress rip = udp.remoteIP();
    bool known = false;
    for (int i = 0; i < NUM_SLAVES; i++) {
      IPAddress si;
      if (slaveIPs[i][0] && si.fromString(slaveIPs[i]) && si == rip) {
        slaveLastSeen[i] = millis();
        known = true;
      }
    }
    if (!known) {
      // A slave got the broadcast but isn't in slaveIPs - config is stale.
      static unsigned long lastUnknownLog = 0;
      if (millis() - lastUnknownLog > 10000) {
        lastUnknownLog = millis();
        Serial.printf("[SYNC] ack from unconfigured slave %s - update slaveIPs[]\n", rip.toString().c_str());
      }
    }
  }
  for (int i = 0; i < NUM_SLAVES; i++) {
    slaveOnline[i] = slaveLastSeen[i] != 0 && millis() - slaveLastSeen[i] < SLAVE_ONLINE_TIMEOUT_MS;
  }
}

// Resolves what a given strip should be showing right now. In music mode every
// strip shares the effect's color but scales it by its own brightness ceiling;
// otherwise each strip uses its own configured color/brightness.
void resolveStrip(int strip, bool& outOn, CRGB& outColor, uint8_t& outBri) {
  outOn = getLedOn() && stripEnabled[strip];
  if (ledMode == SOUND) {
    outColor = musicColor;
    outBri = musicBriFor(strip);
  } else {
    outColor = stripColor[strip];
    outBri = stripBri[strip];
  }
}

// Pushes each strip its own state. `force` skips the per-strip dedupe (the 20Hz
// music path, config changes, the periodic tick); `includeHttp` adds the slow
// HTTP fallback and is only for the periodic 3s tick. Those used to be one flag,
// so the music path also opened two TCP connections to the slaves every 50ms.
void syncStrips(bool force, bool includeHttp) {
  // 1. UDP unicast per configured slave (reliable). This is the only path that
  // can address a single strip, so all per-strip state rides on it.
  for (int i = 0; i < NUM_SLAVES; i++) {
    IPAddress ip;
    if (!slaveIPs[i][0] || !ip.fromString(slaveIPs[i])) continue;

    bool on; CRGB color; uint8_t bri;
    resolveStrip(i + 1, on, color, bri);
    if (!force && on == lastSentOn[i] && color == lastSentColor[i] && bri == lastSentBri[i]) continue;
    lastSentOn[i] = on;
    lastSentColor[i] = color;
    lastSentBri[i] = bri;

    sendUdpPkt(ip, on, color, bri);
  }

  // 2. UDP broadcast (auto-discovery, may be blocked by some routers).
  // The packet format has no strip addressing, so every slave applies it - a
  // strip that is disabled or has its own color would see the shared state
  // here and immediately undo its unicast. Only broadcast while every strip is
  // enabled and showing exactly the same thing; discovery resumes once that
  // holds again.
  // Rate-limited because this runs from every loop() via updateLEDs(): the
  // broadcast used to go out ~20 times a second, each one relayed by the AP to
  // every station at its slowest rate and acked by every slave, for a path that
  // only exists to find slaves missing from slaveIPs[].
  static unsigned long lastBroadcast = 0;
  if (stripsUniform() && millis() - lastBroadcast >= slaveSyncInterval) {
    lastBroadcast = millis();
    bool on; CRGB color; uint8_t bri;
    resolveStrip(0, on, color, bri);
    IPAddress bcast = WiFi.broadcastIP();
    if (bcast != INADDR_NONE) sendUdpPkt(bcast, on, color, bri);
  }

  // HTTP — slow fallback, only for the periodic sync
  if (!includeHttp) return;

  // connect() to a dead host blocks for its full timeout, so offline slaves
  // are only retried every 30s instead of every 3s sync tick.
  static unsigned long lastOfflineHttpTry = 0;
  bool retryOffline = millis() - lastOfflineHttpTry > 30000;
  if (retryOffline) lastOfflineHttpTry = millis();

  for (int i = 0; i < NUM_SLAVES; i++) {
    if (!slaveIPs[i][0]) continue;
    if (!slaveOnline[i] && !retryOffline) continue;

    bool on; CRGB color; uint8_t bri;
    resolveStrip(i + 1, on, color, bri);

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", color.r, color.g, color.b);
    StaticJsonDocument<128> doc; // built per slave - every field can differ
    doc["on"] = on;
    doc["color"] = hex;
    doc["brightness"] = bri;
    String payload;
    serializeJson(doc, payload);

    WiFiClient client;
    if (client.connect(slaveIPs[i], slavePort, 300)) {
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
.strip-btns{display:flex;gap:8px}
.strip-btns button{flex:1;padding:9px 4px;border:none;border-radius:10px;font-size:0.9em;cursor:pointer;color:#fff;background:#34495e;transition:.2s;line-height:1.35}
.strip-btns button.on{background:#2ecc71;transform:scale(0.97);box-shadow:inset 0 0 10px rgba(0,0,0,0.25)}
.strip-btns button small{display:block;font-size:0.72em;opacity:.75}
.strip-btns button:disabled{opacity:.35;cursor:not-allowed;transform:none}
.sel-btns{display:flex;gap:6px}
.sel-btns button{flex:1;padding:8px 4px;border:none;border-radius:8px;font-size:0.82em;cursor:pointer;color:#fff;background:#34495e;transition:.2s}
.sel-btns button.active{background:#e94560;transform:scale(0.96);box-shadow:inset 0 0 10px rgba(0,0,0,0.3)}
.sel-btns button:disabled{opacity:.35;cursor:not-allowed}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-left:5px;vertical-align:middle;border:1px solid rgba(255,255,255,.35)}
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
.aq-panel{background:#1a1a2e;border-radius:8px;padding:10px 12px 8px;margin-bottom:14px;font-size:0.8em}
.aq-head{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:9px}
.aq-head span{color:#888;font-size:0.9em}
.aq-head b{font-size:1.3em}
.aq-wrap{position:relative}
.aq-bar{display:flex;height:10px;border-radius:5px;overflow:hidden;background:#0f0f1e}
.aq-bar i{height:100%;transition:width .3s}
.aq-mark{position:absolute;top:-3px;width:3px;height:16px;background:#fff;border-radius:2px;box-shadow:0 0 5px rgba(0,0,0,.9);transition:left .4s}
.aq-scale{position:relative;height:13px;margin-top:5px;color:#777;font-size:0.8em}
.aq-scale i{position:absolute;transform:translateX(-50%);font-style:normal;white-space:nowrap}
.aq-meta{margin-top:7px;color:#888;display:flex;justify-content:space-between;gap:8px}
.aq-meta b{color:#ccc;font-weight:600}
.aq-thr{display:flex;gap:8px}
.aq-thr div{flex:1;display:flex;flex-direction:column;gap:4px}
.aq-thr small{font-size:0.75em}
.aq-thr input[type=number]{width:100%;padding:9px 4px;border:none;border-radius:8px;background:#1a1a2e;color:#eee;font-size:16px;text-align:center}
.aq-hint{margin-top:7px;color:#777;font-size:0.75em;line-height:1.45}
.aq-gbtn{width:100%;margin-top:9px;padding:9px;border:none;border-radius:8px;background:#0f3460;color:#cfd8e3;font-size:1em;cursor:pointer;transition:.2s}
.aq-gbtn.open{background:#e94560;color:#fff}
.aq-graph{margin-top:9px}
.aq-graph svg{width:100%;height:auto;display:block;touch-action:pan-y}
.aq-gread{display:flex;justify-content:space-between;gap:8px;color:#888;min-height:15px;margin-bottom:3px}
.aq-gread b{color:#ddd;font-weight:600}
.aq-gempty{color:#777;text-align:center;padding:18px 4px;line-height:1.5}
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
</div>
<div class="aq-panel">
<div class="aq-head"><span>Air Quality</span><b id="sAq">--</b></div>
<div class="aq-wrap">
<div class="aq-bar"><i id="aqSg" style="background:#2ecc71"></i><i id="aqSm" style="background:#f1c40f"></i><i id="aqSp" style="background:#e67e22"></i><i id="aqSh" style="background:#e74c3c"></i></div>
<div class="aq-mark" id="aqMark" style="left:0"></div>
</div>
<div class="aq-scale" id="aqScale"></div>
<div class="aq-meta"><span id="aqMetaL">--</span><span id="aqMetaR"></span></div>
<button class="aq-gbtn" id="aqGBtn" onclick="toggleAqGraph()">Show 24 h graph</button>
<div class="aq-graph" id="aqGraph" style="display:none">
<div class="aq-gread"><span id="aqGrL">&nbsp;</span><span id="aqGrR"></span></div>
<div id="aqGBody"></div>
</div>
</div>
<div class="mode-btns">
<button class="btn-off" id="bOff" onclick="setMode('off')">Off</button>
<button class="btn-on" id="bOn" onclick="setMode('on')">On</button>
<button class="btn-auto" id="bAuto" onclick="setMode('auto')">Auto</button>
<button class="btn-sound" id="bSound" onclick="setMode('sound')">Music</button>
</div>
<div class="ctrl">
<label>Strips</label>
<div class="strip-btns" id="stripBtns"></div>
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
<label>Editing</label>
<div class="sel-btns" id="selBtns"></div>
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
<div class="ctrl">
<label>Air quality alert levels (% above this room's clean air)</label>
<div class="aq-thr">
<div><small class="aq-moderate">Moderate</small><input type="number" id="aqmIn" min="5" max="300" onchange="setAqThr()"></div>
<div><small class="aq-poor">Poor</small><input type="number" id="aqpIn" min="10" max="400" onchange="setAqThr()"></div>
<div><small class="aq-hazardous">Hazardous</small><input type="number" id="aqhIn" min="15" max="500" onchange="setAqThr()"></div>
</div>
<div class="aq-hint">"Clean air" is learned from the last 24 h (the cleanest 10% of readings), so the sensor's day-to-day drift no longer needs retuning. You get one notification once <b>Poor</b> has lasted 5 min, one more if it turns <b>Hazardous</b>, a reminder every 3 h if it stays bad, and a silent all-clear. Getting alerts on air that feels fine? Raise <b>Poor</b>.</div>
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
let selStrip=-1
function sendColor(c){fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({strip:selStrip,color:c})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}

function setMode(m){fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setBright(v){document.getElementById('bVal').textContent=v;clearTimeout(toutTimer);toutTimer=setTimeout(()=>{fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({strip:selStrip,brightness:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})},150)}
function setTout(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timeout:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setToff(v){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({toff:v})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setFx(n){fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({musicFx:n})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function setAqThr(){const b={aqmPct:+document.getElementById('aqmIn').value,aqpPct:+document.getElementById('aqpIn').value,aqhPct:+document.getElementById('aqhIn').value}
fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}

// The bar spans 0..top, where top leaves one "poor"-width of headroom above the
// hazardous line so the worst band stays visible instead of being a hairline.
function renderAq(d){
const top=Math.max(d.aqh+(d.aqh-d.aqp),1),pc=v=>Math.max(0,Math.min(100,v*100/top))
document.getElementById('aqSg').style.width=pc(d.aqm)+'%'
document.getElementById('aqSm').style.width=pc(d.aqp-d.aqm)+'%'
document.getElementById('aqSp').style.width=pc(d.aqh-d.aqp)+'%'
document.getElementById('aqSh').style.width=pc(top-d.aqh)+'%'
document.getElementById('aqMark').style.left=pc(d.aq)+'%'
const sc=document.getElementById('aqScale')
sc.innerHTML=[d.aqm,d.aqp,d.aqh].map(v=>'<i style="left:'+pc(v)+'%">'+v+'</i>').join('')
const aq=document.getElementById('sAq')
aq.textContent=d.aqReady?d.aqLabel:'Warming up'
aq.className=d.aqReady?('aq-'+d.aqLabel.toLowerCase()):''
let left='raw <b>'+d.aq+'</b>'
if(typeof d.aqV!=='undefined')left+=' &middot; '+d.aqV.toFixed(2)+'V'
if(typeof d.aqBase!=='undefined')left+=' &middot; '+(d.aqBaseOk?'clean air <b>'+d.aqBase+'</b>':'learning clean air')
document.getElementById('aqMetaL').innerHTML=left
let right=''
if(!d.aqReady){right='warm-up '+d.aqWarm+'s'}
else if(typeof d.aqTrend==='undefined'){right='trend: collecting'}
else if(d.aqTrend>2){right='&uarr; worsening +'+d.aqTrend+' / '+d.aqTrendMin+' min'}
else if(d.aqTrend<-2){right='&darr; clearing '+d.aqTrend+' / '+d.aqTrendMin+' min'}
else{right='steady over '+d.aqTrendMin+' min'}
document.getElementById('aqMetaR').innerHTML=right
if(document.activeElement!==document.getElementById('aqmIn'))document.getElementById('aqmIn').value=d.aqmPct
if(document.activeElement!==document.getElementById('aqpIn'))document.getElementById('aqpIn').value=d.aqpPct
if(document.activeElement!==document.getElementById('aqhIn'))document.getElementById('aqhIn').value=d.aqhPct
}

// --- 24h air quality graph ---
// One point per 5-minute bucket, straight from the ESP32's ring buffer: the line
// is the bucket's mean, the band above it the bucket's peak, so a short spike
// stays visible instead of being averaged flat. Empty slots (-1) are breaks in
// the line, never zeros - the node was off or the sensor was warming up.
const GW=320,GH=134,GL=30,GR=6,GT=7,GB=26,GC='#4aa3df'
let aqGOpen=false,aqGTimer=null

function toggleAqGraph(){
aqGOpen=!aqGOpen
document.getElementById('aqGraph').style.display=aqGOpen?'':'none'
const b=document.getElementById('aqGBtn')
b.classList.toggle('open',aqGOpen)
b.textContent=aqGOpen?'Hide 24 h graph':'Show 24 h graph'
clearInterval(aqGTimer)
// A bucket is 5 min wide, so re-fetching this ~3KB payload faster buys nothing.
if(aqGOpen){loadAqGraph();aqGTimer=setInterval(loadAqGraph,60000)}
}
function loadAqGraph(){fetch('/api/aqhistory').then(r=>r.json()).then(drawAqGraph).catch(()=>{})}

// The device sends UTC; format in the phone's own timezone. With no NTP it sends
// clock:false and a monotonic timeline, which only supports relative labels.
function aqTimeLabel(d,i){
const t=d.t0+i*d.iv
if(d.clock){const x=new Date(t*1000);return('0'+x.getHours()).slice(-2)+':'+('0'+x.getMinutes()).slice(-2)}
const mins=Math.round((d.now-t)/60)
if(mins<3)return'now'
return mins<60?'-'+mins+'m':'-'+Math.round(mins/60)+'h'
}
function aqBand(d,v){return v<d.aqm?'Good':v<d.aqp?'Moderate':v<d.aqh?'Poor':'Hazardous'}

function drawAqGraph(d){
const body=document.getElementById('aqGBody'),rl=document.getElementById('aqGrL'),rr=document.getElementById('aqGrR')
const n=d.avg.length,have=[]
for(let i=0;i<n;i++)if(d.avg[i]>=0)have.push(i)
if(!have.length){
body.innerHTML='<div class="aq-gempty">'+(d.warm?'No samples yet &mdash; the first point lands within '+(d.iv/60)+' min.':'Sensor is still warming up.<br>History starts once it is ready.')+'</div>'
rl.innerHTML='&nbsp;';rr.innerHTML=''
return}

// Truncated y-axis (the readings sit well above 0 and the interesting range is
// narrow), but the Moderate line is always kept in view and the floor is
// labelled, so "how close is this to an alert" stays readable.
let lo=d.aqm,hi=d.aqm
const pk=i=>d.peak[i]<0?d.avg[i]:Math.max(d.peak[i],d.avg[i])
have.forEach(i=>{lo=Math.min(lo,d.avg[i]);hi=Math.max(hi,pk(i))})
const pad=Math.max(6,(hi-lo)*0.12);lo=Math.max(0,lo-pad);hi=hi+pad
const X=i=>GL+(GW-GL-GR)*(n<2?0:i/(n-1)),Y=v=>GT+(GH-GT-GB)*(1-(v-lo)/(hi-lo||1))

let s=''
const band=(a,b,c)=>{const t=Math.min(b,hi),u=Math.max(a,lo);return t<=u?'':'<rect x="'+GL+'" y="'+Y(t).toFixed(1)+'" width="'+(GW-GL-GR)+'" height="'+(Y(u)-Y(t)).toFixed(1)+'" fill="'+c+'" opacity="0.1"/>'}
s+=band(0,d.aqm,'#2ecc71')+band(d.aqm,d.aqp,'#f1c40f')+band(d.aqp,d.aqh,'#e67e22')+band(d.aqh,99999,'#e74c3c')

const thr=[[d.aqm,'#f1c40f'],[d.aqp,'#e67e22'],[d.aqh,'#e74c3c']]
thr.forEach(t=>{if(t[0]<lo||t[0]>hi)return
s+='<line x1="'+GL+'" y1="'+Y(t[0]).toFixed(1)+'" x2="'+(GW-GR)+'" y2="'+Y(t[0]).toFixed(1)+'" stroke="'+t[1]+'" stroke-width="0.6" stroke-dasharray="3 3" opacity="0.8"/>'
s+='<text x="'+(GL-4)+'" y="'+(Y(t[0])+2.5).toFixed(1)+'" text-anchor="end" font-size="7.5" fill="'+t[1]+'">'+t[0]+'</text>'})
s+='<text x="'+(GL-4)+'" y="'+(GH-GB+2.5)+'" text-anchor="end" font-size="7.5" fill="#666">'+Math.round(lo)+'</text>'

const ticks=[0,Math.round((n-1)/4),Math.round((n-1)/2),Math.round(3*(n-1)/4),n-1]
ticks.forEach((i,k)=>{const x=X(i).toFixed(1)
s+='<line x1="'+x+'" y1="'+GT+'" x2="'+x+'" y2="'+(GH-GB)+'" stroke="#fff" stroke-width="0.4" opacity="0.07"/>'
s+='<text x="'+x+'" y="'+(GH-GB+11)+'" text-anchor="'+(k===0?'start':k===4?'end':'middle')+'" font-size="7.5" fill="#777">'+aqTimeLabel(d,i)+'</text>'})

let segs=[],cur=[]
for(let i=0;i<n;i++){if(d.avg[i]<0){if(cur.length)segs.push(cur);cur=[]}else cur.push(i)}
if(cur.length)segs.push(cur)
segs.forEach(g=>{
if(g.length<2){s+='<circle cx="'+X(g[0]).toFixed(1)+'" cy="'+Y(d.avg[g[0]]).toFixed(1)+'" r="1.8" fill="'+GC+'"/>';return}
const up=g.map(i=>X(i).toFixed(1)+' '+Y(pk(i)).toFixed(1)).join(' L')
const dn=g.slice().reverse().map(i=>X(i).toFixed(1)+' '+Y(d.avg[i]).toFixed(1)).join(' L')
s+='<path d="M'+up+' L'+dn+' Z" fill="'+GC+'" opacity="0.25"/>'
s+='<polyline points="'+g.map(i=>X(i).toFixed(1)+','+Y(d.avg[i]).toFixed(1)).join(' ')+'" fill="none" stroke="'+GC+'" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>'})

s+='<line id="aqX" x1="0" y1="'+GT+'" x2="0" y2="'+(GH-GB)+'" stroke="#fff" stroke-width="0.6" opacity="0"/>'
s+='<circle id="aqD" cx="0" cy="0" r="2.4" fill="#fff" stroke="#16213e" stroke-width="0.8" opacity="0"/>'
s+='<rect id="aqH" x="'+GL+'" y="'+GT+'" width="'+(GW-GL-GR)+'" height="'+(GH-GT-GB)+'" fill="transparent"/>'
body.innerHTML='<svg viewBox="0 0 '+GW+' '+GH+'">'+s+'</svg>'

let mn=9999,mx=0,sum=0
have.forEach(i=>{sum+=d.avg[i];mn=Math.min(mn,d.avg[i]);mx=Math.max(mx,pk(i))})
const cov=have.length*d.iv/60
const sumL='24 h &middot; low <b>'+mn+'</b> avg <b>'+Math.round(sum/have.length)+'</b> peak <b>'+mx+'</b>'
const sumR=(cov>=60?(cov/60).toFixed(1)+' h':Math.round(cov)+' min')+' of data'
const svg=body.firstChild,cx=svg.querySelector('#aqX'),dot=svg.querySelector('#aqD')
const rest=()=>{cx.setAttribute('opacity','0');dot.setAttribute('opacity','0');rl.innerHTML=sumL;rr.innerHTML=sumR}
// Scrub: snap to the nearest bucket that actually holds a sample, so dragging
// across a gap reads the real neighbour instead of going blank.
const at=e=>{
const r=svg.getBoundingClientRect(),i=Math.round(((e.clientX-r.left)/r.width*GW-GL)/((GW-GL-GR)/(n-1)))
let j=have[0]
have.forEach(k=>{if(Math.abs(k-i)<Math.abs(j-i))j=k})
cx.setAttribute('x1',X(j));cx.setAttribute('x2',X(j));cx.setAttribute('opacity','0.5')
dot.setAttribute('cx',X(j));dot.setAttribute('cy',Y(d.avg[j]));dot.setAttribute('opacity','1')
rl.innerHTML=aqTimeLabel(d,j)+' &middot; avg <b>'+d.avg[j]+'</b>'+(pk(j)>d.avg[j]?' peak <b>'+pk(j)+'</b>':'')
rr.innerHTML='<span class="aq-'+aqBand(d,d.avg[j]).toLowerCase()+'">'+aqBand(d,d.avg[j])+'</span>'}
svg.querySelector('#aqH').addEventListener('pointermove',at)
svg.querySelector('#aqH').addEventListener('pointerdown',at)
svg.addEventListener('pointerleave',rest)
rest()
}

let stripOn=[]
function renderStrips(list){
const d=document.getElementById('stripBtns')
if(d.childElementCount!==list.length){d.innerHTML='';list.forEach((s,i)=>{const b=document.createElement('button');b.onclick=()=>toggleStrip(i);d.appendChild(b)})}
list.forEach((s,i)=>{const b=d.children[i]
b.innerHTML=s.n+'<small>'+(s.cfg?(s.up?'online':'offline'):'no IP')+'</small>'
b.classList.toggle('on',s.on);b.disabled=!s.cfg})
stripOn=list.map(s=>s.on)
}
function toggleStrip(i){stripOn[i]=!stripOn[i];renderStripsLocal(i);fetch('/api/strips',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({strips:stripOn})}).then(r=>r.json()).then(d=>{if(d.success)getStatus()})}
function renderStripsLocal(i){document.getElementById('stripBtns').children[i].classList.toggle('on',stripOn[i])}

// "Editing" selector: -1 = all strips, otherwise the strip index the colour and
// brightness controls below will write to.
function renderSel(list){
const d=document.getElementById('selBtns')
if(d.childElementCount!==list.length+1){
d.innerHTML=''
const all=document.createElement('button');all.textContent='All';all.onclick=()=>pickStrip(-1);d.appendChild(all)
list.forEach((s,i)=>{const b=document.createElement('button');b.onclick=()=>pickStrip(i);d.appendChild(b)})
}
d.children[0].classList.toggle('active',selStrip===-1)
list.forEach((s,i)=>{const b=d.children[i+1]
b.innerHTML=s.n+'<span class="dot" style="background:'+s.c+'"></span>'
b.classList.toggle('active',selStrip===i);b.disabled=!s.cfg})
}
function pickStrip(i){selStrip=i;getStatus()}

function getStatus(){
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('sLed').textContent=d.ledOn?'ON':'OFF'
document.getElementById('sMode').textContent=d.mode.toUpperCase()
document.getElementById('sMotion').textContent=d.motion?'Yes':'No'
if(typeof d.temp!=='undefined')document.getElementById('sTemp').textContent=d.temp.toFixed(1)
if(typeof d.hum!=='undefined')document.getElementById('sHum').textContent=d.hum.toFixed(1)
if(typeof d.soundLevel!=='undefined')document.getElementById('sSound').textContent=d.soundLevel
if(typeof d.aq!=='undefined')renderAq(d)
// The colour/brightness controls show whichever strip is being edited; "All"
// falls back to the top-level values (which mirror strip 0).
const sel=(selStrip>=0&&d.strips&&d.strips[selStrip])?{c:d.strips[selStrip].c,b:d.strips[selStrip].b}:{c:d.color,b:d.brightness}
document.getElementById('preview').style.background=d.ledOn?sel.c:'#1a1a2e'
const hs=document.getElementById('hueSlider'),ss=document.getElementById('satSlider')
if(document.activeElement!==hs&&document.activeElement!==ss){const[h,s]=hex2hs(sel.c);hs.value=h;ss.value=s;const full=hs2hex(h,100);ss.style.background='linear-gradient(to right,#fff,'+full+')';document.querySelectorAll('.swatches button').forEach(b=>b.classList.toggle('sel',b.dataset.c===sel.c.toUpperCase()))}
if(document.activeElement!==document.getElementById('brightSlider')){document.getElementById('brightSlider').value=sel.b;document.getElementById('bVal').textContent=sel.b}
if(document.activeElement!==document.getElementById('toutInput'))document.getElementById('toutInput').value=d.timeout
if(document.activeElement!==document.getElementById('toffInput'))document.getElementById('toffInput').value=d.toff
document.querySelectorAll('.mode-btns button').forEach(b=>b.classList.remove('active'))
if(d.mode==='off')document.getElementById('bOff').classList.add('active')
else if(d.mode==='on')document.getElementById('bOn').classList.add('active')
else if(d.mode==='auto')document.getElementById('bAuto').classList.add('active')
else if(d.mode==='sound')document.getElementById('bSound').classList.add('active')
document.getElementById('fxCtrl').style.display=d.mode==='sound'?'':'none'
if(typeof d.musicFx!=='undefined')[0,1,2].forEach(n=>document.getElementById('fx'+n).classList.toggle('active',d.musicFx===n))
if(d.strips){renderStrips(d.strips);renderSel(d.strips)}
}).catch(()=>{})
}

getStatus()
setInterval(getStatus,2000)
</script>
</body>
</html>
)=====";

// Must be send_P. The plain send(code, type, const char*) overload copies the
// whole ~25KB page into one heap String first; once the heap is fragmented
// enough that no 25KB block exists, the copy silently comes back empty and the
// browser gets "200 OK, Content-Length: 0" - a white page - until a reboot.
// send_P streams straight from flash and needs no heap at all.
void handleRoot() {
  server.send_P(200, "text/html", MAIN_page);
}

void handleStatus() {
  StaticJsonDocument<2048> doc;

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
  doc["aqV"] = mq135Voltage;
  doc["aqPercent"] = airQualityPercent(mq135Raw);
  doc["aqLabel"] = airQualityLabel(mq135Raw);
  doc["aqReady"] = mq135Ready;
  doc["aqWarm"] = mq135Ready ? 0 : (MQ135_WARMUP_MS - (millis() - mq135WarmupStart)) / 1000;
  doc["aqm"] = aqThreshModerate; // effective raw levels, what the bar and graph draw
  doc["aqp"] = aqThreshPoor;
  doc["aqh"] = aqThreshHazardous;
  doc["aqmPct"] = aqPctModerate; // the settings: % above the learned baseline
  doc["aqpPct"] = aqPctPoor;
  doc["aqhPct"] = aqPctHazardous;
  doc["aqBase"] = aqBaseline;
  doc["aqBaseOk"] = aqBaseTrusted;
  doc["aqSmooth"] = (int)aqSmooth;
  doc["aqAlert"] = aqAlertActive;
  // Trend vs the oldest sample we have; aqTrendMin says how far back that is, so
  // the UI can say "over 4 min" instead of implying a full 10 minutes of history.
  if (aqHistCount > 1) {
    int oldest = aqHist[aqHistCount < AQ_HIST_SLOTS ? 0 : aqHistIdx];
    doc["aqTrend"] = mq135Raw - oldest;
    doc["aqTrendMin"] = aqHistCount - 1;
  }
  doc["rssi"] = WiFi.RSSI(); // weak signal here shows up as flaky OTA long before anything else breaks
  // Uptime, heap and Wi-Fi drop history, readable without a serial cable (see
  // node_health.h). heap should hover flat over hours; a steady fall is a leak.
  doc["up"] = millis() / 1000;
  nodeHealthReport(doc);
  // Local clock, so the timezone behind the daily AUTO reset can be checked
  // from outside. Absent until NTP has answered.
  time_t nowT = time(nullptr);
  if (nowT > 1700000000) {
    struct tm lt;
    localtime_r(&nowT, &lt);
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);
    doc["time"] = hhmm;
  }
  doc["autoAt"] = AUTO_MODE_HOUR;
  doc["wifiCh"] = WiFi.channel();
  doc["wifiPs"] = wifiSleepOn;
  doc["wifiProto"] = wifiProtoName(wifiProto);
  doc["wifiTxp"] = (int)WiFi.getTxPower(); // quarter-dBm: 78 = 19.5dBm (max)

  // Top-level color/brightness mirror strip 0 - kept so anything written
  // against the pre-per-strip API still reads something sensible.
  char hexColor[8];
  snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", stripColor[0].r, stripColor[0].g, stripColor[0].b);
  doc["color"] = hexColor;
  doc["brightness"] = stripBri[0];
  doc["timeout"] = motionTimeout / 1000;

  // n=name, on=enabled, cfg=strip exists (slave has an IP), up=acking over UDP,
  // c/b=this strip's own color and brightness
  char stripHex[NUM_STRIPS][8];
  JsonArray strips = doc.createNestedArray("strips");
  for (int i = 0; i < NUM_STRIPS; i++) {
    snprintf(stripHex[i], sizeof(stripHex[i]), "#%02X%02X%02X", stripColor[i].r, stripColor[i].g, stripColor[i].b);
    JsonObject s = strips.createNestedObject();
    s["n"] = stripNames[i];
    s["on"] = stripEnabled[i];
    s["cfg"] = (i == 0) || slaveIPs[i - 1][0] != '\0';
    s["up"] = (i == 0) ? true : (slaveIPs[i - 1][0] && slaveOnline[i - 1]);
    s["c"] = stripHex[i];
    s["b"] = stripBri[i];
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Data for the 24h graph. Sent oldest-first with an explicit start time and
// bucket width so the browser can put times on the x-axis itself; -1 slots are
// gaps (node off, or sensor warming up) and must be drawn as breaks rather than
// interpolated across. Hand-built rather than via ArduinoJson - 576 numbers in a
// StaticJsonDocument would want more RAM than the whole ring buffer.
void handleAqHistory() {
  uint32_t nowS = aqNowSeconds();

  String out;
  out.reserve(4300);
  out = "{\"iv\":";
  out += AQ_DAY_BUCKET_S;
  out += ",\"now\":";   out += nowS;
  out += ",\"clock\":"; out += aqClockSynced ? "true" : "false";
  out += ",\"warm\":";  out += mq135Ready ? "true" : "false";
  out += ",\"aqm\":";   out += aqThreshModerate;
  out += ",\"aqp\":";   out += aqThreshPoor;
  out += ",\"aqh\":";   out += aqThreshHazardous;
  out += ",\"aqBase\":"; out += aqBaseline;

  if (!aqDayHasData) {
    out += ",\"t0\":"; out += nowS;
    out += ",\"avg\":[],\"peak\":[]}";
    server.send(200, "application/json", out);
    return;
  }

  // Slot 0 of the response is the oldest bucket in the window. Signed: on the
  // millis() fallback the window can start "before" the epoch, which is
  // meaningless but harmless since the UI then labels relative times anyway.
  long t0 = ((long)aqDayNewestBucket - (AQ_DAY_SLOTS - 1)) * AQ_DAY_BUCKET_S;
  out += ",\"t0\":"; out += t0;

  uint16_t oldest = (aqDayNewestIdx + 1) % AQ_DAY_SLOTS;
  out += ",\"avg\":[";
  for (int i = 0; i < AQ_DAY_SLOTS; i++) {
    if (i) out += ',';
    out += aqDayAvg[(oldest + i) % AQ_DAY_SLOTS];
  }
  out += "],\"peak\":[";
  for (int i = 0; i < AQ_DAY_SLOTS; i++) {
    if (i) out += ',';
    out += aqDayPeak[(oldest + i) % AQ_DAY_SLOTS];
  }
  out += "]}";

  server.send(200, "application/json", out);
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
  markSettingsDirty();

  server.send(200, "application/json", "{\"success\":true}");
}

void handleSetLed() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  // "strip": which strip to edit. Omitted or -1 applies to every strip, which
  // is both the old single-strip behaviour and the UI's "All" selection.
  int target = doc.containsKey("strip") ? (int)doc["strip"] : -1;
  if (target >= NUM_STRIPS) { server.send(400, "application/json", "{\"success\":false}"); return; }
  int from = target < 0 ? 0 : target;
  int to = target < 0 ? NUM_STRIPS - 1 : target;

  if (doc.containsKey("color")) {
    String c = doc["color"];
    if (c.startsWith("#")) {
      long hex = strtol(c.c_str() + 1, NULL, 16);
      CRGB col((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
      for (int i = from; i <= to; i++) stripColor[i] = col;
    }
  }
  if (doc.containsKey("brightness")) {
    uint8_t b = constrain((int)doc["brightness"], 1, 255); // 1 = UI slider minimum; 0 would look like a dead strip
    for (int i = from; i <= to; i++) stripBri[i] = b;
    if (from == 0) FastLED.setBrightness(stripBri[0]);
  }
  markSettingsDirty();

  // Slaves dedupe on their own resolved state, so a change that only affects
  // one strip still needs a push - force it rather than waiting up to 3s.
  syncStrips(true);

  server.send(200, "application/json", "{\"success\":true}");
}

void handleSetSettings() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  if (doc.containsKey("timeout")) {
    motionTimeout = (unsigned long)constrain((long)doc["timeout"], 1L, 3600L) * 1000;
  }
  if (doc.containsKey("toff")) {
    tempOffset = constrain((float)doc["toff"], -10.0f, 10.0f);
  }
  if (doc.containsKey("musicFx")) {
    musicFx = constrain((int)doc["musicFx"], 0, 2);
  }
  bool aqChanged = false;
  // Percent above the learned baseline. A standing episode isn't cut short
  // here: if the new levels put the air below Moderate it ends through the
  // normal 10-minute clear, so edits can't produce an alert/all-clear burst.
  if (doc.containsKey("aqmPct")) { aqPctModerate = doc["aqmPct"]; aqChanged = true; }
  if (doc.containsKey("aqpPct")) { aqPctPoor = doc["aqpPct"]; aqChanged = true; }
  if (doc.containsKey("aqhPct")) { aqPctHazardous = doc["aqhPct"]; aqChanged = true; }
  if (aqChanged) {
    sanitizeAqThresholds();
    Serial.printf("[MQ135] levels now +%d/+%d/+%d%% over baseline %d = %d/%d/%d raw\n",
      aqPctModerate, aqPctPoor, aqPctHazardous, aqBaseline,
      aqThreshModerate, aqThreshPoor, aqThreshHazardous);
  }
  markSettingsDirty();

  server.send(200, "application/json", "{\"success\":true}");
}

void handleSetStrips() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  JsonArray arr = doc["strips"];
  if (arr.isNull()) { server.send(400, "application/json", "{\"success\":false}"); return; }

  for (int i = 0; i < NUM_STRIPS && i < (int)arr.size(); i++) {
    stripEnabled[i] = arr[i].as<bool>();
  }
  markSettingsDirty();

  // The global on/color/brightness didn't change, only who it applies to, so
  // the normal dedupe would swallow this - force the push.
  syncStrips(true);

  server.send(200, "application/json", "{\"success\":true}");
}

// POST /api/wifi - diagnostic link knobs, for A/B testing a bad link without a
// reflash per guess. Deliberately not saved: any reboot, including a
// node_health self-heal, puts the defaults back, so a setting that loses the
// link can't strand the node. {"sleep":bool, "txp":8..78, "proto":"b|bg|bgn"}
void handleSetWifi() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"success\":false}"); return; }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false}"); return; }

  if (doc.containsKey("sleep")) {
    wifiSleepOn = doc["sleep"];
    WiFi.setSleep(wifiSleepOn);
  }
  if (doc.containsKey("txp")) {
    WiFi.setTxPower((wifi_power_t)constrain((int)doc["txp"], 8, 78));
  }
  if (doc.containsKey("proto")) {
    const char* p = doc["proto"] | "bgn";
    uint8_t v = !strcmp(p, "b")  ? WIFI_PROTOCOL_11B :
                !strcmp(p, "bg") ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G) :
                                   (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (v != wifiProto) {
      wifiProto = v;
      esp_wifi_set_protocol(WIFI_IF_STA, v);
      // The AP only picks rates from what we offered at association, so this
      // needs a rejoin - from loop(), once this response has gone out.
      wifiRejoinAt = millis() + 500;
    }
  }
  Serial.printf("[WiFi] knobs: sleep=%d txp=%d proto=%s\n", wifiSleepOn, (int)WiFi.getTxPower(), wifiProtoName(wifiProto));
  server.send(200, "application/json", "{\"success\":true}");
}

void handleNotFound() {
  String msg = "File Not Found\nURI: " + server.uri();
  server.send(404, "text/plain", msg);
}

unsigned long lastSerialPrint = 0;
const unsigned long serialPrintInterval = 2000;
volatile bool otaActive = false; // set in ArduinoOTA.onStart, see loop()

void setupServer() {
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/aqhistory", handleAqHistory);
  server.on("/api/mode", HTTP_POST, handleSetMode);
  server.on("/api/led", HTTP_POST, handleSetLed);
  server.on("/api/settings", HTTP_POST, handleSetSettings);
  server.on("/api/strips", HTTP_POST, handleSetStrips);
  server.on("/api/wifi", HTTP_POST, handleSetWifi);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Server started");
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  // Modem sleep parks the radio between DTIM beacons, which pushed LAN ping
  // latency to ~50ms average with 110ms spikes. espota does a synchronous
  // round-trip per 1KB chunk (~1000 of them) and the device aborts the update
  // if any single chunk gap exceeds ~1s, so that jitter made OTA fail most
  // attempts. Costs a little idle power; this node is mains-powered.
  WiFi.setSleep(false);
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
    // TZ only changes localtime() (used by checkDailyAuto); time() stays UTC
    // epoch, which is what the 24h graph sends - the browser formats its x-axis
    // in its own zone. A synced clock is also what makes the history survive a
    // reboot - see aqDayResume().
    configTzTime(LOCAL_TZ, "pool.ntp.org", "time.nist.gov");
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
        Serial.println("  strips     - Show which LED strips are enabled");
        Serial.println("  status     - Show all status");
        Serial.println("  mic        - Read MAX9814 mic level");
        Serial.println("  aq         - Read MQ135 air quality sensor");
        Serial.println("  aqset a b c- Set moderate/poor/hazardous, % above the learned baseline");
        Serial.println("  aqday      - Summarize the 24h history behind the web graph");
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
          if (!slaveIPs[i][0]) {
            Serial.printf("Slave %d: (not configured)\n", i + 1);
          } else if (slaveOnline[i]) {
            Serial.printf("Slave %d (%s): ONLINE (ack %lus ago)\n", i + 1, slaveIPs[i],
              (millis() - slaveLastSeen[i]) / 1000);
          } else {
            Serial.printf("Slave %d (%s): OFFLINE (%s)\n", i + 1, slaveIPs[i],
              slaveLastSeen[i] ? "was online earlier" : "never acked");
          }
        }
      } else if (cmdBuffer == "strips") {
        for (int i = 0; i < NUM_STRIPS; i++) {
          Serial.printf("Strip %d %-8s: %-8s #%02X%02X%02X bri=%-3d ", i + 1, stripNames[i],
            stripEnabled[i] ? "ENABLED" : "disabled",
            stripColor[i].r, stripColor[i].g, stripColor[i].b, stripBri[i]);
          if (i == 0) Serial.println("[this ESP32]");
          else if (!slaveIPs[i - 1][0]) Serial.println("(no IP configured)");
          else Serial.printf("%s %s\n", slaveIPs[i - 1], slaveOnline[i - 1] ? "ONLINE" : "OFFLINE");
        }
        if (!stripsUniform()) Serial.println("Note: UDP broadcast discovery is paused while strips differ.");
      } else if (cmdBuffer == "status") {
        int val = digitalRead(LD2410_PIN);
        Serial.printf("OUT pin (GPIO%d): %s\n", LD2410_PIN, val == HIGH ? "PRESENCE" : "NONE");
        Serial.printf("motionDetected: %s\n", motionDetected ? "true" : "false");
        const char* modeName = ledMode == OFF ? "OFF" : (ledMode == ON ? "ON" : (ledMode == SOUND ? "SOUND" : "AUTO"));
        Serial.printf("LED mode: %s, LEDs: %s\n", modeName, getLedOn() ? "ON" : "OFF");
        Serial.printf("Temp: %.1f C, Humidity: %.1f %%\n", currentTemperature, currentHumidity);
        Serial.printf("Sound level: %d/255 (p2p=%d, bass=%.2f treb=%.2f, ceiling=%.0f)\n",
          soundLevel, soundPeakToPeak, bassEnv, trebEnv, envMax);
        Serial.printf("Air quality: raw=%d volt=%.2fV [%s] %s\n", mq135Raw, mq135Voltage,
          airQualityLabel(mq135Raw), mq135Ready ? "" : "(warming up)");
        Serial.printf("Uptime %lus | heap free=%u min=%u largest block=%u\n", millis() / 1000,
          ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
      } else if (cmdBuffer == "mic") {
        Serial.printf("Mic level: %d/255  peak-to-peak=%d  auto-gain ceiling=%.0f\n", soundLevel, soundPeakToPeak, envMax);
        Serial.printf("Envelopes: bass=%.2f treble=%.2f total=%.2f | beatPulse=%.2f lastBeat=%lums ago\n",
          bassEnv, trebEnv, totalEnv, beatPulse, millis() - lastBeatMs);
        Serial.printf("Music effect: %d (0=Pulse 1=Waves 2=Spectrum)\n", musicFx);
        Serial.println("Make noise near the mic and run this again - level/peak-to-peak should rise.");
      } else if (cmdBuffer == "aq" || cmdBuffer == "air") {
        Serial.printf("MQ135 raw=%d  voltage=%.2fV  scale=%d%%  [%s]\n", mq135Raw, mq135Voltage,
          airQualityPercent(mq135Raw), airQualityLabel(mq135Raw));
        if (!mq135Ready) {
          Serial.printf("Still warming up (%lu s remaining)\n", (MQ135_WARMUP_MS - (millis() - mq135WarmupStart)) / 1000);
        }
        Serial.printf("Baseline %d (%s), smoothed %d | levels +%d/+%d/+%d%% = moderate>=%d poor>=%d hazardous>=%d\n",
          aqBaseline, aqBaseTrusted ? "learned" : "still learning, alerts off", (int)aqSmooth,
          aqPctModerate, aqPctPoor, aqPctHazardous, aqThreshModerate, aqThreshPoor, aqThreshHazardous);
        Serial.printf("Alert episode: %s\n", aqAlertActive ? "active" : "none");
        if (aqHistCount > 1) {
          int oldest = aqHist[aqHistCount < AQ_HIST_SLOTS ? 0 : aqHistIdx];
          Serial.printf("Trend: %+d over the last %d min\n", mq135Raw - oldest, aqHistCount - 1);
        } else {
          Serial.println("Trend: collecting (one sample per minute)");
        }
        Serial.println("Change them with: aqset <moderate%> <poor%> <hazardous%>");
      } else if (cmdBuffer.startsWith("aqset")) {
        int m1, p1, h1;
        if (sscanf(cmdBuffer.c_str(), "aqset %d %d %d", &m1, &p1, &h1) == 3) {
          aqPctModerate = m1; aqPctPoor = p1; aqPctHazardous = h1;
          sanitizeAqThresholds();
          markSettingsDirty();
          Serial.printf("Levels set: +%d/+%d/+%d%% over baseline %d = %d/%d/%d raw (now %d -> %s)\n",
            aqPctModerate, aqPctPoor, aqPctHazardous, aqBaseline,
            aqThreshModerate, aqThreshPoor, aqThreshHazardous, mq135Raw, airQualityLabel(mq135Raw));
        } else {
          Serial.println("Usage: aqset <moderate%> <poor%> <hazardous%>   e.g. aqset 30 60 100");
        }
      } else if (cmdBuffer == "aqday") {
        int filled = 0, mn = 4095, mx = 0;
        long sum = 0;
        for (int i = 0; i < AQ_DAY_SLOTS; i++) {
          if (aqDayAvg[i] < 0) continue;
          filled++;
          sum += aqDayAvg[i];
          if (aqDayAvg[i] < mn) mn = aqDayAvg[i];
          if (aqDayPeak[i] > mx) mx = aqDayPeak[i];
        }
        if (!filled) {
          Serial.printf("24h history: empty (%s)\n",
            aqDayPending ? "restored from NVS, waiting on the clock" :
            (mq135Ready ? "nothing recorded yet" : "sensor still warming up"));
        } else {
          Serial.printf("24h history: %d/%d buckets = %d min of data | min=%d avg=%ld peak=%d\n",
            filled, AQ_DAY_SLOTS, filled * (AQ_DAY_BUCKET_S / 60), mn, sum / filled, mx);
        }
        Serial.printf("Bucket %lu s, clock %s, saved every %lu min\n",
          (unsigned long)AQ_DAY_BUCKET_S,
          aqClockSynced ? "NTP synced (history persists)" : "not synced (relative timeline, not saved)",
          AQ_DAY_SAVE_INTERVAL / 60000UL);
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
  for (int i = 0; i < NUM_SLAVES; i++)
    Serial.printf("  Slave %d: %s\n", i + 1, slaveIPs[i][0] ? slaveIPs[i] : "(not configured)");
  Serial.println();
  Serial.println("Type 'help' for diagnostic commands.");
  Serial.println();

  loadSettings(); // before setupPins so the restored brightness is applied
  aqDayLoad();    // needs prefs open, which loadSettings() does
  setupPins();
  // Without this, "millis() - 0 < timeout" counts boot as recent motion and
  // AUTO mode lights the strip for the first timeout period after every reboot.
  lastMotionTime = millis() - motionTimeout - 1;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  setupServer();
  startWiFi();
  udp.begin(UDP_PORT); // bound socket: outgoing sync + incoming slave acks

  ArduinoOTA.setHostname("esp32-master");
  ArduinoOTA.setPassword(otaPassword);
  // ArduinoOTA aborts the transfer if a chunk gap exceeds _ota_timeout, which
  // defaults to 1000ms. This LAN runs ~50ms average with ~110ms peaks and
  // intermittent 3-6% loss, so a single TCP retransmit could blow that budget
  // and drop the update mid-flight - the reason OTA only landed about 1 attempt
  // in 5. 10s costs nothing on a healthy link and absorbs the bad ones.
  ArduinoOTA.setTimeout(10000);
  ArduinoOTA.onStart([]() {
    // The normal loop only reaches ArduinoOTA.handle() every ~15ms (sampleMic's
    // 128 analogReads + delay(10) + the odd blocking DHT/TCP call), which
    // starves the OTA socket and aborts the transfer partway. Park the strip
    // and let loop() spin on handle() alone until the update finishes.
    otaActive = true;
    nodeHealthOtaStart();
    FastLED.clear();
    FastLED.show();
    // Last chance to keep the 24h history across the reboot this update ends
    // in. One ~2.3KB NVS write, tens of ms - nowhere near the 10s chunk budget.
    aqDaySave();
    Serial.println("OTA: start");
  });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: done"); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaActive = false; // failed update: fall back to normal loop duties
    nodeHealthOtaError();
    Serial.printf("OTA: error %d\n", e);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");

  nodeHealthBegin();
}

void loop() {
  ArduinoOTA.handle();
  if (otaActive) return; // nothing else until the update lands (or errors out)

  // Reconnect, and the reboot if it never comes back, live in node_health.h.
  // aqDaySave so a self-heal reboot doesn't cost up to 30 min of graph.
  if (nodeHealthLoop(ssid, password, aqDaySave)) {
    server.handleClient();
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
    syncStrips(true, true);
  }

  processSerialCommands();

  sampleMic();
  processSlaveAcks();
  checkDailyAuto();
  persistSettings();

  unsigned long m = millis();
  // Single read per interval: the DHT22's minimum sampling period is 2s, so
  // the old 3-reads-in-300ms "average" was the same measurement three times -
  // and its delays visibly froze the music animation every 10s.
  if (m - lastSensorRead >= sensorInterval) {
    lastSensorRead = m;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > 0 && t < 50 && h > 0 && h <= 100) {
      currentTemperature = t + tempOffset;
      currentHumidity = h;
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
    if (mq135Ready) aqDayRecord(mq135Raw); // warm-up curve is not air quality
  }

  aqDayResume(); // no-op once the clock has settled the restored history
  if (m - lastAqDaySave >= AQ_DAY_SAVE_INTERVAL) {
    lastAqDaySave = m;
    aqDaySave();
  }

  // Only log history once the sensor is warm, otherwise the trend spends the
  // first 10 minutes after every boot reporting the warm-up curve as "improving".
  if (mq135Ready && m - lastAqHist >= AQ_HIST_INTERVAL) {
    lastAqHist = m;
    aqHist[aqHistIdx] = mq135Raw;
    aqHistIdx = (aqHistIdx + 1) % AQ_HIST_SLOTS;
    if (aqHistCount < AQ_HIST_SLOTS) aqHistCount++;
    aqUpdateBaseline(); // same once-a-minute cadence; the 24h ring it reads moves slower still
  }

  if (wifiRejoinAt && (long)(millis() - wifiRejoinAt) >= 0) {
    wifiRejoinAt = 0;
    WiFi.reconnect(); // shows up once in wifiDrops - that one is self-inflicted
  }

  updateLEDs();
  delay(10);
}
