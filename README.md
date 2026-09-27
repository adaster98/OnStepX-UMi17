# OnStepX for the Proxisky UMi 17 (FRAM)

[OnStepX](https://github.com/hjd1964/OnStepX) 10.28x, configured for the **Proxisky (Meowastro) UMi 17**
strain-wave mount **with an FRAM chip added to the RTC module**. With FRAM, upstream's own
coordinate memory works, so the mount knows where it is pointing after the power drops
mid-session instead of needing a re-home.

**This branch needs the hardware modification below.** A stock UMi 17 has no FRAM; use the
[`main`](https://github.com/adaster98/OnStepX-UMi17/tree/main) branch and its journal
release instead.

Apart from `Config.h`, the only source change is one line so unpark does not start
tracking. The branch sits directly on the latest upstream.

The UMi 17 ships with Proxisky's OnStep 4.x build. This replaces it with OnStepX while
leaving the factory bootloader and every factory partition offset untouched.

**Tested on two boards.** Both a 2019 (ESP32-D0WDQ6 rev v1.0) and a 2023 (ESP32-D0WD-V3 rev
v3.1) UMi 17. Proxisky wrote one firmware for every generation, so the pin map is common —
but two boards is not many, so read the warnings and take your dumps.

---

## Read this first

Ignoring any of these can cost you the board or your Wi-Fi.

**Take two full flash dumps and confirm they match, before anything else.** They are your rollback path as this version will add a checksum record to your partition table.

**Never flash a partition table from another board.** Generate it from your own dump with
the tool here. If the layouts differ even slightly, a blind flash relocates partitions and
destroys whatever they held.

**Never flash the bootloader.** Not the factory one, not the Arduino-generated one. The
factory bootloader is configured for this board's DIO flash wiring and is not something you
can replace blind. Everything here writes only `0x8000` (partition table) and `0x10000`
(application).

**Never change the Wi-Fi credentials on the vendor web server.** The Wi-Fi is a separate
ESP-07S module, and its `RST` and `GPIO0` pins are **not wired to the ESP32**. Set a wrong
password and you cannot reflash it — the Wi-Fi is gone permanently, with no recovery path
short of soldering.

---

## The hardware modification

An **MB85RC256V** FRAM module (32 KB, I2C) soldered onto the DS3231 RTC module's pins, so it
shares the RTC's power, ground, SDA (GPIO21) and SCL (GPIO22). Leave its address pins low.

Check it before flashing: an I2C scan should show

| Address | Device |
|---|---|
| `0x50` | the FRAM, where OnStepX looks by default |
| `0x57` | the RTC module's own AT24C32 EEPROM, if it has one; unused, harmless |
| `0x68` | the DS3231 |
| `0x7C` | the FRAM again; it answers here when asked for its device ID |

If the FRAM shows up anywhere other than `0x50`, add `#define NV_I2C_ADDRESS 0x..` to
`Config.h` and build from source.

---

## What this fork changes

Two files differ from upstream OnStepX 10.28x, plus a sketch rename:

| File | Change |
|---|---|
| `Config.h` | UMi 17 pin map, 500:1 ratio, slew limits, FRAM and coordinate memory |
| `src/telescope/mount/park/Park.command.cpp` | one line: stop tracking after unpark |

Everything else is upstream, unmodified, so rebasing onto a newer OnStepX is a small job.

**One behaviour difference to know about: unpark does not start tracking.** Upstream's
`:hR#` resumes tracking as part of unparking; here it leaves the mount idle so whatever is
driving it decides when tracking starts. If your client sends its own tracking command
after unparking you will never notice. If it does not, or you are sending LX200 commands
by hand, follow `:hR#` with `:Te#`.

`TRACK_AUTOSTART ON` in `Config.h` restores upstream's unpark behaviour, but it also starts
tracking at boot, which is a larger change than most people want.

`OnStepX.ino` is also renamed to `OnStepX-UMi17.ino`. Arduino requires the sketch filename
to match its folder, so without the rename a plain clone of this fork will not compile.

---

## Flashing the release binary

Download `OnStepX-UMi17-fram.bin` from
[Releases](https://github.com/adaster98/OnStepX-UMi17/releases), or build your own below.

You need `esptool` and Python 3. `pip install esptool` covers both.

### 1. Dump your board, twice

Short the flash jumper (or hold the flash button on newer boards), power on with 12 V, then release. Then:

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset read_flash 0 0x400000 dump-a.bin
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset read_flash 0 0x400000 dump-b.bin
```

**Keep these files somewhere safe** They are irreplaceable.

### 2. Build your partition table

```sh
python3 tools/umi17_prepare_flash.py dump-a.bin dump-b.bin
```

This checks the two dumps are identical, prints the factory layout, and writes
`umi17-partitions-md5-THISBOARD.bin` built from **your** board's own entries. The layout is
the factory one, unchanged. The only addition is the MD5 checksum record that the factory
table lacks: without it, any firmware built on a current Arduino core rejects the table and
reboots forever before it starts.

If the tool refuses, stop and read what it says. It exits rather than guessing.

For both boards I have seen, the result hashes to
`85c5a1fd65a10a933fe597a6bb47e2a54cd89ca0969ca24967402df7d489dd59`. A different hash is not
necessarily wrong — it means your factory layout differs, which is exactly why the tool
reads your own dump rather than shipping a fixed table.

Do not pass `--with-journal`. That is for the `main` branch; this build does not use a
journal.

### 3. Flash

Back into the bootloader — short jumper (or hold flash button), power up, release.

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset write_flash \
  0x8000  umi17-partitions-md5-THISBOARD.bin \
  0x10000 OnStepX-UMi17-fram.bin
```

**Coming from the journal build?** Flash the same two files, then blank the old journal
area so the layout matches factory again (the factory `spiffs` region is empty):

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset erase_region 0x291000 0x40000
```

Then power-cycle normally. The first boot formats the FRAM, so every setting starts at its
default. Nothing is copied over from the old settings.

### 4. Verify

```sh
python3 tools/onstepx_probe.py /dev/ttyUSB0 9600
```

Fifteen read-only commands, nothing moves. Expect `On-Step`, `10.28x`, `:GXE7#` → `32000`,
and `:GU#` ending in `0` (no error).

Then, with the mount at home (counterweight down, pointing at the pole), set the time and
site, send `:hQ#` to save park at home, and `:SX93,1#` for the full slew rate. `:GVT#` reports the build time compiled into the
image, which is the only reliable way to know what is actually running.

---

## Rolling back to factory

This is what the dumps from step 1 are for. Without one you cannot go back — there is no
public factory image that matches your board, and the two I dumped differ in 95% of their
bytes, so someone else's will not do.

Back into the bootloader — short jumper (or hold flash button), power up, release. Then
write the whole dump back:

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset \
  --flash_mode keep --flash_freq keep --flash_size keep \
  write_flash 0x0 dump-a.bin
```

4 MB at 115200 takes several minutes. Raise the baud if the link has been reliable.

The three `keep` flags stop esptool rewriting the flash mode and size bytes in the image
header. Recent esptool defaults to `keep` already, and a write at `0x0` never triggers the
patch anyway — it only applies to writes landing exactly on the bootloader offset — but the
flags cost nothing and make the command safe on older versions.

Then verify before trusting it:

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset read_flash 0 0x400000 rollback-check.bin
sha256sum dump-a.bin rollback-check.bin
```

Two identical hashes means the board is byte-for-byte back to how you found it. Power-cycle
normally and the vendor app should find it again.

### Why this one may write the bootloader

Everywhere else this README says never to flash the bootloader. This is the exception, and
the reason is that the bytes are your own board's, not a stranger's — writing them back is
restoring what is already there, not replacing it with something that may not match.

It is also recoverable if it goes wrong. The ESP32's first-stage bootloader lives in mask
ROM on the die and cannot be erased or overwritten, so GPIO0 download mode keeps working no
matter what state the flash is in. An interrupted write leaves a board that will not boot,
but never one you cannot talk to. Redo the write and it comes back.

### What rollback does not touch

**The Wi-Fi module.** It is a separate ESP-07S with its own flash, and nothing in this repo
ever writes to it. It keeps whatever configuration it had throughout.

**Anything you changed after dumping.** The restore puts back the settings, park position
and pairings as they were the moment you took the dump, not as they were just before you
rolled back.

---

## Building from source

The build pins **arduino-esp32 core 2.0.17**. This is deliberate, not laziness:
`HAL_MAXRATE_LOWER_LIMIT` is 16 µs on core 2.0.0 and 40 µs on 2.0.1+, which changes the
fastest step rate the mount can reach. Newer cores are not drop-in.

```sh
arduino-cli core install esp32:esp32@2.0.17
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build .
```

The image lands at `build/OnStepX.ino.bin`. Check it before flashing — it must fit `app0`
and must be a UMi 17 build, not a stock MaxESP3 one:

```sh
B=build/OnStepX.ino.bin
[ "$(stat -c%s $B)" -lt 1310720 ] && echo "size OK: $(stat -c%s $B) / 1310720"
for s in UMi17 "MaxESP v3" OnStepX; do grep -aq "$s" $B && echo "ident $s present"; done
```

`grep -a` matters — without it most greps skip binary files and report nothing found on a
perfectly good image.

Rebuilds are never byte-identical — `__DATE__` and `__TIME__` are compiled in — so compare
`:GVT#` rather than hashes when you want to know what is on a board.

---

## Coordinate memory

This is upstream's `MOUNT_COORDS_MEMORY`, switched on. Upstream refuses it without FRAM,
because it saves the position every second and would wear out flash in days. FRAM rewrites
in place and lasts about 10^12 writes, so one write a second is fine for centuries.

- **Saved every second**, as two alternating records, each with a checksum written last. A
  write torn by a power cut fails its checksum and the other copy is used.
- **Restored at boot** if the mount was not parked. It comes back unparked and not tracking,
  knowing where it points to within about a second of motion.
- **A parked mount boots parked**, and `:hR#` unparks it as usual.

The settings `Config.h` changes for this:

| Option | Value | Why |
|---|---|---|
| `NV_DRIVER` | `NV_MB85RC32` | FRAM at `0x50`, using 4 KB of the chip; the same partition sizes as the ESP32's own 4 KB settings store |
| `MOUNT_COORDS_MEMORY` | `ON` | the feature |
| `MOUNT_STARTUP_MODE` | `SA_PERMISSIVE` | see below |

**Why `SA_PERMISSIVE`.** Upstream only trusts a remembered position if the mount could not
have moved while it was off. With coordinate memory on, the default `SA_AUTO` never trusts a
mount that was powered off while parked, so `:hR#` is refused after every parked power
cycle. The UMi 17 cannot move while unpowered: the RA brake engages and a 500:1 harmonic
drive is not back-drivable. So trusting it at boot is safe here, and it needs no code
change. It would not be safe on a mount with clutches you might release while it is off.

`:ENVRESET#` wipes the FRAM settings, the same way it wipes the ESP32's settings on a stock
build.

---

## Known limitations

- **The drivers read their microstep pins only when enabled**, so OnStepX's usual switch to a
  coarser mode for fast moves is ignored mid-move. Builds before 2026-09-27 relied on it, which
  made every fast move a quarter of the distance counted. This build runs a fixed 32 microsteps
  instead, which reaches the factory 3°/s.
- **Upgrading from an older build:** send `:SXE7,32000#` once and power-cycle, or start fresh with
  `:ENVRESET#`. The saved PEC worm length (64000) no longer matches 32 microsteps, and until it
  does, PEC reports an init error and the mount refuses gotos and park.
- **The buzzer does not work on one of my two boards.** A full GPIO sweep found no pin that
  drove it. Probably a broken trace on that unit; `STATUS_BUZZER_PIN 19` is correct.
- **The TMC2209 UART bus does not answer.** Tested at multiple bauds and addresses with an
  echo-validated rig. The drivers are almost certainly wired for standalone step/dir, so
  software current control is not available.
- **ST4 is unusable without hardware changes**, so `ST4_INTERFACE` is `OFF`.

---

## Licence and credit

GPL v3, inherited from OnStepX. See [LICENSE](LICENSE).

OnStepX is by **Howard Dutton** — <https://github.com/hjd1964/OnStepX>. Nearly all of the
code here is his; this branch is a configuration, a partition tool and one line.
Bugs you find here are almost certainly mine, not upstream's, so report them here first.

Not affiliated with or endorsed by Proxisky.
