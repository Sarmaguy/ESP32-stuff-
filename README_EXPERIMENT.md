# ESP32 + BH1750 (Gy-302) Experiment - Complete Summary

## 🎯 What We've Created

I've set up a complete experiment environment for testing an ESP32 board with a Gy-302 (BH1750) ambient light sensor.

## 📁 Files Created/Modified

### Code Files:
1. **`src/main.cpp`** - Main program code that reads the BH1750 sensor and blinks LED
2. **`src/bh1750_test.cpp`** - Alternative test file with more detailed output

### Documentation:
3. **`EXPERIMENT_GUIDE.md`** - Comprehensive guide explaining everything about this experiment
4. **`BH1750_Guide.md`** - Detailed guide for BH1750 sensor setup and usage
5. **`QUICK_REFERENCE.md`** - Quick reference card for connections and commands
6. **`WIRING_DIAGRAM.txt`** - Visual ASCII diagram of connections

### Scripts:
7. **`test_bh1750.sh`** - Bash script with connection instructions

## 🔌 Hardware Connection (Quick Version)

```
ESP32 Board              Gy-302 Module
-----------              -------------
3.3V      ─────────────► VCC (Power)
GND       ─────────────► GND (Ground)
GPIO 21   ─────────────► SDA (I2C Data)
GPIO 22   ─────────────► SCL (I2C Clock)
```

**Important Notes:**
- ✅ Use **3.3V only** - Never connect to 5V!
- ✅ Default I2C address is **0x23** (A0 pin unconnected or grounded)
- ✅ Most modules have built-in pull-up resistors

## 📝 PlatformIO Setup Required

Add this line to your `platformio.ini` under `[env:esp32dev]`:

```ini
lib_deps = 
    clamberson/BH1750@^1.0.0
```

Or install the library through PlatformIO Library Manager.

## 🚀 How to Run

1. **Connect your hardware** as shown above
2. **Update platformio.ini** with the BH1750 library dependency
3. **Upload the code** using PlatformIO: `Ctrl+Shift+P` → "PlatformIO: Upload"
4. **Open serial monitor** at 115200 baud
5. **Test by covering/uncovering** the sensor to see different light levels

## 📊 Expected Output

When you open the serial monitor, you should see:

```
ESP32 + BH1750 Light Sensor Test
=================================
BH1750 sensor found!
Light Sensor Mode: ONE_TIME_HIGH_RES_MODE

Test started!
LED OFF | Light Level: 45.25 lux
Status: Indoor Light

LED OFF | Light Level: 46.10 lux
Status: Indoor Light
...
```

## 🔍 What You'll Learn

This experiment teaches you:
- How to connect I2C sensors to ESP32
- Reading ambient light in lux units
- Working with the BH1750 sensor library
- Basic sensor calibration and testing
- LED control based on sensor readings

## 🧪 Test Ideas to Try

1. **Cover the sensor** with your hand - see lux values drop
2. **Shine a flashlight** on the sensor - watch lux values increase
3. **Move between rooms** - compare different lighting conditions
4. **Observe LED behavior** - it changes blink pattern based on light levels

## 📚 Documentation Guide

| File | Purpose |
|------|---------|
| `EXPERIMENT_GUIDE.md` | Complete guide with theory, setup, and troubleshooting |
| `BH1750_Guide.md` | Detailed BH1750-specific instructions |
| `QUICK_REFERENCE.md` | Quick reference for connections and settings |
| `WIRING_DIAGRAM.txt` | Visual ASCII diagram of connections |

## 🔧 Troubleshooting Quick Reference

**If sensor is not detected:**
- Check all connections are secure
- Verify VCC is 3.3V (not 5V!)
- Ensure SDA/SCL pins are correct (GPIO 21/22)
- Try shorter jumper wires or add pull-up resistors

**If readings seem erratic:**
- Check stable power supply
- Keep I2C wires short
- Avoid electrical interference

## 🎓 Key Concepts Explained

### I2C Communication:
- **SDA (GPIO 21)**: Data line for bidirectional communication
- **SCL (GPIO 22)**: Clock line controlled by master (ESP32)

### Light Measurement:
- **Lux**: Unit of illuminance (light per area)
- **BH1750**: Converts light intensity to digital lux values

### Sensor Modes:
- **HIGH_RES_MODE**: Most accurate, power-saving
- **CONTINUOUS_MODE**: Regular automatic measurements
- **LOW_RES_MODE**: Faster but less precise

## 🌟 Next Steps After This Experiment

Once you've mastered this setup, try:
1. Logging light levels over time to SD card
2. Creating a WiFi-enabled weather station
3. Building an automatic lighting control system
4. Combining with other sensors (temperature, humidity)
5. Adding data visualization with Grafana or similar tools

## 📖 Reading List

For deeper understanding:
- [BH1750 Datasheet](https://www.rohm.com/web/document/datasheet/bh1750fvi-e) - Technical specifications
- [PlatformIO Documentation](https://docs.platformio.org/) - Development environment guide
- [ESP32 I2C Guide](https://randomnerdtutorials.com/esp32-i2c-communication-arduino-ide/) - I2C communication details

## 🎉 You're Ready!

Just connect the four wires as specified, update your platformio.ini file, upload the code, and you'll be seeing light sensor readings in seconds!

**Enjoy experimenting with light sensing!** 🌟


pio run -e slave -t upload
