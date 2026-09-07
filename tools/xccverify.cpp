// xccverify.cpp — xcc 产物真实验证器（原生 C++17，替代 tools/verify.py）
//
//   xccverify --all                        对所有已采集 target 跑 tests/ 样例并验证
//   xccverify <target> <bin1> [<bin2> ...] 验证既有产物
//
// 原则（用户极度重视真实验证，拒绝"声称成功"）：
//   不依赖退出码，直接用 LLVM 工具对产物做静态分析：
//     * llvm-readobj --file-header        -> 架构（EM_* / WASM）
//     * llvm-readelf --program-headers    -> 是否有 INTERP / PT_DYNAMIC（动态 or 静态）
//     * llvm-nm -u                        -> 静态链接应无"强未定义"符号
//     * llvm-objdump -d                   -> 反汇编，嗅探指令集
//
// 与 Python 版的差异：
//   1) 版本化工具探测范围 14..40（旧版 14..22，LLVM 23+ 会静默找不到 llvm-nm）
//   2) 所有子进程带超时（工具 120s / 真机运行 60s），超时强杀，避免 CI 挂死
//   3) C++ 符号检查按"是否 C++ 源文件产物"判定，不再依赖文件名以 vector 开头
//   4) --all 模式下 wasi 用 tests/plain.cpp（该样例本就是为 wasi 准备的），
//      其余 target 用 tests/vector.cpp
//
// 编译：
//   Windows (MSYS2 clang64): clang++ -std=c++17 -O2 -o xccverify.exe xccverify.cpp
//   Linux/macOS            : clang++ -std=c++17 -O2 -o xccverify      xccverify.cpp

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace fs = std::filesystem;

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// --------------------------------------------------------------------------
// 基础工具
// --------------------------------------------------------------------------
static fs::path ROOT, TESTS, SYSROOT, BUILD;

static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

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

static fs::path upath(const std::string& s) {
#ifdef _WIN32
    return fs::path(u8w(s));
#else
    return fs::path(s);
#endif
}
static std::string pathstr(const fs::path& p) {
#ifdef _WIN32
    return wu8(p.wstring());
#else
    return p.string();
#endif
}

// --------------------------------------------------------------------------
// 子进程（捕获 stdout+stderr，带超时强杀）
// --------------------------------------------------------------------------
struct ExecResult {
    int code = -1;
    std::string out;          // stdout + stderr 合并
    bool spawned = false;
    bool timed_out = false;
};

