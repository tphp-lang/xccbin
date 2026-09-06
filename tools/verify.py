#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xcc 产物真实验证器

原则（用户极度重视真实验证，拒绝"声称成功"）：
  不依赖退出码，直接用 LLVM 工具对产物做静态分析：
    * llvm-readobj --file-header  -> 架构 (EM_AARCH64 / EM_X86_64 / EM_RISCV /
                                      IMAGE_FILE_MACHINE_AMD64 / WASM)
    * llvm-objdump -d             -> 反汇编，确认指令集正确
    * llvm-readelf --program-headers -> 是否有 INTERP（动态） / PT_DYNAMIC
    * llvm-nm -u                   -> 静态链接应无未定义符号（出现 _DYNAMIC 是 musl 常态，可接受）
    * PE 目标（mingw）在 Windows 宿主上直接真机运行

用法：
    python tools/verify.py aarch64-linux-musl build/hello build/vector
    python tools/verify.py --all        # 对所有已采集 target 跑 tests/ 的 hello.c + vector.cpp
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# target -> 期望的 ELF Machine（来自 llvm-readobj --file-header 的 "Machine:" 字段）
ARCH = {
    "aarch64-linux-musl": "EM_AARCH64",
    "x86_64-linux-musl": "EM_X86_64",
    "riscv64-linux-musl": "EM_RISCV",
    "wasm32-wasi": "WASM",          # wasm 走 "Format: WASM / Arch: wasm32" 判定
    # mingw 产物是 PE/COFF，Machine 形如 IMAGE_FILE_MACHINE_AMD64
    "x86_64-w64-mingw32": "IMAGE_FILE_MACHINE_AMD64",
    "i686-w64-mingw32": "IMAGE_FILE_MACHINE_I386",
}

# PE 目标：CRT 是系统 DLL（msvcrt），动态导入属预期；win64 宿主可真机运行
WINDOWS_TARGETS = {"x86_64-w64-mingw32", "i686-w64-mingw32"}

# target -> 反汇编里应出现的指令助记符（仅做存在性嗅探，确认指令集）
ASM_HINT = {
    "aarch64-linux-musl": ("stp", "ldr", "bl"),
    "x86_64-linux-musl": ("mov", "call", "push"),
    "riscv64-linux-musl": ("addi", "ld", "sd", "ret"),
    "wasm32-wasi": ("i32.", "i64.", "call", "local.get", "block"),
    "x86_64-w64-mingw32": ("mov", "call", "push"),
    "i686-w64-mingw32": ("mov", "call", "push"),
}


def resolve_tool(name):
    """which 解析，兼容发行版的版本化命名（llvm-nm-18 等）。"""
    w = shutil.which(name)
    if w:
        return w
    for v in range(14, 23):
        w = shutil.which("%s-%d" % (name, v))
        if w:
            return w
    return None


def run(tool, *args):
    exe = resolve_tool(tool)
    if not exe:
        return None, "找不到 %s" % tool
    try:
        p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
        return p.stdout + p.stderr, None
    except Exception as e:
        return None, str(e)


def _pe_run(path: Path):
    """Windows 宿主上真机运行 PE 产物。返回 (ok, lines)。"""
    if sys.platform != "win32":
        return True, ["  · 非 Windows 宿主，跳过真机运行"]
    try:
        r = subprocess.run([str(path)], capture_output=True, text=True, timeout=60)
    except Exception as e:
        return False, ["  ? 真机运行异常: %s" % e]
    out = (r.stdout + r.stderr).strip().splitlines()
    tail = " | ".join(out[-2:]) if out else "(无输出)"
    if r.returncode == 0:
        return True, ["  ✓ 真机运行 exit=0 输出: %s" % tail[:120]]
    return False, ["  ? 真机运行 exit=%d 输出: %s" % (r.returncode, tail[:120])]


