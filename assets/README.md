# ESP32 Arduino Client Assets

This directory contains converted image assets for the ESP32 e-paper display.

## Files

- **lock-locked.xbm** - Locked padlock icon (20x20 pixels)
- **lock-unlock.xbm** - Unlocked padlock icon (20x20 pixels)

## Format: XBM (X BitMap)

XBM is a monochrome bitmap format that can be embedded directly in C code:
- **Size**: 20x20 pixels
- **Memory**: ~60 bytes per icon in flash (PROGMEM)
- **RAM usage**: 0 bytes (stored in flash, not RAM)
- **Format**: Plain text C array

## Source Files

These XBM files are generated from:
- `client/assets/images/lock-locked.png`
- `client/assets/images/lock-unlock.png`

## Regenerating XBM Files

To regenerate these files from source PNG icons:

```bash
cd /path/to/slack-reactions
source env/bin/activate
python3 convert_icons_to_xbm.py
```

The conversion script:
1. Loads PNG files from `client/assets/images/`
2. Converts to monochrome (black/white)
3. Saves as XBM in this directory
4. Preserves original PNG files (no modification)

## Usage in Code

The XBM data is embedded directly in `src/main.cpp`:

```cpp
namespace LockIcons {
    const unsigned char locked_bits[] PROGMEM = {
        // ... bitmap data from lock-locked.xbm ...
    };
}

// Render on display
display->drawBitmap(x, y, LockIcons::locked_bits,
                   20, 20, GxEPD_BLACK);
```

## Memory Efficiency

XBM format is ideal for ESP32 because:
- **No decoder library needed** - just raw bitmap data
- **Stored in flash** - uses PROGMEM, not precious RAM
- **Monochrome** - perfect for e-paper displays
- **Tiny footprint** - ~60 bytes per 20x20 icon

Compare to alternatives:
- PNG file: ~500 bytes + decoder overhead (~2KB RAM)
- BMP file: ~400 bytes + parser overhead (~1KB RAM)
- XBM: ~60 bytes, 0 RAM overhead ✅