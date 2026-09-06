#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xcc sysroot 采集器

    python tools/build_sysroot.py aarch64-linux-musl
    python tools/build_sysroot.py --list

设计原则：
  * 幂等   —— 重复执行结果一致，已有 sysroot 先清空重建
  * 可缓存 —— 下载物落在 cache/，记录 sha256
  * 零特权 —— 不需要 root / dpkg / apk / ar，纯 Python 解包
  * 版本自适应 —— 从发行版索引页抓最新版本，不写死版本号

musl 家族的素材选型（关键，全部来自 Alpine，纯 musl 原生）：
  musl 头 + libc.a + CRT  -> Alpine musl-dev
  C++ 头 + libstdc++.a    -> Alpine libstdc++-dev （musl 原生，不带 glibc 依赖）
  crtbegin/crtend/libgcc  -> Alpine libgcc-static + gcc
                            （必须是 musl 原生：Debian 的 libgcc_eh.a 引用
                              glibc 专有符号 _dl_find_object，链接 C++ 异常代码会 undefined）

  之所以不用 Debian 的 libstdc++-dev/gcc：它们的 os_defines.h / libgcc_eh.a
  依赖 __GLIBC_PREREQ / _dl_find_object，喂给 musl 会直接编译/链接失败。
"""

import argparse
import gzip
import hashlib
import io
import json
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "cache"
SYSROOT = ROOT / "sysroot"
BUILD = ROOT / "build"

# 镜像：阿里云 Alpine 在国内可达；Debian 官方源亦可直连
ALPINE_MIRROR = "https://mirrors.aliyun.com/alpine"
ALPINE_BRANCH = "latest-stable"
DEB_MIRROR = "https://deb.debian.org/debian/pool/main"

RECIPES = {
    "aarch64-linux-musl": dict(alpine_arch="aarch64", deb_arch="arm64",
                               triplet="aarch64-linux-musl", family="musl"),
    "x86_64-linux-musl": dict(alpine_arch="x86_64", deb_arch="amd64",
                              triplet="x86_64-linux-musl", family="musl"),
    "riscv64-linux-musl": dict(alpine_arch="riscv64", deb_arch="riscv64",
                               triplet="riscv64-linux-musl", family="musl"),
    "x86_64-w64-mingw32": dict(triplet="x86_64-w64-mingw32", mw_arch="x86-64",
                               deb_arch="amd64", family="windows"),
    "i686-w64-mingw32":   dict(triplet="i686-w64-mingw32", mw_arch="i686",
                               deb_arch="amd64", family="windows"),
    "wasm32-wasi": dict(family="wasi", version="wasi-sdk-25"),
}

# wasi-sysroot 下载源（依次回退：直连 -> 国内加速）
WASI_SYSROOT_URLS = [
    "https://github.com/WebAssembly/wasi-sdk/releases/download/{ver}/wasi-sysroot-{ver2}.tar.gz",
    "https://gh-proxy.com/https://github.com/WebAssembly/wasi-sdk/releases/download/{ver}/wasi-sysroot-{ver2}.tar.gz",
]


def log(*a):
    print("[sysroot]", *a, flush=True)


# --------------------------------------------------------------------------
# 通用网络
# --------------------------------------------------------------------------
def http_get(url, binary=True, timeout=300):
    req = urllib.request.Request(url, headers={"User-Agent": "xcc-sysroot/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    return data if binary else data.decode("utf-8", "replace")


def version_key(v):
    parts = re.split(r"([0-9]+)", v)
    return tuple(int(p) if p.isdigit() else p for p in parts if p != "")


def download(url, dest, note=""):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 0:
        log("缓存命中 %s" % dest.name)
        return dest
    log("下载 %s%s" % (url.rsplit("/", 1)[-1], " (%s)" % note if note else ""))
    data = http_get(url)
    dest.write_bytes(data)
    log("  -> %.2f MB" % (len(data) / 1048576.0))
    dest.with_suffix(dest.suffix + ".sha256").write_text(hashlib.sha256(data).hexdigest() + "\n")
    return dest


# --------------------------------------------------------------------------
# Debian: 目录页挑最新包 + ar/tar 解包
# --------------------------------------------------------------------------
def deb_pick_latest(deb_dir, pattern, deb_arch):
    html = http_get(deb_dir + "/", binary=False)
    pat = re.compile(pattern % re.escape(deb_arch))
    cands = {}
    for m in re.finditer(r'href="([^"]+?\.deb)"', html):
        name = m.group(1).split("/")[-1]
        pm = pat.search(name)
        if pm:
            cands[pm.group(1)] = name
    if not cands:
        raise RuntimeError("Debian 目录页未找到匹配包: %s" % deb_dir)
    return deb_dir + "/" + cands[sorted(cands, key=version_key)[-1]]


def ar_members(buf):
    if buf[:8] != b"!<arch>\n":
        raise RuntimeError("不是合法的 ar 归档")
    off = 8
    while off + 60 <= len(buf):
        hdr = buf[off:off + 60]
        name = hdr[0:16].decode("ascii", "replace").rstrip()
        size_field = hdr[48:58].decode("ascii", "replace").strip()
        if not size_field:
            break
        size = int(size_field)
        yield name.rstrip("/"), buf[off + 60: off + 60 + size]
        off += 60 + size
        if off % 2:
            off += 1


def decompress(name, body):
    if name.endswith(".xz"):
        return lzma.decompress(body)
    if name.endswith(".gz"):
        return gzip.decompress(body)
    if name.endswith(".zst"):
        try:
            with tarfile.open(fileobj=io.BytesIO(body), mode="r:zstd") as _:
                pass
        except Exception:
            pass
        exe = shutil.which("zstd")
        if not exe:
            raise RuntimeError("data.tar.zst 需要 zstd（或 Python 3.14+）")
        p = subprocess.run([exe, "-dc"], input=body, capture_output=True)
        if p.returncode != 0:
            raise RuntimeError("zstd 解压失败")
        return p.stdout
    raise RuntimeError("未知压缩格式: %s" % name)


def deb_extract(deb_path, destdir):
    for name, body in ar_members(deb_path.read_bytes()):
        if name.startswith("data.tar"):
            tar_bytes = decompress(name, body)
            break
    else:
        raise RuntimeError("deb 中没有 data.tar: %s" % deb_path.name)
    destdir.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as tf:
        members = []
        for m in tf.getmembers():
            if not (m.isfile() or m.issym()):
                continue
            m.name = m.name.lstrip("./")
            if m.name.startswith(".."):
                continue
            members.append(m)
        tf.extractall(destdir, members=members)


# --------------------------------------------------------------------------
# Alpine: APKINDEX 解析 + apk 解包
# --------------------------------------------------------------------------
def alpine_index(arch):
    dest = CACHE / ("APKINDEX-%s.tar.gz" % arch)
    download("%s/%s/main/%s/APKINDEX.tar.gz" % (ALPINE_MIRROR, ALPINE_BRANCH, arch), dest,
             note="alpine index")
    with tarfile.open(dest) as tf:
        raw = tf.extractfile("APKINDEX").read().decode("utf-8", "replace")
    pkgs = {}
    for rec in raw.split("\n\n"):
        f = {}
        for line in rec.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                f.setdefault(k, v)
        if "P" in f and "V" in f and f.get("A", arch) == arch:
            pkgs[f["P"]] = f
    return pkgs


def alpine_apk_urls(arch, names):
    pkgs = alpine_index(arch)
    out = {}
    for n in names:
        p = pkgs.get(n)
        if not p:
            raise RuntimeError("Alpine 索引中未找到包: %s" % n)
        out[n] = ("%s/%s/main/%s/%s-%s.apk" % (ALPINE_MIRROR, ALPINE_BRANCH, arch, n, p["V"]),
                  p["V"])
    return out


def apk_extract(apk_path, destdir):
    """apk = 若干拼接的 gzip 流，最后一段是 data tar。"""
    buf, streams = apk_path.read_bytes(), []
    while buf[:2] == b"\x1f\x8b":
        d = zlib.decompressobj(31)
        out = d.decompress(buf)
        streams.append(out)
        buf = d.unused_data
        if not buf:
            break
    if not streams:
        raise RuntimeError("apk 格式异常: %s" % apk_path.name)
    destdir.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(streams[-1]), mode="r:") as tf:
        members = []
        for m in tf.getmembers():
            if not (m.isfile() or m.issym()):
                continue
            m.name = m.name.lstrip("./")
            if m.name.startswith(".."):
                continue
            members.append(m)
        tf.extractall(destdir, members=members)


# --------------------------------------------------------------------------
# 组装
# --------------------------------------------------------------------------
def copy_tree(src, dst, label=""):
    n = 0
    src, dst = Path(src), Path(dst)
    for f in src.rglob("*"):
        if f.is_file():
            rel = f.relative_to(src)
            t = dst / rel
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, t)
            n += 1
    if label:
        log("  %-26s %d 个文件" % (label, n))
    return n


def fix_linker_scripts(libdir):
    for f in libdir.glob("*.so"):
        try:
            txt = f.read_bytes().decode("utf-8")
        except UnicodeDecodeError:
            continue                      # 真实 ELF，不是链接脚本
        if "GROUP" not in txt and "OUTPUT_FORMAT" not in txt:
            continue
        orig = txt
        # /usr/lib/<multiarch>/libc.so.6 -> libc.so.6（同时支持 lib/lib64/lib32）
        txt = re.sub(r"/(?:usr/)?lib\d*/[^ )]*/([A-Za-z0-9_.+-]+\.(?:a|so[.0-9]*))", r"\1", txt)
        # 动态链接器引用 /lib64/ld-linux-x86-64.so.2 -> ld-linux-x86-64.so.2
        txt = re.sub(r"/(?:usr/)?lib\d*/(ld-musl-[A-Za-z0-9_-]+\.so\.[0-9.]+)", r"\1", txt)
        txt = re.sub(r"/(?:usr/)?lib\d*/(ld-linux-[A-Za-z0-9_-]+\.so\.[0-9.]+)", r"\1", txt)
        txt = re.sub(r"/(?:usr/)?lib\d*/(libc\.musl-[A-Za-z0-9_-]+\.so\.1)", r"\1", txt)
        if txt != orig:
            f.write_text(txt)
            log("  修正链接脚本 %s" % f.name)


def build_musl(target, recipe, workdir):
    alpine_arch = recipe["alpine_arch"]
    deb_arch = recipe["deb_arch"]
    triplet = recipe["triplet"]
    gnu_triplet = triplet.replace("-musl", "-gnu")
    out = SYSROOT / target
    if out.exists():
        shutil.rmtree(out)
    (out / "include").mkdir(parents=True)
    (out / "lib").mkdir(parents=True)

    stage = workdir / "stage" / target
    if stage.exists():
        shutil.rmtree(stage)

    # 1) Alpine: musl 头 / libc.a / CRT / C++ 运行时
    #    注意：Alpine 各架构的 apk 文件名相同（如 libgcc-static-15.2.0-r5.apk），
    #    必须按架构加前缀做缓存隔离，否则会串包（x86_64 拿到 aarch64 的 linker）。
    urls = alpine_apk_urls(alpine_arch, ["musl-dev", "musl", "libstdc++-dev"])
    for name, (url, ver) in urls.items():
        apk = download(url, CACHE / (alpine_arch + "-" + url.rsplit("/", 1)[-1]), note=ver)
        d = stage / name
        apk_extract(apk, d)
        log("解包 %s (%s)" % (apk.name, ver))

    # Alpine 的头文件直接在 usr/include（无 triplet 子目录）
    if (stage / "musl-dev" / "usr" / "include").is_dir():
        copy_tree(stage / "musl-dev" / "usr" / "include", out / "include", "musl 头文件")

    # musl 包提供动态链接器 ld-musl-<arch>.so.1
    for pat in ("musl/lib/ld-musl-*.so.1", "musl/usr/lib/ld-musl-*.so.1",
                "musl/lib/libc.musl-*.so.1", "musl/usr/lib/libc.musl-*.so.1"):
        for f in stage.glob(pat):
            if f.is_file():
                shutil.copy2(f, out / "lib" / f.name)
                log("  %-26s %s" % ("动态链接器", f.name))

    # musl-dev 的库与 CRT
    for f in sorted((stage / "musl-dev" / "usr" / "lib").glob("*")) if (stage / "musl-dev" / "usr" / "lib").is_dir() else []:
        if f.is_file() and (f.suffix in (".a", ".o") or f.name.endswith(".so") or ".so." in f.name):
            shutil.copy2(f, out / "lib" / f.name)
    log("  %-26s %d 个文件" % ("musl 库/CRT",
                               len(list((out / "lib").glob("*.a"))) + len(list((out / "lib").glob("*.o")))))

    # 2) Alpine libstdc++-dev：C++ 头 + 静态库
    cxx_stage = stage / "libstdc++-dev"
    cxx_ver = None
    cxx_inc = cxx_stage / "usr" / "include" / "c++"
    if cxx_inc.is_dir():
        for v in cxx_inc.iterdir():
            if v.is_dir():
                cxx_ver = v.name
                copy_tree(v, out / "include" / "c++" / v.name, "C++ 头文件 (%s)" % v.name)
                # Alpine 的平台子头目录形如 <ver>/aarch64-alpine-linux-musl，
                # 只有它是 arch 专属的（其余如 bits/ext/backward 是公共头，上面已整棵拷过）
                for sub in v.iterdir():
                    if sub.is_dir() and sub.name != triplet and "-" in sub.name \
                            and sub.name.split("-")[0] in ("aarch64", "x86_64", "riscv64",
                                                           "armv7", "ppc64le", "s390x", "i586"):
                        copy_tree(sub, out / "include" / "c++" / v.name / triplet,
                                  "C++ 平台子头 -> %s" % triplet)
    if cxx_ver is None:
        log("  警告：未拿到 C++ 头文件，C++ 将不可用")

    for f in ("libstdc++.a", "libstdc++.so", "libstdc++exp.a", "libstdc++fs.a", "libsupc++.a"):
        p = cxx_stage / "usr" / "lib" / f
        if p.is_file():
            shutil.copy2(p, out / "lib" / f)
    if cxx_ver:
        log("  %-26s %s" % ("C++ 静态库", "libstdc++.a (musl 原生)"))

    # 3) Alpine gcc 包：crtbegin/crtend/libgcc/libgcc_eh
    #    必须是 musl 原生的 —— Debian 的 libgcc_eh.a 引用 glibc 专有符号 _dl_find_object，
    #    链接 C++ 异常代码时会 undefined symbol。
    #    libgcc-static -> libgcc.a ；gcc 包 -> crt*.o 与 libgcc_eh.a
    for name, (url, ver) in alpine_apk_urls(alpine_arch, ["libgcc-static", "gcc"]).items():
        apk = download(url, CACHE / (alpine_arch + "-" + url.rsplit("/", 1)[-1]), note=ver)
        apk_extract(apk, stage / name)
        log("解包 %s (%s)" % (apk.name, ver))
    gcc_files = ["crtbegin.o", "crtend.o", "crtbeginS.o", "crtendS.o", "crtbeginT.o",
                 "crtbeginD.o", "crtendD.o", "libgcc.a", "libgcc_eh.a"]
    gcc_ver = None
    for gd in sorted(stage.glob("*/usr/lib/gcc/*/*")):
        if not gd.is_dir():
            continue
        gcc_ver = gd.name
        gdst = out / "lib" / "gcc" / gnu_triplet / gd.name
        gdst.mkdir(parents=True, exist_ok=True)
        n = 0
        for f in gcc_files:
            p = gd / f
            if p.is_file():
                shutil.copy2(p, gdst / f)
                shutil.copy2(p, out / "lib" / f)   # 同时在 lib/ 放一份，便于 -lgcc 命中
                n += 1
        if n:
            log("  %-26s %d 个文件 (gcc %s)" % ("gcc CRT/运行时", n, gd.name))

    fix_linker_scripts(out / "lib")

    # 4) 元数据：驱动靠它显式注入 C++ 头，不依赖 clang 的 GCC 探测
    manifest = dict(target=target, triplet=triplet, family="musl",
                    cxx_include=("include/c++/%s" % cxx_ver) if cxx_ver else None,
                    cxx_triplet_include=("include/c++/%s/%s" % (cxx_ver, triplet)) if cxx_ver else None,
                    gcc_version=gcc_ver,
                    alpine_arch=alpine_arch, deb_arch=deb_arch,
                    runtime="libgcc",
                    libs=sorted(p.name for p in (out / "lib").glob("*.a")))
    (out / "xcc.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")

    log("完成 sysroot: %s" % out)
    log("  C++: %s" % ("可用 (%s)" % cxx_ver if cxx_ver else "不可用"))
    return out


def densify_symlinks(root):
    """把符号链接替换成真实副本（Windows 上 zip 分发更稳，避免跨平台 symlink 失效）。"""
    n = 0
    for link in sorted(Path(root).rglob("*")):
        if link.is_symlink():
            real = link.resolve()
            if real.is_file():
                link.unlink()
                shutil.copy2(real, link)
                n += 1
    if n:
        log("  %-26s %d 个 symlink -> 真实副本" % ("符号链接固化", n))


def deb_pick(deb_dir, pattern, deb_arch):
    return deb_pick_latest(deb_dir, pattern, deb_arch)


def build_wasi(target, recipe, workdir):
    """wasi-sysroot：官方预编译 tarball，直接抽取 include/ 与 lib/wasm32-wasi。

    自带 libc.a / crt1-command.o / libc++.a / libc++abi.a（C++ 可用）。
    另需 libclang_rt.builtins-wasm32.a（clang 链接 wasm 时硬性要求，
    且必须位于宿主 clang 资源目录 lib/wasm32-unknown-wasi/ 下）——
    从 Debian libclang-rt-22-dev-wasm32（约 100KB）获取，存进 sysroot，
    由驱动在首次链接时落到 clang 资源目录（make_release 也会预置）。
    """
    ver = recipe["version"]                    # wasi-sdk-25
    ver2 = ver.split("-")[-1] + ".0"           # 25.0
    out = SYSROOT / target
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    tarball = CACHE / ("wasi-sysroot-%s.tar.gz" % ver2)
    if not tarball.exists():
        last_err = None
        for tpl in WASI_SYSROOT_URLS:
            url = tpl.format(ver=ver, ver2=ver2)
            try:
                download(url, tarball, note="wasi-sysroot")
                break
            except Exception as e:
                last_err = e
                tarball.unlink(missing_ok=True)
        else:
            raise RuntimeError("wasi-sysroot 全部源失败: %s" % last_err)

    stage = workdir / "stage" / target
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    log("解包 %s" % tarball.name)
    with tarfile.open(tarball, mode="r:gz") as tf:
        members = []
        for m in tf.getmembers():
            if not (m.isfile() or m.issym()):
                continue
            m.name = m.name.lstrip("./")
            if m.name.startswith(".."):
                continue
            members.append(m)
        tf.extractall(stage, members=members)

    src = next(stage.iterdir())               # wasi-sysroot-25.0/
    copy_tree(src / "include", out / "include", "wasi 头文件 (含 C++/v1)")
    copy_tree(src / "lib" / "wasm32-wasi", out / "lib" / "wasm32-wasi", "wasi 库/CRT")

    # compiler-rt builtins for wasm32（Debian all 架构包，~100KB）
    url = deb_pick_latest(DEB_MIRROR + "/l/llvm-toolchain-22",
                          r"libclang-rt-22-dev-wasm32_([\d.]+[-~\w.+]*?)_%s\.deb", "all")
    deb = download(url, CACHE / url.rsplit("/", 1)[-1], note="wasm builtins")
    bdir = stage / "builtins"
    deb_extract(deb, bdir)
    for p in bdir.rglob("libclang_rt.builtins-wasm32.a"):
        shutil.copy2(p, out / "lib" / "wasm32-wasi" / p.name)
        log("  %-26s %s (%.0f KB)" % ("wasm builtins", p.name, p.stat().st_size / 1024))
        break
    else:
        raise RuntimeError("builtins 包中没有 libclang_rt.builtins-wasm32.a")

    # wasi-sdk 21+ 布局：C++ 头按 target 放在 include/<target>/c++/v1
    cxx_include = "include/wasm32-wasi/c++/v1" \
        if (out / "include" / "wasm32-wasi" / "c++" / "v1").is_dir() else None
    builtins = (out / "lib" / "wasm32-wasi" / "libclang_rt.builtins-wasm32.a").is_file()
    manifest = dict(target=target, triplet="wasm32-wasi", family="wasi",
                    cxx_include=cxx_include, cxx_triplet_include=None,
                    lib_dirs=["lib/wasm32-wasi"], runtime="compiler-rt",
                    has_builtins=builtins,
                    libs=sorted(p.name for p in (out / "lib" / "wasm32-wasi").glob("*.a")))
    (out / "xcc.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")

    log("完成 sysroot: %s" % out)
    log("  C++: %s | builtins: %s" % ("可用 (libc++)" if cxx_include else "不可用",
                                      "自带" if builtins else "缺失(按需补)"))
    return out


def build_mingw(target, recipe, workdir):
    """mingw-w64：Debian 交叉包重组为规范 mingw sysroot 布局。

      mingw-w64-common            -> include/（公共头，stdio.h 等）
      mingw-w64-<arch>-dev       -> include/ + lib/（arch 头/CRT/导入库）
      gcc-mingw-w64-<arch>-posix -> lib/gcc/<triplet>/<ver>/（libgcc/crtbegin）
      g++-mingw-w64-<arch>-posix -> 同目录补 libstdc++.a 与 C++ 头

    clang 的 MinGW 工具链以 --sysroot 为根，自动探测 lib/gcc/<triplet>/<ver>，
    CRT 与导入库（crt2.o/libkernel32.a 等）从 <sysroot>/lib 取。
    默认动态链 msvcrt.dll（Windows 自带），libgcc/libstdc++ 静态（驱动注入
    -static-libgcc -static-libstdc++），产物不拖额外 DLL。
    """
    triplet = recipe["triplet"]
    mw_arch = recipe["mw_arch"]
    host_arch = recipe["deb_arch"]
    out = SYSROOT / target
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    stage = workdir / "stage" / target
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    pkgs = {
        "common": (deb_pick(DEB_MIRROR + "/m/mingw-w64",
                            r"mingw-w64-common_([\d.]+[-~\w.+]*?)_%s\.deb", "all"),
                   "mingw 公共头 (stdio.h 等)"),
        "dev": (deb_pick(DEB_MIRROR + "/m/mingw-w64",
                         r"mingw-w64-%s-dev_([\d.]+[-~\w.+]*?)_%%s\.deb" % re.escape(mw_arch),
                         "all"), "mingw 头/CRT/导入库"),
        "gcc": (deb_pick(DEB_MIRROR + "/g/gcc-mingw-w64",
                         r"gcc-mingw-w64-%s-posix_([\d.]+[-~\w.+]*?)_%%s\.deb" % re.escape(mw_arch),
                         host_arch), "gcc 运行时 (libgcc/CRT)"),
        "gpp": (deb_pick(DEB_MIRROR + "/g/gcc-mingw-w64",
                         r"g\+\+-mingw-w64-%s-posix_([\d.]+[-~\w.+]*?)_%%s\.deb" % re.escape(mw_arch),
                         host_arch), "C++ 头/库 (libstdc++)"),
    }
    for name, (url, what) in pkgs.items():
        deb = download(url, CACHE / url.rsplit("/", 1)[-1], note=what)
        deb_extract(deb, stage / name)
        log("解包 %s (%s)" % (deb.name, what))

    # 1) 头文件 + CRT/导入库：/usr/<triplet>/{include,lib} -> sysroot 根
    src_root = stage / "dev" / "usr" / triplet
    if not src_root.is_dir():
        raise RuntimeError("mingw-w64-dev 布局异常：缺 /usr/%s" % triplet)
    copy_tree(src_root / "include", out / "include", "mingw arch 头")
    copy_tree(src_root / "lib", out / "lib", "CRT/导入库")

    # common 包的真实头文件树（Debian 新版把公共头拆到这里，dev 里的只是 symlink）
    stdio = list((stage / "common").rglob("stdio.h"))
    if not stdio:
        raise RuntimeError("mingw-w64-common 中未找到头文件树")
    incsrc = stdio[0].parent
    copy_tree(incsrc, out / "include", "mingw 公共头 (%s)" % incsrc.name)

    # 2) gcc 运行时目录（libgcc.a/crtbegin.o；g++ 的 libstdc++.a 也放这里）
    gcc_dirs = sorted((stage / "gcc" / "usr" / "lib" / "gcc" / triplet).glob("*"))
    if not gcc_dirs:
        raise RuntimeError("gcc-mingw-w64 布局异常：缺 /usr/lib/gcc/%s" % triplet)
    gcc_ver = gcc_dirs[-1].name
    gdst = out / "lib" / "gcc" / triplet / gcc_ver
    gdst.mkdir(parents=True)
    copy_tree(gcc_dirs[-1], gdst, "gcc 运行时 (%s)" % gcc_ver)

    # 3) C++ 运行时：Debian 把 mingw 的 C++ 头放在 gcc 目录的 include/c++ 下
    #    （GCC 16 布局：无版本子目录，<triplet> 平台子头在其内层），libstdc++.a
    #    与其余 gcc 运行时同层。合并进 gdst，并在 manifest 里按相对路径声明。
    cxx_include = cxx_triplet_include = None
    gpp_dirs = sorted((stage / "gpp" / "usr" / "lib" / "gcc" / triplet).glob("*"))
    if gpp_dirs:
        gsrc = gpp_dirs[-1]
        for f in gsrc.glob("*.a"):                 # 只取库 + 头，跳过 cc1plus 等宿主二进制
            shutil.copy2(f, gdst / f.name)
        inc_cpp = gsrc / "include" / "c++"
        if inc_cpp.is_dir():
            copy_tree(inc_cpp, gdst / "include" / "c++", "C++ 头文件 (libstdc++)")
            cxx_include = "lib/gcc/%s/%s/include/c++" % (triplet, gcc_ver)
            if (inc_cpp / triplet).is_dir():
                cxx_triplet_include = "lib/gcc/%s/%s/include/c++/%s" % (triplet, gcc_ver, triplet)

    # 4) 元数据
    lib_dirs = ["lib/gcc/%s/%s" % (triplet, gcc_ver)]
    libs = sorted(set(
        p.name for p in list((out / "lib").glob("*.a")) +
                        list((out / "lib" / "gcc" / triplet / gcc_ver).glob("*.a"))))
    manifest = dict(target=target, triplet=triplet, family="windows",
                    cxx_autodetect=False,
                    cxx_include=cxx_include, cxx_triplet_include=cxx_triplet_include,
                    lib_dirs=lib_dirs, runtime="libgcc",
                    gcc_version=gcc_ver, libs=libs)
    (out / "xcc.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")

    log("完成 sysroot: %s" % out)
    log("  CRT: %s | C++: %s" % ("msvcrt (系统 DLL)",
                                 "可用 (%s)" % gcc_ver if cxx_include else "不可用"))
    return out


def main():
    ap = argparse.ArgumentParser(description="xcc sysroot 采集器")
    ap.add_argument("target", nargs="?", help="目标三元组，如 aarch64-linux-musl")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list or not args.target:
        print("支持的 target:")
        for k, v in RECIPES.items():
            ok = (SYSROOT / k / "xcc.json").is_file()
            print("  %-22s %-8s %s" % (k, v["family"], "已安装" if ok else "未采集"))
        return 0

    recipe = RECIPES.get(args.target)
    if not recipe:
        log("错误：未知 target '%s'（--list 查看）" % args.target)
        return 1
    try:
        BUILD.mkdir(exist_ok=True)
        if recipe["family"] == "musl":
            build_musl(args.target, recipe, BUILD)
        elif recipe["family"] == "windows":
            build_mingw(args.target, recipe, BUILD)
        elif recipe["family"] == "wasi":
            build_wasi(args.target, recipe, BUILD)
        else:
            log("错误：未实现的 family '%s'" % recipe["family"])
            return 1
    except Exception as e:
        log("失败：%s" % e)
        import traceback
        traceback.print_exc()
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
