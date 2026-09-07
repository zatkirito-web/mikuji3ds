#!/usr/bin/env python3
"""Report whether a .3ds file is encrypted, and which keys it would need.

Reads only the NCSD/NCCH headers. No content is decrypted and nothing is written.

    python tools/check_rom_crypto.py "path/to/game.3ds"
"""

import struct
import sys

MEDIA_UNIT = 0x200

SECONDARY_SLOT = {
    0x00: ("Secure1", "slot0x2CKeyX"),
    0x01: ("Secure2", "slot0x25KeyX"),
    0x0A: ("Secure3", "slot0x18KeyX"),
    0x0B: ("Secure4", "slot0x1BKeyX"),
}


def read_ncch(f, base):
    f.seek(base + 0x100)
    if f.read(4) != b"NCCH":
        return None

    f.seek(base + 0x118)
    program_id = struct.unpack("<Q", f.read(8))[0]

    f.seek(base + 0x188)
    flags = f.read(8)

    return {
        "program_id": program_id,
        "secondary_key_slot": flags[3],
        "fixed_key": bool(flags[7] & 0x01),
        "no_romfs": bool(flags[7] & 0x02),
        "no_crypto": bool(flags[7] & 0x04),
        "seed_crypto": bool(flags[7] & 0x20),
    }


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = sys.argv[1]
    needed = set()

    with open(path, "rb") as f:
        f.seek(0x100)
        magic = f.read(4)

        partitions = []
        if magic == b"NCSD":
            f.seek(0x120)
            for index in range(8):
                offset, size = struct.unpack("<II", f.read(8))
                if size:
                    partitions.append((index, offset * MEDIA_UNIT))
            print("Container: NCSD (.3ds / cartridge image), %d partitions" % len(partitions))
        elif magic == b"NCCH":
            partitions = [(0, 0)]
            print("Container: bare NCCH")
        else:
            print("Not an NCSD or NCCH file (magic %r at 0x100)." % magic)
            return 1

        print()
        for index, base in partitions:
            ncch = read_ncch(f, base)
            if ncch is None:
                print("  partition %d at 0x%X: no NCCH header" % (index, base))
                continue

            if ncch["no_crypto"]:
                state = "DECRYPTED (no keys needed)"
            elif ncch["fixed_key"]:
                state = "encrypted with the fixed zero key (no keys needed)"
            else:
                name, key = SECONDARY_SLOT.get(ncch["secondary_key_slot"],
                                               ("unknown slot 0x%02X" % ncch["secondary_key_slot"],
                                                None))
                state = "ENCRYPTED, %s" % name
                needed.add("generatorConstant")
                needed.add("slot0x2CKeyX")
                if key:
                    needed.add(key)
                if ncch["seed_crypto"]:
                    state += " + seed crypto"
                    needed.add("seeddb.bin")

            print("  partition %d  program id %016X  %s"
                  % (index, ncch["program_id"], state))

    print()
    if needed:
        print("This ROM needs: %s" % ", ".join(sorted(needed)))
    else:
        print("This ROM needs no keys. It will load without importing anything.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
