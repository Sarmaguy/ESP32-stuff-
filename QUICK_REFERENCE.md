# Quick Reference: BH1750 (Gy-302) Connection

## Hardware Connections

```
ESP32 Board              Gy-302 Module
┌─────────────┐          ┌─────────────┐
│ 3.3V   ────┼─────────►│ VCC         │
│ GPIO 21├───┼─────────►│ SDA         │
│ GPIO 22├───┼─────────►│ SCL         │
│ GND    ────┼─────────►│ GND         │
└─────────────┘          └─────────────┘
```

## PlatformIO Configuration

Add to `platformio.ini`:
```ini
lib_deps = 
    clamberson/BH1750@^1.0.0
```

## Code Location

- Main file: `src/main.cpp`
- Separate test: `src/bh1750_test.cpp`

## Key Points

✅ **3.3V only** - Never connect to 5V!
✅ **Default I2C address**: 0x23 (A0 unconnected/grounded)
✅ **Alternative address**: 0x5C (if A0 connected to VCC)

## Testing Steps

1. Connect sensor as shown above
2. Update `platformio.ini` with library dependency
3. Upload code to ESP32
4. Open serial monitor at 115200 baud
5. Cover/uncover sensor to test response

## Expected Output

```
ESP32 + BH1750 Light Sensor Test
=================================
BH1750 sensor found!
Light Sensor Mode: ONE_TIME_HIGH_RES_MODE

Test started!
LED OFF | Light Level: 45.25 lux
Status: Indoor Light
...
```

## Typical Light Levels

| Condition | Lux Range |
|-----------|-----------|
| Full moon | 0.1-1 |
| Dark room | 1-10 |
| Office light | 100-500 |
| Sunny day | 10,000-50,000 |
