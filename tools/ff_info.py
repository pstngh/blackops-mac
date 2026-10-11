#!/usr/bin/env python3
"""Print a Black Ops fastfile's header: version, XFile block sizes, the zone's
script string list and its asset list (types and how each header is stored).

    tools/ff_info.py <zone.ff> [--strings] [--assets]

The stream after the asset list is not parsed (that is db_load.cpp's job), so
asset names are not shown; --strings lists the script strings."""
import argparse
import struct
import sys
import zlib

ASSET_TYPES = [
    "xmodelpieces", "physpreset", "physconstraints", "destructibledef", "xanim",
    "xmodel", "material", "techset", "image", "sound", "sound_patch", "col_map_sp",
    "col_map_mp", "com_map", "game_map_sp", "game_map_mp", "map_ents", "gfx_map",
    "lightdef", "ui_map", "font", "menufile", "menu", "localize", "weapon",
    "weapondef", "weaponvariant", "snddriverglobals", "fx", "impactfx", "aitype",
    "mptype", "mpbody", "mphead", "character", "xmodelalias", "rawfile",
    "stringtable", "packindex", "xGlobals", "ddl", "glasses", "emblemset",
]
BLOCKS = ["temp", "runtime", "large_runtime", "physical_runtime", "virtual",
          "large", "physical"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zone")
    ap.add_argument("--strings", action="store_true")
    ap.add_argument("--assets", action="store_true")
    a = ap.parse_args()
    raw = open(a.zone, "rb").read()
    magic, version = raw[:8], struct.unpack_from("<I", raw, 8)[0]
    data = zlib.decompress(raw[12:])
    size, external = struct.unpack_from("<II", data, 0)
    blocks = struct.unpack_from("<7I", data, 8)
    print(f"{a.zone}: {magic.decode(errors='replace')} version {version}, "
          f"{len(raw)} bytes, {len(data)} decompressed")
    print(f"stream size {size} (+36 header), external {external}")
    for name, s in zip(BLOCKS, blocks):
        print(f"  block {name:17s} {s:>11d}")
    pos = 36
    scount, sptr, acount, aptr = struct.unpack_from("<iIiI", data, pos)
    pos += 16
    print(f"script strings {scount}, assets {acount}")
    strings = []
    if sptr:
        # Load_ScriptStringList: the pointer array (4-byte slots), then each
        # -1 string inline, null-terminated.
        slots = struct.unpack_from(f"<{scount}I", data, pos)
        pos += 4 * scount
        for s in slots:
            if s == 0xFFFFFFFF:
                end = data.index(b"\0", pos)
                strings.append(data[pos:end].decode("latin-1"))
                pos = end + 1
            else:
                strings.append(None if s == 0 else f"<alias {s:#x}>")
    if a.strings:
        for i, s in enumerate(strings):
            print(f"  string {i}: {s}")
    counts = {}
    kinds = {}
    if aptr:
        for i in range(acount):
            t, h = struct.unpack_from("<iI", data, pos + 8 * i)
            name = ASSET_TYPES[t] if 0 <= t < len(ASSET_TYPES) else str(t)
            counts[name] = counts.get(name, 0) + 1
            k = {0xFFFFFFFF: "inline", 0xFFFFFFFE: "inline+slot"}.get(h, "alias")
            kinds[k] = kinds.get(k, 0) + 1
            if a.assets:
                print(f"  asset {i}: {name} {k}")
    for name in sorted(counts, key=lambda n: -counts[n]):
        print(f"  {name:16s} {counts[name]}")
    print("  headers: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
