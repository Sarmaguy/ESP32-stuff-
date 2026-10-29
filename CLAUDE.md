# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-node ESP32 LED controller built with PlatformIO/Arduino. A **master** drives its own
WS2812B strip plus sensors (presence, temperature/humidity, microphone, air quality), serves a
web UI, and pushes LED state to one or more **slaves**, which are dumb strip followers. Both
node types live in this one repo and are selected by PlatformIO environment.

- `src/main.cpp` — master firmware (everything: sensors, web UI, sync, notifications)
- `src/slave_main.cpp` — slave firmware (receive state → render strip → ack)
- `include/led_sync.h` — the shared wire format; **both** builds include it so the protocol
  cannot silently drift between them
- `include/node_health.h` — shared Wi-Fi recovery, loop watchdog and health counters (see
  "Staying reachable" below), shared for the same reason

## Build / upload

`build_src_filter` in `platformio.ini` is what keeps the two `main`s from colliding, so always
build via an environment — never plain `pio run`.

```bash
pio run -e master                    # build master
pio run -e slave                     # build slave
pio run -e master -t upload          # USB flash (add --upload-port COMx if needed)
pio run -e slave -t upload --upload-port COM6   # slave has enumerated as COM6 on this machine
pio run -e master -t upload -t monitor
pio device list                      # find the COM port / check anything is plugged in
```

On this machine, bare `pio` is a stray pip install (Core 6.1.19) while the VS Code extension
uses `~/.platformio/penv/Scripts/pio.exe` (6.2.0). When the two alternate, PlatformIO wipes
`.pio/build` mid-run, which shows up as `.sconsign*.tmp` / `opening dependency file ... .d: No
such file` errors that look like a broken build or a failed OTA. Use the penv one.

OTA environments (`master_ota`, `slave_ota`) live in `platformio_local.ini`, which is gitignored
because it embeds the OTA password. Copy `platformio_local.ini.example` and fill it in. Same for
`include/secrets.h` (from `secrets.h.example`) — Wi-Fi credentials, ntfy topic, OTA password.

There are no tests and no linter in this project.

## OTA uploads are unreliable on the master — read this before chasing it

**OTA to the master succeeds roughly 1 attempt in 5. This is a known, measured network
limitation, not a bug to re-diagnose.** Slave OTA works first try.

Two rules that matter more than the cause:

1. **`espota` reports `Error Uploading` even on attempts that succeed.** Its exit code is
   worthless here. *Always* verify against the device instead — `curl http://<ip>/api/status`
   and check whether the change you made is actually present. Concluding "it didn't flash" from
   the exit code has already produced one confidently wrong diagnosis.
2. **A failed OTA does not brick anything.** The ESP32 rolls back and reboots the old image. But
   each attempt does reboot it and restart the MQ135 3-minute warm-up, so don't retry blindly.

Mechanism: espota does a synchronous round-trip per 1024-byte chunk (~1030 of them for this
image), and the ESP32 aborts the update if any single chunk gap exceeds ~1s, so one bad
retransmit drops the connection mid-transfer. `--timeout` does not help; it only covers the
invitation socket, and the transfer timeout at `espota.py:178` is hardcoded.

**What actually works (2026-10-19):** use a copy of `espota.py` with line 178 changed from
`settimeout(10)` to `settimeout(40)`, run directly against `.pio/build/<env>/firmware.bin`. After
~25 failed stock attempts that day, the patient copy landed on the master on its first try and
rode out an 11s Wi-Fi outage mid-transfer. TCP survives these short dropouts; stock espota just
gives up first. This only helps if the *device* is patient too: `ArduinoOTA.setTimeout(10000)`.
The master always had that. The slave didn't (1s default, ~4s with re-prompts) until
2026-10-19, so a slave still on older firmware can't be helped from the PC side.

**The "the LAN is lossy" explanation that used to live here was wrong — corrected 2026-08-01.**
It claimed 50ms average / ~110ms peak latency and 3–6% packet loss as a property of the network.
Re-measured from the Windows box on the same AP: **gateway 192.168.0.1 is 0% loss over 60 pings,
2–6ms, avg 3ms**, and 1.1.1.1 is 1% / 21ms. The air is clean — the AP has no other Wi-Fi
networks in scan range at all. What was actually being measured was the ESP32s' own modem sleep:

