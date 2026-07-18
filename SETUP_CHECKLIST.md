# Setup Checklist: ESP32 + BH1750 Experiment

## ✅ Pre-Flight Check (Do this first)

### Hardware Checklist:
- [ ] ESP32 development board available and working
- [ ] Gy-302 (BH1750) sensor module available
- [ ] Jumper wires (male-to-male) ready
- [ ] USB cable for programming
- [ ] Working computer with PlatformIO installed

### Software Checklist:
- [ ] PlatformIO IDE installed and working
- [ ] Existing ESP32 project structure ready
- [ ] Backup of original `platformio.ini` (optional but recommended)

---

## 🔌 Hardware Connection Phase

### Step 1: Power Connections
- [ ] Connect ESP32 **3.3V** pin to Gy-302 **VCC**
- [ ] Connect ESP32 **GND** pin to Gy-302 **GND**

### Step 2: I2C Communication Lines
- [ ] Connect ESP32 **GPIO 21** to Gy-302 **SDA**
- [ ] Connect ESP32 **GPIO 22** to Gy-302 **SCL**

### Step 3: Verify Connections
- [ ] Double-check all four connections are secure
- [ ] Confirm NO connections to 5V pins (BH1750 is 3.3V only!)
- [ ] Ensure SDA and SCL are not swapped

---

## 🛠 Software Setup Phase

### Step 4: Library Installation
**Option A - Edit platformio.ini manually:**
- [ ] Open `platformio.ini`
- [ ] Add `lib_deps = clamberson/BH1750@^1.0.0` under `[env:esp32dev]`
- [ ] Save the file

**Option B - Use PlatformIO Library Manager:**
- [ ] Open PlatformIO Home
- [ ] Go to Libraries tab
- [ ] Search for "BH1750"
- [ ] Find library by clamberson
- [ ] Click "Add to Project"

### Step 5: Code Preparation
- [ ] Verify `src/main.cpp` contains the BH1750 code (or copy from our version)
- [ ] Check that I2C pins are set correctly (SDA=21, SCL=22)

---

## 🚀 Testing Phase

### Step 6: Upload Code
- [ ] Connect ESP32 board to computer via USB
- [ ] Wait for board to be recognized by PlatformIO
- [ ] Click "Upload" button or use `Ctrl+Shift+P` → "PlatformIO: Upload"
- [ ] Wait for compilation and upload to complete successfully

### Step 7: Monitor Serial Output
- [ ] Open serial monitor (PlatformIO: Serial Monitor)
- [ ] Set baud rate to **115200**
- [ ] Look for initialization messages
- [ ] Verify "BH1750 sensor found!" message appears

---

## 🧪 Sensor Testing Phase

### Step 8: Basic Functionality Tests
**Test 1 - Cover the sensor:**
- [ ] Place your hand over the BH1750 sensor
- [ ] Observe lux values dropping (should go below 10 lux)
- [ ] Check that status changes to "Dark"

**Test 2 - Shine light on sensor:**
- [ ] Use phone flashlight or desk lamp
- [ ] Point light at the sensor
- [ ] Observe lux values increasing significantly
- [ ] Verify status changes appropriately

**Test 3 - LED behavior:**
- [ ] Watch built-in LED on ESP32 (GPIO 2)
- [ ] Confirm LED blinks during operation
- [ ] Note different blink patterns in different light conditions

---

## 📊 Verification Checklist

### Expected Output Verification:
- [ ] Serial monitor shows "ESP32 + BH1750 Light Sensor Test"
- [ ] Shows "BH1750 sensor found!" (not error message)
- [ ] Displays light level in lux format (e.g., "45.25 lux")
- [ ] Provides status classification ("Dark", "Indoor Light", etc.)

### Reading Range Verification:
- [ ] Dark conditions: 1-10 lux
- [ ] Normal indoor lighting: 10-100 lux
- [ ] Bright area: 100-1000+ lux

---

## 🔧 Troubleshooting Checklist

If something doesn't work, check:

### Hardware Issues:
- [ ] All connections are secure (reseat all wires)
- [ ] Using exactly 3.3V for power (NOT 5V!)
- [ ] SDA and SCL pins correctly connected
- [ ] Try different USB cable or port

### Software Issues:
- [ ] Library dependency added to platformio.ini
- [ ] Code uploaded successfully (no errors)
- [ ] Serial monitor set to correct baud rate (115200)

### Sensor Issues:
- [ ] BH1750 sensor is not damaged (check for physical damage)
- [ ] Try different I2C address if A0 pin connected differently
- [ ] Add external pull-up resistors (4.7kΩ) if readings are unstable

---

## 📝 Documentation Checklist

### File Setup:
- [ ] `README_EXPERIMENT.md` created and read
- [ ] `EXPERIMENT_GUIDE.md` reviewed for deeper understanding
- [ ] `WIRING_DIAGRAM.txt` kept handy for reference
- [ ] `QUICK_REFERENCE.md` printed or opened for quick lookup

### Code Files:
- [ ] `src/main.cpp` contains working BH1750 code
- [ ] `src/bh1750_test.cpp` available as alternative
- [ ] `platformio.ini` updated with library dependency

---

## 🎯 Success Criteria

You have successfully completed the experiment when:

✅ **Hardware**: All four connections are secure and correct  
✅ **Software**: BH1750 library installed in platformio.ini  
✅ **Upload**: Code uploads without errors to ESP32  
✅ **Communication**: "BH1750 sensor found!" message appears  
✅ **Functionality**: Light level readings change when covering/uncovering sensor  
✅ **LED Behavior**: Built-in LED blinks and responds to light conditions  

---

## 🚀 Next Steps (After Success)

Once everything works, try:
- [ ] Document your light level measurements in different environments
- [ ] Modify the code to add custom features
- [ ] Add data logging functionality
- [ ] Create a WiFi-enabled version with cloud integration
- [ ] Combine with other sensors for comprehensive monitoring

---

## 📞 Need Help?

If you get stuck:
1. Re-read this checklist and verify each item
2. Check the troubleshooting section in `EXPERIMENT_GUIDE.md`
3. Review wiring diagram in `WIRING_DIAGRAM.txt`
4. Test connections with multimeter if available

---

**Happy Experimenting! 🌟**
