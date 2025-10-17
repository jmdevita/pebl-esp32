# USB/Battery Detection Guide

## How It Works

The ESP32 detects whether it's running on USB power or battery by measuring voltage at the battery terminals (GPIO 35 or 36).

**TP4054 Charging IC:**
```
USB Power (5V) → TP4054 → Battery (4.0-4.2V while charging)
```

**Detection Logic:**
- **Battery → USB:** Voltage ≥ 4.05V **AND** stable (low variance)
- **USB → Battery:** Voltage < 4.15V
- **Hysteresis zone (4.05V - 4.15V):** Stability check distinguishes USB (stable) from battery (unstable under WiFi load)

## Behavior

**When you plug in USB:**
- Detection: 3-13 seconds (3s USB check + 0-10s power loop)
- Display updates automatically

**When you unplug USB:**
- Stays "USB" for 1-2 minutes (voltage 4.2V → 4.15V)
- May oscillate 2-3 minutes (hysteresis zone)
- Settles on "BATTERY" (voltage < 4.05V)
- Total: ~3-5 minutes

**Wake from sleep with USB plugged in:**
- Forced refresh after WiFi connects (~3-5 seconds)
- Ensures accurate detection with WiFi load

## Tunable Parameters

Located in `main.cpp` lines 112-117:

### 1. ADC Calibration
```cpp
constexpr float ADC_MAX_VOLTAGE = 3.6f;  // ESP32 ADC max with 12dB attenuation
```
**Adjust if:** Voltage readings consistently wrong
- Most ESP32: 3.6V - 3.9V
- Check actual readings and adjust accordingly

### 2. USB Detection Threshold
```cpp
constexpr float USB_HIGH_VOLTAGE_THRESHOLD = 4.05f;
```
**Lower if:** Weak USB sources (bad cable/hub) not detected
- Typical range: 3.9V - 4.1V
- Too low → false USB detection from full battery

### 3. Battery Switch Threshold
```cpp
constexpr float USB_TO_BATTERY_THRESHOLD = 4.15f;
```
**Adjust for:** Oscillation duration after unplugging
- Higher → faster switch but more oscillation at boundary
- Lower → slower switch but cleaner transition

## Common Issues

**USB not detected while charging:**
- Check voltage reading (should be 4.0-4.2V)
- Lower `USB_HIGH_VOLTAGE_THRESHOLD` to 4.0V
- Verify WiFi is active during checks (stability detection)

**Stuck showing "USB" after unplugging:**
- Wait 3-5 minutes for voltage to drop
- If > 10 minutes, check thresholds

**Oscillating between USB/Battery:**
- Normal for 2-3 minutes in hysteresis zone (4.05V - 4.15V)
- If constant, widen gap between thresholds

**Lock icon disappears on power change:**
- Fixed in current code - all partial refreshes redraw lock icon

## Hardware Notes

- **Battery pin:** Auto-detects GPIO 35 or GPIO 36 (whichever has valid reading)
- **Wall adapter vs USB:** No difference - TP4054 regulates to same voltage
- **Weak USB (< 4.5V input):** May charge to < 4.05V, won't detect as USB
