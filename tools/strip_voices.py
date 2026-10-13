#!/usr/bin/env python3
"""Rebuild a Black Ops game folder's archives (main/*.iwd) without the voices.

    tools/strip_voices.py <game dir> <out dir> [--dry-run]

A voice is a file in one of the VOICE_FOLDERS below (dialogue, announcer, battle
chatter, the numbers broadcasts, ...). A voice that a multiplayer zone names
(zone/Common and zone/English: mp_*, *_mp, kb_dlc_weapons, found by scanning the
decompressed zones for .wav names; the same list as `listzone ... sound` gives)
becomes a placeholder: the same file format with one block of silence (512
frames); every other voice is left out. Every other entry is copied byte for
byte (local header and data), in the original order. Each changed archive is
written to <out dir> under its own name; an archive left with no entries is
not written (the report says to remove it). <out dir>/strip_voices.txt lists
each voice and what happened to it. Nothing in <game dir> is changed.

The streamed sound format (snd_stream.cpp): a 2096-byte header (snd_asset, then
the seek table space) and the data; the loader wants file size == data_size +
2096 (looping files over 0x106000 bytes are padded). Voices are MS-ADPCM,
blocks of 262 bytes a channel, 512 frames."""
import argparse
import os
import re
import struct
import sys
import zlib

VOICE_FOLDERS = (
    "english/sound/vox/",           # dialogue, announcer (mpl, mp/tst), chatter
    "english/sound/dds/",           # battle chatter
    "english/sound/voiceovers/",    # the zombie announcer
    "english/sound/mid/",           # Hudson's briefing lines
    "english/sound/evt/front_end/tvs/",  # the campaign menu's TV speeches
    "sound/vox/",                   # chatter, dialogue, radio and PA voices
    "sound/evt/num/",               # the numbers broadcasts
    "sound/wpn/matroyshka/chrs/",   # the zombie dolls' voices
)
HEADER_SIZE = 2096
FORMAT_MSADPCM = 6
WAV_NAME = re.compile(rb"[A-Za-z0-9_/\\.\-~&]{3,}\.wav\x00", re.I)


def multiplayer_zone(name):
    base = name[3:] if name.startswith("en_") else name
    return base.startswith("mp_") or base.endswith("_mp.ff") or base == "kb_dlc_weapons.ff"


def multiplayer_sounds(game):
    names = set()
    for sub in ("zone/Common", "zone/English"):
        folder = os.path.join(game, sub)
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".ff") or not multiplayer_zone(name):
                continue
            with open(os.path.join(folder, name), "rb") as f:
                data = zlib.decompressobj().decompress(f.read()[12:])
            for m in WAV_NAME.finditer(data):
                names.add(m.group(0)[:-1].decode("latin-1").replace("\\", "/").lower())
    return names


def read_zip(path):
    """The central directory: (name, crc, size, local header offset, central record)."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - 65558))
        tail = f.read()
        end = tail.rfind(b"PK\x05\x06")
        if end < 0:
            sys.exit(f"{path}: no end of central directory")
        count, cd_size, cd_offset = struct.unpack_from("<HII", tail, end + 10)
        f.seek(cd_offset)
        cd = f.read(cd_size)
    entries = []
    pos = 0
    for _ in range(count):
        if cd[pos:pos + 4] != b"PK\x01\x02":
            sys.exit(f"{path}: bad central directory")
        (flags, method, crc, csize, usize, nlen, xlen, clen,
         offset) = struct.unpack_from("<4xHH4xIIIHHH8xI", cd, pos + 4)
        if flags & 8 or csize == 0xFFFFFFFF:
            sys.exit(f"{path}: data descriptors or zip64 are not handled")
        record = cd[pos:pos + 46 + nlen + xlen + clen]
        name = record[46:46 + nlen].decode("latin-1")
        entries.append({"name": name, "method": method, "crc": crc, "csize": csize,
                        "usize": usize, "offset": offset, "record": record})
        pos += len(record)
    return entries


def read_local(f, entry):
    """The entry's local header and data, as stored."""
    f.seek(entry["offset"])
    head = f.read(30)
    if head[:4] != b"PK\x03\x04":
        sys.exit(f"{entry['name']}: bad local header")
    nlen, xlen = struct.unpack_from("<HH", head, 26)
    rest = f.read(nlen + xlen + entry["csize"])
    return head + rest


def placeholder(original):
    """The original's header with one block of MS-ADPCM silence a channel."""
    fields = list(struct.unpack_from("<14I", original, 0))
    version, channels, header_size, fmt = fields[0], fields[3], fields[4], fields[7]
    if version != 1 or header_size != HEADER_SIZE or fmt != FORMAT_MSADPCM or channels not in (1, 2):
        return None
    block = 262 * channels
    fields[1] = 512           # frame_count
    fields[12] = block        # data_size
    header = struct.pack("<14I", *fields) + original[56:HEADER_SIZE]
    # A block: the predictor index, delta, sample 1 and sample 2 of each channel
    # (interleaved), then 510 frames of 4-bit codes. Predictor 0 (coefficients
    # 256, 0), samples 0 and codes 0 decode to zeros at any delta; 16 is the
    # smallest the encoder writes, as the game's files start.
    head = bytes(channels) + struct.pack("<h", 16) * channels + bytes(4 * channels)
    return header + head + bytes(block - len(head))


