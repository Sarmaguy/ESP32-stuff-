#pragma once
// Keeps a node reachable with nobody touching it, and records why it wasn't.
// Shared by master and slave for the same reason led_sync.h is: the two copies
// of the reconnect code already drifted once (only one restored setSleep).
//
// Neither node can be reached over USB where it is installed, and one that fell
// off Wi-Fi and never rejoined used to need someone to pull its power. Recovery
// now escalates instead:
//   0-30s  leave it to the core's own auto-reconnect, which normally rejoins in
//          seconds. The old loop code killed that by switching the radio off the
//          instant it saw a drop, and again every 10s - which also tore down and
//          re-initialised the whole Wi-Fi driver each time.
//   30s+   disconnect()/begin() every 30s. The core gives up by itself on some
//          reasons (e.g. AUTH_FAIL, which a noisy link can produce).
//   3min   reboot. A power cycle has always fixed it, so do the same thing.
// Separately, the loop watchdog reboots the node if loop() stops returning.
//
// The counters live in RTC memory, which survives ESP.restart(), crashes and
// watchdog resets (not power loss), so after a self-heal they still say why.

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#define WIFI_KICK_MS      30000UL
#define WIFI_SELFHEAL_MS  180000UL
#define LOOP_WDT_S        60  // generous: a stalled web client can hold one send for ~10s
#define NODE_RTC_MAGIC    0x4E480001UL
#define NH_REASON_NONE    0
#define NH_REASON_LOST_IP 255 // not an 802.11 code: lease lost while still associated

struct NodeHealthRtc {
  uint32_t magic;
  uint32_t wifiDrops;    // working connections lost
  uint32_t selfHeals;    // reboots done by nodeHealthLoop()
  uint32_t lastOutageS;  // length of the most recent outage
  uint8_t dropReason;    // why the most recent working connection was lost
  uint8_t lastReason;    // latest disconnect of any kind, incl. failed rejoins
  bool selfHealBoot;     // set just before a self-heal restart
};

RTC_NOINIT_ATTR static NodeHealthRtc nodeRtc;
static bool nhSelfHealBoot = false;
static volatile bool nhWasUp = false;  // event-side view: had an IP since the last drop
static bool nhLoopUp = false;          // loop-side view, for counting drops
static unsigned long nhDownSince = 0;  // millis() the current outage began; 0 = up
static unsigned long nhLastKick = 0;

static const char* nhReasonName(uint8_t r) {
  if (r == NH_REASON_LOST_IP) return "LOST_IP";
  return WiFi.disconnectReasonName((wifi_err_reason_t)r);
}

// Runs in the core's event task, not loop() - keep it to plain stores.
static void nhOnWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    nhWasUp = true;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r == WIFI_REASON_ASSOC_LEAVE) return; // our own disconnect(), not news
    if (r == NH_REASON_NONE) r = WIFI_REASON_UNSPECIFIED; // same remap the core does
    nodeRtc.lastReason = r;
    if (nhWasUp) { nhWasUp = false; nodeRtc.dropReason = r; }
  } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    if (nhWasUp) { nhWasUp = false; nodeRtc.dropReason = NH_REASON_LOST_IP; }
  }
}

static const char* nodeBootReason() {
  if (nhSelfHealBoot) return "selfheal";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power";
    case ESP_RST_SW:       return "restart"; // OTA lands here too
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:      return "resetpin";
    default:               return "other";
  }
}

// Call at the end of setup(), after the first WiFi.begin().
static void nodeHealthBegin() {
  if (nodeRtc.magic != NODE_RTC_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
    memset(&nodeRtc, 0, sizeof(nodeRtc));
    nodeRtc.magic = NODE_RTC_MAGIC;
  }
  nhSelfHealBoot = nodeRtc.selfHealBoot;
  nodeRtc.selfHealBoot = false;
  nhWasUp = WiFi.status() == WL_CONNECTED; // startWiFi() may already have connected
  WiFi.onEvent(nhOnWiFiEvent);
  esp_task_wdt_init(LOOP_WDT_S, true); // reconfigures the already-running TWDT
  enableLoopWDT();
}

// Call every loop(). Returns true while the node is on the network.
// beforeRestart gets a last chance to save state ahead of a self-heal reboot.
static bool nodeHealthLoop(const char* ssid, const char* password, void (*beforeRestart)() = nullptr) {
  unsigned long now = millis();
  // Associated-but-no-IP counts as down: a lease that failed to renew leaves
  // status() at WL_CONNECTED with nothing reachable.
  bool up = WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0;

  if (up) {
    if (nhDownSince) {
      nodeRtc.lastOutageS = (now - nhDownSince) / 1000;
      Serial.printf("[WiFi] back after %lus\n", (unsigned long)nodeRtc.lastOutageS);
      nhDownSince = 0;
    }
    nhLoopUp = true;
    return true;
  }

  if (!nhDownSince) {
    nhDownSince = now ? now : 1;
    nhLastKick = now;
    if (nhLoopUp) nodeRtc.wifiDrops++;
    nhLoopUp = false;
    Serial.printf("[WiFi] down (%s), leaving it to auto-reconnect first\n",
      nodeRtc.lastReason ? nhReasonName(nodeRtc.lastReason) : "no reason yet");
  }

  unsigned long down = now - nhDownSince;
  if (down >= WIFI_SELFHEAL_MS) {
    Serial.printf("[WiFi] down %lus, rebooting\n", down / 1000);
    nodeRtc.lastOutageS = down / 1000;
    nodeRtc.selfHeals++;
    nodeRtc.selfHealBoot = true;
    if (beforeRestart) beforeRestart();
    delay(100);
    ESP.restart();
  }
  if (now - nhLastKick >= WIFI_KICK_MS) {
    nhLastKick = now;
    Serial.printf("[WiFi] still down after %lus (%s), re-issuing begin()\n", down / 1000,
      nodeRtc.lastReason ? nhReasonName(nodeRtc.lastReason) : "no reason yet");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
  return false;
}

// Health fields for a status endpoint. heapBlock is the largest single
// allocation still possible - fragmentation shows up there before heap drops.
static void nodeHealthReport(JsonDocument& doc) {
  doc["boot"] = nodeBootReason();
  doc["heap"] = ESP.getFreeHeap();
  doc["heapMin"] = ESP.getMinFreeHeap();
  doc["heapBlock"] = ESP.getMaxAllocHeap();
  doc["wifiDrops"] = nodeRtc.wifiDrops;
  if (nodeRtc.dropReason) doc["wifiDropWhy"] = nhReasonName(nodeRtc.dropReason);
  if (nodeRtc.lastReason) doc["wifiLastWhy"] = nhReasonName(nodeRtc.lastReason);
  doc["wifiOutageS"] = nodeRtc.lastOutageS;
  doc["selfHeals"] = nodeRtc.selfHeals;
}

// The OTA transfer blocks inside ArduinoOTA.handle() for minutes, which the
// loop watchdog would read as a hang. Call these from onStart / onError.
static void nodeHealthOtaStart() { disableLoopWDT(); }
static void nodeHealthOtaError() { enableLoopWDT(); }
