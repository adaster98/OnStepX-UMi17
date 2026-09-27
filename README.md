# OnStepX for the Proxisky UMi 17 (Original)

[OnStepX](https://github.com/hjd1964/OnStepX) 10.28x, configured for the **Proxisky (Meowastro) UMi 17**
strain-wave mount, plus two things upstream does not have:

- a **partition tool** that builds the flash layout your specific board needs, from your own
  board's dump, so a flash cannot silently relocate partitions
- a **position journal** that survives power loss, so the mount knows where it is pointing
  after the power drops mid-session instead of needing a re-home

**Added an FRAM chip to the RTC module?** Use the
[`fram` branch](https://github.com/adaster98/OnStepX-UMi17/tree/fram) and its release instead:
upstream's own coordinate memory, no journal, factory partition layout.

The UMi 17 ships with Proxisky's OnStep 4.x build. This replaces it with OnStepX while
leaving the factory bootloader and every factory partition offset untouched.

**Tested on two boards.** Both a 2019 (ESP32-D0WDQ6 rev v1.0) and a 2023 (ESP32-D0WD-V3 rev
v3.1) UMi 17. Proxisky wrote one firmware for every generation, so the pin map is common —
but two boards is not many, so read the warnings and take your dumps.

---

## Read this first

Ignoring any of these can cost you the board or your Wi-Fi.

**Take two full flash dumps and confirm they match, before anything else.** They are your rollback path as this version will change your partition table.

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

## What this fork changes

Eight files differ from upstream OnStepX 10.28x, plus a sketch rename:

| File | Change |
|---|---|
| `Config.h` | UMi 17 pin map, 500:1 ratio, slew limits, journal options |
| `src/Config.defaults.h` | defaults for the new options |
| `src/lib/journal/Journal.{h,cpp}` | **new** — the flash journal |
| `src/telescope/mount/Mount.{h,cpp}` | journal hooks, absolute position limits |
| `src/telescope/mount/park/Park.command.cpp` | one line: stop tracking after unpark |
| `src/telescope/mount/park/Park.cpp`, `goto/Goto.cpp` | under `SA_PERMISSIVE`, park and gotos wake the drivers from standby instead of refusing |

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

Download `OnStepX-UMi17.bin` from
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
python3 tools/umi17_prepare_flash.py dump-a.bin dump-b.bin --with-journal
```

This checks the two dumps are identical, prints the factory layout, and writes
`umi17-partitions-journal.bin` built from **your** board's own entries. It preserves every
factory offset and carves the journal out of the unused `spiffs` region:

```
nvs        off=0x0009000 size=0x0005000     unchanged - settings and park position
otadata    off=0x000e000 size=0x0002000     unchanged
app0       off=0x0010000 size=0x0140000     unchanged - the firmware
app1       off=0x0150000 size=0x0140000     unchanged
eeprom     off=0x0290000 size=0x0001000     unchanged
journal    off=0x0291000 size=0x0040000     new, carved from spiffs
spiffs     off=0x02d1000 size=0x012f000     shrunk; OnStepX never uses it
```

If the tool refuses, stop and read what it says. It exits rather than guessing.

For both boards I have seen, the result hashes to
`d1893df020b9bffc28a2c4e25bd8bd4f91ef8a2bb55a37139599bf2bc17e16dc`. A different hash is not
necessarily wrong — it means your factory layout differs, which is exactly why the tool
reads your own dump rather than shipping a fixed table.

Leave off `--with-journal` if you would rather not have the journal. Then build with
`JOURNAL OFF` in `Config.h`, or the firmware will log that the partition is missing and
carry on without it.

### 3. Flash

Back into the bootloader — short jumper (or hold flash button), power up, release.

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  --before no_reset --after no_reset write_flash \
  0x8000  umi17-partitions-journal.bin \
  0x10000 OnStepX-UMi17.bin
```

Then power-cycle normally. First boot spends about 3 seconds erasing the journal partition.

### 4. Verify

```sh
python3 tools/onstepx_probe.py /dev/ttyUSB0 9600
```

Fifteen read-only commands, nothing moves. Expect `On-Step`, `10.28x`, `:GXE7#` → `32000`,
and `:GU#` ending in `0` (no error). `:GVT#` reports the build time compiled into the
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

### Testing the journal without hardware

```sh
cd test/journal-sim && make
```

Compiles the real `Journal.cpp`, straight out of the tree, against a fake flash that
enforces NOR semantics: writes may only clear bits, erases must be 4 KB aligned, and a power
cut can leave a record half programmed. Runs under ASan and UBSan.

It covers what is impractical to reach on hardware — a full wrap of the ring takes about
five nights of real tracking. Run it after any change to `Journal.cpp`.

---

## The position journal

Cut the power mid-session and the mount comes back knowing roughly where it is pointing,
instead of at home.

OnStepX normally refuses this without FRAM, and it is right to: `Mount::poll()` would
rewrite one flash sector every second and wear it out in about **3.5 nights**. So this does
not rewrite anything. It appends 16-byte records into a 256 KB ring and never erases on the
write path, because a 4 KB erase blocks for ~64 ms and that would show up in a guide trace.
Erases happen only while the mount is idle, keeping a runway of pre-erased sectors ahead.

At a 10-second heartbeat that is roughly **56,000 nights** of flash endurance, from two
multipliers: 256 appends per erase, and 63 sectors sharing the wear.

On boot it restores the last recorded position, but deliberately does **not** resume
tracking — you come back unparked and idle, with the position known.

Measured on hardware over a 12-minute tracking run ending in an abrupt power cut:

| | |
|---|---|
| Records | 79 valid, **0 bad CRC**, no sequence gaps |
| Tracking rate from the journal | 15.04080 arcsec/s vs 15.04107 true, **−0.0018%** |
| Restore granularity | one heartbeat, so up to **2.5 arcmin** worst case |

That last row is the honest limitation: the journal knows where you were as of the last
record. It is a good starting point for a plate solve, not a replacement for one.

Options in `Config.h`:

| Option | Default | What |
|---|---|---|
| `JOURNAL` | `ON` | the whole feature |
| `JOURNAL_HEARTBEAT_SECONDS` | `10` | position record interval while moving |
| `JOURNAL_RUNWAY_SECTORS` | `16` | pre-erased sectors; 16 is ~12 h of tracking |
| `JOURNAL_RESTORE` | `ON` | apply the recorded position at boot |
| `JOURNAL_FLIGHT_RECORDER` | `ON` | also log boots, park/unpark, slews, limit trips |
| `APS` | `ON` | absolute axis backstops, independent of coordinate transforms |

`APS_AXIS1_MAX_DEG` and `APS_AXIS2_MAX_DEG` are hard limits on the axis positions, wider
than the normal limits, so a bad sync cannot drive a 500:1 strain wave into the tripod.

- **Axis1** is checked as the raw shaft angle, which is the hour angle east of the pier and
  the hour angle plus 180° west of it. With the default 15° meridian limits it stays within
  −15° to 195°, so 200° sits 20° past the meridian on the west side. If you widen the west
  meridian limit beyond 20°, raise `APS_AXIS1_MAX_DEG` with it.
- **Axis2** is checked as a declination, so 100° is 10° beyond the ±90° limits on either side
  of the pier. The raw axis2 angle passes through the pole to change sides and reads 180° −
  Dec west of the pier, which is why it has to be converted first.

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
- **The ring wrap is verified in simulation, not on hardware.** It needs about five nights
  of continuous tracking to reach.

---

## Licence and credit

GPL v3, inherited from OnStepX. See [LICENSE](LICENSE).

OnStepX is by **Howard Dutton** — <https://github.com/hjd1964/OnStepX>. Nearly all of the
code here is his; this fork is a configuration, a partition tool and one storage feature.
Bugs you find here are almost certainly mine, not upstream's, so report them here first.

Not affiliated with or endorsed by Proxisky.
