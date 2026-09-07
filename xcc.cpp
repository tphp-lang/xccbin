// xcc.cpp — xcc 原生驱动（单文件 C++17，无第三方依赖）
//
//    xcc   -target aarch64-linux-musl hello.c   -o hello
//    xcc++ -target wasm32-wasi hello.cpp        -o hello
//    xcc -print-targets
//
// 角色由 argv[0] 决定（多副本复用同一份代码）：
//    xcc  xcc++  xcc-gcc  xcc-ar  xcc-ranlib  xcc-nm  xcc-objcopy  xcc-strip ...
//
// 布局：
//    安装版  <root>/bin/xcc.exe + <root>/lib/llvm + <root>/sysroot/<target>
//    开发版  <root>/xcc.exe     + <root>/tools/llvm + <root>/sysroot/<target>
//
// 平台：Windows (CreateProcessW, UTF-16 命令行) / POSIX (fork+execvp)。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <filesystem>
namespace fs = std::filesystem;

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#ifdef _WIN32
#  define EXE_EXT ".exe"
#else
#  define EXE_EXT ""
#endif

static const char* XCC_VERSION = "0.3.0";

// --------------------------------------------------------------------------
// 编码辅助：内部统一 UTF-8；Windows 与 fs::path / 进程 API 交界处转 UTF-16。
// --------------------------------------------------------------------------
#ifdef _WIN32
static std::wstring u8w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wu8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

static fs::path upath(const std::string& s) {   // UTF-8 string -> path
#ifdef _WIN32
    return fs::path(u8w(s));
#else
    return fs::path(s);
#endif
}
static std::string pathstr(const fs::path& p) { // path -> UTF-8 string
#ifdef _WIN32
    return wu8(p.wstring());
#else
    return p.string();
#endif
}

static void die(const std::string& msg, int code = 1) {
    std::fprintf(stderr, "xcc: error: %s\n", msg.c_str());
    std::exit(code);
}

// --------------------------------------------------------------------------
// 进程执行
// --------------------------------------------------------------------------
struct ProcResult {
    int code = -1;
    std::string out;      // stdout+stderr 合并
    bool spawned = false;
};

#ifdef _WIN32

// Windows 命令行引号规则（MSVCRT 约定）
static std::wstring quote_arg(const std::string& a8) {
    std::wstring a = u8w(a8);
    if (a.find_first_of(L" \t\"") == std::wstring::npos && !a.empty())
        return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') {
            q.append(bs * 2 + 1, L'\\');
            q += L'"';
        } else {
            if (bs) q.append(bs, L'\\');
            q += c;
        }
        bs = 0;
    }
    if (bs) q.append(bs * 2, L'\\');
    q += L'"';
    return q;
}

static std::wstring build_cmdline(const std::vector<std::string>& cmd) {
    std::wstring s;
    for (size_t i = 0; i < cmd.size(); i++) {
        if (i) s += L' ';
        s += quote_arg(cmd[i]);
    }
    return s;
}

