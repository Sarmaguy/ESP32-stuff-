# ESP32 + BH1750 (Gy-302) Experiment Guide

## 📋 What You'll Learn
This experiment will teach you:
- How to connect the Gy-302 (BH1750) ambient light sensor to an ESP32
- I2C communication basics between ESP32 and sensors
- Reading light intensity in lux units
- Basic sensor calibration and testing techniques

## 🔌 Hardware Connection Guide

### Required Components
- ESP32 development board ✓
- Gy-302 (BH1750) sensor module ✓
- Jumper wires (male-to-male) ✓
- Breadboard (recommended) ✓
- USB cable for programming ✓

### Wiring Diagram

```
ESP32 Board              Gy-302 Module
┌─────────────┐          ┌─────────────┐
│             │          │           │
│ 3.3V   ────┼─────────►│ VCC         │
│ GPIO 21├───┼─────────►│ SDA         │
│ GPIO 22├───┼─────────►│ SCL         │
│ GND    ────┼─────────►│ GND         │
│             │          │           │
└─────────────┘          └─────────────┘
```

### Pin Connection Table

| ESP32 Pin | Gy-302 Module | Function | Notes |
|-----------|---------------|----------|-------|
| 3.3V      | VCC           | Power    | **IMPORTANT**: Use 3.3V, NOT 5V! |
| GND       | GND           | Ground   | Common ground reference |
| GPIO 21   | SDA           | I2C Data | Serial Data Line |
| GPIO 22   | SCL           | I2C Clock| Serial Clock Line |

### I2C Address Configuration
The BH1750 sensor has an address selection pin (A0):
- **Default (A0 unconnected or grounded)**: I2C address = 0x23
- **Alternative (A0 connected to VCC)**: I2C address = 0x5C

Most Gy-302 modules use the default address (0x23), which is what our code assumes.

## 🛠 Software Setup

### Step 1: Install Required Library

Open your `platformio.ini` file and add the BH1750 library dependency:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

; Upload options for reliable flashing
upload_speed = 921600
monitor_speed = 115200

; Library dependencies
lib_deps = 
    clamberson/BH1750@^1.0.0
```

**Alternative method:** You can also install the library through PlatformIO Library Manager:
1. Open PlatformIO Home (VS Code → PlatformIO → Home)
2. Go to Libraries tab
3. Search for "BH1750"
4. Find "BH1750" by clamberson
5. Click "Add to Project"

### Step 2: Understanding the Code

The main code is in `src/main.cpp` and includes:

#### Setup Function (`setup()`)
- Initializes serial communication at 115200 baud
- Sets up built-in LED on GPIO 2 as output
- Configures I2C pins (SDA=GPIO 21, SCL=GPIO 22) for ESP32
- Initializes the BH1750 sensor
- Attempts to read from the sensor to verify connection
- Provides helpful error messages if connection fails

#### Loop Function (`loop()`)
- Toggles the LED on/off
- Reads light level from BH1750 in high-resolution mode
- Displays lux value (light intensity unit)
- Classifies light conditions (Dark/Indoor/Bright/Very Bright)

### Step 3: Upload and Test

1. **Connect your hardware** as described above
2. **Open PlatformIO**
3. **Upload the code**: Press `Ctrl+Shift+P` → Type "PlatformIO: Upload"
4. **Monitor serial output**: Click the serial monitor icon or press `Ctrl+Shift+M`

You should see output like this:

```
ESP32 + BH1750 Light Sensor Test
=================================
BH1750 sensor found!
Light Sensor Mode: ONE_TIME_HIGH_RES_MODE

