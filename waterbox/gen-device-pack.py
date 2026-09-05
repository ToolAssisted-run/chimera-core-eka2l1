#!/usr/bin/env python3
"""Packs an installed device's drive Z into the single file the guest reads.

The sandbox mounts named files and has no directories, so the machine inside it
serves its drives from its own memory (waterbox/memfs.cpp) and reads the
contents out of this pack as it needs them. One file crosses the boundary
instead of a thousand, and none of it is copied into the machine's memory until
the machine actually reads it.

    gen-device-pack.py --storage <installed storage root> --out device.pack

Deterministic by construction: entries are sorted by path, and nothing about
the host - timestamps, permissions, the order a directory happens to list in -
reaches the pack.
"""
import argparse
import os
import struct
import sys

MAGIC = b"CHIMERADEVPK"
VERSION = 1


def read_devices_yml(path):
    """The five fields the guest needs to register the device.

    devices.yml is a tiny, flat mapping written by the emulator's own
    installer; a YAML parser would be a dependency for four lookups.
    """
    device = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.endswith(":") and ":" not in stripped[:-1]:
                continue
            if ":" not in stripped:
                continue
            key, _, value = stripped.partition(":")
            device[key.strip()] = value.strip()
    return device


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--storage", required=True, help="a storage root install-device wrote")
    parser.add_argument("--out", required=True, help="the pack to write")
    args = parser.parse_args()

    devices_yml = os.path.join(args.storage, "devices.yml")
    if not os.path.exists(devices_yml):
        sys.exit("no devices.yml in %s - run install-device first" % args.storage)

    device = read_devices_yml(devices_yml)
    firmcode = device.get("firmcode")
    if not firmcode:
        sys.exit("devices.yml names no firmware code")

    root = os.path.join(args.storage, "drives", "z", firmcode.lower())
    if not os.path.isdir(root):
        sys.exit("no drive Z for %s in %s" % (firmcode, args.storage))

    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            entries.append((rel_dir.replace(os.sep, "\\"), True, None))
        for name in filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            entries.append((rel.replace(os.sep, "\\"), False, os.path.join(dirpath, name)))

    entries.sort(key=lambda e: e[0].lower())

    epocver_names = {
        "epocu6": 0, "epoc6": 2, "epoc80": 3, "epoc81a": 4, "epoc81b": 5,
        "epoc93": 6, "epoc94": 7, "epoc95": 8, "epoc10": 9,
    }
    platver = device.get("platver", "epoc6")
    epocver = epocver_names.get(platver)
    if epocver is None:
        sys.exit("unknown platform version %s" % platver)

    firm = firmcode.encode("utf-8")
    model = device.get("model", "").encode("utf-8")
    manufacturer = device.get("manufacturer", "").encode("utf-8")

    header = bytearray()
    header += MAGIC
    header += struct.pack("<II", VERSION, len(entries))
    header += struct.pack("<II", epocver, int(device.get("machine-uid", "0")))
    for blob in (firm, model, manufacturer):
        header += struct.pack("<I", len(blob)) + blob

    # The index is fixed-width per entry apart from the path, so the data
    # offsets are known only after the whole index is laid out.
    index = bytearray()
    offset = 0
    sizes = []
    for path, is_dir, source in entries:
        encoded = path.encode("utf-8")
        size = 0 if is_dir else os.path.getsize(source)
        sizes.append(size)
        index += struct.pack("<H", len(encoded)) + encoded
        index += struct.pack("<BQQ", 1 if is_dir else 0, offset, size)
        if not is_dir:
            offset += size

    data_start = len(header) + len(index)

    # Rewrite the offsets now that the index length is known.
    index = bytearray()
    offset = data_start
    for (path, is_dir, source), size in zip(entries, sizes):
        encoded = path.encode("utf-8")
        index += struct.pack("<H", len(encoded)) + encoded
        index += struct.pack("<BQQ", 1 if is_dir else 0, 0 if is_dir else offset, size)
        if not is_dir:
            offset += size

    with open(args.out, "wb") as out:
        out.write(header)
        out.write(index)
        for path, is_dir, source in entries:
            if is_dir:
                continue
            with open(source, "rb") as f:
                out.write(f.read())

    files = sum(1 for e in entries if not e[1])
    print("%s: %d entries (%d files), %d bytes, device %s %s (%s)"
          % (args.out, len(entries), files, os.path.getsize(args.out),
             device.get("manufacturer", ""), device.get("model", ""), firmcode))


if __name__ == "__main__":
    main()
