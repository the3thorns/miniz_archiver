#!/usr/bin/env python3
"""Characterization driver for the store-only miniz port.

Builds a corpus with Python's zipfile, extracts it through a caller-supplied
miniz CLI, verifies byte-for-byte equality, and checks interoperability of the
archives written by the C tests with unzip/zipinfo/zipfile.

Usage:
  characterize.py --extractor PATH [--baseline PATH] [--interop ZIP ...]
                  [--expect-stored] [--reject-deflate]
                  [--sparse-zip64] [--entries-70k]
                  [--workdir DIR]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("FAIL:", msg, file=sys.stderr)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def make_basic_corpus(path):
    entries = [
        ("empty.txt", b""),
        ("small.txt", b"the quick brown fox jumps over the lazy dog\n" * 20),
        ("bin1.dat", bytes(range(256)) * 4096),
        ("unicode-\u00f1andu.txt", "contenido con acentos: áéíóú ñ\n".encode("utf-8")),
        ("dir/", b""),
        ("dir/nested.bin", bytes((i * 37) & 0xFF for i in range(100000))),
    ]
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        z.comment = b"legacy stored corpus"
        for name, data in entries:
            z.writestr(name, data)
    return [(n, d) for n, d in entries if not n.endswith("/")]


def make_forced_zip64(path):
    data = bytes((i * 11) & 0xFF for i in range(5000))
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        with z.open("forced.bin", "w", force_zip64=True) as f:
            f.write(data)
    return [("forced.bin", data)]


def make_deflate_corpus(path):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("deflated.txt", b"compress me " * 1000)


def make_70k_corpus(path):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        for i in range(70000):
            z.writestr("d/%05d.txt" % i, b"x")


def crc32_zeros(size, chunk=1 << 20):
    block = b"\0" * chunk
    crc = 0
    while size:
        n = min(size, chunk)
        crc = zlib.crc32(block[:n], crc)
        size -= n
    return crc & 0xFFFFFFFF


def make_sparse_zip64(path, size=0x100000001):
    """Write a ZIP64 archive with one stored entry of `size` zero bytes.

    The entry data is a hole in the file, so this uses almost no disk space.
    """
    name = b"huge.bin"
    crc = crc32_zeros(size)
    local_extra = struct.pack("<HHQQ", 1, 16, size, size)
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0, 0, 0, 0,
                            crc, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(local_extra)))
        f.write(name)
        f.write(local_extra)
        f.seek(size, os.SEEK_CUR)  # sparse hole
        cd_ofs = f.tell()

        central_extra = struct.pack("<HHQQ", 1, 16, size, size)
        f.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0, 0, 0, 0,
                            crc, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(central_extra),
                            0, 0, 0, 0, 0))
        f.write(name)
        f.write(central_extra)
        cd_size = f.tell() - cd_ofs

        eocd64_ofs = f.tell()
        f.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0,
                            1, 1, cd_size, cd_ofs))
        f.write(struct.pack("<IIQI", 0x07064B50, 0, eocd64_ofs, 1))
        f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF,
                            0xFFFFFFFF, 0xFFFFFFFF, 0))


def extract_one(extractor, archive, name, out_path):
    r = run([extractor, archive, name, out_path])
    if r.returncode != 0:
        return False, r.stderr.strip()
    return True, ""


def check_corpus(extractor, archive, expected, tmpdir, label):
    for i, (name, data) in enumerate(expected):
        out = os.path.join(tmpdir, "extract-%d.bin" % i)
        ok, err = extract_one(extractor, archive, name, out)
        check(ok, "%s: extract %r failed: %s" % (label, name, err))
        if ok:
            with open(out, "rb") as f:
                got = f.read()
            check(got == data,
                  "%s: %r mismatch (%d vs %d bytes)" % (label, name, len(got), len(data)))
            os.unlink(out)


def interop_check(archive, expect_stored):
    r = run(["unzip", "-t", archive])
    check(r.returncode == 0, "unzip -t failed for %s: %s" % (archive, r.stdout + r.stderr))
    r = run(["zipinfo", archive])
    check(r.returncode == 0, "zipinfo failed for %s: %s" % (archive, r.stderr))
    try:
        with zipfile.ZipFile(archive) as z:
            bad = z.testzip()
            check(bad is None, "zipfile testzip found %r in %s" % (bad, archive))
            names = z.namelist()
            check(len(names) == 3, "unexpected entry count in %s" % archive)
            for info in z.infolist():
                if info.is_dir():
                    continue
                with z.open(info) as f:
                    f.read()
                if expect_stored:
                    check(info.compress_type == zipfile.ZIP_STORED,
                          "entry %r is not stored" % info.filename)
                    check(info.compress_size == info.file_size,
                          "entry %r sizes differ" % info.filename)
    except Exception as exc:  # noqa: BLE001
        check(False, "zipfile cannot read %s: %s" % (archive, exc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extractor", required=True)
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--interop", action="append", default=[])
    ap.add_argument("--expect-stored", action="store_true")
    ap.add_argument("--reject-deflate", action="store_true")
    ap.add_argument("--sparse-zip64", action="store_true")
    ap.add_argument("--entries-70k", action="store_true")
    ap.add_argument("--workdir", default="/tmp/opencode/miniz-store-corpus")
    args = ap.parse_args()

    work = args.workdir
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)

    basic = os.path.join(work, "basic.zip")
    expected = make_basic_corpus(basic)
    check_corpus(args.extractor, basic, expected, work, "basic")
    if args.baseline:
        check_corpus(args.baseline, basic, expected, work, "basic/baseline")

    z64 = os.path.join(work, "forced-zip64.zip")
    expected64 = make_forced_zip64(z64)
    check_corpus(args.extractor, z64, expected64, work, "forced-zip64")
    if args.baseline:
        check_corpus(args.baseline, z64, expected64, work, "forced-zip64/baseline")

    empty = os.path.join(work, "empty.zip")
    with zipfile.ZipFile(empty, "w"):
        pass
    r = run([args.extractor, empty, "--validate-all", "-"])
    check(r.returncode == 0, "empty archive validation failed: " + r.stderr)

    if args.interop:
        for archive in args.interop:
            check(os.path.exists(archive), "missing interop archive " + archive)
            if os.path.exists(archive):
                interop_check(archive, args.expect_stored)

    if args.reject_deflate:
        dpath = os.path.join(work, "deflate.zip")
        make_deflate_corpus(dpath)
        r = run([args.extractor, dpath, "deflated.txt", os.path.join(work, "deflated.out")])
        check(r.returncode != 0, "store-only accepted a deflate entry")
        check("unsupported" in (r.stdout + r.stderr).lower(),
              "deflate rejection did not mention unsupported: " + r.stdout + r.stderr)
        if args.baseline:
            r = run([args.baseline, dpath, "deflated.txt", os.path.join(work, "deflated.out")])
            check(r.returncode == 0, "baseline failed to read deflate entry: " + r.stderr)

    if args.entries_70k:
        many = os.path.join(work, "entries-70k.zip")
        make_70k_corpus(many)
        with zipfile.ZipFile(many) as z:
            check(len(z.namelist()) == 70000, "70k archive has wrong entry count")
        r = run([args.extractor, many, "--validate-all", "-"])
        check(r.returncode == 0, "70k validation failed: " + r.stderr)

    if args.sparse_zip64:
        sparse = os.path.join(work, "sparse-zip64.zip")
        make_sparse_zip64(sparse)
        with zipfile.ZipFile(sparse) as z:
            infos = z.infolist()
            check(len(infos) == 1 and infos[0].file_size == 0x100000001,
                  "Python cannot see the sparse ZIP64 entry")
        r = run([args.extractor, sparse, "huge.bin", os.devnull])
        check(r.returncode == 0, "sparse ZIP64 extraction failed: " + r.stderr)
        if args.baseline:
            r = run([args.baseline, sparse, "huge.bin", os.devnull])
            check(r.returncode == 0, "baseline sparse ZIP64 extraction failed: " + r.stderr)

    if FAILS:
        print("%d failure(s)" % len(FAILS), file=sys.stderr)
        return 1
    print("characterization corpus passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