- Ping RTT to a slave is a **sawtooth** — `14 38 48 63 77 94 103 127 | 20 46 51 64 84 ...` —
  climbing and resetting every ~8 probes, avg 73ms. That shape is DTIM wake scheduling, not
  network loss. The apparent "1–3% loss" is ICMP dropped while the radio is parked.
- `slave_main.cpp` never called `WiFi.setSleep(false)` at all; only the master did. Fixed.
- Both reconnect paths used to do `WiFi.mode(WIFI_OFF)` → `WIFI_STA` → `begin()`, which **resets
  power save to the default**, silently undoing `startWiFi()`'s `setSleep(false)`. Reconnect now
  lives in `node_health.h` and never switches the radio off, so this can't recur. If you ever
  reintroduce a `WIFI_OFF`, re-apply `setSleep(false)` after it.

So treat OTA flakiness as unquantified again rather than as a known network floor. The master was
powered down when this was measured, so its own latency was **not** re-tested; re-measure it
before drawing conclusions. RSSI is still worth checking — `/api/status` reports `rssi` without
needing a serial cable, and this master has historically read -63 to -71 dBm versus -58 to -61 for
the slaves, so a weak link may still be a contributing factor on top of the power-save bug.

**Before firing an attempt, poll `/api/status` until it returns 200.** A prior failed attempt
rolls back and reboots, which takes a few seconds; starting the next `espota` run while the device
is still mid-boot produces a fast, spurious failure indistinguishable from a mid-transfer one, and
inflates the apparent failure count. Confirmed working 2026-07-27: six attempts with this
readiness check between them, succeeded on the 6th, each of the five failures independently
verified (not inferred from exit code) via `/api/status` before retrying.

If OTA won't land after a handful of tries, **flash over USB** (`pio run -e master -t upload`);
that always works and is faster than fighting it.

Separately, `loop()` has an `otaActive` guard (`ArduinoOTA.onStart` sets it; `loop()`
early-returns) so the heavy per-iteration work — `sampleMic()`'s 128 blocking `analogRead()`s,
`FastLED.show()`, DHT reads, blocking `client.connect()` calls — is skipped during an update.
Keep any new blocking work in `loop()` on the guarded side of that check. Note this was
originally added believing loop starvation *was* the OTA cause; it is not (ESP32's
`ArduinoOTA::handle()` blocks internally in `_runUpdate()` for the whole transfer anyway), but
the guard is still correct and worth preserving.

## Architecture

### Strips and addressing

Strips are numbered 0..N: **strip 0 is the master's own strip**, strips 1..`NUM_SLAVES` map to
`slaveIPs[]` in order. `stripEnabled[]` gates each one individually while mode, color and
brightness stay shared. Slave IPs are compile-time constants; blank entries (`""`) are skipped
everywhere and render as unconfigured in the UI.

`NUM_LEDS` differs per node (master 100, slave 122) — it is a `#define` in each `.cpp`, not shared.

### Master → slave sync (two paths, one state)

`broadcastLedState()` is the single choke point:

1. **UDP unicast** per configured slave — the only path that can address one strip, so this is
   where the per-strip enable is applied (`on && stripEnabled[i + 1]`).
2. **UDP broadcast** — auto-discovery for slaves not yet in `slaveIPs[]`. `UdpLedPkt` carries no
   strip addressing, so every slave applies it; a disabled slave would see `on=1` here and undo
   its own unicast "off". The broadcast is therefore **suppressed whenever any strip is
   disabled**. This deliberately avoids bumping `LED_SYNC_VERSION`, which would force reflashing
   every deployed slave. It is **rate-limited to once per `slaveSyncInterval`**. Until
   2026-10-27 it went out on every `syncStrips()` call, i.e. every `loop()`, ~20/s. Each one was
   relayed by the AP at its slowest rate and acked by every slave (~14M packets per slave in 8
   days).
3. **HTTP `POST /set`** — slow fallback, only on the periodic 3s sync. That is the separate
   `includeHttp` argument, *not* `force`. They used to be one flag, which made the 20Hz music
   path open two TCP connections to the slaves every 50ms. Payload is built per slave because
   `on` differs by strip. Offline slaves are only retried every 30s because `connect()` to a dead
   host blocks for its full timeout.

Liveness (`slaveOnline[]`) comes from slaves' `UdpAckPkt` replies, not from whether an IP string
parses — an important past bug was stale `192.168.1.x` IPs on a `192.168.0.x` LAN silently
killing sync while blocking the loop on HTTP timeouts.

