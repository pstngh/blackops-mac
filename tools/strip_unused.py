#!/usr/bin/env python3
"""Rebuild a Black Ops game folder's archives (main/*.iwd) without the images
and sounds that nothing names.

    tools/strip_unused.py <game dir> <out dir> [--dry-run] [--exe FILE]... [--names FILE]...
    tools/strip_unused.py <game dir> - --check

Only two kinds of archive file can go: images (images/<name>.iwi) and sounds
(sound/... and english/sound/... .wav). Every other file (weapons/ text, .cfg,
.csv, .accu) is kept. In fastfile mode the engine opens an image only as
"images/" + a GfxImage asset's name + ".iwi" (r_image_load_obj.cpp) and a
streamed sound only by its alias's file name (SND_AliasGetFileName), and both
names come from the zones. So an image or sound is kept if its name (the image
name, the sound's path; any case, / or \\) is a substring of a string in:

  - any zone of the game folder (zone/Common, zone/English: multiplayer,
    zombies and the small shared zones), decompressed, the zlib-packed scripts
    inside them unpacked too;
  - the executables (--exe; default <game dir>/blackops and build_macos/blackops);
  - the archives' own other files (weapon text files, configs);
  - a --names file (archive paths, one a line: e.g. the files a test run opened).

A string here is a run of the characters archive names use (letters, digits,
#$&-./@_~ and \\), so a name also counts when it is part of a longer string:
an image's name follows a 4-byte hash in the zone, and printable hash bytes join
it (session 35: 4,508 of the images the zones hold are seen only that way).

Each changed archive is written to <out dir> under its own name, entries in
the original order, kept ones byte for byte (local header, data, and central
record but for the offset), and checked: names, bytes, and every file unpacked
by the central directory's sizes (as the engine's unzip reads it) to its size
and CRC. An archive left with no entries is not written (the report says to
remove it). <out dir>/strip_unused.txt lists every archive file, what happens
to it and why (the groups that name it: mp, zombie, shared zones, exe, archive;
the first zone). --check only checks every archive of <game dir>/main that way.
Nothing in <game dir> is changed."""
import argparse
import bisect
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strip_voices import read_zip, read_local  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The characters of archive names (all 51,907 of the retail archives': #$&-./0-9@A-Za-z_~), and \.
RUN = re.compile(rb"[#$&\-./0-9@A-Za-z_~\\]{3,}")
ZLIB_HEAD = re.compile(rb"\x78[\x01\x5e\x9c\xda]")


def zone_group(name):
    base = name[3:] if name.startswith("en_") else name
    if base.startswith("mp_") or base.endswith("_mp.ff") or base == "kb_dlc_weapons.ff":
        return "mp"
    if base.startswith("zombie") or base.startswith("common_zombie"):
        return "zombie"
    return "shared"


def candidate(path):
    """The key an image or sound is named by, or None for any other file."""
    low = path.lower()
    if low.startswith("images/") and low.endswith(".iwi"):
        return low[7:-4]
    if low.endswith(".wav") and (low.startswith("sound/") or low.startswith("english/sound/")):
        return low
    return None


def runs(data):
    return {m.group(0).lower().replace(b"\\", b"/") for m in RUN.finditer(data)}


def packed_scripts(data):
    """The text of the zlib streams inside a zone (the packed .gsc/.csc raw files:
    unpacked length, packed length, zlib data; Scr_ReadFile_FastFile)."""
    texts = []
    for m in ZLIB_HEAD.finditer(data):
        pos = m.start()
        if pos < 8:
            continue
        unpacked, packed = struct.unpack_from("<II", data, pos - 8)
        if not 0 < packed <= len(data) - pos or not 0 < unpacked < 64 << 20:
            continue
        try:
            text = zlib.decompress(data[pos:pos + packed])
        except zlib.error:
            continue
        if len(text) == unpacked:
            texts.append(text)
    return texts


class Source:
    """A zone (or executable, or the archives' text) as its set of strings,
    with the reversed strings sorted for suffix lookups."""

    def __init__(self, name, group, strings):
        self.name, self.group = name, group
        self.strings = strings
        self.reversed = sorted(s[::-1] for s in strings)

    def names(self, key):
        """Whether key ends one of the strings (the usual case: the whole string,
        or the string after binary bytes that happen to be printable)."""
        rkey = key[::-1]
        i = bisect.bisect_left(self.reversed, rkey)
        return i < len(self.reversed) and self.reversed[i].startswith(rkey)


def load_sources(game, exes, archive_text):
    sources = []
    for sub in ("zone/Common", "zone/English"):
        folder = os.path.join(game, sub)
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".ff"):
                continue
            with open(os.path.join(folder, name), "rb") as f:
                raw = f.read()
            d = zlib.decompressobj()
            data = d.decompress(raw[12:])
            if not d.eof:
                sys.exit(f"{sub}/{name}: the zone doesn't decompress to its end")
            strings = runs(data)
            scripts = packed_scripts(data)
            for text in scripts:
                strings |= runs(text)
            sources.append(Source(name, zone_group(name), strings))
            print(f"  {sub}/{name}: {len(data) / 1e6:.1f} MB, {len(strings)} strings, "
                  f"{len(scripts)} packed scripts", flush=True)
    for exe in exes:
        with open(exe, "rb") as f:
            sources.append(Source(os.path.basename(exe) + " (executable " + exe + ")", "exe", runs(f.read())))
    sources.append(Source("the archives' other files", "archive", archive_text))
    return sources