Test started!
LED OFF | Light Level: 45.25 lux
Status: Indoor Light
```

## 🧪 Testing the Setup

### Basic Tests to Try

1. **Cover the sensor**: Put your hand over the sensor - light level should drop significantly (possibly below 10 lux)

2. **Shine a flashlight**: Point a phone flashlight at the sensor - light level should increase dramatically (could reach 100+ lux)

3. **Test different lighting conditions**:
   - Dark room (nighttime): ~1-5 lux
   - Office lighting: ~100-500 lux
   - Window area (daylight): ~1000-5000 lux
   - Direct sunlight: ~10,000-50,000+ lux

4. **Observe LED behavior**: The built-in LED will blink differently based on light levels:
   - Dark conditions: Fast blinking (200ms ON, 800ms OFF)
   - Normal indoor light: Regular blinking (500ms each)
   - Bright light: Slow or no blinking

### Expected Light Level Ranges

| Condition | Lux Range | Example |
|-----------|-----------|---------|
| Full moon night | 0.1-1 lux | Very dark, almost no light |
| Emergency lighting | 1-10 lux | Dark room with minimal light |
| Residential lighting | 10-100 lux | Living room, bedroom |
| Office lighting | 100-500 lux | Computer work areas |
| Bright daylight | 1000-10,000 lux | Near windows, shaded outdoors |
| Direct sunlight | 32,000-100,000+ lux | Clear summer noon |

## 🔧 Troubleshooting

### Sensor Not Detected
If you see "BH1750 sensor not detected!" error:

**Check connections:**
- Verify VCC is connected to 3.3V (not 5V!)
- Ensure GND is properly connected
- Check SDA is connected to GPIO 21
- Verify SCL is connected to GPIO 22

**Hardware issues:**
- Try shorter jumper wires
- Add external pull-up resistors (4.7kΩ) between SDA/3.3V and SCL/3.3V
- Check for loose connections or damaged pins
- Test with a different ESP32 board if available

### Inconsistent Readings
If readings seem erratic:

- **Stable power**: Ensure your USB cable provides stable 5V
- **Short wires**: Keep I2C wires as short as possible (<10cm recommended)
- **No interference**: Keep away from high-current devices or RF sources
- **Different light sources**: LED lights may cause flickering readings due to AC frequency

### Serial Monitor Shows Garbage
- Verify baud rate is set to 115200 in the serial monitor
- Check for loose connections on RX/TX lines if using USB-to-Serial adapter

## 📊 How It Works

### BH1750 Sensor原理

The BH1750 sensor contains:
1. **Photodiode**: Converts light into electrical current
2. **ADC (Analog-to-Digital Converter)**: Converts analog signal to digital
3. **I2C Interface**: Communicates with ESP32 using I2C protocol

### I2C Communication

I2C (Inter-Integrated Circuit) is a two-wire serial communication protocol:
- **SDA** (Serial Data Line): Carries data bidirectionally
- **SCL** (Serial Clock Line): Carries clock signal from master to slave

The ESP32 acts as the I2C master, initiating all communications with the BH1750 sensor (the slave device).

### Measurement Process

1. ESP32 sends start condition on I2C bus
2. ESP32 sends sensor address (0x23) + write bit
3. ESP32 sends command for measurement mode
4. Sensor performs light measurement (integration)
5. Sensor sends data back via I2C
6. ESP32 reads the 16-bit light value
7. Library converts raw value to lux units

## 🚀 Advanced Features

### Different Measurement Modes

The BH1750 supports multiple measurement modes:

```cpp
// ONE_TIME_HIGH_RES_MODE - Most accurate, power-saving
float lux = lightSensor.readLightLevel(BH1750::ONE_TIME_HIGH_RES_MODE);

// CONTINUOUS_HIGH_RES_MODE - Regular readings without reconfiguration
lux = lightSensor.readLightLevel(BH1750::CONTINUOUS_HIGH_RES_MODE);

// ONE_TIME_LOW_RES_MODE - Faster, less accurate
lux = lightSensor.readLightLevel(BH1750::ONE_TIME_LOW_RES_MODE);
```

### Changing I2C Address

If you've connected the A0 pin to VCC:

```cpp
BH1750 lightSensor(0x5C); // Use address 0x5C instead of default 0x23
```

### Custom Measurement Timing

You can modify the integration time (how long sensor collects light):

```cpp
// Set sensitivity (default is M_SENS_1_LX)
lightSensor.setMode(BH1750::ONE_TIME_HIGH_RES_MODE);
delay(180); // Wait for measurement to complete (max 120ms for HR mode)
```

## 📝 Project Ideas

Once you've mastered the basics, try these projects:

1. **Automatic Lighting Control**: Turn on/off lights based on ambient light
2. **Light Data Logger**: Log light levels over time to SD card or cloud
3. **Smart Garden Monitor**: Monitor plant light exposure
4. **Photography Light Meter**: Measure light for optimal camera settings
5. **Night Light Controller**: Automatically turn on LEDs in darkness
6. **Weather Station**: Combine with other sensors for comprehensive monitoring

## 📚 Additional Resources

- [BH1750 Datasheet](https://www.rohm.com/web/document/datasheet/bh1750fvi-e)
- [PlatformIO Documentation](https://docs.platformio.org/)
- [ESP32 I2C Guide](https://randomnerdtutorials.com/esp32-i2c-communication-arduino-ide/)
- [BH1750 Library GitHub](https://github.com/clamberson/BH1750)

## 🎓 Key Takeaways

✅ **Always use 3.3V** for the BH1750 sensor - 5V will damage it!
✅ **I2C uses only two wires** (SDA/SCL) for communication
✅ **Light is measured in lux** - a standard unit of illuminance
✅ **The library handles complex protocols** - you just call `readLightLevel()`
✅ **Test with different light conditions** to understand the sensor range

## 📞 Need Help?

If you encounter issues:
1. Check all connections are secure
2. Verify power is 3.3V (not 5V)
3. Test with a multimeter if available
4. Review the troubleshooting section above
5. Consult the detailed BH1750_Guide.md file
 
Happy experimenting! 🌟
