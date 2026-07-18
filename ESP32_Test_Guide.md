# ESP32 Test Application Guide

## Overview
This guide will help you upload and test the simple LED blinking application on your ESP32 board. The application will blink an LED connected to GPIO 2 (usually the built-in LED) every second.

## Prerequisites
1. ESP32 development board
2. USB cable for programming and serial communication
3. Arduino IDE or PlatformIO installed
4. Proper drivers installed for your ESP32 board

## Uploading the Application

### Step 1: Connect Your ESP32 Board
- Connect your ESP32 board to your computer using a USB cable
- Ensure the board is properly powered

### Step 2: Configure Board Settings (PlatformIO)
If using PlatformIO, verify your `platformio.ini` file has the correct board configuration:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
```

### Step 3: Upload the Code
In PlatformIO, you can upload using:
- VS Code: Press `Ctrl+Shift+P` and type "PlatformIO: Upload"
- Or use the PlatformIO menu in the bottom status bar

Alternatively, if using Arduino IDE:
1. Open the sketch
2. Select the correct board (Tools > Board > ESP32 Arduino > ESP32 Dev Module)
3. Select the correct port (Tools > Port > [your ESP32 port])
4. Click Upload button

## Testing the Application

### Step 1: Monitor Serial Output
After uploading, open the serial monitor to see the status messages:
- In PlatformIO: Use "PlatformIO: Serial Monitor" command
- In Arduino IDE: Tools > Serial Monitor

You should see output like:
```
ESP32 Test App Started
LED should be blinking...
LED ON
LED OFF
LED ON
LED OFF
...
```

### Step 2: Verify LED Behavior
The built-in LED on most ESP32 boards should blink every second:
- LED on for 1 second
- LED off for 1 second
- Repeat continuously

### Step 3: Troubleshooting
If the LED doesn't blink:
1. Check USB connection
2. Verify board selection in IDE
3. Check if board is properly powered
4. Ensure correct serial monitor baud rate (115200)
5. Try a different USB cable or port

## Code Explanation
The main application code (`src/main.cpp`) implements:
- Setup function to initialize serial communication
- Loop function that toggles LED state every second
- Serial output for status monitoring

## Next Steps
Once this basic test works, you can:
- Modify the blink pattern
- Add additional sensors or peripherals
- Expand functionality with more complex programs