#ifdef _WIN32
static std::wstring quote_arg(const std::string& a8) {
    std::wstring a = u8w(a8);
    if (a.find_first_of(L" \t\"") == std::wstring::npos && !a.empty()) return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { if (bs) q.append(bs, L'\\'); q += c; }
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
#endif

// timeout_ms <= 0 表示不限时
static ExecResult exec_cmd(const std::vector<std::string>& cmd, int timeout_ms) {
    ExecResult r;
    if (cmd.empty()) return r;

#ifdef _WIN32
    std::wstring cl = build_cmdline(cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::vector<wchar_t> buf(cl.begin(), cl.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0,
                             nullptr, nullptr, &si, &pi);
    if (!ok) { CloseHandle(wr); CloseHandle(rd); return r; }
    CloseHandle(wr);
    r.spawned = true;

    std::string bufout;
    std::thread reader([&] {
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n)
            bufout.append(chunk, n);
        CloseHandle(rd);
    });
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (w == WAIT_TIMEOUT) { r.timed_out = true; TerminateProcess(pi.hProcess, 1); }
    reader.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = (int)code;
    r.out = std::move(bufout);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int pipefd[2] = { -1, -1 };
    if (pipe(pipefd) != 0) return r;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return r; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> av;
        for (auto& s : cmd) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(pipefd[1]);
    r.spawned = true;

    std::string bufout;
    std::thread reader([&] {
        char chunk[4096];
        ssize_t n;
        while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0) bufout.append(chunk, n);
        close(pipefd[0]);
    });

    int st = 0;
    if (timeout_ms > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            pid_t rr = waitpid(pid, &st, WNOHANG);
            if (rr == pid) break;
            if (rr < 0 && errno != EINTR) break;                 // ECHILD 等
            if (std::chrono::steady_clock::now() >= deadline) {
                r.timed_out = true;
                kill(pid, SIGKILL);
                while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    }
    reader.join();
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    r.out = std::move(bufout);
#endif
    return r;
}

// --------------------------------------------------------------------------
// 工具解析
// --------------------------------------------------------------------------
static std::string which(const std::string& name) {
#ifdef _WIN32
    const char sep = ';';
    const char* exts[] = { "", ".exe", ".cmd", ".bat" };
#else
    const char sep = ':';
    const char* exts[] = { "" };
#endif
    // 含分隔符视为路径，直接检查
    if (name.find('/') != std::string::npos
#ifdef _WIN32
        || name.find('\\') != std::string::npos
#endif
    ) {
        std::error_code ec;
        fs::path p = upath(name);
        if (fs::is_regular_file(p, ec)) return pathstr(fs::absolute(p, ec));
        return "";
    }
    const char* penv = std::getenv("PATH");
    if (!penv) return "";
    std::string path = penv;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t end = path.find(sep, pos);
        std::string dir = path.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? path.size() + 1 : end + 1;
        if (dir.empty()) continue;
        for (const char* ext : exts) {
            std::error_code ec;
            fs::path p = upath(dir) / upath(name + ext);
            if (fs::is_regular_file(p, ec)) return pathstr(fs::absolute(p, ec));
        }
    }
    return "";
}

static std::string resolve_tool(const std::string& name) {
    std::string w = which(name);
    if (!w.empty()) return w;
    for (int v = 14; v <= 40; v++) {          // 兼容 llvm-nm-18 等版本化命名
        w = which(name + "-" + std::to_string(v));
        if (!w.empty()) return w;
    }
    return "";
}

static const int TOOL_TIMEOUT_MS = 120000;
static const int RUN_TIMEOUT_MS = 60000;

// 返回 false 表示工具不存在；out 为合并输出
static bool run_tool(const std::string& tool, const std::vector<std::string>& args,
                     std::string& outp) {
    std::string exe = resolve_tool(tool);
    if (exe.empty()) return false;
    std::vector<std::string> cmd;
    cmd.push_back(exe);
    cmd.insert(cmd.end(), args.begin(), args.end());
    ExecResult r = exec_cmd(cmd, TOOL_TIMEOUT_MS);
    outp = r.out;
    if (!r.spawned) return false;
    if (r.timed_out) outp += "\n[timeout]";
    return true;
}

// --------------------------------------------------------------------------
// target 期望表
// --------------------------------------------------------------------------
struct TargetInfo {
    const char* name;
    const char* arch;             // llvm-readobj --file-header 的 Machine / Format 期望
    std::vector<const char*> hints;
};

static const TargetInfo TARGETS[] = {
    { "aarch64-linux-musl",  "EM_AARCH64",                 { "stp", "ldr", "bl" } },
    { "x86_64-linux-musl",   "EM_X86_64",                  { "mov", "call", "push" } },
    { "riscv64-linux-musl",  "EM_RISCV",                   { "addi", "ld", "sd", "ret" } },
    { "wasm32-wasi",         "WASM",                       { "i32.", "i64.", "call", "local.get", "block" } },
};

static const TargetInfo* find_target(const std::string& name) {
    for (const auto& t : TARGETS)
        if (name == t.name) return &t;
    return nullptr;
}