def check_binary(path: Path, target: str):
    """返回 (passed: bool, lines: [str])。"""
    lines = []
    ok = True
    expect_arch = ARCH.get(target, "AArch64")

    out, err = run("llvm-readobj", "--file-header", str(path))
    if out is None:
        return False, ["  ! llvm-readobj 失败: %s" % err]

    # wasm 产物：readobj 输出 "Format: WASM / Arch: wasm32"，无 program headers / nm -u
    if expect_arch == "WASM":
        m = re.search(r"Format:\s*(\S+)", out)
        a = re.search(r"Arch:\s*(\S+)", out)
        if m and m.group(1).lower() == "wasm" and a and a.group(1) == "wasm32":
            lines.append("  ✓ WASM 格式 (Arch: wasm32)")
        else:
            ok = False
            lines.append("  ? 期望 wasm32，实际 Format=%s Arch=%s"
                         % (m.group(1) if m else "未知", a.group(1) if a else "未知"))
        lines += _wasm_extra(path)
        return ok, lines

    m = re.search(r"Machine:\s*(\S+)", out)
    if not m or expect_arch not in m.group(1):
        ok = False
        lines.append("  ? 架构期望 %s，实际 %s" % (expect_arch, m.group(1) if m else "未知"))
    else:
        lines.append("  ✓ Machine = %s (%s)" % (
            m.group(1).replace("IMAGE_FILE_MACHINE_", ""),
            "PE/COFF" if m.group(1).startswith("IMAGE_FILE_") else "ELF"))

    if target in WINDOWS_TARGETS:
        # PE：CRT 是系统 DLL（msvcrt），nm 未定义符号是导入表，属预期；
        # 真正的强验证是直接在 Windows 宿主上运行（win64 可跑 x64 与 x86/WoW64）。
        runok, rlines = _pe_run(path)
        for L in rlines:
            lines.append(L)
        ok = ok and runok
        od, _ = run("llvm-objdump", "-d", str(path))
        seen = set()
        if od:
            for hint in ASM_HINT.get(target, ()):
                if re.search(r"\b%s" % re.escape(hint), od):
                    seen.add(hint)
        if seen:
            lines.append("  ✓ 指令集嗅探命中: %s" % ", ".join(sorted(seen)))
        return ok, lines

    # 动态/静态判定
    po, _ = run("llvm-readelf", "--program-headers", str(path))
    has_interp = bool(po and "INTERP" in po)
    has_dynamic_ph = bool(po and "PT_DYNAMIC" in po)
    if has_interp:
        lines.append("  · 动态链接 (有 INTERP 段)")
    elif has_dynamic_ph:
        lines.append("  · 部分静态 (有 PT_DYNAMIC 但无 INTERP)")
    else:
        lines.append("  ✓ 完全静态链接 (无 INTERP / 无 PT_DYNAMIC)")

    # 未定义符号：只关心"强未定义"(U 大写)。弱未定义(w/W/v)是 Itanium ABI 常态，可解析为 0。
    # 动态链接的二进制本就有大量强未定义符号（运行期由 ld.so 解析），属正常；
    # 只有静态链接才要求零强未定义。
    nm, _ = run("llvm-nm", "-u", str(path))
    if nm:
        strong = [s.split(None, 1)[1] for s in nm.splitlines()
                  if re.match(r"\s+U\s", s) and "_DYNAMIC" not in s]
        if strong:
            if has_interp or has_dynamic_ph:
                lines.append("  · 动态链接：%d 个强未定义符号将由 ld.so 解析（正常）" % len(strong))
            else:
                ok = False
                lines.append("  ? 静态二进制存在强未定义符号 (%d): %s"
                             % (len(strong), ", ".join(strong[:5])))
        else:
            lines.append("  ✓ 无强未定义符号 (静态自洽；弱未定义 w 视为已解析)")

    # 反汇编嗅探指令集
    od, _ = run("llvm-objdump", "-d", str(path))
    seen = set()
    if od:
        for hint in ASM_HINT.get(target, ()):
            # 不用尾部 \b：x86 助记符带宽度后缀（movq/callq），aarch64 不带，统一按前缀匹配
            if re.search(r"\b%s" % re.escape(hint), od):
                seen.add(hint)
        if seen:
            lines.append("  ✓ 指令集嗅探命中: %s" % ", ".join(sorted(seen)))
        else:
            lines.append("  · 指令集嗅探未命中 (可能已 strip 或体量极小)")

    return ok, lines


def _wasm_extra(path: Path):
    """wasm 附加检查：反汇编嗅探 + 代码段存在。"""
    lines = []
    od, _ = run("llvm-objdump", "-d", str(path))
    seen = set()
    if od:
        for hint in ASM_HINT.get("wasm32-wasi", ()):
            if re.search(r"\b%s" % re.escape(hint), od):
                seen.add(hint)
    if seen:
        lines.append("  ✓ wasm 指令嗅探命中: %s" % ", ".join(sorted(seen)))
    else:
        lines.append("  · wasm 指令嗅探未命中")
    # 体积信号：链接了 libc 的模块不会是空壳
    size = path.stat().st_size
    if size > 1024:
        lines.append("  ✓ 模块体量 %d 字节 (>1KB，libc 已链接)" % size)
    return lines


