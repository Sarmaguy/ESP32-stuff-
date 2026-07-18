╔══════════════════════════════════════════════════════════════════════════════╗
║                  ESP32 + BH1750 (Gy-302) CONNECTION GUIDE                    ║
╚══════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────┐          ┌──────────────────────────┐
│                          │          │                          │
│      ESP32 BOARD         │  WIRING  │   BH1750 MODULE          │
│                          │          │                          │
│  ┌──────────────────┐    │          │  ┌──────────────────┐    │
│  │                  │    │          │  │                  │    │
│  │  3.3V    [●] ●   │────┼──────────┼──►│ VCC              │    │
│  │  GND     [●] ●   │────┼──────────┼──►│ GND              │    │
│  │  GPIO 21 [●] ●   │────┼──────────┼──►│ SDA              │    │
│  │  GPIO 22 [●] ●   │────┼──────────┼──►│ SCL              │    │
│  │                  │    │          │  │                  │    │
│  └──────────────────┘    │          │  └──────────────────┘    │
│                          │          │                          │
└──────────────────────────┘          └──────────────────────────┘

╔══════════════════════════════════════════════════════════════════════════════╗
║                         PIN CONNECTION TABLE                                 ║
╚══════════════════════════════════════════════════════════════════════════════╝

  ESP32 Pin          Gy-302 Module      Function         Notes
─────────────    ───────────────────   ────────────   ───────────────────────
    3.3V     ───►     VCC           Power Supply        IMPORTANT: Use 3.3V!
     GND     ───►     GND           Ground              Common ground
   GPIO 21   ───►     SDA           I2C Data Line       Serial Data
   GPIO 22   ───►     SCL           I2C Clock Line      Serial Clock

╔══════════════════════════════════════════════════════════════════════════════╗
║                        PLATFORMIO CONFIGURATION                              ║
╚══════════════════════════════════════════════════════════════════════════════╝

Add this to your platformio.ini under [env:esp32dev]:

  lib_deps = 
      clamberson/BH1750@^1.0.0

╔══════════════════════════════════════════════════════════════════════════════╗
║                          IMPORTANT SAFETY NOTES                              ║
╚══════════════════════════════════════════════════════════════════════════════╝

  ⚠️  DO NOT connect VCC to 5V - BH1750 is a 3.3V device only!
  ⚠️  Always double-check connections before powering up
  ⚠️  Handle the sensor gently - it's delicate

╔══════════════════════════════════════════════════════════════════════════════╗
║                             QUICK START CHECKLIST                            ║
╚══════════════════════════════════════════════════════════════════════════════╝

  [ ] 1. Connect VCC to ESP32 3.3V pin
  [ ] 2. Connect GND to ESP32 GND pin
  [ ] 3. Connect SDA to ESP32 GPIO 21
  [ ] 4. Connect SCL to ESP32 GPIO 22
  [ ] 5. Add BH1750 library to platformio.ini
  [ ] 6. Upload code to ESP32
  [ ] 7. Open serial monitor at 115200 baud
  [ ] 8. Test by covering/uncovering sensor

╔══════════════════════════════════════════════════════════════════════════════╗
║                            TROUBLESHOOTING                                   ║
╚══════════════════════════════════════════════════════════════════════════════╝

SENSOR NOT DETECTED:
  ✓ Check all connections are secure
  ✓ Verify VCC is connected to 3.3V (NOT 5V!)
  ✓ Ensure SDA is connected to GPIO 21
  ✓ Verify SCL is connected to GPIO 22
  ✓ Try shorter jumper wires
  ✓ Add external pull-up resistors (4.7kΩ)

╔══════════════════════════════════════════════════════════════════════════════╗
║                         EXPECTED SERIAL OUTPUT                               ║
╚══════════════════════════════════════════════════════════════════════════════╝

  ESP32 + BH1750 Light Sensor Test
  =================================
  BH1750 sensor found!
  Light Sensor Mode: ONE_TIME_HIGH_RES_MODE
  
  Test started!
  LED OFF | Light Level: 45.25 lux
  Status: Indoor Light

╔══════════════════════════════════════════════════════════════════════════════╗
║                        Happy Experimenting! 🌟                                ║
╚══════════════════════════════════════════════════════════════════════════════╝
