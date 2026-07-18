╔══════════════════════════════════════════════════════════════════════════════╗
║                  ESP32 + BH1750 (Gy-302) WIRING GUIDE                        ║
║                         INCLUDING A0 PIN INFORMATION                          ║
╚══════════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════════════════════════════════════════════════╗
║                           RECOMMENDED CONNECTION (Default)                    ║
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
│  │  (A0 PIN:        │    │          │  │  A0              │    │
│  │   leave          │    │          │  │  (NOT CONNECTED) │    │
│  │   unconnected)   │    │          │  │                  │    │
│  └──────────────────┘    │          │  └──────────────────┘    │
│                          │          │                          │
└──────────────────────────┘          └──────────────────────────┘

╔══════════════════════════════════════════════════════════════════════════════╗
║                           PIN CONNECTION TABLE                                ║
╚══════════════════════════════════════════════════════════════════════════════╝

  ESP32 Pin          Gy-302 Module      Function         Notes
─────────────    ───────────────────   ────────────   ───────────────────────
    3.3V     ───►     VCC           Power Supply        IMPORTANT: Use 3.3V!
     GND     ───►     GND           Ground              Common ground
   GPIO 21   ───►     SDA           I2C Data Line       Serial Data
   GPIO 22   ───►     SCL           I2C Clock Line      Serial Clock
     -       ───►     A0            Address Select      NOT CONNECTED (default)

╔══════════════════════════════════════════════════════════════════════════════╗
║                         A0 PIN EXPLANATION                                   ║
╚══════════════════════════════════════════════════════════════════════════════╝

  A0 Pin Connection    I2C Address    When to Use
───────────────────  ─────────────  ───────────────────────────────────
     NOT CONNECTED         0x23      Default (recommended for most cases)
        GND                0x23      Alternative way to get default
        VCC (3.3V)        0x5C       Use only if you need alternative address

  ⚠️  For this experiment, leave A0 UNCONNECTED (not connected to anything)
  ⚠️  Our code is configured to use I2C address 0x23 (default)

╔══════════════════════════════════════════════════════════════════════════════╗
║                        PLATFORMIO CONFIGURATION                              ║
╚══════════════════════════════════════════════════════════════════════════════╝

Add this to your platformio.ini under [env:esp32dev]:

  lib_deps = 
      clamberson/BH1750@^1.0.0

╔══════════════════════════════════════════════════════════════════════════════╗
║                           MODULE PINOUT                                      ║
╚══════════════════════════════════════════════════════════════════════════════╝

 Typical BH1750/Gy-302 Module Pinout:

      ┌──────────────────┐
      │                  │
      │  ● VCC 3.3V      │
      │  ● GND           │
      │  ● SDA           │
      │  ● SCL           │
      │  ● A0            │ ← Leave unconnected or connect to GND
      │                  │
      └──────────────────┘

  Most modules: A0 is already connected to GND on the module, or left floating
  Either way, they default to I2C address 0x23

╔══════════════════════════════════════════════════════════════════════════════╗
║                          TROUBLESHOOTING                                     ║
╚══════════════════════════════════════════════════════════════════════════════╝

SENSOR NOT DETECTED:
  ✓ Check all connections are secure (especially VCC, GND, SDA, SCL)
  ✓ Verify VCC is connected to 3.3V (NOT 5V!)
  ✓ Ensure SDA is connected to GPIO 21
  ✓ Verify SCL is connected to GPIO 22
  ✓ Try shorter jumper wires
  ✓ Add external pull-up resistors (4.7kΩ) between SDA/3.3V and SCL/3.3V

  Note: If your module uses address 0x5C (A0 connected to VCC),
        modify the code: BH1750 lightSensor(0x5C);

╔══════════════════════════════════════════════════════════════════════════════╗
║                         EXPECTED SERIAL OUTPUT                               ║
╚══════════════════════════════════════════════════════════════════════════════╝

  ESP32 + BH1750 Light Sensor Test
  =================================
  BH1750 sensor found!
  Light Sensor Mode: ONE_TIME_HIGH_RES_MODE
  I2C Address: 0x23 (A0 connected to GND or unconnected)

  Test started!
  LED OFF | Light Level: 45.25 lux
  Status: Indoor Light

╔══════════════════════════════════════════════════════════════════════════════╗
║                        Quick Start Summary                                   ║
╚══════════════════════════════════════════════════════════════════════════════╝

  4 Wires to Connect:
  -------------------
  1. ESP32 3.3V  → Gy-302 VCC  (Red wire)
  2. ESP32 GND   → Gy-302 GND  (Black wire)
  3. ESP32 GPIO21→ Gy-302 SDA  (Blue wire)
  4. ESP32 GPIO22→ Gy-302 SCL  (Yellow wire)

  A0 Pin:
  -------
  LEAVE IT UNCONNECTED (not connected to anything)

  PlatformIO:
  -----------
  Add to platformio.ini:
    lib_deps = clamberson/BH1750@^1.0.0

  Upload and Test:
  ----------------
  Upload code → Open serial monitor at 115200 → Cover/uncover sensor

╔══════════════════════════════════════════════════════════════════════════════╗
║                        Happy Experimenting! 🌟                               ║
╚══════════════════════════════════════════════════════════════════════════════╝
