# ESP32 + BH1750 (Gy-302) Light Sensor Guide

## Overview
This guide will help you connect and test an ESP32 board with a Gy-302 (BH1750) ambient light sensor. The BH1750 measures light intensity in lux and communicates via I2C.

## What You'll Need
1. ESP32 development board
2. Gy-302 (BH1750) sensor module
3. Jumper wires (male-to-male)
4. USB cable for programming
5. Breadboard (optional but recommended)

## Hardware Connection

### Pinout:
```
ESP32 Board          Gy-302 (BH1750) Module
-----------          ---------------------
3.3V      ----->     VCC (Power)
GND       ----->     GND (Ground)
GPIO 21   ----->     SDA (I2C Data)
GPIO 22   ----->     SCL (I2C Clock)
```

### Detailed Connection Table:
| ESP32 Pin | Gy-302 Module Pin | Description |
|-----------|-------------------|-------------|
| 3.3V      | VCC               | Power supply (3.3V) |
| GND       | GND               | Ground reference |
| GPIO 21   | SDA               | I2C data line |
| GPIO 22   | SCL               | I2C clock line |

### Notes:
- **Important:** The BH1750 sensor operates at 3.3V logic. Do NOT connect to 5V!
- Most Gy-302 modules include pull-up resistors, so no external resistors needed
- The A0 pin on the module can be used to change the I2C address:
  - Leave unconnected or connect to GND → Address = 0x23 (default)
  - Connect to VCC → Address = 0x5C

## Software Setup

### Install Required Libraries

In PlatformIO, add the BH1750 library to your `platformio.ini`:

```ini
lib_deps = 
    clamberson/BH1750@^1.0.0
```

Or you can install it manually through PlatformIO Library Manager:
1. Open PlatformIO Home
2. Go to Libraries tab
3. Search for "BH1750"
4. Click on "BH1750" by clamberson
5. Click "Add to Project"

### Code Structure

The test code (`src/bh1750_test.cpp`) includes:

1. **Setup Function:**
   - Initializes serial communication at 115200 baud
   - Sets up the built-in LED on GPIO 2
   - Initializes I2C communication with correct pins (SDA=21, SCL=22)
   - Attempts to communicate with the BH1750 sensor
   - Provides helpful error messages if connection fails

2. **Loop Function:**
   - Reads light level from the BH1750 in high-resolution mode
   - Displays the value in lux (light units)
   - Adjusts LED blink pattern based on ambient light:
     - Dark (<10 lux): Fast blinking
     - Indoor (10-100 lux): Normal blinking
     - Bright (>100 lux): Slow or no blinking
   - Provides light level classification

## Testing the Setup

### Step 1: Upload the Code
1. Connect your ESP32 board to your computer
2. In PlatformIO, click "Upload" (or press Ctrl+Shift+P → PlatformIO: Upload)
3. Wait for compilation and upload to complete

### Step 2: Monitor Serial Output
1. Open serial monitor (PlatformIO: Serial Monitor)
2. Set baud rate to 115200
3. You should see output like:

```
ESP32 + BH1750 Light Sensor Test
=================================
BH1750 sensor found!
Light Sensor Mode: ONE_TIME_HIGH_RES_MODE

Test started!
Light Level: 45.25 lux
Status: Indoor Light
Light Level: 46.10 lux
Status: Indoor Light
...
```

### Step 3: Test the Sensor
Try these experiments:
1. **Cover the sensor** with your hand - light level should drop significantly
2. **Shine a flashlight** on the sensor - light level should increase dramatically
3. **Move between rooms** - compare indoor vs outdoor readings
4. **Change lighting conditions** - turn lights on/off to see response

### Expected Light Level Ranges:
- **1-10 lux:** Very dark (nighttime, covered sensor)
- **10-100 lux:** dimly lit room, evening indoor lighting
- **100-500 lux:** typical office/home lighting
- **500-1000 lux:** bright daylight, overcast sky
- **>1000 lux:** direct sunlight (can go up to 100,000 lux)

## Troubleshooting

### "BH1750 sensor not detected!" Error:
1. Check all connections are secure
2. Verify VCC is connected to 3.3V (not 5V!)
3. Ensure SDA and SCL are correctly wired (GPIO 21 and 22)
4. Try adding pull-up resistors (4.7kΩ) between SDA/3.3V and SCL/3.3V
5. Use an I2C scanner to check if the sensor appears on the bus

### Inconsistent Readings:
1. Ensure stable power supply
2. Keep wires as short as possible
3. Avoid electrical interference from other components
4. The BH1750 is sensitive to infrared light - results may vary with different light sources

### LED Not Responding:
1. Check LED connection (GPIO 2)
2. Verify the code was uploaded successfully
3. Test LED separately by toggling it in the serial monitor output

## Advanced Features

### Different Measurement Modes:
The BH1750 supports several modes:

```cpp
// ONE_TIME_HIGH_RES_MODE - High resolution, one-time measurement
lightSensor.readLightLevel(BH1750::ONE_TIME_HIGH_RES_MODE);

// CONTINUOUS_HIGH_RES_MODE - Continuous high-resolution measurements
lightSensor.readLightLevel(BH1750::CONTINUOUS_HIGH_RES_MODE);

// ONE_TIME_LOW_RES_MODE - Low resolution, faster reading
lightSensor.readLightLevel(BH1750::ONE_TIME_LOW_RES_MODE);
```

### Changing I2C Address:
If you connected the A0 pin to VCC:

```cpp
BH1750 lightSensor(0x5C); // Use address 0x5C instead of default 0x23
```

## How It Works

The BH1750 sensor uses a photodiode that generates current proportional to light intensity. This current is converted to a digital signal by an ADC and output via I2C. The sensor automatically adjusts its sensitivity based on ambient light conditions.

The library handles all the complex communication protocols, allowing you to simply call `readLightLevel()` to get accurate lux measurements.

## Next Steps

Once this basic test works, try:
1. Logging light levels over time
2. Creating an automatic lighting control system
3. Building a light meter for photography
4. Adding data logging to an SD card
5. Making a WiFi-enabled light monitor with dashboard

Enjoy experimenting with light sensing!
