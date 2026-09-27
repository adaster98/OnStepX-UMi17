# Tools

Python 3. `umi17_prepare_flash.py` uses only the standard library; `onstepx_probe.py` needs
`pyserial` (`pip install pyserial`).

---

## `umi17_prepare_flash.py`

```sh
python3 umi17_prepare_flash.py dump-a.bin [dump-b.bin] [--with-journal]
```

Builds the partition table your board needs, from that board's own flash dump.

**Run it on a dump of the board you are about to flash.** Never reuse a table built from a
different board. If the layouts differ even slightly, flashing it relocates partitions and
destroys whatever they held.

What it does:

1. Confirms two dumps are byte-identical, so you know the read was reliable
2. Parses and prints the factory partition table at `0x8000`
3. Reports whether the MD5 checksum record is present
4. With `--with-journal`, carves a 256 KB `journal` partition out of the front of `spiffs`
5. Writes a corrected table built from your board's entries, preserving every offset above

Flash the result at `0x8000`.

It exits rather than guessing whenever something does not add up: dumps that differ, a table
describing space beyond the flash, no `spiffs` to carve from, or a layout that would overrun.

### Why the MD5 record matters

The stock UMi 17 table has **no MD5 record**. Arduino-ESP32 2.0.17 builds refuse to boot
without one — the app crash-loops before `setup()` runs.

This is worth knowing because of how it looks from outside: the board comes up, the serial
port appears, and everything it sends is garbage. That reads exactly like a wrong baud rate,
so it is easy to spend a long time sweeping baud rates at a board whose problem is a missing
checksum. If a first flash appears dead, check this before anything else.

---

## `onstepx_probe.py`

```sh
python3 onstepx_probe.py /dev/ttyUSB0 9600
```

Health check. Sends fifteen read-only commands and prints the replies — identity, status
flags, RTC, site, PEC values. **Nothing moves.** Run it first after any flash.

Expect `On-Step`, `10.28x`, `:GXE7#` → `32000`, `:GXE8#` → `864`, and `:GU#` ending in `0`.

| What you see | What it means |
|---|---|
| `:GU#` ends in `0` | no error |
| `:GU#` ends in anything else | that digit is an error code |
| garbage on every command | usually a crash loop, not a baud rate — see above |
| silence on every command | wrong port, no 12 V, or the board is held in reset |

It holds DTR and RTS low after opening the port, so connecting neither resets the board nor
drops it into the bootloader.
