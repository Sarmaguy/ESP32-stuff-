# BH1750 A0 (ADDR) Pin Guide

## Quick Answer: **Leave A0 unconnected or connect to GND**

For most experiments, you don't need to connect anything to the A0 pin on the BH1750 sensor. Here's why:

### Default Configuration (Recommended)

| Connection | I2C Address | Use Case |
|------------|-------------|----------|
| **A0 unconnected** (not connected to anything) | 0x23 | Default - works with our code |
| **A0 connected to GND** | 0x23 | Alternative way to get default address |
| **A0 connected to VCC (3.3V)** | 0x5C | Alternative address |

## Why This Matters

The BH1750 uses I2C communication, which requires each device on the bus to have a unique address. The A0 pin lets you choose between two possible addresses:
- **0x23** (default) - when A0 is at logic low (GND or unconnected)
- **0x5C** - when A0 is at logic high (VCC/3.3V)

## What You Should Do

### Most Common Setup (Use This):
```
ESP32 Pin  →  Gy-302 Module
-----------  ----------------
3.3V       →  VCC          (Power - 3.3V ONLY!)
GND        →  GND          (Ground)
SDA        →  SDA          (I2C Data)
SCL        →  SCL          (I2C Clock)

A0         →  (leave unconnected)
             OR
A0         →  GND          (alternative)
```

### Alternative Setup (When You Need Address 0x5C):
```
ESP32 Pin  →  Gy-302 Module
-----------  ----------------
3.3V       →  VCC          (Power - 3.3V ONLY!)
GND        →  GND          (Ground)
SDA        →  SDA          (I2C Data)
SCL        →  SCL          (I2C Clock)
3.3V       →  A0          (connect A0 to 3.3V to use address 0x5C)
```

## How to Check What Address Your Sensor Uses

1. **Visual Check:** Look at your Gy-302 module. Many modules have a jumper or resistor that already connects A0 to GND or leaves it unconnected.

2. **Code Check:** Our default code looks for address 0x23. If it finds your sensor, you're using address 0x23.

3. **Multimeter Check:** Use continuity mode to check if A0 is connected to GND on your module.

## If You Need to Change the Address

### To Use Address 0x5C (A0 connected to VCC):

1. Connect A0 pin on the sensor to 3.3V
2. Modify the code in `src/main.cpp`:

```cpp
// Initialize the BH1750 sensor with address 0x5C
BH1750 lightSensor(0x5C);  // Changed from default 0x23 to 0x5C
```

## Multiple BH1750 Sensors

If you want to use two BH1750 sensors simultaneously, you'll need different addresses:

```
Sensor 1: A0 unconnected → Address 0x23
Sensor 2: A0 connected to VCC → Address 0x5C
```

Then in code:
```cpp
BH1750 sensor1(0x23);  // First sensor
BH1750 sensor2(0x5C);  // Second sensor

void setup() {
  sensor1.begin();
  sensor2.begin();
  // ...
}
```

## Visual Guide

```
Typical BH1750 Module Pinout:

    ┌───────────────┐
    │               │
    │   ● VCC       │
    │   ● GND       │
    │   ● SDA       │
    │   ● SCL       │
    │   ● A0        │ ← You usually DON'T need to connect this!
    │               │
    └───────────────┘

Most modules: A0 is left unconnected (default 0x23)
Or: A0 connected to onboard resistor → GND (also 0x23)
```

## Summary

For your experiment:
1. ✅ **Don't connect anything to A0** (leave it unconnected)
2. ✅ Or connect A0 to GND if you prefer
3. ✅ Our code is configured for address 0x23 (default)
4. ✅ If you get "sensor not detected", check your wiring first before thinking about A0

The key point: **For most Gy-302/BH1750 modules, you can ignore the A0 pin completely.**

Happy Experimenting! 🌟