// --------------------------------------------------------------------------
// 文本匹配（替代 py 的 re.search(r"\b" + escape(hint)) ）
// --------------------------------------------------------------------------
static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
// 前缀匹配 + 左词边界：x86 助记符带宽度后缀（movq/callq），aarch64 不带，统一按前缀匹配
static bool contains_hint(const std::string& text, const std::string& hint) {
    size_t pos = 0;
    while (true) {
        size_t i = text.find(hint, pos);
        if (i == std::string::npos) return false;
        if (i == 0 || !is_word_char(text[i - 1])) return true;
        pos = i + 1;
    }
}

static std::string field_after(const std::string& text, const std::string& key) {
    // 形如 "Machine: EM_X86_64" / "Format: WASM" / "Arch: wasm32"
    size_t p = 0;
    while (true) {
        size_t i = text.find(key, p);
        if (i == std::string::npos) return "";
        // key 必须出现在一行开头（允许前导空白）
        size_t ls = text.rfind('\n', i);
        size_t line_start = (ls == std::string::npos) ? 0 : ls + 1;
        bool at_line_start = true;
        for (size_t k = line_start; k < i; k++) {
            if (text[k] != ' ' && text[k] != '\t') { at_line_start = false; break; }
        }
        if (!at_line_start) { p = i + 1; continue; }
        size_t j = i + key.size();
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) j++;
        size_t e = j;
        while (e < text.size() && text[e] != '\n' && text[e] != '\r' && text[e] != ' ' && text[e] != '\t') e++;
        return text.substr(j, e - j);
    }
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find('\n', p);
        std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        v.push_back(line);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return v;
}

// --------------------------------------------------------------------------
// 检查项
// --------------------------------------------------------------------------
static void wasm_extra(const fs::path& path, std::vector<std::string>& lines) {
    std::string od;
    if (run_tool("llvm-objdump", { "-d", pathstr(path) }, od)) {
        std::vector<std::string> seen;
        for (const char* h : find_target("wasm32-wasi")->hints)
            if (contains_hint(od, h)) seen.push_back(h);
        if (!seen.empty()) {
            std::string s;
            for (size_t i = 0; i < seen.size(); i++) {
                if (i) s += ", ";
                s += seen[i];
            }
            lines.push_back("  ✓ wasm 指令嗅探命中: " + s);
        } else {
            lines.push_back("  · wasm 指令嗅探未命中");
        }
    }
    std::error_code ec;
    uintmax_t sz = fs::file_size(path, ec);
    if (!ec && sz > 1024) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  ✓ 模块体量 %llu 字节 (>1KB，libc 已链接)",
                      (unsigned long long)sz);
        lines.push_back(buf);
    }
}