def archive_text(archives):
    """The strings of every archive file that isn't an image or sound."""
    strings = set()
    for path, entries in archives:
        with open(path, "rb") as f:
            for e in entries:
                if candidate(e["name"]) is not None or e["name"].endswith("/"):
                    continue
                data = read_local(f, e)[-e["csize"]:] if e["csize"] else b""
                if e["method"] == 8:
                    data = zlib.decompress(data, -15)
                elif e["method"] != 0:
                    sys.exit(f"{path}: {e['name']}: compression method {e['method']}")
                strings |= runs(data)
    return strings


def moved_record(entry, offset):
    """The entry's central directory record with only its local header offset changed
    (the sizes stay: most image files are deflated, so their two sizes differ)."""
    record = bytearray(entry["record"])
    struct.pack_into("<I", record, 42, offset)
    return bytes(record)


def rebuild(path, out_path, plan):
    """Writes the archive with the kept entries (in order, byte for byte)."""
    records = []
    written = 0
    with open(path, "rb") as src, open(out_path + ".tmp", "wb") as out:
        for e, keep in plan:
            if not keep:
                continue
            blob = read_local(src, e)
            records.append(moved_record(e, written))
            out.write(blob)
            written += len(blob)
        cd = b"".join(records)
        out.write(cd + struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, len(records), len(records),
                                   len(cd), written, 0))
    os.replace(out_path + ".tmp", out_path)
    verify(path, out_path, [e for e, keep in plan if keep])


def verify(original, rebuilt, kept):
    """The rebuilt archive's entries: the kept ones, in order, each with the same local
    header and data and the same central record but for the offset; every file unpacked
    by the central directory's sizes (as the engine's unzip reads them) matches its CRC."""
    entries = read_zip(rebuilt)
    if [e["name"] for e in entries] != [e["name"] for e in kept]:
        sys.exit(f"{rebuilt}: entry list differs")
    with open(original, "rb") as a, open(rebuilt, "rb") as b:
        for got, e in zip(entries, kept):
            if got["record"] != moved_record(e, got["offset"]):
                sys.exit(f"{rebuilt}: {got['name']}: central record changed")
            data = read_local(b, got)
            if data != read_local(a, e):
                sys.exit(f"{rebuilt}: {got['name']}: changed")
            check_entry(rebuilt, got, data)


def check_entry(path, entry, local):
    """The local header's sizes and CRC agree with the central record's, and the data
    unpacks to the central record's size and CRC."""
    crc, csize, usize = struct.unpack_from("<III", local, 14)
    if (crc, csize, usize) != (entry["crc"], entry["csize"], entry["usize"]):
        sys.exit(f"{path}: {entry['name']}: local header and central directory disagree")
    stored = local[len(local) - entry["csize"]:]
    if entry["method"] == 0:
        data = stored
    elif entry["method"] == 8:
        d = zlib.decompressobj(-15)
        data = d.decompress(stored) + d.flush()
        if not d.eof:
            sys.exit(f"{path}: {entry['name']}: deflate data ends early")
    else:
        sys.exit(f"{path}: {entry['name']}: compression method {entry['method']}")
    if len(data) != entry["usize"] or zlib.crc32(data) & 0xFFFFFFFF != entry["crc"]:
        sys.exit(f"{path}: {entry['name']}: size or CRC mismatch")