`notifySlaves()` dedupes against `lastSent*`. Changing something that alters *who* the state
applies to rather than the state itself (e.g. the strip mask) must call `notifySlaves(true)` or
the dedupe swallows it.

Sound mode bypasses the 3s sync and pushes at ~20Hz from `updateSoundLEDs()` instead, so the two
don't fight and flash stale color.

### Rendering

`updateLEDs()` tracks what is physically on the strip in statics and only calls `FastLED.show()`
when something changed (a full show costs ~3ms and used to run every loop). Note the distinction
it maintains: `shouldBeOn` is the *logical* state broadcast to slaves, `localOn` is what this
node's own strip renders (`shouldBeOn && stripEnabled[0]`). In sound mode the local strip is
blanked via `setBrightness(0)` rather than clearing the buffer, because `musicColor` still has to
be computed for the slaves.

### Settings

Persisted to NVS via `Preferences` behind a 3s debounce (`markSettingsDirty()` →
`persistSettings()`) so slider drags batch into one flash write. Strip enables are stored as a
bitmask. Air-quality level percentages ride the same path — see the calibration section below.

The 24h air-quality history is the one thing in NVS that does *not* use that path — it is ~2.3KB
of blob (`aqda`/`aqdp`/`aqdb`/`aqdi`) written on its own 30-minute timer plus a flush in
`ArduinoOTA.onStart`, because the debounced writer runs far too often for a payload that size.

### Notifications

Air-quality alerts POST to ntfy.sh. When they fire is the episode logic in the calibration
section below. TLS + HTTP takes seconds, which froze LEDs/web UI/OTA when done inline, so it
runs in a one-shot FreeRTOS task (`ntfyTask`, 10KB stack for the TLS handshake) with one send in
flight at a time. A send that finds another in flight is dropped, not queued. Episode state has
already advanced by then, so a dropped alert is not retried.

Keep the `WiFiClientSecure`/`HTTPClient` inside the inner `{}` block in `ntfyTask`.
`vTaskDelete(NULL)` never returns, so destructors of anything still in scope never run. Until
2026-10-19 that leaked the whole TLS session (~35KB of mbedTLS buffers) plus a socket on *every*
alert, because ntfy.sh answers keep-alive and `HTTPClient` defaults to reuse, so `http.end()`
left the session open. A few alerts in, the heap was too depleted to serve the web UI.

## HTTP API (master)

`GET /api/status` is the whole state; the UI polls it every 2s. `GET /api/aqhistory` backs the
24h graph (see below) and is only fetched while that graph is open. `POST /api/mode`, `/api/led`,
`/api/settings`, `/api/strips`. The web UI is a single `PROGMEM` string literal in `main.cpp` —
edit it there, there is no separate asset pipeline.

**Serve that page with `send_P`, never `send`.** In this framework (Arduino-ESP32 2.0.17),
`send(code, type, const char*)` copies all ~25KB into one heap `String`. Once the heap is
fragmented and no block that size exists, the copy silently comes back empty and the browser
gets `200 OK` with `Content-Length: 0`: a white page until reboot.

`/api/status` also reports `up` (seconds) plus the `node_health.h` fields below. Check those
first if the UI degrades after hours of uptime. It also has `time` (local HH:MM, absent until
NTP) and `autoAt`, the daily schedule below.

**Daily AUTO reset.** `checkDailyAuto()` puts the mode back to AUTO once per local day from
`AUTO_MODE_HOUR` (17:00). It catches up until midnight if the node was off at 17:00. It runs
once per day, stored in NVS `autod`, so a mode picked by hand afterwards sticks across reboots.
With nothing stored (first boot, or NVS wiped) after 17:00 it counts the day as done instead of
overriding the current mode.

`POST /api/wifi` sets **runtime-only** link knobs for diagnosis: `{"sleep":bool, "txp":8..78
(quarter-dBm), "proto":"b|bg|bgn"}`. A protocol change re-associates, because the AP only uses
rates offered at association. That rejoin counts once in `wifiDrops`. Nothing is saved, so any
reboot restores the defaults and a bad setting can't strand the node. `/api/status` echoes them
as `wifiPs`/`wifiTxp`/`wifiProto`, plus `wifiCh`.

Slaves expose `GET /status`, `GET /ping`, `POST /set`. `/status` carries the same health fields.

## Staying reachable (`node_health.h`)

