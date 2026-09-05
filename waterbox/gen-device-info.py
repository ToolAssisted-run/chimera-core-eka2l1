#!/usr/bin/env python3
"""Packs an installed device's drive Z into the single file the guest reads.

The sandbox mounts named files and has no directories, so the machine inside it
serves its drives from its own memory (waterbox/memfs.cpp) and reads the
contents out of this pack as it needs them. One file crosses the boundary
instead of a thousand, and none of it is copied into the machine's memory until
the machine actually reads it.

    gen-device-info.py --storage <installed storage root> --out device.info

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
    header += struct.pack("<II", VERSION, 0)
    header += struct.pack("<II", epocver, int(device.get("machine-uid", "0")))
    for blob in (firm, model, manufacturer):
        header += struct.pack("<I", len(blob)) + blob

    with open(args.out, "wb") as out:
        out.write(header)

    print("%s: %d bytes, device %s %s (%s), %s"
          % (args.out, os.path.getsize(args.out), device.get("manufacturer", ""),
             device.get("model", ""), firmcode, platver))


if __name__ == "__main__":
    main()
