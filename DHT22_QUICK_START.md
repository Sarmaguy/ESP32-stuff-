# ESP32 + DHT22 Quick Start Guide

## What You'll Need

- ESP32 development board
- DHT22 (AM2302) temperature and humidity sensor
- Jumper wires (male to female)
- USB cable for ESP32
- Computer with PlatformIO installed
- WiFi network credentials

## Wiring Instructions

Connect the DHT22 sensor to your ESP32 as follows:

| DHT22 Pin | ESP32 Pin |
|-----------|-----------|
| VCC       | 3.3V      |
| DATA      | GPIO 4    |
| NC        | -         |
| GND       | GND       |

**Important:** The DHT22 requires a 4.7K-10K ohm pull-up resistor between the DATA pin and VCC for reliable operation.

## Uploading the Code

1. Open PlatformIO in VS Code
2. Navigate to this project folder
3. **Edit WiFi credentials** in `src/main.cpp`:
   - Change `ssid = "YOUR_WIFI_SSID"` to your WiFi network name
   - Change `password = "YOUR_WIFI_PASSWORD"` to your WiFi password
4. Click the "Upload" button (arrow pointing right)
5. Wait for compilation and upload to complete

## Viewing Sensor Data

### Option 1: Serial Monitor (No WiFi needed)

After uploading, open the Serial Monitor:
- Click the "Serial Monitor" button (terminal icon) in PlatformIO
- Set baud rate to 115200
- You should see temperature and humidity readings every 2 seconds

### Option 2: Web Browser (WiFi required)

If WiFi connection is successful, you can access the sensor data from any device on your network:

1. Check the Serial Monitor for the ESP32's IP address
2. Open a web browser on your phone, tablet, or computer
3. Enter the IP address in the address bar
4. You'll see a responsive webpage showing live temperature and humidity readings

## Troubleshooting

**No data showing?**
- Check all wiring connections
- Verify sensor is powered (3.3V or 5V)
- Ensure DATA pin is connected to GPIO 4
- Try a different USB cable/port
- Add a pull-up resistor if not already present

**Garbage characters in serial monitor?**
- Make sure baud rate matches (115200)

**WiFi not connecting?**
- Verify WiFi credentials are correct
- Check if your network requires special configuration
- Try moving closer to the router

**Still not working?**
- Check if the sensor is faulty
- Try connecting to a different GPIO pin and update DHTPIN in code
- Test with a known working ESP32 board

## Technical Details

- **Sensor:** DHT22 (AM2302)
- **Accuracy:**
  - Temperature: ±0.5°C
  - Humidity: ±5%
- **Range:**
  - Temperature: -40°C to +80°C
  - Humidity: 0% to 100%
- **Update Rate:** Every 2 seconds

## Advanced Usage

To change the GPIO pin:
1. Edit `DHTPIN` in `src/main.cpp`
2. Re-upload the code
3. Reconnect wires accordingly

To change WiFi network:
1. Update `ssid` and `password` in `src/main.cpp`
2. Re-upload the code