def local_header(entry, crc, size):
    name = entry["name"].encode("latin-1")
    version, flags, method, time, date = struct.unpack_from("<HHHHH", entry["record"], 6)
    return struct.pack("<4sHHHHHIIIHH", b"PK\x03\x04", version, flags, 0, time, date,
                       crc, size, size, len(name), 0) + name


def central_record(entry, crc, csize, usize, offset):
    record = bytearray(entry["record"])
    struct.pack_into("<III", record, 16, crc, csize, usize)
    struct.pack_into("<I", record, 42, offset)
    return bytes(record)


def rebuild(path, out_path, voices_used, log, dry_run):
    entries = read_zip(path)
    stats = {"entries": len(entries), "kept": 0, "removed": 0, "placeholders": 0,
             "before": os.path.getsize(path), "after": 0, "removed_bytes": 0, "placeholder_bytes": 0}
    plan = []
    for e in entries:
        lower = e["name"].lower()
        if e["name"].endswith("/") or not lower.startswith(VOICE_FOLDERS):
            plan.append((e, "keep"))
        elif lower in voices_used:
            plan.append((e, "placeholder"))
        else:
            plan.append((e, "remove"))
    if all(action == "keep" for _, action in plan):
        return None
    with open(path, "rb") as src:
        out = None if dry_run else open(out_path + ".tmp", "wb")
        written = 0
        records = []
        for e, action in plan:
            if action == "remove":
                stats["removed"] += 1
                stats["removed_bytes"] += e["csize"]
                log.write(f"remove {os.path.basename(path)} {e['name']}\n")
                continue
            if action == "keep":
                blob = read_local(src, e)
                crc, size, usize = e["crc"], e["csize"], e["usize"]
                stats["kept"] += 1
            else:
                if e["method"] != 0:
                    sys.exit(f"{e['name']}: a voice that isn't stored")
                stored = read_local(src, e)
                data = placeholder(stored[-e["csize"]:])
                if data is None:
                    sys.exit(f"{e['name']}: not a version 1 MS-ADPCM stream, no placeholder")
                crc, size = zlib.crc32(data) & 0xFFFFFFFF, len(data)
                usize = size
                blob = local_header(e, crc, size) + data
                stats["placeholders"] += 1
                stats["placeholder_bytes"] += e["csize"]
                log.write(f"placeholder {os.path.basename(path)} {e['name']}\n")
            records.append(central_record(e, crc, size, usize, written))
            if out:
                out.write(blob)
            written += len(blob)
        cd = b"".join(records)
        end = struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, len(records), len(records),
                          len(cd), written, 0)
        if out:
            out.write(cd + end)
            out.close()
        stats["after"] = written + len(cd) + len(end) if records else 0
    if not dry_run:
        if records:
            os.replace(out_path + ".tmp", out_path)
            verify(path, out_path, plan)
        else:
            os.remove(out_path + ".tmp")
    return stats


def verify(original, rebuilt, plan):
    """The rebuilt archive's entries, in order, with the kept ones' bytes unchanged."""
    entries = read_zip(rebuilt)
    expected = [e for e, action in plan if action != "remove"]
    if [e["name"] for e in entries] != [e["name"] for e in expected]:
        sys.exit(f"{rebuilt}: entry list differs")
    with open(original, "rb") as a, open(rebuilt, "rb") as b:
        for got, (e, action) in zip(entries, [p for p in plan if p[1] != "remove"]):
            data = read_local(b, got)[-got["csize"]:]
            unpacked = zlib.decompress(data, -15) if got["method"] == 8 else data
            if len(unpacked) != got["usize"] or zlib.crc32(unpacked) & 0xFFFFFFFF != got["crc"]:
                sys.exit(f"{rebuilt}: {got['name']}: size or CRC mismatch")
            if action == "keep" and data != read_local(a, e)[-e["csize"]:]:
                sys.exit(f"{rebuilt}: {got['name']}: changed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    used = multiplayer_sounds(a.game)
    os.makedirs(a.out, exist_ok=True)
    main_dir = os.path.join(a.game, "main")
    totals = {"removed": 0, "placeholders": 0, "freed": 0}
    with open(os.path.join(a.out, "strip_voices.txt"), "w") as log:
        for name in sorted(os.listdir(main_dir)):
            if not name.lower().endswith(".iwd"):
                continue
            stats = rebuild(os.path.join(main_dir, name), os.path.join(a.out, name), used, log, a.dry_run)
            if not stats:
                continue
            for k in ("removed", "placeholders"):
                totals[k] += stats[k]
            totals["freed"] += stats["before"] - stats["after"]
            what = "empty: remove the archive" if not stats["after"] else f"{stats['after'] / 1e6:.1f} MB"
            print(f"{name}: {stats['entries']} entries, {stats['removed']} voices left out, "
                  f"{stats['placeholders']} placeholders, {stats['before'] / 1e6:.1f} MB -> {what}")
    print(f"total: {totals['removed']} voices left out, {totals['placeholders']} placeholders, "
          f"{totals['freed'] / 1e6:.1f} MB freed")


if __name__ == "__main__":
    main()
