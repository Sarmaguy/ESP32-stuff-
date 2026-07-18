# ESP32 + BH1750 Experiment Setup Guide

## Quick Setup Instructions

### 1. Hardware Connection

Connect your ESP32 board to the Gy-302 (BH1750) sensor module using these 4 wires:

| ESP32 Pin | Gy-302 Module | Color (Typical) |
|-----------|---------------|-----------------|
| 3.3V      | VCC           | Red             |
| GND       | GND           | Black           |
| GPIO 21   | SDA           | Blue/Green      |
| GPIO 22   | SCL           | Yellow/White    |

**Important:** The BH1750 operates at **3.3V only** - do NOT use 5V!

### 2. PlatformIO Configuration

Open your `platformio.ini` file and add the BH1750 library dependency:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
upload_speed = 921600
monitor_speed = 115200

; Add this line for BH1750 sensor support
lib_deps = 
    clamberson/BH1750@^1.0.0
```

### 3. Upload the Code

1. Connect your ESP32 board to your computer
2. In PlatformIO, click the upload button (or press Ctrl+Shift+P → PlatformIO: Upload)
3. Wait for compilation and upload to complete

### 4. Test the Setup

1. Open serial monitor (PlatformIO: Serial Monitor)
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

## Troubleshooting

### If you see "Error: BH1750 sensor not detected!":

1. **Check wiring:**
   - VCC → 3.3V (NOT 5V!)
   - GND → GND
   - SDA → GPIO 21
   - SCL → GPIO 22

2. **Check library installation:**
   - Verify `lib_deps = clamberson/BH1750@^1.0.0` is in platformio.ini
   - Try building again to ensure library downloads

3. **Hardware issues:**
   - Try shorter jumper wires
   - Add external pull-up resistors (4.7kΩ) between SDA/3.3V and SCL/3.3V
   - Check for loose connections

## Testing Your Setup

Try these experiments:

1. **Cover the sensor** with your hand - lux value should drop to 1-10 lux
2. **Shine a flashlight** on the sensor - lux value should increase to 100+ lux
3. **Move between rooms** - compare different lighting conditions

## Expected Light Level Ranges

| Condition | Lux Range |
|-----------|-----------|
| Full darkness | 0.1-1 lux |
| Emergency lighting | 1-10 lux |
| Office lighting | 100-500 lux |
| Bright daylight | 1000-10000 lux |
| Direct sunlight | 10000-100000+ lux |

## How It Works

The code:
1. Initializes I2C communication (SDA=21, SCL=22)
2. Sets up the BH1750 sensor in high-resolution mode
3. Continuously reads light levels in lux
4. Displays readings and LED status every 2 seconds
5. Toggles the built-in LED to show the board is working

## What You'll See

When successful, you'll see serial output like:

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

The LED on your ESP32 board will also blink once every 2.5 seconds, providing visual confirmation that the board is running the code.

## Next Steps

Once you have the basic setup working, try:
- Changing the measurement interval
- Adding data logging to SD card
- Creating a WiFi-enabled light monitor
- Combining with other sensors for a weather station

Happy experimenting! 🌟
