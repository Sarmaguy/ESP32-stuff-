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
link quality is checkable without a serial cable.

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
bitmask.

### Notifications

Air-quality alerts POST to ntfy.sh. TLS + HTTP takes seconds, which froze LEDs/web UI/OTA when
done inline, so it runs in a one-shot FreeRTOS task (`ntfyTask`, 10KB stack for the TLS
handshake) with one send in flight at a time.

## HTTP API (master)

`GET /api/status` is the whole state; the UI polls it every 2s. `POST /api/mode`, `/api/led`,
`/api/settings`, `/api/strips`. The web UI is a single `PROGMEM` string literal in `main.cpp` —
edit it there, there is no separate asset pipeline.

Slaves expose `GET /status`, `GET /ping`, `POST /set`.

## Serial diagnostics

115200 baud, type `help`. Notably `slaves`, `strips`, `aq`, `mic`, `status`, `testnotify`.

## Sensor calibration is hardware-specific

MQ135 thresholds (`AQ_THRESHOLD_MODERATE` / `_POOR` / `_HAZARDOUS`) are **raw ADC values tuned to
one physical sensor and its resistor divider**, not ppm. They were calibrated against a measured
clean-air baseline and a breath-exposure peak. If the sensor or the divider changes, re-measure
via `/api/status` (`aq` field) or the `aq` serial command rather than guessing — placeholder
values that don't match the hardware make the Good/Poor/Hazardous bands meaningless in both
directions. `MQ135_DIVIDER_RATIO` must match the actual resistors (`R2/(R1+R2)`).

The MQ135 needs a 24–48h burn-in once, and the firmware enforces a 3-minute warm-up each boot
before it will alert or report a category.

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