static ProcResult spawn_win(const std::vector<std::string>& cmd, bool capture) {
    ProcResult r;
    std::wstring cl = build_cmdline(cmd);

    // 可执行文件路径解析交给 CreateProcess（对带空格路径已由引号保护）
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    HANDLE rd = nullptr, wr = nullptr;
    if (capture) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        if (!CreatePipe(&rd, &wr, &sa, 0))
            die("CreatePipe 失败");
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::vector<wchar_t> buf(cl.begin(), cl.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                             capture ? TRUE : FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (capture && wr) { CloseHandle(wr); CloseHandle(rd); }
        return r;                      // spawned=false
    }
    if (capture && wr) CloseHandle(wr);

    if (capture) {
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n)
            r.out.append(chunk, n);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = (int)code;
    r.spawned = true;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

#else  // POSIX

static ProcResult spawn_posix(const std::vector<std::string>& cmd, bool capture) {
    ProcResult r;
    std::vector<char*> argv;
    for (auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    int pipefd[2] = {-1, -1};
    if (capture && pipe(pipefd) != 0)
        die("pipe 失败");

    pid_t pid = fork();
    if (pid < 0) {
        if (capture) { close(pipefd[0]); close(pipefd[1]); }
        return r;
    }
    if (pid == 0) {
        if (capture) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (capture) {
        close(pipefd[1]);
        char chunk[4096];
        ssize_t n;
        while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0)
            r.out.append(chunk, n);
        close(pipefd[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { /* EINTR 重试 */ }
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    r.spawned = true;
    return r;
}

#endif

static ProcResult run_capture(const std::vector<std::string>& cmd) {
#ifdef _WIN32
    return spawn_win(cmd, true);
#else
    return spawn_posix(cmd, true);
#endif
}

static int run_inherit(const std::vector<std::string>& cmd) {
#ifdef _WIN32
    ProcResult r = spawn_win(cmd, false);
#else
    ProcResult r = spawn_posix(cmd, false);
#endif
    if (!r.spawned) {
        std::string p = cmd[0];
        die("执行失败: " + p + " (errno 提示：文件不存在或权限不足)", 127);
    }
    return r.code;
}

// --------------------------------------------------------------------------
// 迷你 JSON 解析（manifest 专用：顶层对象 + 字符串/布尔/字符串数组）
// --------------------------------------------------------------------------
struct JParser {
    const std::string& s;
    size_t i = 0;
    explicit JParser(const std::string& src) : s(src) {}

    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i]==c) { i++; return true; } return false; }
    char peek() { ws(); return i < s.size() ? s[i] : '\0'; }

    std::string string_value() {          // 前置：peek()=='"'
        eat('"');
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '/': out += '/'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case 'u': {                    // \uXXXX —— 仅取 BMP，够 manifest 用
                        if (i + 4 <= s.size()) {
                            unsigned v = 0;
                            for (int k = 0; k < 4; k++) {
                                char h = s[i++];
                                v <<= 4;
                                if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                                else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                            }
                            // UTF-8 编码
                            if (v < 0x80) out += char(v);
                            else if (v < 0x800) {
                                out += char(0xC0 | (v >> 6));
                                out += char(0x80 | (v & 0x3F));
                            } else {
                                out += char(0xE0 | (v >> 12));
                                out += char(0x80 | ((v >> 6) & 0x3F));
                                out += char(0x80 | (v & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        eat('"');
        return out;
    }

    void skip_value() {                   // 通用值跳过（对象/数组/串/字面量）
        char c = peek();
        if (c == '"') { string_value(); return; }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']';
            eat(open);
            if (peek() == close) { eat(close); return; }
            while (true) {
                if (open == '{') { string_value(); eat(':'); }
                skip_value();
                if (eat(',')) continue;
                eat(close);
                break;
            }
            return;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
    }

    bool parse_bool() {
        ws();
        if (s.compare(i, 4, "true") == 0)  { i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; return false; }
        return false;
    }
};

struct Manifest {
    std::string cxx_include;
    std::string cxx_triplet_include;
    std::vector<std::string> lib_dirs;
    bool cxx_autodetect = false;
    bool present = false;
};

static Manifest load_manifest(const fs::path& sysroot) {
    Manifest m;
    fs::path p = sysroot / "xcc.json";
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return m;
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    JParser j(src);
    if (j.peek() != '{') return m;
    j.eat('{');
    while (true) {
        if (j.peek() == '}') { j.eat('}'); break; }
        std::string key = j.string_value();
        j.eat(':');
        if      (key == "cxx_include")         m.cxx_include = j.string_value();
        else if (key == "cxx_triplet_include") m.cxx_triplet_include = j.string_value();
        else if (key == "cxx_autodetect")      m.cxx_autodetect = j.parse_bool();
        else if (key == "lib_dirs") {
            j.eat('[');
            while (j.peek() != ']') {
                m.lib_dirs.push_back(j.string_value());
                if (!j.eat(',')) break;
            }
            j.eat(']');
        }
        else j.skip_value();
        if (j.eat(',')) continue;
        j.eat('}');
        break;
    }
    m.present = true;
    return m;
}

// --------------------------------------------------------------------------
// target 定义表
//
//   clang  : 传给 clang 的 --target 字符串
//   family : musl / wasi（windows/mingw 家族已移除）
//   statik : 默认全静态（musl 招牌能力；wasi 天然全静态）
//   pie    : 默认生成 PIE
// --------------------------------------------------------------------------
struct TargetSpec {
    const char* name;
    const char* clang;
    const char* family;
    bool statik;
    bool pie;
};

static const TargetSpec TARGETS[] = {
    {"aarch64-linux-musl",  "aarch64-linux-musl",  "musl",    true,  false},
    {"x86_64-linux-musl",   "x86_64-linux-musl",   "musl",    true,  false},
    {"riscv64-linux-musl",  "riscv64-linux-musl",  "musl",    true,  false},
    {"wasm32-wasi",         "wasm32-wasi",         "wasi",    true,  false},
};

static const TargetSpec* find_target(const std::string& name) {
    for (const auto& t : TARGETS)
        if (name == t.name) return &t;
    return nullptr;
}

// 角色 -> llvm 工具（binutils 类直接透传）
struct RoleTool { const char* role; const char* tool; };
static const RoleTool BINUTILS[] = {
    {"ar", "llvm-ar"}, {"ranlib", "llvm-ranlib"}, {"nm", "llvm-nm"},
    {"objcopy", "llvm-objcopy"}, {"strip", "llvm-strip"}, {"dlltool", "llvm-dlltool"},
    {"readobj", "llvm-readobj"}, {"objdump", "llvm-objdump"},
};

static bool has_flag(const std::vector<std::string>& args,
                     std::initializer_list<const char*> names) {
    for (auto& a : args)
        for (const char* n : names)
            if (a == n) return true;
    return false;
}

static bool is_link_step(const std::vector<std::string>& args) {
    return !has_flag(args, {"-c", "-S", "-E", "-fsyntax-only"});
}

// --------------------------------------------------------------------------
// 路径探测
// --------------------------------------------------------------------------
static fs::path self_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf);
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;
    return fs::path();   // 回退 argv[0]
#endif
}

static fs::path find_root(const std::string& argv0) {
    fs::path here = self_exe_path();
    if (here.empty() || !fs::exists(here))
        here = fs::absolute(upath(argv0));
    fs::path parent = here.parent_path().parent_path();
    std::error_code ec;
    if (fs::is_directory(parent / "sysroot", ec) ||
        fs::is_directory(parent / "lib" / "llvm", ec))
        return parent;
    // 开发布局：<root>/xcc.exe
    return here.parent_path();
}

static fs::path find_llvm(const fs::path& root) {
    for (const char* cand : {"lib/llvm", "tools/llvm"}) {
        fs::path p = root / cand / "bin";
        std::error_code ec;
        if (fs::is_directory(p, ec)) return root / cand;
    }
    return fs::path();
}

static fs::path find_clang(const fs::path& llvm) {
    if (!llvm.empty()) {
        fs::path p = llvm / "bin" / "clang"
#ifdef _WIN32
            ".exe"
#endif
            ;
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return fs::path();   // 交由调用方回退 PATH（clang 在 PATH 里也能跑）
}

// which 的朴素实现（POSIX 与 Windows 各自 PATH 扫描）
static std::string which(const std::string& name) {
#ifdef _WIN32
    const char* sep = ";";
    std::string exts[] = {"", ".exe", ".cmd", ".bat"};
#else
    const char* sep = ":";
    std::string exts[] = {""};
#endif
    const char* penv = std::getenv("PATH");
    if (!penv) return "";
    std::stringstream ss(penv);
    std::string dir;
    while (std::getline(ss, dir, *sep)) {
        if (dir.empty()) continue;
        for (auto& ext : exts) {
            fs::path p = upath(dir) / upath(name + ext);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return pathstr(p);
        }
    }
    return "";
}

static std::string resolve_tool(const std::string& name) {
    std::string w = which(name);
    if (!w.empty()) return w;
    for (int v = 14; v <= 22; v++) {          // 兼容 llvm-nm-18 等版本化命名
        w = which(name + "-" + std::to_string(v));
        if (!w.empty()) return w;
    }
    return "";
}

// --------------------------------------------------------------------------
// 命令构造
// --------------------------------------------------------------------------
static bool has_compiler_rt(const std::string& clang, const std::string& clang_target) {
    // clang 找不到时 -print-file-name 会原样回显文件名（非绝对路径），据此判断。
    ProcResult r = run_capture({clang, "--target=" + clang_target,
                                "-print-file-name=libclang_rt.builtins.a"});
    if (!r.spawned) return false;
    std::string out = r.out;
    // 去尾随空白
    while (!out.empty() && (out.back()=='\n'||out.back()=='\r'||out.back()==' ')) out.pop_back();
    if (out.empty()) return false;
#ifdef _WIN32
    bool absolute = out.size() > 2 && out[1] == ':';
#else
    bool absolute = !out.empty() && out[0] == '/';
#endif
    if (!absolute) return false;
    std::error_code ec;
    return fs::is_regular_file(upath(out), ec);
}

static void ensure_wasm_builtins(const std::string& clang, const fs::path& sysroot) {
    // clang 链接 wasm 时硬性要求资源目录里有 lib/wasm32-unknown-wasi/libclang_rt.builtins.a。
    // sysroot 自带（采集器从 Debian builtins 包获取）；首次链接时落位一次即可。
    ProcResult r = run_capture({clang, "-print-resource-dir"});
    if (!r.spawned) return;
    std::string res = r.out;
    while (!res.empty() && (res.back()=='\n'||res.back()=='\r'||res.back()==' ')) res.pop_back();
    if (res.empty()) return;
    fs::path dst = upath(res) / "lib" / "wasm32-unknown-wasi" / "libclang_rt.builtins.a";
    std::error_code ec;
    if (fs::is_regular_file(dst, ec)) return;
    fs::path src = sysroot / "lib" / "wasm32-wasi" / "libclang_rt.builtins-wasm32.a";
    if (!fs::is_regular_file(src, ec)) return;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec)
        std::fprintf(stderr, "xcc: 已安装 wasm builtins -> %s\n", pathstr(dst).c_str());
}

// 头文件目录存在才注入 isystem
static void add_isystem(std::vector<std::string>& cmd, const fs::path& sysroot,
                        const std::string& rel) {
    if (rel.empty()) return;
    fs::path d = sysroot / upath(rel);
    std::error_code ec;
    if (fs::is_directory(d, ec))
        cmd.push_back("-isystem" + pathstr(d));
}

static std::vector<std::string> build_command(const std::string& role,
                                              const std::string& target_name,
                                              const std::vector<std::string>& args,
                                              const fs::path& root,
                                              const std::string& clang) {
    std::vector<std::string> cmd;
    cmd.push_back(clang);

    if (role == "c++")
        cmd.push_back("--driver-mode=g++");

    if (target_name.empty()) {
        cmd.insert(cmd.end(), args.begin(), args.end());   // 原生编译，透传
        return cmd;
    }

    const TargetSpec* spec = find_target(target_name);
    if (!spec)
        die("未知 target: " + target_name + "\n可用 target 见 xcc -print-targets");

    fs::path sysroot = root / "sysroot" / target_name;
    std::error_code ec;
    if (!fs::is_directory(sysroot, ec))
        die("target '" + target_name + "' 的 sysroot 未采集：" + pathstr(sysroot) +
            "\n请先运行 ./build/xccsysroot " + target_name);

    Manifest mf = load_manifest(sysroot);
    std::string family = spec->family;
    cmd.push_back("--target=" + std::string(spec->clang));
    cmd.push_back("--sysroot=" + pathstr(sysroot));

    // C++ 标准库：
    //   wasi  用自带 libc++（no-exceptions 构建，用户代码须 -fno-exceptions）；
    //   glibc 走 Debian 标准布局，clang 的 GCCInstallationDetector 自动找 libstdc++；
    //   musl 显式注入 sysroot 内的 C++ 头（manifest.cxx_autodetect=false）。
    if (role == "c++") {
        if (family == "wasi") {
            cmd.push_back("-stdlib=libc++");
            cmd.push_back("-nostdinc++");
            cmd.push_back("-fno-exceptions");
            add_isystem(cmd, sysroot, mf.cxx_include);
        } else {
            cmd.push_back("-stdlib=libstdc++");
            if (!mf.cxx_autodetect) {
                cmd.push_back("-nostdinc++");
                add_isystem(cmd, sysroot, mf.cxx_include);
                add_isystem(cmd, sysroot, mf.cxx_triplet_include);
            }
        }
    }

    if (is_link_step(args)) {
        if (family == "wasi")
            ensure_wasm_builtins(clang, sysroot);
        else
            cmd.push_back("-fuse-ld=lld");

        // 库搜索路径：manifest 声明的多架构库目录，兜底 sysroot/lib
        for (auto& d : mf.lib_dirs) {
            fs::path dd = sysroot / upath(d);
            if (fs::is_directory(dd, ec))
                cmd.push_back("-L" + pathstr(dd));
        }
        cmd.push_back("-L" + pathstr(sysroot / "lib"));

        if (family == "musl" || family == "glibc") {
            // 运行时库：底座带该 target 的 compiler-rt 就用，否则回退 sysroot 的 libgcc
            bool user_rtlib = false;
            for (auto& a : args) user_rtlib = user_rtlib || a.rfind("-rtlib", 0) == 0;
            if (!user_rtlib) {
                if (has_compiler_rt(clang, spec->clang)) {
                    cmd.push_back("-rtlib=compiler-rt");
                    cmd.push_back("-unwindlib=libunwind");
                } else {
                    cmd.push_back("-rtlib=libgcc");
                    cmd.push_back("-unwindlib=libgcc");
                }
            }
        }
        // 默认静态/PIE：musl 默认全静态；glibc 默认 PIE；wasi 天然全静态。
        bool nostdlib = has_flag(args, {"-nostdlib"});
        if (spec->statik && family != "wasi" &&
            !has_flag(args, {"-shared", "-static", "-nostdlib"}))
            cmd.push_back("-static");
        if (spec->pie && !has_flag(args, {"-shared", "-static", "-no-pie", "-nopie"}))
            cmd.push_back("-pie");

        cmd.push_back("-Qunused-arguments");
    }

    cmd.insert(cmd.end(), args.begin(), args.end());

    // wasi C++：libc++abi 必须在目标文件之后按序拉取
    if (role == "c++" && family == "wasi" && is_link_step(args) &&
        !has_flag(args, {"-nostdlib"}))
        cmd.push_back("-lc++abi");

    return cmd;
}

// --------------------------------------------------------------------------
// 角色与入口
// --------------------------------------------------------------------------
static std::string detect_role(const std::string& argv0) {
    std::string stem = upath(argv0).filename().string();
    // 剥离后缀：xcc++.exe / xcc++.py / xcc.cmd 都应识别为 xcc++
    for (const char* suf : {".exe", ".py", ".cmd", ".bat"}) {
        size_t n = std::strlen(suf);
        if (stem.size() >= n) {
            std::string tail = stem.substr(stem.size() - n);
            std::string low = tail;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            if (low == suf) { stem.resize(stem.size() - n); break; }
        }
    }
    if (stem == "xcc" || stem == "xcc-cc" || stem == "xcc-gcc") return "cc";
    if (stem == "xcc++" || stem == "xcc-c++" || stem == "xcc-g++") return "c++";
    for (auto& rt : BINUTILS)
        if (std::string("xcc-") + rt.role == stem) return rt.role;
    return "cc";
}

static void print_targets(const fs::path& root) {
    std::printf("xcc %s\n", XCC_VERSION);
    std::printf("已注册 target:\n");
    size_t width = 0;
    for (auto& t : TARGETS) width = std::max(width, std::string(t.name).size());
    for (auto& t : TARGETS) {
        std::error_code ec;
        bool ok = fs::is_directory(root / "sysroot" / t.name, ec);
        std::printf("  %-*s  %-8s  %s\n", (int)width, t.name, t.family,
                    ok ? "已安装" : "未采集 (sysroot/<target> 不存在)");
    }
}

static void split_target(const std::vector<std::string>& in,
                         std::string& target, std::vector<std::string>& rest) {
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if (a == "-target" || a == "--target") {
            if (i + 1 >= in.size()) die("-target 缺少参数");
            target = in[++i];
            continue;
        }
        if (a.rfind("--target=", 0) == 0) { target = a.substr(9); continue; }
        if (a.rfind("-target=", 0) == 0)  { target = a.substr(8); continue; }
        rest.push_back(a);
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> av;
#ifdef _WIN32
    (void)argv; (void)argc;
    int wn = 0;
    LPWSTR* wz = CommandLineToArgvW(GetCommandLineW(), &wn);
    if (!wz) die("CommandLineToArgvW 失败");
    for (int i = 0; i < wn; i++) av.push_back(wu8(wz[i]));
    LocalFree(wz);
#else
    for (int i = 0; i < argc; i++) av.push_back(argv[i]);
#endif
    if (av.empty()) return 1;

    if (av.size() > 1 && (av[1] == "-print-targets" || av[1] == "--print-targets")) {
        print_targets(find_root(av[0]));
        return 0;
    }
    if (av.size() > 1 && (av[1] == "--version" || av[1] == "-v")) {
        std::printf("xcc %s\n", XCC_VERSION);
        return 0;
    }

    // --role= 显式角色覆盖（单源真值，便于脚本调用）
    std::string force_role;
    std::vector<std::string> cleaned;
    for (size_t i = 1; i < av.size(); i++) {
        if (av[i].rfind("--role=", 0) == 0) force_role = av[i].substr(7);
        else cleaned.push_back(av[i]);
    }

    fs::path root = find_root(av[0]);
    fs::path llvm = find_llvm(root);
    std::string role = !force_role.empty() ? force_role : detect_role(av[0]);

    std::string target_name;
    std::vector<std::string> args;
    split_target(cleaned, target_name, args);
    if (target_name.empty()) {
        const char* env = std::getenv("XCC_TARGET");
        if (env && *env) target_name = env;
    }

    std::vector<std::string> cmd;
    bool is_binutils = false;
    std::string tool;
    for (auto& rt : BINUTILS)
        if (rt.role == role) { is_binutils = true; tool = rt.tool; break; }

    if (is_binutils) {
        std::string exe;
        if (!llvm.empty()) {
            fs::path p = llvm / "bin" / (tool + std::string(EXE_EXT));
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) exe = pathstr(p);
        }
        if (exe.empty()) {
            exe = resolve_tool(tool);
            if (exe.empty()) die("找不到 " + tool);
        }
        cmd.push_back(exe);
        cmd.insert(cmd.end(), args.begin(), args.end());
    } else {
        fs::path clangp = find_clang(llvm);
        std::string clang = !clangp.empty() ? pathstr(clangp) : resolve_tool("clang");
        if (clang.empty())
            die("找不到 clang，请检查 LLVM 底座（<root>/lib/llvm）");
        cmd = build_command(role, target_name, args, root, clang);
    }

    if (const char* dbg = std::getenv("XCC_DEBUG"); dbg && *dbg) {
        std::string line = "xcc:";
        for (auto& c : cmd) {
            line += ' ';
            if (c.find(' ') != std::string::npos) line += '"' + c + '"';
            else line += c;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    return run_inherit(cmd);
}
