#!/bin/sh
# 构建 xcc 的 C++ 工具
#
#   sh tools/build-tools.sh
#
# 产出：
#   build/xccsysroot[.exe]   sysroot 采集器（依赖 zlib + liblzma；POSIX 另需 libcurl，Windows 用系统 WinHTTP）
#   build/xccverify[.exe]    产物验证器（零第三方依赖，仅用系统 llvm-* 工具）
#
# 可用 CXX 环境变量指定编译器。

set -e
cd "$(dirname "$0")/.."
mkdir -p build

find_cxx() {
    if [ -n "$CXX" ]; then echo "$CXX"; return; fi
    if command -v clang++ >/dev/null 2>&1; then echo "clang++"; return; fi
    for c in "C:/msys64/clang64/bin/clang++.exe" \
             "C:/msys2/clang64/bin/clang++.exe" \
             "C:/Program Files/LLVM/bin/clang++.exe"; do
        if [ -x "$c" ]; then echo "$c"; return; fi
    done
    echo "clang++"
}

CXX_BIN="$(find_cxx)"
echo "[build] C++ = $CXX_BIN"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        "$CXX_BIN" -std=c++17 -O2 -o build/xccsysroot.exe tools/xccsysroot.cpp \
            -lwinhttp -lz -llzma
        "$CXX_BIN" -std=c++17 -O2 -o build/xccverify.exe tools/xccverify.cpp
        "$CXX_BIN" -std=c++17 -O2 -o build/xccrelease.exe tools/xccrelease.cpp \
            -lwinhttp -lz -llzma
        ;;
    Darwin)
        XZ_PREFIX="$(brew --prefix xz 2>/dev/null || true)"
        XZ_FLAGS=""
        if [ -n "$XZ_PREFIX" ]; then
            XZ_FLAGS="-I$XZ_PREFIX/include -L$XZ_PREFIX/lib"
        fi
        # shellcheck disable=SC2086
        "$CXX_BIN" -std=c++17 -O2 $XZ_FLAGS -o build/xccsysroot tools/xccsysroot.cpp \
            -lcurl -lz -llzma
        "$CXX_BIN" -std=c++17 -O2 -o build/xccverify tools/xccverify.cpp -pthread
        "$CXX_BIN" -std=c++17 -O2 $XZ_FLAGS -o build/xccrelease tools/xccrelease.cpp \
            -lcurl -lz -llzma -pthread
        ;;
    *)
        "$CXX_BIN" -std=c++17 -O2 -o build/xccsysroot tools/xccsysroot.cpp \
            -lcurl -lz -llzma
        "$CXX_BIN" -std=c++17 -O2 -o build/xccverify tools/xccverify.cpp -pthread
        "$CXX_BIN" -std=c++17 -O2 -o build/xccrelease tools/xccrelease.cpp \
            -lcurl -lz -llzma -pthread
        ;;
esac

echo "[build] 完成"