Neither node can be reached over USB where it is installed. On 2026-10-19 the master and one
slave fell off Wi-Fi and never rejoined until power-cycled. The old reconnect switched the radio
off (re-initialising the whole Wi-Fi driver) the instant it saw a drop and then every 10s, which
also cancelled the core's own auto-reconnect. `nodeHealthLoop()` now escalates instead:

1. **0–30s:** leave it to the core's auto-reconnect.
2. **Every 30s after that:** `disconnect()` + `begin()`. The core stops retrying on its own for
   some reasons, e.g. `AUTH_FAIL`.
3. **3 min:** `ESP.restart()`. The master saves the 24h AQ history first.

The loop watchdog (60s) also reboots a node whose `loop()` stops returning. The OTA transfer
blocks inside `ArduinoOTA.handle()` for minutes, so `onStart`/`onError` must call
`nodeHealthOtaStart()`/`nodeHealthOtaError()`. Any new OTA hook needs the same, or every update
dies at the 60s mark.

Health fields, shared by both status endpoints:

| Field | Meaning |
|---|---|
| `boot` | why the last boot happened: `power`, `restart` (incl. OTA), `selfheal`, `crash`, `watchdog`, `brownout` |
| `heap` / `heapMin` / `heapBlock` | free heap now / lowest since boot / largest allocatable block. `heap` should stay flat; `heapBlock` falls first when the heap fragments |
| `wifiDrops` | working connections lost |
| `wifiDropWhy` | reason name for the latest drop; `LOST_IP` means a DHCP problem, not a radio one |
| `wifiLastWhy` | latest disconnect of any kind, including failed rejoins |
| `wifiOutageS` | length of the latest outage |
| `selfHeals` | reboots done because Wi-Fi never came back |

These counters live in `RTC_NOINIT` memory. They survive restarts, crashes and watchdog resets,
so after a self-heal they still explain it. They reset on power loss.

## Serial diagnostics

115200 baud, type `help`. Notably `slaves`, `strips`, `aq`, `aqset`, `aqday`, `mic`, `status`,
`testnotify`.

## Sensor calibration is hardware-specific

MQ135 readings are **raw ADC values from one uncalibrated sensor and its resistor divider**, not
ppm. `MQ135_DIVIDER_RATIO` must match the actual resistors (`R2/(R1+R2)`).

**Don't go back to fixed raw thresholds.** This unit's clean-air level was ~55–66 on 2026-07-26,
~108–112 a day later, ~110–140 mid-October, and 206–541 (median 321) by 2026-10-29. Every fixed
set went stale within days. The last one (220/221/290) sat below the median: it alerted all
day, and with Moderate and Poor 1 apart it flapped between alert and all-clear.

Since 2026-10-29 the levels are **relative to a learned baseline**:

- **The baseline** (`aqUpdateBaseline()`, once a minute and right after `aqDayResume()`): the
  10th percentile of the valid 5-minute averages in the 24h ring. In other words, "the cleanest
  the air got today". A percentile rather than the minimum, so one odd bucket can't drag it
  down. Alerts stay off until the ring holds `AQ_BASE_MIN_BUCKETS` (2h). The history persists
  across reboots, so that only applies to a fresh install or after more than 24h off.
- **The settings** are percentages above it: `aqPctModerate/Poor/Hazardous`, defaults
  30/60/100, NVS `aqmP/aqpP/aqhP`. They are set from the web UI inputs or
  `aqset <m%> <p%> <h%>`. The pre-percentage `aqm/aqp/aqh` NVS keys are deleted on boot.
  Percent rather than fixed offsets because the response scales with the baseline.
- **The effective raw levels** `aqThresh*` = baseline × (1 + pct/100), recomputed by
  `aqApplyLevels()`. Label, bar and graph read these, which is why the API's `aqm/aqp/aqh` are
  still raw numbers. `aqmPct/aqpPct/aqhPct`, `aqBase`, `aqBaseOk`, `aqSmooth` and `aqAlert` sit
  alongside them in `/api/status`.

**Alerts are episodes** (`checkAirQualityAlert()`), decided on `aqSmooth`, a ~1-minute EMA, and
never on single readings:

- **Start:** Poor held for 5 min sends one alert. A new episode can't alert within 60 min of
  the last one ending.
- **During:** one escalation if Hazardous holds 5 min, and a reminder every 3 h while still
  Poor.
- **End:** only after 10 min below Moderate, with a `low`-priority (silent) all-clear.

