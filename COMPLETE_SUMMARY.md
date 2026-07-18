# Complete Setup Summary - ESP32 + BH1750 Experiment

## 📁 Code Files Created

### Main Code: `src/main.cpp`
This file contains the complete code to interface with the BH1750 light sensor. The code:
- Initializes I2C communication (SDA=GPIO 21, SCL=GPIO 22)
- Sets up the BH1750 sensor in high-resolution mode
- Continuously reads light levels in lux
- Displays readings and LED status every 2 seconds
- Toggles the built-in LED to show the board is running
- Provides helpful error messages if connection fails

## 🔌 Hardware Connection

**Required Wires (4 total):**
```
ESP32 Pin  →  Gy-302 Module
-----------  ----------------
3.3V       →  VCC          (Power - 3.3V ONLY!)
GND        →  GND          (Ground)
GPIO 21    →  SDA          (I2C Data)
GPIO 22    →  SCL          (I2C Clock)
```

**Important Safety Notes:**
- ⚠️ **3.3V only** - BH1750 is NOT 5V tolerant!
- ⚠️ Always double-check connections before powering up
- ⚠️ Handle the sensor gently - it's delicate

## 🛠 PlatformIO Setup

### Step 1: Update `platformio.ini`
Add the BH1750 library dependency to your `platformio.ini` file:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
upload_speed = 921600
monitor_speed = 115200

; Library dependency for BH1750 sensor
lib_deps = 
    clamberson/BH1750@^1.0.0
```

### Step 2: Upload the Code
1. Connect your ESP32 board to your computer
2. In PlatformIO: Press `Ctrl+Shift+P` → Type "PlatformIO: Upload"
3. Wait for compilation and upload to complete

### Step 3: Monitor Serial Output
1. Open serial monitor: `Ctrl+Shift+M` or PlatformIO: Serial Monitor
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
LED OFF | Light Level: 44.80 lux
Status: Indoor Light
...
```

## 🧪 Testing Your Setup

### Basic Tests:

1. **Cover the sensor** with your hand:
   - Light level should drop to ~1-10 lux
   - Status should change to "Dark"

2. **Shine a flashlight** on the sensor:
   - Light level should increase to 100+ lux
   - Status should change to "Bright" or "Very Bright"

3. **Move between rooms**:
   - Compare different lighting conditions
   - Dark room vs. well-lit area

### Expected Light Level Ranges:
| Condition | Lux Range |
|-----------|-----------|
| Full darkness | 0.1-1 lux |
| Emergency lighting | 1-10 lux |
| Office lighting | 100-500 lux |
| Bright daylight | 1000-10000 lux |
| Direct sunlight | 10000-100000+ lux |

## 🔧 Troubleshooting

### If you see "Error: BH1750 sensor not detected!":

1. **Check wiring:**
   - Verify VCC is connected to 3.3V (NOT 5V!)
   - Ensure GND is properly connected
   - Confirm SDA is connected to GPIO 21
   - Verify SCL is connected to GPIO 22

2. **Check library installation:**
   - Verify `lib_deps = clamberson/BH1750@^1.0.0` is in platformio.ini
   - Try building again to ensure library downloads correctly

3. **Hardware issues:**
   - Try shorter jumper wires (less than 10cm recommended)
   - Add external pull-up resistors (4.7kΩ) between SDA/3.3V and SCL/3.3V
   - Check for loose connections or damaged pins

### If LED is not blinking:
- Verify the code was uploaded successfully
- Check that GPIO 2 is the correct LED pin for your ESP32 board
- Test LED separately by modifying the delay values

### If readings are inconsistent:
- Ensure stable power supply
- Keep I2C wires as short as possible
- Avoid electrical interference from other components
- Try different light sources to verify sensor response

## 📚 Documentation Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Main program code (already created) |
| `EXPERIMENT_SETUP.md` | Detailed setup instructions |
| `WIRING.md` | Visual wiring diagram and quick reference |
| `quick_setup.sh` | Bash script with setup information |
| `COMPLETE_SUMMARY.md` | This file - complete overview |

## 🎯 What You'll Learn

This experiment teaches you:
- How to connect I2C sensors to ESP32
- Reading ambient light in lux units
- Working with external sensor libraries in PlatformIO
- Basic sensor calibration and testing techniques
- LED control based on sensor readings

## 🚀 Next Steps

Once you have the basic setup working, try:
1. **Logging data** to SD card for long-term monitoring
2. **Creating a WiFi monitor** that uploads light levels to cloud
3. **Building a lighting control system** that turns lights on/off automatically
4. **Combining with other sensors** for a complete weather station
5. **Adding data visualization** with Grafana or similar tools

## 💡 Key Concepts

### I2C Communication:
- **SDA (GPIO 21)**: Bidirectional data line
- **SCL (GPIO 22)**: Clock line controlled by master (ESP32)
- Both lines need pull-up resistors (typically built into modules)

### Light Measurement:
- **Lux**: Standard unit of illuminance (light per unit area)
- **BH1750**: Converts light intensity to digital lux values
- High-resolution mode provides the most accurate readings

### Sensor Modes:
- **ONE_TIME_HIGH_RES_MODE**: Most accurate, power-saving (used in our code)
- **CONTINUOUS_HIGH_RES_MODE**: Regular automatic measurements
- **ONE_TIME_LOW_RES_MODE**: Faster but less precise

## 📖 Quick Reference

### Wiring (4 wires):
- 3.3V → VCC
- GND → GND  
- GPIO 21 → SDA
- GPIO 22 → SCL

### PlatformIO:
Add to `platformio.ini`: `lib_deps = clamberson/BH1750@^1.0.0`

### Serial Monitor:
- Baud rate: 115200
- Should see "BH1750 sensor found!" message
- Display updates every 2 seconds

## 🎉 You're Ready!

Just connect the four wires as specified, update your platformio.ini file with the library dependency, upload the code, and you'll be seeing light sensor readings in seconds!

**Enjoy experimenting with light sensing!** 🌟