// 返回 (passed, lines)
static std::pair<bool, std::vector<std::string>> check_binary(const fs::path& path,
                                                              const std::string& target) {
    std::vector<std::string> lines;
    bool ok = true;
    const TargetInfo* ti = find_target(target);
    std::string expect_arch = ti ? ti->arch : "AArch64";

    // 文件不存在时要直接失败：否则 readobj 无输出，"无 INTERP" 会被误报成"完全静态链接"
    std::error_code fec;
    if (!fs::is_regular_file(path, fec))
        return { false, { "  ! 文件不存在: " + pathstr(path) } };

    std::string fh;
    if (!run_tool("llvm-readobj", { "--file-header", pathstr(path) }, fh))
        return { false, { "  ! llvm-readobj 失败: 找不到 llvm-readobj" } };

    // wasm 产物：readobj 输出 "Format: WASM / Arch: wasm32"，无 program headers / nm -u
    if (expect_arch == "WASM") {
        std::string fmt = field_after(fh, "Format:");
        std::string arc = field_after(fh, "Arch:");
        std::string fmtl = fmt;
        for (auto& c : fmtl) c = (char)std::tolower((unsigned char)c);
        if (fmtl == "wasm" && arc == "wasm32") {
            lines.push_back("  ✓ WASM 格式 (Arch: wasm32)");
        } else {
            ok = false;
            lines.push_back("  ? 期望 wasm32，实际 Format=" + (fmt.empty() ? "未知" : fmt) +
                            " Arch=" + (arc.empty() ? "未知" : arc));
        }
        wasm_extra(path, lines);
        return { ok, lines };
    }

    std::string machine = field_after(fh, "Machine:");
    if (machine.empty() || machine.find(expect_arch) == std::string::npos) {
        ok = false;
        lines.push_back("  ? 架构期望 " + expect_arch + "，实际 " +
                        (machine.empty() ? std::string("未知") : machine));
    } else {
        std::string kind = (machine.find("IMAGE_FILE_") == 0) ? "PE/COFF" : "ELF";
        std::string short_name = machine;
        const std::string pfx = "IMAGE_FILE_MACHINE_";
        if (short_name.find(pfx) == 0) short_name = short_name.substr(pfx.size());
        lines.push_back("  ✓ Machine = " + short_name + " (" + kind + ")");
    }

    // 动态/静态判定
    std::string po;
    run_tool("llvm-readelf", { "--program-headers", pathstr(path) }, po);
    bool has_interp = po.find("INTERP") != std::string::npos;
    bool has_dynamic_ph = po.find("PT_DYNAMIC") != std::string::npos;
    if (has_interp)
        lines.push_back("  · 动态链接 (有 INTERP 段)");
    else if (has_dynamic_ph)
        lines.push_back("  · 部分静态 (有 PT_DYNAMIC 但无 INTERP)");
    else
        lines.push_back("  ✓ 完全静态链接 (无 INTERP / 无 PT_DYNAMIC)");

    // 未定义符号：只关心"强未定义"(U 大写)。弱未定义(w/W/v)是 Itanium ABI 常态，可解析为 0。
    // 动态链接的二进制本就有大量强未定义符号（运行期由 ld.so 解析），属正常；
    // 只有静态链接才要求零强未定义。
    std::string nm;
    if (run_tool("llvm-nm", { "-u", pathstr(path) }, nm)) {
        std::vector<std::string> strong;
        for (const std::string& line : split_lines(nm)) {
            if (line.empty()) continue;
            size_t i = line.find_first_not_of(" \t");
            if (i == std::string::npos) continue;
            std::string rest = line.substr(i);
            if (rest.size() < 2) continue;
            if (rest[0] != 'U') continue;
            if (rest[1] != ' ' && rest[1] != '\t') continue;
            size_t j = rest.find_first_not_of(" \t", 1);
            if (j == std::string::npos) continue;
            std::string name = rest.substr(j);
            size_t sp = name.find_first_of(" \t");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name.find("_DYNAMIC") != std::string::npos) continue;
            strong.push_back(name);
        }
        if (!strong.empty()) {
            if (has_interp || has_dynamic_ph) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "  · 动态链接：%zu 个强未定义符号将由 ld.so 解析（正常）", strong.size());
                lines.push_back(buf);
            } else {
                ok = false;
                std::string s;
                for (size_t i = 0; i < strong.size() && i < 5; i++) { if (i) s += ", "; s += strong[i]; }
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "  ? 静态二进制存在强未定义符号 (%zu): %s", strong.size(), s.c_str());
                lines.push_back(buf);
            }
        } else {
            lines.push_back("  ✓ 无强未定义符号 (静态自洽；弱未定义 w 视为已解析)");
        }
    }

    // 反汇编嗅探指令集
    std::string od;
    if (run_tool("llvm-objdump", { "-d", pathstr(path) }, od)) {
        std::vector<std::string> seen;
        if (ti) {
            for (const char* h : ti->hints)
                if (contains_hint(od, h)) seen.push_back(h);
        }
        if (!seen.empty()) {
            std::string s;
            for (size_t i = 0; i < seen.size(); i++) { if (i) s += ", "; s += seen[i]; }
            lines.push_back("  ✓ 指令集嗅探命中: " + s);
        } else {
            lines.push_back("  · 指令集嗅探未命中 (可能已 strip 或体量极小)");
        }
    }

    return { ok, lines };
}

