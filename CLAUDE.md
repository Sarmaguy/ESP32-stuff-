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

Measured cause: espota does a synchronous round-trip per 1024-byte chunk (~1030 of them for this
image), and the ESP32 aborts the update if any single chunk gap exceeds ~1s. The LAN shows
50ms average / ~110ms peak latency with intermittent 3–6% packet loss to all three nodes, so a
retransmit occasionally blows that 1s budget and drops the connection mid-transfer. `--timeout`
does not help; it only covers the invitation socket, and the transfer timeout at
`espota.py:178` is hardcoded.

`WiFi.setSleep(false)` is set in `startWiFi()` for exactly this reason — modem sleep parks the
radio between DTIM beacons and was a large part of that latency. `/api/status` reports `rssi` so
link quality is checkable without a serial cable — this master typically reads -63 to -71 dBm,
noticeably weaker than the slaves (-58 to -61 dBm as seen from the master, -72 dBm as the slaves
see the AP). That marginal link is the actual reason retransmits are frequent enough to matter;
moving the master or the AP would reduce OTA flakiness more than any code change here.

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
   every deployed slave.
3. **HTTP `POST /set`** — slow fallback, only on the periodic 3s sync (`includeHttp`). Payload is
   built per slave because `on` differs by strip. Offline slaves are only retried every 30s
   because `connect()` to a dead host blocks for its full timeout.

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
bitmask. Air-quality thresholds ride the same path — see the calibration section below.

The 24h air-quality history is the one thing in NVS that does *not* use that path — it is ~2.3KB
of blob (`aqda`/`aqdp`/`aqdb`/`aqdi`) written on its own 30-minute timer plus a flush in
`ArduinoOTA.onStart`, because the debounced writer runs far too often for a payload that size.

### Notifications

Air-quality alerts POST to ntfy.sh. TLS + HTTP takes seconds, which froze LEDs/web UI/OTA when
done inline, so it runs in a one-shot FreeRTOS task (`ntfyTask`, 10KB stack for the TLS
handshake) with one send in flight at a time.

## HTTP API (master)

`GET /api/status` is the whole state; the UI polls it every 2s. `GET /api/aqhistory` backs the
24h graph (see below) and is only fetched while that graph is open. `POST /api/mode`, `/api/led`,
`/api/settings`, `/api/strips`. The web UI is a single `PROGMEM` string literal in `main.cpp` —
edit it there, there is no separate asset pipeline.

Slaves expose `GET /status`, `GET /ping`, `POST /set`.

## Serial diagnostics

115200 baud, type `help`. Notably `slaves`, `strips`, `aq`, `aqset`, `aqday`, `mic`, `status`,
`testnotify`.

## Sensor calibration is hardware-specific

MQ135 thresholds (`aqThreshModerate` / `aqThreshPoor` / `aqThreshHazardous`) are **raw ADC values
tuned to one physical sensor and its resistor divider**, not ppm. `MQ135_DIVIDER_RATIO` must
match the actual resistors (`R2/(R1+R2)`).

**The baseline drifts, so these are runtime settings, not constants.** They live in NVS
(`aqm`/`aqp`/`aqh`) and are editable from the web UI's "Air quality alert levels" inputs or the
`aqset <moderate> <poor> <hazardous>` serial command — retune there instead of reflashing, which
matters because master OTA lands about 1 in 5 tries. `sanitizeAqThresholds()` keeps them ordered
and in range on every path that writes them; `AQ_DEFAULT_*` in `main.cpp` are only the
first-boot fallback.

Measured drift on this unit: a clean-air baseline of ~55–66 on 2026-07-26 (breathing directly on
the sensor peaked ~125) had moved to a steady 108–112 by 2026-07-27 in a room that smelled fine
— which made the old 90/115/160 thresholds fire alerts on ordinary air. Current defaults
(130/155/195) assume a fresh-air floor near ~105 and preserve the deltas that calibration
implied. If alerts fire on air that seems fine, the baseline has moved again: read the live raw
value (the web UI shows it under the bar, or `/api/status` `aq`, or the `aq` serial command) and
raise the levels — don't guess placeholder values, which make the bands meaningless in both
directions.

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

Buckets are keyed to **wall-clock time**, which is why `startWiFi()` calls `configTime()`. That is
what lets the ring survive a reboot: on boot `aqDayLoad()` reads the blob but leaves it unusable
until `aqDayResume()` sees a synced clock and can tell how many buckets elapsed while the node was
off (it blanks those, or drops the lot if it is all older than 24h). NTP is requested as **UTC
with no offset** — `/api/aqhistory` sends epoch seconds and the browser formats the x-axis in its
own timezone, so the firmware never deals with DST. With no NTP at all the history still records
against a monotonic count, the response sets `clock:false`, the UI switches to relative labels
(`-6h`), and nothing is persisted, because samples with no timestamp can't be placed after a
reboot.

`aqday` on serial summarizes the buffer (coverage, min/avg/peak, clock state) when the graph looks
wrong.

## Hardware documentation

`MAX9814_MQ135_WIRING.md` (mic + air quality, including the mandatory MQ135 voltage divider) and
`DHT22_WIRING.md` / `DHT22_QUICK_START.md` describe the currently wired sensors. The LD2410
presence sensor is only documented in the `main.cpp` pin defines.

This repo accumulated a pile of docs from unrelated earlier experiments; eleven BH1750
light-sensor files were deleted in July 2026 because they described a completely different
program. Two stragglers remain and are **not** about this firmware: `ESP32_Test_Guide.md` (a GPIO
2 blink test) and `model_explanation.md` (notes on a local LLM's context window). Don't use
either to infer current behavior, and be wary of adding new top-level docs that will rot the same
way — prefer comments next to the code they describe.