Replayed over the 2026-10-28/29 history, that was **2 notifications in ~19h** (Wed 18:20 and
Thu 10:45), versus about one every 30 min before. `aq_replay.py` in that session's scratchpad
did the replay; it is easy to rewrite from this description if the rules change.

If alerts fire on air that seems fine, raise **Poor**'s percentage. Don't touch raw values.

The MQ135 needs a 24–48h burn-in once, and the firmware enforces a 3-minute warm-up each boot
before it will alert or report a category. `/api/status` exposes `aqWarm` (seconds remaining),
plus `aqTrend`/`aqTrendMin` — the change over a ring buffer of one sample per minute, ~10 minutes
deep, which only fills once the sensor is warm so the warm-up curve isn't reported as "improving".

### The 24h history and its graph

A *second*, longer ring buffer backs the web UI's "Show 24 h graph" button: 288 buckets of 5
minutes each (`aqDayAvg[]`/`aqDayPeak[]`). Each bucket stores both the mean of the ~150 raw reads
that land in it **and** their peak — a 5-minute mean alone smooths away exactly the short spikes
(cooking, aerosols) the graph exists to show. Both are rewritten on every sample, so the newest
point is live rather than 5 minutes stale. Like the 10-minute trend, it only records once the
sensor is warm. Empty slots are `-1` and the UI draws them as breaks in the line; never
interpolate across them, they mean "node was off" or "still warming up".

Buckets are keyed to **wall-clock time**, which is why `startWiFi()` calls `configTzTime()`. That is
what lets the ring survive a reboot: on boot `aqDayLoad()` reads the blob but leaves it unusable
until `aqDayResume()` sees a synced clock and can tell how many buckets elapsed while the node was
off (it blanks those, or drops the lot if it is all older than 24h). The history uses `time()`,
which is **UTC epoch** whatever the TZ. `/api/aqhistory` sends epoch seconds and the browser
formats the x-axis in its own timezone. `LOCAL_TZ` (CET/CEST) only affects `localtime()`, which
nothing but the daily AUTO reset uses. Keep it that way: don't build history timestamps from
`localtime()`. With no NTP at all the history still records
against a monotonic count, the response sets `clock:false`, the UI switches to relative labels
(`-6h`), and nothing is persisted, because samples with no timestamp can't be placed after a
reboot.

`aqday` on serial summarizes the buffer (coverage, min/avg/peak, clock state) when the graph looks
wrong.

## Hardware documentation

`MAX9814_MQ135_WIRING.md` (mic + air quality, including the mandatory MQ135 voltage divider) and
`DHT22_WIRING.md` / `DHT22_QUICK_START.md` describe the currently wired sensors. The LD2410
presence sensor is only documented in the `main.cpp` pin defines.

**The LD2410B's built-in Bluetooth must stay off.** The "B" model advertises BLE by default, and
the module sits a few cm from the master's antenna. Until 2026-10-29 its bursts destroyed
full-size Wi-Fi frames while small ones got through. The web page stalled for 15s+ and OTA died,
yet ping, `/api/status` and the node_health counters all looked healthy. The same-time test
against a slave at similar RSSI was decisive: master 103/160 pings vs slave 160/160. With BT
off, the master got 153/160 and the page loaded in 0.13–0.22s. **Symptom to recognise:** loss
that grows with packet size (`ping -l 32 / 500 / 1000 / 1472`).

It was turned off from a phone with nRF Connect, because the HLKRadarTool app showed no toggle.
Write these one at a time to characteristic `FFF2` of service `FFF0`: password `HiLink`
`FDFCFBFA0800A80048694C696E6B04030201`, enable config `FDFCFBFA0400FF00010004030201`, BT off
`FDFCFBFA0400A400000004030201`, restart `FDFCFBFA0200A30004030201`. Success means the module
no longer appears in a BLE scan. A replacement module needs the same treatment. Re-enabling
needs the module's UART (TX/RX), which the firmware doesn't wire up.

This repo accumulated a pile of docs from unrelated earlier experiments; eleven BH1750
light-sensor files were deleted in July 2026 because they described a completely different
program. Two stragglers remain and are **not** about this firmware: `ESP32_Test_Guide.md` (a GPIO
2 blink test) and `model_explanation.md` (notes on a local LLM's context window). Don't use
either to infer current behavior, and be wary of adding new top-level docs that will rot the same
way — prefer comments next to the code they describe.
