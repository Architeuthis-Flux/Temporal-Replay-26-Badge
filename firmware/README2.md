# Temporal Badge Firmware

Firmware for the Temporal Replay Badge. The production build is PlatformIO
Arduino firmware for the ESP32-S3 badge hardware, with native C++ screens,
badge-to-badge IR flows, WiFi-backed conference features, Doom, and embedded
MicroPython apps.

Start with [`src/README.md`](src/README.md) for the C++ firmware map,
[`initial_filesystem/docs/README.md`](initial_filesystem/docs/README.md) for
badge-visible MicroPython app docs, and
[`docs/STORAGE-MODEL.md`](docs/STORAGE-MODEL.md) for the NVS / FATFS / app0
mental model + every supported flashing workflow.

## Filesystem source of truth

- `initial_filesystem/` — canonical, hand-edited, in git.
- `data/` — gitignored byte-mirror produced by
  `scripts/generate_startup_files.py`. PlatformIO's `pio run -t buildfs / uploadfs`
  reads from here. **Don't edit `data/` by hand**; your changes will be
  overwritten on the next build.

The generator runs as a `pre:` script on every PlatformIO build, so you
shouldn't normally need to invoke it explicitly. Manual run:
`python3 scripts/generate_startup_files.py`.

## Common commands

```bash
# Always use the bundled pio binary — the shell `pio` shim regularly
# resolves to a Python without platformio installed (ModuleNotFoundError).
~/.platformio/penv/bin/pio run -e echo                  # build firmware
~/.platformio/penv/bin/pio run -e echo -t uploadfs      # flash FATFS only
                                                        #   (apps + docs + doom1.wad)
python3 flash_loop_gui2.py echo                          # batch firmware + FATFS
python3 scripts/badge_sync.py sync /dev/cu.usbmodemXXX  # USB diff sync (no fatfs.bin)
```