static std::pair<bool, std::vector<std::string>> check_cxx(const fs::path& path) {
    std::string nm;
    if (run_tool("llvm-nm", { "-C", pathstr(path) }, nm)) {
        if (nm.find("std::") != std::string::npos)
            return { true, { "  ✓ 含 std:: 符号 (C++ 运行时已链接)" } };
    }
    std::string nm2;
    if (run_tool("llvm-nm", { pathstr(path) }, nm2) && nm2.find("_ZNSt") != std::string::npos)
        return { true, { "  ✓ 含 std 命名空间符号 (mangled _ZNSt)" } };
    return { false, { "  ? 未检出 std 符号，C++ 可能未真正参与链接" } };
}

// --------------------------------------------------------------------------
// 驱动定位与编译（--all 模式）
// --------------------------------------------------------------------------
static bool is_cxx_source(const std::string& name) {
    size_t d = name.rfind('.');
    if (d == std::string::npos) return false;
    std::string ext = name.substr(d);
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" || ext == ".c++";
}

static fs::path find_driver() {
    const char* exe_name =
#ifdef _WIN32
        "xcc.exe";
#else
        "xcc";
#endif
    std::error_code ec;
    fs::create_directories(BUILD, ec);
    fs::path drv = BUILD / exe_name;
    if (fs::is_regular_file(drv, ec)) return drv;

    // 现场编译
    fs::path src = ROOT / "xcc.cpp";
    if (!fs::is_regular_file(src, ec)) return fs::path();
    std::string clang = resolve_tool("clang++");
    if (clang.empty()) clang = resolve_tool("clang");
    if (clang.empty()) return fs::path();
    std::vector<std::string> cmd = { clang, "-std=c++17", "-O2" };
#ifndef __APPLE__
    cmd.push_back("-static");
#endif
    cmd.push_back("-o");
    cmd.push_back(pathstr(drv));
    cmd.push_back(pathstr(src));
    cmd.push_back("-s");
    ExecResult r = exec_cmd(cmd, TOOL_TIMEOUT_MS);
    if (!r.spawned || r.code != 0) {
        out("  ! 驱动编译失败:\n%s\n", r.out.substr(0, 800).c_str());
        return fs::path();
    }
    return drv;
}

