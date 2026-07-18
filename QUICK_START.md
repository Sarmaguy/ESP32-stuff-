# 🚀 Quick Start - ESP32 + BH1750 Experiment

## ⚡ 5-Minute Setup Guide

### Step 1: Hardware Connection (2 minutes)

Connect these 4 wires from your ESP32 to the Gy-302 sensor:

| ESP32 Pin | Gy-302 Pin | What to connect |
|-----------|------------|-----------------|
| 3.3V      | VCC        | Red wire        |
| GND       | GND        | Black wire      |
| GPIO 21   | SDA        | Blue wire       |
| GPIO 22   | SCL        | Yellow wire     |

**⚠️ IMPORTANT: Use 3.3V only - never 5V!**

### Step 2: Software Setup (2 minutes)

1. Open `platformio.ini` in your project folder
2. Find the `[env:esp32dev]` section
3. Add these two lines:

```ini
lib_deps = 
    clamberson/BH1750@^1.0.0
```

Your `platformio.ini` should look like:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
upload_speed = 921600
monitor_speed = 115200

lib_deps = 
    clamberson/BH1750@^1.0.0
```

### Step 3: Upload Code (1 minute)

1. Connect your ESP32 board to your computer
2. Press `Ctrl+Shift+P` and type "PlatformIO: Upload"
3. Wait for compilation and upload to complete

### Step 4: Test (30 seconds)

1. Open serial monitor: Press `Ctrl+Shift+M`
2. Set baud rate to **115200**
3. You should see output like:

```
ESP32 + BH1750 Light Sensor Test
=================================
BH1750 sensor found!
Light Sensor Mode: ONE_TIME_HIGH_RES_MODE

Test started!
LED OFF | Light Level: 45.25 lux
Status: Indoor Light
```

## 🎯 Test the Sensor

1. **Cover the sensor** with your hand - lux should drop (dark)
2. **Shine a flashlight** on it - lux should increase (bright)
3. **Watch the LED** - it should blink once every 2.5 seconds

## ❓ Don't See "BH1750 sensor found!"?

Check these common issues:

1. **Wiring**: Double-check all 4 connections
   - VCC → 3.3V (NOT 5V!)
   - GND → GND
   - SDA → GPIO 21
   - SCL → GPIO 22

2. **Library**: Verify `lib_deps = clamberson/BH1750@^1.0.0` is in platformio.ini

3. **Upload**: Make sure code uploaded successfully (no errors)

## 📊 Expected Results

| Light Condition | Lux Value | Status |
|----------------|-----------|--------|
| Covered with hand | 1-10 lux | Dark |
| Office lighting | 100-500 lux | Indoor Light |
| Bright room | 500-1000 lux | Bright |
| Direct sunlight | 1000+ lux | Very Bright |

## 🎓 What You're Learning

- I2C communication between ESP32 and sensors
- Reading ambient light in lux units
- Using external libraries in PlatformIO
- Basic sensor testing and troubleshooting

## 📚 Need More Help?

- See `COMPLETE_SUMMARY.md` for detailed explanation
- Check `EXPERIMENT_SETUP.md` for comprehensive guide
- Review `WIRING.md` for visual diagrams

## ✅ Success Checklist

- [ ] All 4 wires connected correctly (3.3V, GND, GPIO 21, GPIO 22)
- [ ] Library added to platformio.ini
- [ ] Code uploaded successfully
- [ ] Serial monitor shows "BH1750 sensor found!"
- [ ] Light level readings appear and change when covering sensor

## 🎉 You're Done!

Now you can experiment with different lighting conditions, modify the code, or add more features!

**Happy Experimenting! 🌟**