def check_cxx(path: Path):
    # -C 反 mangling，便于直接检索 std::
    nm, _ = run("llvm-nm", "-C", str(path))
    if not nm:
        return False, ["  ? 无法 nm"]
    has_std = "std::" in nm
    if has_std:
        return True, ["  ✓ 含 std:: 符号 (C++ 运行时已链接)"]
    # 退化检查：raw mangled 名里是否有 _ZNSt (std 命名空间)
    nm2, _ = run("llvm-nm", str(path))
    if nm2 and "_ZNSt" in nm2:
        return True, ["  ✓ 含 std 命名空间符号 (mangled _ZNSt)"]
    return False, ["  ? 未检出 std 符号，C++ 可能未真正参与链接"]


def main():
    args = sys.argv[1:]
    if "--all" in args:
        targets = [p.name for p in (ROOT / "sysroot").iterdir() if (p / "xcc.json").is_file()]
        targets = [t for t in targets if t in ARCH]
        all_ok = True
        for t in targets:
            sys.stdout.write("=== target %s ===\n" % t)
            rc, built = build_all(t)
            if not rc:
                all_ok = False
                sys.stdout.write("  ! 编译失败，跳过验证\n")
                continue
            for b in built:
                p = Path(b)
                sys.stdout.write("--- %s ---\n" % p.name)
                ok, lines = check_binary(p, t)
                for L in lines:
                    sys.stdout.write(L + "\n")
                if p.name.startswith("vector"):
                    cok, clines = check_cxx(p)
                    for L in clines:
                        sys.stdout.write(L + "\n")
                    ok = ok and cok
                if not ok:
                    all_ok = False
            sys.stdout.write("\n")
        return 0 if all_ok else 1

    # 形如: python tools/verify.py <target> <bin1> [<bin2> ...]
    if len(args) < 2:
        print("用法:")
        print("  python tools/verify.py --all")
        print("  python tools/verify.py <target> <bin1> [<bin2> ...]")
        return 2
    target = args[0]
    all_ok = True
    for b in args[1:]:
        p = Path(b)
        sys.stdout.write("--- %s ---\n" % p.name)
        ok, lines = check_binary(p, target)
        for L in lines:
            sys.stdout.write(L + "\n")
        if p.name.startswith("vector"):
            cok, clines = check_cxx(p)
            for L in clines:
                sys.stdout.write(L + "\n")
            ok = ok and cok
        if not ok:
            all_ok = False
        sys.stdout.write("\n")
    return 0 if all_ok else 1


def find_driver():
    """定位原生 xcc 驱动；不存在则用宿主 clang++ 现场编译（CI/开发环境均适用）。"""
    exe = "xcc.exe" if sys.platform == "win32" else "xcc"
    out_dir = ROOT / "build"
    out_dir.mkdir(exist_ok=True)
    drv = out_dir / exe
    if drv.is_file():
        return drv
    src = ROOT / "xcc.cpp"
    if not src.is_file():
        return None
    clang = shutil.which("clang++") or shutil.which("clang")
    if not clang:
        return None
    static = [] if sys.platform == "darwin" else ["-static"]
    r = subprocess.run([clang, "-std=c++17", "-O2", *static, "-o", str(drv), str(src), "-s"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write("  ! 驱动编译失败:\n%s\n" % r.stderr[:800])
        return None
    return drv


def build_all(target):
    """用原生 xcc 驱动编译 tests/hello.c 与 tests/vector.cpp（C++ 走 --role=c++）。返回 (ok, [bin_paths])。"""
    out_dir = ROOT / "build"
    out_dir.mkdir(exist_ok=True)
    drv = find_driver()
    if drv is None:
        sys.stdout.write("  ! 找不到/编不出原生驱动 (build/xcc)\n")
        return False, []
    built = []
    cases = [("hello.c", "cc"), ("vector.cpp", "c++")]
    for src, role in cases:
        sp = TESTS / src
        if not sp.exists():
            continue
        binp = out_dir / (src.rsplit(".", 1)[0] + "_" + target.replace("-", "_")
                          + (".exe" if target in WINDOWS_TARGETS else ""))
        cmd = [str(drv), "--role=%s" % role,
               "-target", target, str(sp), "-o", str(binp)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stdout.write("  ! %s 编译失败:\n%s\n" % (src, r.stderr.strip()[:800]))
            return False, built
        built.append(str(binp))
    return True, built


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
