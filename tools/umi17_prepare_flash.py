#!/usr/bin/env python3
"""Verify a UMi 17 flash dump and generate the partition table it needs.

Usage:
  python3 umi17_prepare_flash.py dump-a.bin [dump-b.bin]

Given one or two full flash dumps of a board, this:
  - checks the two dumps are identical (if two given)
  - reports the dump SHA-256
  - parses and prints the partition table at 0x8000
  - says whether the MD5 checksum record IDF 4.4 requires is present
  - writes a corrected table matching THAT board's own layout

Pass --with-journal to also carve a 256 KB "journal" partition out of the
front of the unused spiffs region, leaving every other offset untouched.

Never assume another board's table applies. Always run this on the dump
taken from the board you are about to flash.
"""
import hashlib
import struct
import sys

MAGIC = b"\xaa\x50"
MD5_MAGIC = b"\xeb\xeb"
TABLE_OFF = 0x8000
TABLE_LEN = 0x1000

TYPES = {0x00: "app", 0x01: "data"}
SUBTYPES = {
    (0x00, 0x10): "ota_0", (0x00, 0x11): "ota_1", (0x00, 0x00): "factory",
    (0x01, 0x00): "ota(data)", (0x01, 0x02): "nvs", (0x01, 0x82): "spiffs",
    (0x01, 0x99): "eeprom",
}


def sha(b):
    return hashlib.sha256(b).hexdigest()


def parse(table):
    parts, body, md5_ok, md5_present = [], b"", None, False
    for i in range(0, TABLE_LEN, 32):
        e = table[i:i + 32]
        if e[:2] == MAGIC:
            _, ty, sub, off, size, name, flags = struct.unpack("<2sBBLL16sL", e)
            parts.append({
                "name": name.split(b"\x00")[0].decode(errors="replace"),
                "type": ty, "subtype": sub, "off": off, "size": size, "flags": flags,
            })
            body += e
        elif e[:2] == MD5_MAGIC:
            md5_present = True
            md5_ok = (e[16:] == hashlib.md5(body).digest())
        elif e[:2] == b"\xff\xff":
            break
        else:
            print(f"  !! unrecognised entry at table offset {i:#x}: magic {e[:2].hex()}")
    return parts, body, md5_present, md5_ok


def add_journal(parts, size=0x40000):
    """Carve a journal partition out of the front of spiffs.

    The journal needs its own partition so a bug there can never reach NVS,
    which holds park position and settings. spiffs is unused by this firmware.
    """
    out = []
    for p in parts:
        if p["name"] != "spiffs":
            out.append(p)
            continue
        if p["size"] <= size:
            print("  *** spiffs too small to carve a journal from - skipping ***")
            out.append(p)
            continue
        out.append({"name": "journal", "type": 0x01, "subtype": 0x82,
                    "off": p["off"], "size": size, "flags": 0})
        out.append({"name": "spiffs", "type": p["type"], "subtype": p["subtype"],
                    "off": p["off"] + size, "size": p["size"] - size, "flags": 0})
    return out


def build_table(parts):
    body = b""
    for p in parts:
        body += struct.pack("<2sBBLL16sL", MAGIC, p["type"], p["subtype"],
                            p["off"], p["size"], p["name"].encode(), p["flags"])
    table = body + MD5_MAGIC + b"\xff" * 14 + hashlib.md5(body).digest()
    return table + b"\xff" * (TABLE_LEN - len(table))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    want_journal = "--with-journal" in sys.argv
    argv = [a for a in sys.argv if a != "--with-journal"]
    paths = argv[1:3]
    dumps = []
    for p in paths:
        with open(p, "rb") as f:
            dumps.append(f.read())
        print(f"{p}: {len(dumps[-1]):,} bytes  sha256 {sha(dumps[-1])}")

    if len(dumps) == 2:
        if dumps[0] == dumps[1]:
            print("\nBoth dumps are byte-for-byte identical. Good.")
        else:
            print("\n*** DUMPS DIFFER — the read is unreliable. Re-dump before going further. ***")
            sys.exit(2)
    else:
        print("\nOnly one dump given. Take a second and re-run to confirm the read.")

    data = dumps[0]
    if len(data) % 0x100000:
        print(f"*** Unexpected dump size {len(data):#x}; expected a whole number of MB. ***")

    print(f"\nPartition table at {TABLE_OFF:#x}:")
    parts, body, md5_present, md5_ok = parse(data[TABLE_OFF:TABLE_OFF + TABLE_LEN])
    if not parts:
        print("  *** No valid partition entries found. Do not flash this board. ***")
        sys.exit(2)

    for p in parts:
        label = SUBTYPES.get((p["type"], p["subtype"]), f"{p['subtype']:#04x}")
        print(f"  {p['name']:<10} {TYPES.get(p['type'], p['type']):<5} {label:<10} "
              f"off={p['off']:#09x} size={p['size']:#09x} end={p['off']+p['size']:#09x}")

    end = max(p["off"] + p["size"] for p in parts)
    print(f"\n  entries: {len(parts)}   highest end: {end:#x}   dump size: {len(data):#x}")
    if end > len(data):
        print("  *** Table describes space beyond the dump. Sizes disagree — stop. ***")
        sys.exit(2)

    print(f"  MD5 record present: {md5_present}" + ("" if not md5_present else f" (valid: {md5_ok})"))

    app0 = next((p for p in parts if p["name"] == "app0"), None)
    if app0:
        print(f"\n  app0 accepts an image up to {app0['size']:,} bytes at {app0['off']:#x}")

    if md5_present and md5_ok and not want_journal:
        print("\nThis table already has a valid MD5 record. Arduino-ESP32 2.0.17 will")
        print("accept it as-is, so flash ONLY the application. No table write needed.")
        return

    if want_journal:
        # A journal partition has to be added even when the existing table is
        # already valid, so this runs before the MD5 shortcut above can apply.
        if not any(p["name"] == "journal" for p in parts):
            if not any(p["name"] == "spiffs" for p in parts):
                print("\n  *** No spiffs partition to carve a journal from. ***")
                print("  Build with JOURNAL OFF, or free space in the layout by hand.")
                sys.exit(2)
            parts = add_journal(parts)
        else:
            print("\n  This board already has a journal partition; keeping it as-is.")
        print("\n  with journal partition:")
        for p in parts:
            print(f"    {p['name']:<10} off={p['off']:#09x} size={p['size']:#09x} "
                  f"end={p['off']+p['size']:#09x}")
        end = max(p["off"] + p["size"] for p in parts)
        if end > len(data):
            print("  *** layout exceeds the flash size - refusing ***")
            sys.exit(2)

    out = ("umi17-partitions-journal.bin" if want_journal
           else "umi17-partitions-md5-THISBOARD.bin")
    table = build_table(parts)
    with open(out, "wb") as f:
        f.write(table)

    print(f"\nNo valid MD5 record — this board needs a corrected table, same as the spare.")
    print(f"Wrote {out} ({len(table)} bytes), built from THIS board's own entries.")
    print(f"  sha256 {sha(table)}")
    print(f"Flash it at {TABLE_OFF:#x}. It preserves every offset above.")


if __name__ == "__main__":
    main()
