# ESP32 Display Support Documentation

## Quick Reference

**Which environment should I use?**

Check your display chip label and use the matching environment:

- **DEPG0213BN** → `lilygo_t5_depg_bw` (2-level BW) ✅ Tested
- **GDEW0213T5D** → `lilygo_t5_gdew_4g` (4-level grayscale) ✅ Tested & Recommended
- **GDEW0213T5D** → `lilygo_t5_gdew_bw` (2-level BW fallback) ✅ Tested
- **GDEM0213B74** → `lilygo_t5_gdem_4g` (4-level) ⚠️ Untested
- **GDEY0213B74** → `lilygo_t5_gdey_4g` (4-level) ⚠️ Untested
- **GDEY027T91** → `lilygo_t5s_27` (2.7" BW) ⚠️ Untested

**⚠️ Important**: Don't mix up display chips - using the wrong driver causes "Busy Timeout!" errors!

## Supported Displays

The firmware supports multiple LilyGo T5 e-paper displays through **compile-time environment selection**:

### 2.13" Displays (212×104 pixels)
- **DEPG0213BN** (2-level BW) - Most common T5 V2.3.1
- **GDEW0213T5D** (4-level grayscale with GDEW0213I5F driver)
- **GDEM0213B74** (4-level grayscale) - Untested
- **GDEY0213B74** (4-level grayscale) - Untested

### 2.7" Displays (264×176 pixels)
- **GDEY027T91** (2-level BW) - Untested

## How Display Selection Works

The correct display driver is **automatically selected at compile-time** based on:
1. **Display dimensions** (`DISPLAY_WIDTH` and `DISPLAY_HEIGHT` in platformio.ini)
2. **Grayscale flag** (`DISPLAY_4G_GRAYSCALE` in platformio.ini)

You **do not** need to modify `main.cpp` - just select the correct PlatformIO environment.

## Selecting Your Display

### Option 1: Use Pre-configured Environments

```bash
# For 2.13" BW (DEPG0213BN)
pio run -e lilygo_t5_depg_bw -t upload

# For 2.13" BW (GDEW0213T5D without grayscale)
pio run -e lilygo_t5_gdew_bw -t upload

# For 2.13" 4G grayscale (GDEM/GDEY0213B74)
pio run -e lilygo_t5_gdem_4g -t upload

# For 2.13" 4G grayscale (GDEW0213T5D - uses GDEW0213I5F driver)
pio run -e lilygo_t5_gdew_4g -t upload

# For 2.7" BW (GDEY027T91)
pio run -e lilygo_t5s_27 -t upload
```

### Option 2: Set Default Environment

Edit `platformio.ini`:
```ini
[platformio]
default_envs = lilygo_t5_depg_bw  # Change to your display type
```

Then just run:
```bash
pio run -t upload
```

## Configuration

Display settings in `data/config.json` control **runtime behavior** only (not driver selection):

```json
{
  "display": {
    "width": 212,        # Must match your hardware
    "height": 104,       # Must match your hardware
    "rotation": 1,       # 0=portrait, 1=landscape, 2=inverted portrait, 3=inverted landscape
    "pins": {
      "cs": 5,
      "dc": 17,
      "rst": 16,
      "busy": 4,
      "sclk": 18,
      "mosi": 23
    }
  }
}
```

**Note**: Width and height are used for bounds checking, not driver selection.

## Known Display Models

| Display Chip | Environment | Driver Used | Resolution | Grayscale | Status | Notes |
|--------------|-------------|-------------|------------|-----------|--------|-------|
| **DEPG0213BN** | `lilygo_t5_depg_bw` | GxEPD2_213_B74 | 212×104 | 2-level | ✅ **Working** | Most common T5 V2.3.1 |
| **GDEW0213T5D** | `lilygo_t5_gdew_bw` | GxEPD2_213_T5D | 212×104 | 2-level | ✅ **Working** | BW only (UC8151D) |
| **GDEW0213T5D** | `lilygo_t5_gdew_4g` | GxEPD2_213_flex (I5F) | 212×104 | 4-level | ✅ **Working** | **Recommended for grayscale** |
| **GDEM0213B74** | `lilygo_t5_gdem_4g` | GxEPD2_213_GDEY0213B74 | 212×104 | 4-level | ⚠️ Untested | For actual GDEM chip only |
| **GDEY0213B74** | `lilygo_t5_gdey_4g` | GxEPD2_213_GDEY0213B74 | 212×104 | 4-level | ⚠️ Untested | For actual GDEY chip only |
| **GDEY027T91** | `lilygo_t5s_27` | GxEPD2_270 | 264×176 | 2-level | ⚠️ Untested | 2.7" display |

## Pin Configuration

Standard LilyGo T5 pin configuration (same for all models):
- **CS**: GPIO 5
- **DC**: GPIO 17
- **RST**: GPIO 16
- **BUSY**: GPIO 4
- **SCLK**: GPIO 18
- **MOSI**: GPIO 23

These can be adjusted in `config.json` if your board uses different pins.

## Adding New Display Types

To add support for a new display:

1. **Add environment to platformio.ini:**
```ini
[env:my_new_display]
build_flags =
    ${env.build_flags}
    -D DISPLAY_WIDTH=xxx
    -D DISPLAY_HEIGHT=yyy
    -D DISPLAY_4G_GRAYSCALE  # Only if 4-level grayscale
lib_deps =
    ${env.lib_deps}
    zinggjm/GxEPD2@^1.5.3  # Or GxEPD2_4G for grayscale
```

2. **Add driver to main.cpp** (if not auto-detected):
```cpp
#elif defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == xxx
    display = new GxEPD2_BW<GxEPD2_YOUR_DRIVER, ...>(...)
```

3. **Update config.json** with dimensions:
```json
{
  "display": {
    "width": xxx,
    "height": yyy
  }
}
```

## Testing Display Configuration

After selecting your environment:

```bash
# Build and upload
pio run -e your_environment -t upload

# Upload config
pio run -e your_environment -t uploadfs

# Monitor output
pio device monitor -b 115200
```

Look for initialization messages:
```
[INFO][DISPLAY] Driver: 2.13" BW (GxEPD2_213_B74)
[INFO][DISPLAY] Initialized | width=212 height=104 rotation=1
```

## Special Notes

### GDEW0213T5D Grayscale Support

The **GDEW0213T5D** display uses the UC8151D controller, which supports 4-level grayscale. However, the GxEPD2_4G library doesn't have a native driver for this specific chip.

**Solution**: Use the **GDEW0213I5F driver** (GxEPD2_213_flex) for grayscale support:
- Both displays use UC8151-based controllers (UC8151D vs IL0373/UC8151)
- Same resolution (104×212 / 212×104)
- Compatible LUT timings for grayscale operation
- **Environment**: `lilygo_t5_gdew_4g`

**Verified Working**:
- ✅ 4-level grayscale rendering works correctly
- ✅ Faster refresh times compared to BW mode
- ✅ Better emoji and image rendering
- ✅ Tested and confirmed on actual hardware

**Important**: Do NOT use `lilygo_t5_gdem_4g` or `lilygo_t5_gdey_4g` for GDEW0213T5D - these use different drivers (GDEY0213B74) and will cause "Busy Timeout!" errors.

If you experience issues, fall back to BW mode using `lilygo_t5_gdew_bw`.

## Technical Notes

- **C++ Templates**: GxEPD2 uses compile-time templates, so the driver must be selected at build time
- **Memory Efficiency**: Each environment only includes the libraries it needs
- **Automatic Selection**: Display driver is chosen based on `DISPLAY_WIDTH`, `DISPLAY_HEIGHT`, and `DISPLAY_4G_GRAYSCALE` flags
- **Runtime Configuration**: `config.json` provides pin mappings and rotation, not driver selection
- **Driver Compatibility**: Some displays can use drivers for similar chips with compatible controllers