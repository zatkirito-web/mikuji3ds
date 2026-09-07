#!/usr/bin/env python3
"""Check an aes_keys.txt before putting it on the iPad.

Mirrors the parser in Cytrus/System/core/hw/aes/key.cpp, so what this reports is what the
emulator will see. Key values are never printed - only which names are present.

    python tools/check_aes_keys.py path/to/aes_keys.txt
"""

import sys
import re

# Keys needed to decrypt encrypted NCCH content, and what each one is for.
NCCH_KEYS = {
    "generatorConstant": "derives every normal key from a KeyX/KeyY pair",
    "slot0x2CKeyX": "Secure1, used by almost every retail title",
    "slot0x25KeyX": "Secure2, titles from system version 7.x onwards",
    "slot0x18KeyX": "Secure3",
    "slot0x1BKeyX": "Secure4",
}

SLOT_RE = re.compile(r"^slot0x([0-9A-Fa-f]{2})Key([XYN])$")
COMMON_RE = re.compile(r"^common([0-9]+)$")


def normalize(line):
    """Same normalization the emulator does: strip a BOM, CR, and surrounding whitespace."""
    if line.startswith("﻿"):
        line = line[1:]
    return line.strip(" \t\r\n")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = sys.argv[1]
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        print("Could not read %s: %s" % (path, e))
        return 1

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        print("This file is not text. Is it really an aes_keys.txt?")
        return 1

    lines = [normalize(l) for l in text.splitlines()]
    has_section = any(l.startswith(":") for l in lines)

    print("File:    %s (%d bytes)" % (path, len(raw)))
    print("Layout:  %s" % ("sectioned (Azahar keys.txt style)" if has_section
                           else "legacy Citra aes_keys.txt - the emulator treats it as :AES"))
    if raw.startswith(b"\xef\xbb\xbf"):
        print("Note:    starts with a UTF-8 BOM, which is handled.")
    if b"\r\n" in raw:
        print("Note:    CRLF line endings, which are handled.")
    print()

    mode = "AES" if not has_section else ""
    names = {}
    bad_lines = []

    for number, line in enumerate(lines, start=1):
        if not line or line.startswith("#"):
            continue
        if line.startswith(":"):
            mode = line[1:]
            continue
        if mode != "AES":
            continue

        parts = line.split("=")
        if len(parts) != 2:
            bad_lines.append((number, line))
            continue

        name, value = parts[0], parts[1]
        if not re.fullmatch(r"[0-9A-Fa-f]+", value) or len(value) < 32:
            bad_lines.append((number, name + "=<value rejected: needs at least 32 hex digits>"))
            continue

        names[name] = True

    print("Entries read: %d" % len(names))
    if bad_lines:
        print("Lines the emulator will reject: %d" % len(bad_lines))
        for number, line in bad_lines[:10]:
            print("  line %d: %s" % (number, line[:60]))
    print()

    print("Needed for encrypted titles:")
    missing = []
    for name, why in NCCH_KEYS.items():
        present = name in names
        print("  [%s] %-18s %s" % ("x" if present else " ", name, why))
        if not present:
            missing.append(name)

    # The generator constant can be solved for if a slot supplies KeyX, KeyY and the normal key.
    slots = {}
    for name in names:
        m = SLOT_RE.match(name)
        if m:
            slots.setdefault(m.group(1).upper(), set()).add(m.group(2))
    solvable = [slot for slot, kinds in slots.items() if kinds >= {"X", "Y", "N"}]

    print()
    if "generatorConstant" in missing:
        if solvable:
            print("generatorConstant is absent, but slot(s) %s supply KeyX, KeyY and KeyN, so the"
                  % ", ".join("0x" + s for s in sorted(solvable)))
            print("emulator can solve for it at runtime.")
            missing.remove("generatorConstant")
        else:
            print("generatorConstant is absent and cannot be solved for: no slot in this file has")
            print("KeyX, KeyY and KeyN together.")
            print()
            print("No console dumping script writes this value. Citra's DumpKeys and the Azahar")
            print("fork's version both read everything from boot9.bin, which does not contain it,")
            print("and neither emits it. Old Citra had it hardcoded in its source instead, which")
            print("is why key files dumped for Citra never include it.")
            print()
            print("Add a line of the form  generatorConstant=<32 hex digits>  to this file.")

    print()
    if missing:
        print("RESULT: encrypted titles will NOT boot. Missing: %s" % ", ".join(missing))
        return 1

    print("RESULT: this file has what is needed for encrypted titles.")
    print("Titles that use seed crypto also need seeddb.bin from the same dump.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
