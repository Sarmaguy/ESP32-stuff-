# ESP32 + DHT22 Wiring Guide

## Connections

Connect your DHT22 sensor to the ESP32 as follows:

```
ESP32        DHT22
-----        -----
3.3V         VCC (Pin 1)
GND          GND (Pin 4)
GPIO 4       DATA (Pin 2)
```

## Pin Configuration

- **DHT22 VCC**: Connect to ESP32 3.3V pin
- **DHT22 GND**: Connect to ESP32 GND pin
- **DHT22 DATA**: Connect to ESP32 GPIO 4 (can be changed in code)
- **DHT22 NC** (Pin 3): Leave unconnected

## Important Notes

1. **Power Supply**:
   - DHT22 can work with both 3.3V and 5V
   - For best results, use a 4.7KΩ to 10KΩ resistor between VCC and DATA pin (pull-up resistor)

2. **Wiring Tips**:
   - Use short wires to minimize noise
   - Avoid running wires near power cables or motors
   - Make sure all connections are secure

3. **Sensor Orientation**:
   - The DHT22 has a grid pattern with 4 pins
   - Pin 1 (VCC) is usually marked or has a small dot/triangle
   - Pin 2 is the DATA pin
   - Pin 3 is NC (not connected)
   - Pin 4 is GND

## Troubleshooting

If you get "Failed to read from DHT sensor!" error:

1. Check all connections are secure
2. Verify power is reaching the sensor (multimeter can help)
3. Try a different GPIO pin (change DHTPIN in code)
4. Add a pull-up resistor between VCC and DATA
5. Make sure no other devices are using the same GPIO pin

## Expected Output

After uploading the code, open Serial Monitor (115200 baud) to see:
```
ESP32 + DHT22 Temperature & Humidity Sensor
===========================================

DHT22 connected to GPIO 4

Waiting for first reading...
-------------------------------------------
Humidity: 45.2%
Temperature: 22.1°C
-------------------------------------------