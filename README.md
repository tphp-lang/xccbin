# xcc — 类 zig cc 的一站式交叉编译工具链

一条命令交叉编译 C/C++，开箱即用。单文件原生 C++ 驱动，无 Python、无运行时依赖：

```
xcc   -target aarch64-linux-musl  hello.c   -o hello
xcc++ -target x86_64-w64-mingw32  vector.cpp -o vector.exe
xcc   -target wasm32-wasi         hello.c   -o hello.wasm
```

## 特性

- **6 个 target**：aarch64/x86_64/riscv64 × linux-musl（全静态）、x86_64/i686 × windows-mingw、wasm32-wasi
- **C 与 C++**：libstdc++（ELF/PE）/ libc++（wasm）随 sysroot 分发；wasm 的 C++ 为 no-exceptions 语义（与 wasi-sdk 官方 libc++abi 一致）
- **原生驱动**：`xcc.cpp` 单文件 C++17，全静态编译约 900KB；argv[0] 多路复用出 `xcc` / `xcc++` / `xcc-ar` / `xcc-nm` / `xcc-objcopy` / `xcc-strip` / `xcc-ranlib` 等全部角色
- **开箱即用**：发行包自带 clang/lld/llvm-binutils + 全部 sysroot，解压即用，不依赖宿主环境
- **静态优先**：musl 目标默认 `-static`，产出完全静态 ELF，扔到任何同架构 Linux 都能跑；mingw 产物动态链系统 msvcrt.dll（Windows 自带），libgcc/libstdc++ 静态不拖 DLL
- **CI 多 host 打包**：`.github/workflows/build.yml` 矩阵产出 win64 / linux-x64 / linux-arm64 / macos-arm64 四种发行包，每个 host 都跑全量回归 + 包内冒烟（Windows 上 mingw 产物直接真机运行）

## 支持矩阵

| target | 产物 | 默认链接 | sysroot 体积 |
|---|---|---|---|
| `x86_64-linux-musl` | 静态 ELF64 | `-static` | 43 MB |
| `aarch64-linux-musl` | 静态 ELF64 | `-static` | 45 MB |
| `riscv64-linux-musl` | 静态 ELF64 | `-static` | 70 MB |
| `x86_64-w64-mingw32` | PE64 exe | 动态 msvcrt + 静态 libgcc/libstdc++ | 365 MB |
| `i686-w64-mingw32` | PE32 exe | 同上 | ~300 MB |
| `wasm32-wasi` | wasm32 模块 | 静态 | 86 MB |

任意 host（win/linux/macos）的发行包都能编译以上全部目标。

## 目录结构

```
xcc.cpp              原生驱动（单文件 C++17，全静态编译）
tools/
  build_sysroot.py   sysroot 采集器（Debian deb / Alpine apk / wasi-sdk tarball，纯 Python 解包）
  make_release.py    打包脚本（底座 clang++ 编译驱动 + LLVM 底座 + sysroot + zip）
  verify.py          真实验证（ELF/PE/WASM 架构判定、静态自洽、指令集嗅探、C++ 符号、PE 真机运行）
  probe_alpine.py    Alpine 仓库索引探测
sysroot/<target>/    已采集 sysroot（含 xcc.json manifest）
tests/               hello.c / vector.cpp / plain.cpp 测试样例
dist/xcc-<host>.zip  自包含发行包
cache/               下载缓存（不进 git）
```

## 素材来源（全部 CI/本地下载，不进仓库）

| target 家族 | 头文件/libc | gcc 运行时 | C++ |
|---|---|---|---|
| `*-linux-musl` | Alpine `musl-dev` `musl` | Alpine `libgcc-static` `gcc`(crtbegin/eh) | Alpine `libstdc++-dev` |
| `*-w64-mingw32` | Debian `mingw-w64-common` + `mingw-w64-<arch>-dev` | Debian `gcc-mingw-w64-<arch>-posix` | Debian `g++-mingw-w64-<arch>-posix` |
| `wasm32-wasi` | wasi-sdk `wasi-sysroot-25.0` 官方预编译 | Debian `libclang-rt-22-dev-wasm32`(builtins ~100KB) | sysroot 自带 `libc++.a`/`libc++abi.a` |

镜像：Debian 走 `deb.debian.org`，Alpine 走 `mirrors.aliyun.com`，GitHub 走 `gh-proxy.com` 加速（可回退直连）。

## 使用

```
# 编译（musl 默认静态）
xcc -target aarch64-linux-musl hello.c -o hello

# C++（驱动模式由 argv[0] 或 --role 决定）
xcc++ -target x86_64-w64-mingw32 vector.cpp -o vector.exe
xcc --role=c++ -target wasm32-wasi vector.cpp -o vector.wasm

# binutils
xcc-ar rcs libfoo.a foo.o
xcc-nm hello
xcc-objcopy -O binary hello hello.bin

# 查询
xcc -print-targets
xcc --version
```

环境变量：`XCC_TARGET`（默认 target）、`XCC_DEBUG=1`（打印底层 clang 命令）。

## 新增 target 步骤

1. `tools/build_sysroot.py` 的 `RECIPES` 加一行（alpine/deb 架构 + triplet + family）
2. `xcc.cpp` 的 `TARGETS` 加一行
3. `python tools/build_sysroot.py <target>` 采集
4. `python tools/verify.py --all` 全量回归
5. `tools/make_release.py` 重打包

## 验证哲学

不做"声称成功"：`verify.py` 用 llvm-readobj/nm/objdump 检查产物架构
（ELF Machine / PE IMAGE_FILE_MACHINE / WASM Format）、静态自洽（强未定义符号为零）、
目标指令集嗅探、C++ 运行时符号；mingw 产物在 Windows 宿主上直接真机运行。

## CI 打包

`.github/workflows/build.yml` 矩阵（4 个 host，驱动均为底座 clang++ 现场编译的静态原生二进制）：

| host | LLVM 底座 |
|---|---|
| windows-x86_64 | `--fetch-llvm` 拉官方 clang+llvm-22.1.8-msvc 包 |
| linux-x64 / linux-arm64 | apt `clang lld llvm`（`XCC_LLVM_DIR=/usr/lib/llvm-<v>`） |
| macos-arm64 | brew `llvm lld`（`XCC_LLVM_DIR=$(brew --prefix llvm)`） |

注意：LLVM 官方 22.x 只发 Windows 预编译资产，Linux/macOS 底座必须来自发行版包。
push tag `v*` 时各 host 的 zip 自动挂到 GitHub Release。

## 已知边界

- 底座 clang 随包分发（Windows 为官方包，Linux/macOS 为发行版包）
- 非 Windows target 缺 compiler-rt builtins 时自动回退 `-rtlib=libgcc -unwindlib=libgcc`；
  wasm builtins 由采集器放进 sysroot、打包时预置进 clang 资源目录
- **wasm C++ 为 no-exceptions 语义**：wasi-sdk 官方 libc++abi 即 no-exception 构建，
  驱动对 wasm C++ 自动加 `-fno-exceptions`（throw 路径退化为 abort）
- 无 glibc 动态目标：Linux 产物全部 musl 静态（部署零依赖）；需要 glibc 兼容模式时
  按"新增 target 步骤"把 glibc 配方加回 `RECIPES`（历史版本有现成实现）
- macOS 目标受 Xcode SDK 许可限制，暂不支持
