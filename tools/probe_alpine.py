#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时探测：解析 Alpine APKINDEX，下载 musl-dev / libstdc++-dev，列出包内容。"""
import gzip
import io
import re
import sys
import tarfile
import urllib.request
from pathlib import Path

MIRROR = "https://mirrors.aliyun.com/alpine"
BRANCH = "latest-stable"
ARCH = "aarch64"
WORK = Path("/tmp/alpine-probe")
WORK.mkdir(parents=True, exist_ok=True)


def get(url, binary=True):
    req = urllib.request.Request(url, headers={"User-Agent": "xcc-probe/0.1"})
    with urllib.request.urlopen(req, timeout=120) as r:
        d = r.read()
    return d if binary else d.decode("utf-8", "replace")


def parse_index(text):
    """APKINDEX: 记录用空行分隔，字段 P: V: S: o: 等。"""
    pkgs = {}
    for rec in text.split("\n\n"):
        fields = {}
        for line in rec.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                fields.setdefault(k, v)
        if "P" in fields and "V" in fields:
            pkgs[fields["P"]] = fields
    return pkgs


def apk_data_tar(blob):
    """apk = 若干拼接的 gzip 流，最后一段是 data tar。"""
    streams, buf = [], blob
    while buf[:2] == b"\x1f\x8b":
        with gzip.GzipFile(fileobj=io.BytesIO(buf)) as g:
            out = g.read()
        streams.append(out)
        # 找到剩余数据：重新解压一次拿 unused_data
        import zlib
        d = zlib.decompressobj(31)
        d.decompress(buf)
        buf = d.unused_data
        if not buf:
            break
    return streams[-1] if streams else None


index_path = WORK / "APKINDEX.tar.gz"
if not index_path.exists():
    index_path.write_bytes(get("%s/%s/main/%s/APKINDEX.tar.gz" % (MIRROR, BRANCH, ARCH)))

with tarfile.open(index_path) as tf:
    raw = tf.extractfile("APKINDEX").read().decode("utf-8", "replace")

pkgs = parse_index(raw)
print("索引包数: %d" % len(pkgs))
for name in ("musl-dev", "libstdc++-dev", "libgcc", "linux-headers"):
    p = pkgs.get(name)
    if p:
        print("  %-16s V=%s  S=%s bytes  origin=%s" % (name, p.get("V"), p.get("S"), p.get("o")))
    else:
        print("  %-16s 未找到" % name)

for name in ("musl-dev", "libstdc++-dev"):
    p = pkgs.get(name)
    if not p:
        continue
    fname = "%s-%s.apk" % (name, p["V"])
    local = WORK / fname
    if not local.exists():
        local.write_bytes(get("%s/%s/main/%s/%s" % (MIRROR, BRANCH, ARCH, fname)))
    print("\n=== %s (%.2f MB) ===" % (fname, local.stat().st_size / 1048576))
    tar_bytes = apk_data_tar(local.read_bytes())
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tf:
        entries = []
        for m in tf.getmembers():
            entries.append((m.name, m.isdir(), m.issym(), m.linkname if m.issym() else ""))
    dirs = sorted(set(n for n, isd, _, _ in entries if "/" in n))
    print("  顶层: %s" % sorted(set(n.split("/")[1] for n in dirs if n.startswith("usr/"))))
    for key in ("crt1.o", "Scrt1.o", "crti.o", "libc.a", "libstdc++.a", "libsupc++.a",
                "libc.so", "ld-musl"):
        hit = [n for n, isd, _, _ in entries if n.endswith("/" + key) or n.endswith(key)]
        print("  %-16s %s" % (key, hit[:3] or "-"))
    cxx = sorted(set(n.split("/")[3] for n, _, _, _ in entries if n.startswith("usr/include/c++/")))
    print("  C++ 头版本目录: %s" % cxx)
    osdef = [n for n, _, _, _ in entries if n.endswith("os_defines.h")]
    print("  os_defines.h: %s" % (osdef or "-"))
