# Quad UART Adapter

## Reproducible Framework Patches

This project depends on local changes in `framework-arduinopico` (RP2040 Arduino core and bundled Adafruit TinyUSB sources).

To make builds reproducible across machines:

- `platformio.ini` pins `framework-arduinopico` to commit `275e73d13e5248418e13507d3988ebbf785cff36`
- `scripts/apply_framework_patches.py` runs before each build
- patch files live in `patches/framework-arduinopico/*.patch`

Build as usual:

```bash
pio run
```

or upload:

```bash
pio run -t upload
```

### Updating the Patch Set

If you intentionally change framework files again, regenerate patches from your local package checkout and commit the updated patch files.

```bash
git -C ~/.platformio/packages/framework-arduinopico diff -- \
  cores/rp2040/SerialPIO.h \
  cores/rp2040/SerialPIO.cpp \
  > patches/framework-arduinopico/0001-serialpio-fixes.patch

git -C ~/.platformio/packages/framework-arduinopico/libraries/Adafruit_TinyUSB_Arduino diff -- \
  src/arduino/Adafruit_USBD_Device.h \
  src/arduino/ports/rp2040/tusb_config_rp2040.h \
  | awk '
/^diff --git a\// {sub("^diff --git a/","diff --git a/libraries/Adafruit_TinyUSB_Arduino/"); sub(" b/"," b/libraries/Adafruit_TinyUSB_Arduino/"); print; next}
/^--- a\// {sub("^--- a/","--- a/libraries/Adafruit_TinyUSB_Arduino/"); print; next}
/^\+\+\+ b\// {sub("^\+\+\+ b/","+++ b/libraries/Adafruit_TinyUSB_Arduino/"); print; next}
{print}
' > patches/framework-arduinopico/0002-adafruit-tinyusb-cdc-and-descriptor.patch
```

Current patch coverage:

- `cores/rp2040/SerialPIO.h`
- `cores/rp2040/SerialPIO.cpp`
- `libraries/Adafruit_TinyUSB_Arduino/src/arduino/Adafruit_USBD_Device.h`
- `libraries/Adafruit_TinyUSB_Arduino/src/arduino/ports/rp2040/tusb_config_rp2040.h`
