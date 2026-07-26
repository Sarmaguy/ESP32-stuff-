# MAX9814 Mic + MQ135 Air Quality - Wiring Guide

## MAX9814 electret microphone (sound-reactive LEDs)

| ESP32 Pin | MAX9814 Pin |
|-----------|-------------|
| 3.3V      | VDD         |
| GND       | GND         |
| GPIO34    | OUT         |

- **Power it from 3.3V, not 5V.** The MAX9814's output is biased around a fixed
  ~1.25V level regardless of supply, but keeping VDD at 3.3V guarantees the
  output swing never exceeds the ESP32 ADC's safe 0-3.3V input range.
- Leave the `GAIN` pin floating for the default 60dB gain (good for normal
  room volume). Tie it to VDD for 40dB (loud sources) or GND for 50dB if the
  strip is maxing out constantly.
- Leave `A/R` (attack/release) floating - default timing works fine for a VU
  effect.
- GPIO34 is an input-only ADC1 pin, so it reads reliably even while Wi-Fi is
  active (ADC2 pins can't be trusted while Wi-Fi is on).

**Using it:** switch the web UI to **Music** mode (or POST `{"mode":"sound"}`
to `/api/mode`), then pick one of three effects (all designed for LEDs whose
glow you see indirectly - behind shelves/furniture - so they use whole-glow
dynamics and beat detection rather than per-pixel meters you couldn't see):

- **Pulse** - detects beats (bass energy spikes vs. its own running average,
  the same trick WLED's audio-reactive mod uses) and on every beat snaps the
  strip to a new color and punches the brightness, which breathes back down
  until the next hit. Most dramatic with rhythmic music.
- **Waves** - broad blobs of color (~30-40 LEDs wide, so they read as glow
  drifting along the shelf) that move faster the louder the music is; beats
  surge the whole glow and rotate the palette.
- **Spectrum** - the glow's color follows the music's tone: bass = red,
  mids = green, treble = blue, blended into one wash. A bass drop literally
  turns the room red.

Beat detection self-calibrates to volume within a couple of seconds of music
starting. Use the `mic` serial command to watch envelopes/beat state while
tuning gain and placement.

## MQ135 air quality sensor

| ESP32 Pin | MQ135 Module Pin |
|-----------|-------------------|
| 5V (VIN)  | VCC               |
| GND       | GND               |
| GPIO35 *(via divider, see below)* | AOUT |

**Important - voltage divider required.** MQ135 modules need 5V to drive
their heater properly, and at 5V the analog output (`AOUT`) can swing close
to 5V. The ESP32's ADC pins are only safe up to 3.3V, so **do not** wire
`AOUT` straight to GPIO35 - it can damage the pin.

Build a simple divider:

```
AOUT ──[10kΩ]──●──[20kΩ]── GND
                │
             GPIO35
```

This scales a 0-5V signal down to roughly 0-3.3V (`MQ135_DIVIDER_RATIO` in
`main.cpp` is set to `0.667` to match - if you use different resistor
values, update that constant to `R2/(R1+R2)`).

- GPIO35 is also an input-only ADC1 pin - same Wi-Fi-safe reasoning as above.
- **Burn-in / warm-up:** MQ135 sensors need a fresh-air burn-in of 24-48
  hours the first time (and a ~3 minute warm-up on every power-on, which the
  firmware already waits out before it will fire alerts or show a category
  other than "warming up").
- The firmware doesn't attempt a full ppm calibration (that needs a known
  clean-air baseline resistance, temperature/humidity correction, etc.) - it
  just classifies the raw ADC reading into Good/Moderate/Poor/Hazardous
  bands. After burn-in, watch the `aq` serial command in clean air vs. near
  a known pollutant (e.g. rubbing alcohol, breath) and adjust
  `AQ_THRESHOLD_MODERATE` / `AQ_THRESHOLD_POOR` / `AQ_THRESHOLD_HAZARDOUS` in
  `main.cpp` to match your specific sensor and environment.

## Phone notifications (ntfy.sh)

Poor/hazardous air quality triggers a push notification via
[ntfy.sh](https://ntfy.sh) - no account needed:

1. Install the ntfy app ([Android](https://play.google.com/store/apps/details?id=io.heckel.ntfy) / [iOS](https://apps.apple.com/us/app/ntfy/id1625396347)), or just open `https://ntfy.sh/<your-topic>` in a browser.
2. Pick a hard-to-guess topic name (anyone who knows it can read/post to it -
   it's a public broker unless you self-host).
3. Subscribe to that topic in the app.
4. Put the same topic name in `include/secrets.h` as `ntfyTopic`.

The firmware sends one alert when air quality first crosses into "Poor",
reminds you every 30 minutes while it stays poor, and sends an all-clear
once it drops back to "Good"/"Moderate". Use the `testnotify` serial command
to confirm delivery without waiting for real bad air.
