#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xcc 发行包构建器（多 host）

产出自包含目录 + 压缩包：
    dist/xcc-<tag>/                # 解压即用（tag: win64 / linux-x64 / linux-arm64 / macos-...）
        bin/xcc[.exe]  xcc++  xcc-ar ...   # 原生驱动角色副本（argv[0] 多路复用）
        lib/llvm/                  # clang + lld + llvm 工具（Windows 另含 DLL 闭包）
        lib/clang 资源头 + wasm builtins
        sysroot/<target>/...       # 所有已采集的 sysroot

LLVM 底座来源（按优先级）：
    1. --llvm-dir / 环境变量 XCC_LLVM_DIR   显式指定（CI 的 Linux/macOS 用 apt/brew 装）
    2. Windows 本机 MSYS2 clang64 自动探测
    3. --fetch-llvm                          下载 LLVM 官方预编译包
       注意：官方 22.x 只发 Windows 资产（x86_64/aarch64）；Linux/macOS 需走 1。

驱动打包：
    用底座自带的 clang++ 把 xcc.cpp 编译成原生驱动（全静态，除 macOS 用系统
    libc++），再按角色复制。无 Python、无 PyInstaller，目标机零依赖。
"""

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYSROOT = ROOT / "sysroot"
CACHE = ROOT / "cache"

LLVM_VERSION = "22.1.8"
# LLVM 官方预编译资产（22.x 仅 Windows 有）
LLVM_FETCH_URLS = {
    ("windows", "x86_64"): f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz",
    ("windows", "aarch64"): f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/clang+llvm-{LLVM_VERSION}-aarch64-pc-windows-msvc.tar.xz",
}

# 需要随包发布的 LLVM 工具（不依赖后缀）
LLVM_TOOLS = [
    "clang", "clang++", "ld.lld", "lld-link", "wasm-ld", "lld",
    "llvm-ar", "llvm-ranlib", "llvm-nm", "llvm-objcopy", "llvm-strip",
    "llvm-readobj", "llvm-objdump", "llvm-readelf",
]

# 角色名（不带后缀；Windows 加 .exe）
DRIVER_ROLES = ["xcc", "xcc++", "xcc-c++", "xcc-g++", "xcc-cc", "xcc-gcc"]
BINUTILS_ROLES = ["xcc-ar", "xcc-ranlib", "xcc-nm", "xcc-objcopy", "xcc-strip",
                  "xcc-dlltool", "xcc-readobj", "xcc-objdump"]


def log(*a):
    print("[release]", *a, flush=True)


# --------------------------------------------------------------------------
# host 探测
# --------------------------------------------------------------------------
def detect_host():
    if sys.platform == "win32" or os.name == "nt":
        system, tag = "windows", "win64"
    elif sys.platform == "darwin":
        system = "macos"
        tag = "macos-arm64" if platform.machine() == "arm64" else "macos-x64"
    else:
        system = "linux"
        tag = "linux-arm64" if platform.machine() == "aarch64" else "linux-x64"
    return system, tag


# --------------------------------------------------------------------------
# LLVM 底座
# --------------------------------------------------------------------------
def find_msys2_clang64():
    for c in (Path("C:/env/Msys2/clang64"), Path("C:/env/Msys2/ucrt64"),
              Path("C:/msys64/clang64"), Path("C:/msys64/ucrt64")):
        if (c / "bin" / "clang.exe").exists():
            return c
    w = shutil.which("clang")
    if w:
        return Path(w).resolve().parent.parent
    return None


def fetch_llvm(tag):
    """下载并解包 LLVM 官方预编译包，返回底座目录。仅 Windows 资产可用。"""
    import urllib.request
    url = LLVM_FETCH_URLS.get((tag.split("-")[0], "x86_64" if tag == "win64" else "aarch64"))
    if url is None:
        raise RuntimeError("LLVM 官方没有 %s 的预编译资产；"
                           "请用 apt/brew 安装 clang 并传 --llvm-dir" % tag)
    dest = CACHE / ("llvm-%s.tar.xz" % tag)
    if not dest.exists():
        CACHE.mkdir(exist_ok=True)
        log("下载 LLVM 官方底座 (%s) ..." % url)
        req = urllib.request.Request(url, headers={"User-Agent": "xcc-release/0.1"})
        with urllib.request.urlopen(req, timeout=7200) as r, open(dest, "wb") as f:
            shutil.copyfileobj(r, f, length=1 << 22)
        log("下载完成: %.0f MB" % (dest.stat().st_size / 1048576))
    out = CACHE / ("llvm-" + tag)
    if not (out / "bin" / "clang.exe").exists():
        log("解包 %s ..." % dest.name)
        import tarfile
        with tarfile.open(dest, mode="r:xz") as tf:
            tf.extractall(out)
    inner = next(d for d in out.iterdir() if d.is_dir())
    return inner


def resolve_llvm_base(args, system, tag):
    if args.llvm_dir:
        p = Path(args.llvm_dir)
    elif os.environ.get("XCC_LLVM_DIR"):
        p = Path(os.environ["XCC_LLVM_DIR"])
    elif system == "windows":
        p = find_msys2_clang64() if not args.fetch_llvm else None
        if p is None and args.fetch_llvm:
            p = fetch_llvm(tag)
        elif p is None:
            log("未找到 MSYS2 clang64，回退 --fetch-llvm")
            p = fetch_llvm(tag)
    else:
        return None
    if p is None or not (p / "bin").is_dir():
        raise RuntimeError("LLVM 底座无效: %s" % p)
    return p


def dll_closure(src_bin: Path):
    """Windows：在源 bin 目录递归解析 clang.exe 的 DLL 依赖闭包。"""
    todo = [src_bin / "clang.exe"]
    seen, closure = set(), set()
    while todo:
        cur = todo.pop()
        if cur in seen:
            continue
        seen.add(cur)
        p = subprocess.run(["objdump", "-p", str(cur)], capture_output=True, text=True)
        for line in p.stdout.splitlines():
            if "DLL Name:" in line:
                dll = line.split("DLL Name:")[1].strip()
                dp = src_bin / dll
                if dp.exists() and dp not in seen:
                    closure.add(dp)
                    todo.append(dp)
    return closure


def copy_llvm(src, dst_llvm, system, exe_suffix):
    bin_src, bin_dst = src / "bin", dst_llvm / "bin"
    bin_dst.mkdir(parents=True, exist_ok=True)

    copied = []
    for t in set(LLVM_TOOLS):
        for cand in (t + exe_suffix, t):
            s = bin_src / cand
            if s.exists():
                shutil.copy2(s, bin_dst / s.name)
                copied.append(s.name)
                break
    log("复制 LLVM 工具: %s" % ", ".join(sorted(set(copied))))

    if system == "windows":
        closure = dll_closure(bin_src)
        for d in sorted(closure):
            shutil.copy2(d, bin_dst / d.name)
        log("复制 DLL 依赖闭包: %d 个 (%s)" % (len(closure), ", ".join(sorted(d.name for d in closure))))
    else:
        # 官方/发行版 clang 基本静态或仅依赖系统库；确保动态库一并带走
        extra = 0
        for d in bin_src.iterdir():
            if d.suffix in (".so", ".dylib") or ".so." in d.name:
                shutil.copy2(d, bin_dst / d.name)
                extra += 1
        log("复制共享库: %d 个" % extra)

    # clang 资源头（主机无关）
    for ver in (src / "lib" / "clang").glob("*"):
        if ver.is_dir() and (ver / "include").is_dir():
            dst_inc = dst_llvm / "lib" / "clang" / ver.name / "include"
            shutil.copytree(ver / "include", dst_inc, dirs_exist_ok=True)
            log("复制 clang 资源头: lib/clang/%s/include" % ver.name)

    clang_bin = bin_dst / ("clang" + exe_suffix)
    try:
        out = subprocess.run([str(clang_bin), "--version"],
                             capture_output=True, text=True, timeout=60).stdout
        ver_line = out.splitlines()[0] if out else "unknown"
    except Exception:
        ver_line = "unknown"
    (dst_llvm / "VERSION.txt").write_text(ver_line + "\n")
    log("LLVM 底座: %s" % ver_line)


# --------------------------------------------------------------------------
# 原生驱动：用底座 clang++ 把 xcc.cpp 编译到每个角色名
# --------------------------------------------------------------------------
def build_driver(dst_bin, system, exe_suffix, llvm_base):
    dst_bin.mkdir(parents=True, exist_ok=True)
    src = ROOT / "xcc.cpp"
    if not src.is_file():
        raise RuntimeError("找不到 %s" % src)

    clangxx = None
    cand = llvm_base / "bin" / ("clang++" + exe_suffix)
    if cand.exists():
        clangxx = cand
    if clangxx is None:
        w = shutil.which("clang++") or shutil.which("clang")
        if not w:
            raise RuntimeError("找不到 clang++，无法编译驱动")
        clangxx = Path(w)

    # macOS 链系统 libc++（永远在）；win/linux 全静态，目标机零依赖
    flags = ["-std=c++17", "-O2", "-s"]
    if system != "macos":
        flags.append("-static")

    # 每个角色独立编译到最终名（而非复制）：Windows 上刚落地的 exe 会被杀软
    # 瞬时扫描锁定，复制新鲜 exe 会撞锁；逐个编译只写不读，天然规避。
    import time
    roles = DRIVER_ROLES + BINUTILS_ROLES
    for role in roles:
        p = dst_bin / (role + exe_suffix)
        for attempt in range(10):
            r = subprocess.run([str(clangxx), *flags, "-o", str(p), str(src)],
                               capture_output=True, text=True)
            if r.returncode == 0:
                break
            if attempt == 9:
                raise RuntimeError("驱动编译失败 (%s):\n%s" % (role, r.stderr[:2000]))
            time.sleep(1.0)
        if system != "windows":
            p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    log("原生驱动角色: %d 个" % len(roles))


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def place_wasm_builtins(llvm_dst, sysroot_dst):
    """把 sysroot 自带的 wasm builtins 预置进 clang 资源目录（驱动亦有运行时兜底）。"""
    src = sysroot_dst / "wasm32-wasi" / "lib" / "wasm32-wasi" / "libclang_rt.builtins-wasm32.a"
    if not src.is_file():
        return
    for ver in (llvm_dst / "lib" / "clang").glob("*"):
        if ver.is_dir():
            dst = ver / "lib" / "wasm32-unknown-wasi" / "libclang_rt.builtins.a"
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            log("预置 wasm builtins -> lib/clang/%s/lib/wasm32-unknown-wasi/" % ver.name)
            break


def make_release(fetch_llvm_flag=False, llvm_dir=None):
    system, tag = detect_host()
    exe_suffix = ".exe" if system == "windows" else ""
    log("host: %s (%s)" % (system, tag))

    base = resolve_llvm_base(
        argparse.Namespace(llvm_dir=llvm_dir, fetch_llvm=fetch_llvm_flag), system, tag)
    log("LLVM 底座来源: %s" % base)

    dist = ROOT / "dist"
    out = dist / ("xcc-" + tag)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    log("=== 1/4 复制 LLVM 底座 ===")
    llvm_dst = out / "lib" / "llvm"
    copy_llvm(base, llvm_dst, system, exe_suffix)

    log("=== 2/4 编译原生驱动 ===")
    bin_dir = out / "bin"
    build_driver(bin_dir, system, exe_suffix, base)

    log("=== 3/4 复制 sysroot ===")
    sysroot_dst = out / "sysroot"
    n = 0
    if SYSROOT.is_dir():
        for t in sorted(p for p in SYSROOT.iterdir() if (p / "xcc.json").is_file()):
            shutil.copytree(t, sysroot_dst / t.name)
            n += 1
    log("复制 sysroot: %d 个 target" % n)
    place_wasm_builtins(llvm_dst, sysroot_dst)

    log("=== 4/4 写入口与压缩 ===")
    if system == "windows":
        (out / "xcc.bat").write_text(
            "@echo off\n"
            "rem xcc 便捷入口：把 bin 目录加入当前会话 PATH\n"
            "set PATH=%~dp0bin;%PATH%\n"
            "xcc.exe %*\n")
    else:
        sh = out / "xcc.sh"
        sh.write_text(
            "#!/bin/sh\n"
            "# xcc 便捷入口：把 bin 目录加入当前会话 PATH\n"
            'export PATH="$(cd "$(dirname "$0")" && pwd)/bin:$PATH"\n'
            'exec xcc "$@"\n')
        sh.chmod(sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    (out / "README.txt").write_text(
        "xcc — 多平台 C/C++ 交叉编译工具链（开箱即用）\n\n"
        "用法（bin/ 已在 PATH 内）：\n"
        "  xcc   -target aarch64-linux-musl  hello.c   -o hello\n"
        "  xcc++ -target x86_64-linux-musl   hello.cpp -o hello\n"
        "  xcc   -target wasm32-wasi         hello.c   -o hello.wasm\n"
        "  xcc -print-targets\n\n"
        "支持: aarch64/x86_64/riscv64 x linux-musl(全静态),\n"
        "      wasm32-wasi\n")

    zip_path = dist / ("xcc-%s.zip" % tag)
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in out.rglob("*"):
            z.write(f, f.relative_to(out))
    log("完成: %s (%.1f MB)" % (zip_path, zip_path.stat().st_size / 1048576.0))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch-llvm", action="store_true",
                    help="下载 LLVM 官方预编译包作为底座（仅 Windows 资产存在）")
    ap.add_argument("--llvm-dir", help="显式指定 LLVM 底座目录（含 bin/clang）")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    return make_release(fetch_llvm_flag=args.fetch_llvm,
                        llvm_dir=args.llvm_dir)


if __name__ == "__main__":
    sys.exit(main())