def check_archives(main_dir):
    """Every file of every archive in main_dir: headers agree, data unpacks to its size and CRC."""
    total = 0
    for name in sorted(os.listdir(main_dir)):
        if not name.lower().endswith(".iwd"):
            continue
        path = os.path.join(main_dir, name)
        entries = read_zip(path)
        with open(path, "rb") as f:
            for e in entries:
                check_entry(path, e, read_local(f, e))
        total += len(entries)
        print(f"{name}: {len(entries)} files ok", flush=True)
    print(f"all {total} files ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--exe", action="append", default=None)
    ap.add_argument("--names", action="append", default=[])
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        check_archives(os.path.join(a.game, "main"))
        return
    exes = a.exe if a.exe is not None else [p for p in (os.path.join(a.game, "blackops"),
                                                        os.path.join(ROOT, "build_macos", "blackops"))
                                             if os.path.exists(p)]
    main_dir = os.path.join(a.game, "main")
    archives = [(os.path.join(main_dir, n), read_zip(os.path.join(main_dir, n)))
                for n in sorted(os.listdir(main_dir)) if n.lower().endswith(".iwd")]
    listed = {}
    for path in a.names:
        for line in open(path, errors="replace"):
            line = line.strip().lower().replace("\\", "/")
            if line:
                listed.setdefault(line, os.path.basename(path))

    print("reading the zones, executables and archive text files", flush=True)
    sources = load_sources(a.game, exes, archive_text(archives))
    keys = {candidate(e["name"]) for _, entries in archives for e in entries} - {None}
    keys = {k.encode("latin-1") for k in keys}

    # Who names each key: the zones (and others) whose strings end with it ...
    named = {k: [s for s in sources if s.names(k)] for k in keys}
    # ... else any string holding it anywhere (one search of all strings at once).
    rest = [k for k in keys if not named[k]]
    blob = b"\n".join(set().union(*(s.strings for s in sources)))
    inside = {}
    for k in rest:
        if k in blob:
            inside[k] = [s for s in sources if any(k in t for t in s.strings)]
    print(f"{len(keys)} image and sound names: {len(keys) - len(rest)} end a string, "
          f"{len(inside)} more are inside one, {len(rest) - len(inside)} named nowhere", flush=True)

    os.makedirs(a.out, exist_ok=True)
    totals = {"files": 0, "removed": 0, "removed_bytes": 0, "before": 0, "after": 0}
    by_kind = {}
    with open(os.path.join(a.out, "strip_unused.txt"), "w") as log:
        log.write("# action archive file stored_bytes reason\n")
        for path, entries in archives:
            name = os.path.basename(path)
            plan = []
            for e in entries:
                key = candidate(e["name"])
                lower = e["name"].lower()
                if key is None:
                    reason = "not an image or sound"
                else:
                    k = key.encode("latin-1")
                    who = named[k] or inside.get(k, [])
                    how = "names it" if named[k] else "holds it inside a string"
                    if who:
                        groups = sorted({s.group for s in who})
                        zones = [s.name for s in who if s.group in ("mp", "zombie", "shared")]
                        reason = (f"{','.join(groups)} {how}: " + (f"{zones[0]} (+{len(zones) - 1} zones)" if zones
                                                                 else who[0].name))
                    elif lower in listed:
                        reason = f"listed in {listed[lower]} (no zone names it)"
                    else:
                        reason = None
                keep = reason is not None
                kind = "image" if lower.startswith("images/") else "sound" if key else "other"
                if not keep:
                    totals["removed"] += 1
                    totals["removed_bytes"] += e["csize"]
                    by_kind[kind] = by_kind.get(kind, 0) + e["csize"]
                log.write(f"{'keep' if keep else 'remove'} {name} {e['name']} {e['csize']} "
                          f"{reason or 'nothing names it'}\n")
                plan.append((e, keep))
            totals["files"] += len(entries)
            removed = sum(1 for _, keep in plan if not keep)
            before = os.path.getsize(path)
            totals["before"] += before
            if not removed:
                totals["after"] += before
                continue
            left = [e for e, keep in plan if keep]
            if not left:
                print(f"{name}: {len(entries)} files, all unnamed: remove the archive "
                      f"({before / 1e6:.1f} MB)")
                continue
            if not a.dry_run:
                rebuild(path, os.path.join(a.out, name), plan)
                after = os.path.getsize(os.path.join(a.out, name))
            else:
                after = (sum(30 + len(e["name"].encode("latin-1")) + len(read_local_extra(path, e)) + e["csize"]
                             for e in left) + sum(len(e["record"]) for e in left) + 22)
            totals["after"] += after
            print(f"{name}: {len(entries)} files, {removed} removed, {before / 1e6:.1f} MB -> {after / 1e6:.1f} MB")
    print(f"total: {totals['files']} files, {totals['removed']} removed "
          f"({', '.join(f'{k}s {v / 1e6:.1f} MB' for k, v in sorted(by_kind.items()))}), "
          f"archives {totals['before'] / 1e6:.1f} MB -> {totals['after'] / 1e6:.1f} MB "
          f"({(totals['before'] - totals['after']) / 1e6:.1f} MB freed)")


def read_local_extra(path, entry):
    """The local header's extra field (for the dry run's size)."""
    with open(path, "rb") as f:
        f.seek(entry["offset"] + 26)
        nlen, xlen = struct.unpack("<HH", f.read(4))
        f.seek(nlen, 1)
        return f.read(xlen)


if __name__ == "__main__":
    main()