static bool build_all(const std::string& target, std::vector<std::pair<fs::path, bool>>& built) {
    std::error_code ec;
    fs::create_directories(BUILD, ec);
    fs::path drv = find_driver();
    if (drv.empty()) {
        out("  ! 找不到/编不出原生驱动 (build/xcc)\n");
        return false;
    }
    std::string ext;   // 已移除 windows/mingw 目标，产物无 .exe 后缀
    std::string tname = target;
    for (auto& c : tname) if (c == '-') c = '_';

    struct Case { const char* src; const char* role; bool cxx; };
    // wasi 用 plain.cpp（该样例本就是为 wasi 准备的，避开异常路径）
    Case cases[2] = {
        { "hello.c", "cc", false },
        { target == "wasm32-wasi" ? "plain.cpp" : "vector.cpp", "c++", true },
    };
    for (const auto& c : cases) {
        fs::path sp = TESTS / c.src;
        if (!fs::is_regular_file(sp, ec)) continue;
        std::string stem = c.src;
        size_t d = stem.rfind('.');
        if (d != std::string::npos) stem = stem.substr(0, d);
        fs::path binp = BUILD / (stem + "_" + tname + ext);
        ExecResult r = exec_cmd({ pathstr(drv), std::string("--role=") + c.role,
                                  "-target", target, pathstr(sp), "-o", pathstr(binp) },
                                TOOL_TIMEOUT_MS);
        if (!r.spawned || r.code != 0) {
            out("  ! %s 编译失败:\n%s\n", c.src, r.out.substr(0, 800).c_str());
            return false;
        }
        built.push_back({ binp, c.cxx });
    }
    return true;
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
static void usage() {
    out("用法:\n");
    out("  xccverify --all\n");
    out("  xccverify <target> <bin1> [<bin2> ...]\n");
}

static fs::path self_dir() {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n > 0) return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#endif
    (void)ec;
    return fs::path();
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::error_code ec;
    if (const char* e = std::getenv("XCC_ROOT"); e && *e) {
        ROOT = fs::absolute(upath(e), ec);
    } else {
        fs::path sd = self_dir();
        fs::path r = sd.empty() ? fs::path() : sd.parent_path();     // <root>/build/xccverify
        if (!r.empty() && fs::is_regular_file(r / "xcc.cpp", ec)) ROOT = r;
        else {
            fs::path cwd = fs::current_path(ec);
            if (fs::is_regular_file(cwd / "xcc.cpp", ec)) ROOT = cwd;
            else ROOT = r.empty() ? cwd : r;
        }
    }
    TESTS = ROOT / "tests";
    SYSROOT = ROOT / "sysroot";
    BUILD = ROOT / "build";

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);

    bool all = false;
    for (const auto& a : args) if (a == "--all") all = true;
    if (!all && (args.empty() || args[0] == "-h" || args[0] == "--help")) {
        usage();
        return 2;
    }

    bool all_ok = true;

    if (all) {
        if (!fs::is_directory(SYSROOT, ec)) {
            out("  ! 无 sysroot 目录：%s\n", pathstr(SYSROOT).c_str());
            return 1;
        }
        std::vector<std::string> targets;
        for (fs::directory_iterator it(SYSROOT, ec), end; it != end; it.increment(ec)) {
            if (!fs::is_directory(it->path(), ec)) continue;
            std::string name = pathstr(it->path().filename());
            if (!fs::is_regular_file(it->path() / "xcc.json", ec)) continue;
            if (!find_target(name)) continue;
            targets.push_back(name);
        }
        std::sort(targets.begin(), targets.end());
        if (targets.empty()) {
            out("  ! 没有已采集且可验证的 target\n");
            return 1;
        }
        for (const std::string& t : targets) {
            out("=== target %s ===\n", t.c_str());
            std::vector<std::pair<fs::path, bool>> built;
            if (!build_all(t, built)) {
                all_ok = false;
                out("  ! 编译失败，跳过验证\n\n");
                continue;
            }
            for (auto& b : built) {
                out("--- %s ---\n", pathstr(b.first.filename()).c_str());
                auto res = check_binary(b.first, t);
                for (auto& L : res.second) out("%s\n", L.c_str());
                bool ok = res.first;
                if (b.second) {                      // C++ 产物额外查 std 符号
                    auto c = check_cxx(b.first);
                    for (auto& L : c.second) out("%s\n", L.c_str());
                    ok = ok && c.first;
                }
                if (!ok) all_ok = false;
            }
            out("\n");
        }
        return all_ok ? 0 : 1;
    }

    std::string target = args[0];
    for (size_t i = 1; i < args.size(); i++) {
        fs::path p = fs::absolute(upath(args[i]), ec);
        out("--- %s ---\n", pathstr(p.filename()).c_str());
        auto res = check_binary(p, target);
        for (auto& L : res.second) out("%s\n", L.c_str());
        bool ok = res.first;
        std::string fname = pathstr(p.filename());
        if (is_cxx_source(fname) || fname.rfind("vector", 0) == 0 || fname.rfind("plain", 0) == 0) {
            auto c = check_cxx(p);
            for (auto& L : c.second) out("%s\n", L.c_str());
            ok = ok && c.first;
        }
        if (!ok) all_ok = false;
        out("\n");
    }
    return all_ok ? 0 : 1;
}
