#!/usr/bin/env python3
"""Rebuild a Black Ops game folder's archives (main/*.iwd) without the second
copies of images and sounds stored in more than one archive.

    tools/strip_dupes.py <game dir> <out dir> [--dry-run] [--language english]

The engine looks a file up along its search paths (com_files.cpp) and opens the
first copy it finds: FS_AddIwdFilesForGameDirectory sorts main's archives
(iwdsort: localized_ names as ten spaces, English before other languages, then
FS_PathCmp) and FS_AddSearchPath puts each at the head of the list, a localized
one at the head of the localized part, which follows every other path. So every
iw_ archive is searched before any localized_ one, the highest-sorted first in
each group, and FS_UseSearchPath skips localized archives of other languages.
Names compare without case, \\ and : as / (FS_FilenameCompare).

For each name in more than one searched archive the copy the engine opens stays.
Another copy goes only if it is an image (images/*.iwi) or a sound (sound/ or
english/sound/ .wav) and every copy of the name has the same uncompressed size,
CRC and SHA-256 of the unpacked bytes; a name with any differing copy keeps all
of them. So the game reads the same bytes from the same archive as before.
Archives the engine doesn't load (not iw_ or localized_, other languages) are
left alone.

The changed archives are written to <out dir> as tools/strip_unused.py writes
them (entries in order, kept ones byte for byte, checked). <out dir>/strip_dupes.txt
lists every copy of each name stored more than once: what happens to it and
which copy the game opens. Nothing in <game dir> is changed."""
import argparse
import hashlib
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strip_unused import candidate, rebuild, read_local_extra  # noqa: E402
from strip_voices import read_zip, read_local  # noqa: E402


def path_key(name):
    """FS_PathCmp's order: ASCII letters upper case, \\ and : as /."""
    return name.upper().replace("\\", "/").replace(":", "/")


def localized_language(name):
    """IwdFileLanguage: the word after localized_ (empty for none)."""
    rest = name[10:]
    return rest.split("_", 1)[0] if "_" in rest else ""


def search_order(names, language):
    """The archives of main/ in the order FS_FOpenFileReadForThread searches them
    (the first holding a name is the one it opens), and the ones it never loads."""
    plain = [n for n in names if not n.startswith("localized_")]
    local = [n for n in names if n.startswith("localized_")]
    skipped = [n for n in plain if not n.lower().startswith("iw_")]  # "Invalid IWD ... in \main"
    plain = [n for n in plain if n.lower().startswith("iw_")]
    skipped += [n for n in local if localized_language(n).lower() != language.lower()]
    local = [n for n in local if localized_language(n).lower() == language.lower()]
    # Added in ascending order, each at the head of its part: searched in descending order.
    plain.sort(key=lambda n: path_key(n), reverse=True)
    local.sort(key=lambda n: path_key(" " * 10 + n[10:]), reverse=True)
    return plain + local, sorted(skipped)


def unpacked(f, entry):
    stored = read_local(f, entry)[-entry["csize"]:] if entry["csize"] else b""
    if entry["method"] == 0:
        data = stored
    elif entry["method"] == 8:
        data = zlib.decompress(stored, -15)
    else:
        sys.exit(f"{entry['name']}: compression method {entry['method']}")
    if len(data) != entry["usize"] or zlib.crc32(data) & 0xFFFFFFFF != entry["crc"]:
        sys.exit(f"{entry['name']}: size or CRC mismatch")
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--language", default="english")
    a = ap.parse_args()
    main_dir = os.path.join(a.game, "main")
    order, skipped = search_order([n for n in os.listdir(main_dir) if n.lower().endswith(".iwd")], a.language)
    print("search order: " + " ".join(order))
    if skipped:
        print("not loaded (left alone): " + " ".join(skipped))
    archives = {n: read_zip(os.path.join(main_dir, n)) for n in order}

    copies = {}
    for n in order:
        seen = set()
        for e in archives[n]:
            key = path_key(e["name"])
            if key in seen:
                sys.exit(f"{n}: {e['name']} is in the archive twice (not handled)")
            seen.add(key)
            copies.setdefault(key, []).append((n, e))
    multi = {k: v for k, v in copies.items() if len(v) > 1}

    # SHA-256 of every copy's unpacked bytes.
    digests = {}
    handles = {n: open(os.path.join(main_dir, n), "rb") for n in order}
    for v in multi.values():
        for n, e in v:
            digests[(n, e["name"])] = hashlib.sha256(unpacked(handles[n], e)).hexdigest()
    for f in handles.values():
        f.close()

    remove = {}
    counts = {"names": len(multi), "differ": 0, "other": 0, "removed": 0, "bytes": 0}
    by_kind = {}
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "strip_dupes.txt"), "w") as log:
        log.write("# action archive file stored_bytes size crc sha256 opened_copy reason\n")
        for key in sorted(multi):
            v = multi[key]
            first_n, first_e = v[0]
            same = all((e["usize"], e["crc"]) == (first_e["usize"], first_e["crc"])
                       and digests[(n, e["name"])] == digests[(first_n, first_e["name"])] for n, e in v)
            kind = candidate(first_e["name"])
            if not same:
                counts["differ"] += 1
                reason = "copies differ: all kept"
            elif kind is None:
                counts["other"] += 1
                reason = "not an image or sound: all kept"
            else:
                reason = None
            for i, (n, e) in enumerate(v):
                go = i > 0 and reason is None
                if go:
                    remove.setdefault(n, set()).add(e["name"])
                    counts["removed"] += 1
                    counts["bytes"] += e["csize"]
                    k = "image" if e["name"].lower().startswith("images/") else "sound"
                    by_kind[k] = by_kind.get(k, 0) + e["csize"]
                why = reason or ("the copy the game opens" if i == 0 else "identical to the copy the game opens")
                log.write(f"{'remove' if go else 'keep'} {n} {e['name']} {e['csize']} {e['usize']} {e['crc']:08x} "
                          f"{digests[(n, e['name'])]} {first_n} {why}\n")
    print(f"{counts['names']} names in more than one archive: {counts['differ']} with differing copies and "
          f"{counts['other']} not images or sounds (all kept); {counts['removed']} copies removed "
          f"({', '.join(f'{k}s {v / 1e6:.1f} MB' for k, v in sorted(by_kind.items()))})")

    before_total = after_total = 0
    for n in order:
        path = os.path.join(main_dir, n)
        before = os.path.getsize(path)
        before_total += before
        gone = remove.get(n, set())
        if not gone:
            after_total += before
            continue
        plan = [(e, e["name"] not in gone) for e in archives[n]]
        left = [e for e, keep in plan if keep]
        if not left:
            print(f"{n}: all {len(plan)} files are second copies: remove the archive ({before / 1e6:.1f} MB)")
            continue
        if a.dry_run:
            after = (sum(30 + len(e["name"].encode("latin-1")) + len(read_local_extra(path, e)) + e["csize"]
                         for e in left) + sum(len(e["record"]) for e in left) + 22)
        else:
            rebuild(path, os.path.join(a.out, n), plan)
            after = os.path.getsize(os.path.join(a.out, n))
        after_total += after
        print(f"{n}: {len(plan)} files, {len(gone)} removed, {before / 1e6:.1f} MB -> {after / 1e6:.1f} MB")
    print(f"archives {before_total / 1e6:.1f} MB -> {after_total / 1e6:.1f} MB "
          f"({(before_total - after_total) / 1e6:.1f} MB freed)")


if __name__ == "__main__":
    main()
