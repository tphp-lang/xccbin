// xccrelease.cpp — xcc 发行包构建器（原生 C++17，替代 tools/make_release.py）
//
//   xccrelease                   用探测到的 LLVM 底座打包
//   xccrelease --llvm-dir DIR    显式指定底座（含 bin/clang）
//   xccrelease --fetch-llvm      下载 LLVM 官方预编译包（22.x 仅 Windows 有资产）
//
// 产出（解压即用）：
//   dist/xcc-<tag>/bin/xcc[.exe] xcc++ ...   14 个原生驱动角色（逐个编译，非复制）
//   dist/xcc-<tag>/lib/llvm/                 clang + lld + llvm 工具（Windows 另含 DLL 闭包）
//   dist/xcc-<tag>/sysroot/<target>/         所有已采集 sysroot
//   dist/xcc-<tag>.zip
//
// 与 Python 版的差异：
//   1) zip 用 zlib 的 deflate 自己写（crc32 + 目录条目 + unix 权限位），
//      压缩后体积不小于原文件（method=8 反而变大）时自动回退 store
//   2) --fetch-llvm 走流式下载 + 流式 xz 解压 + 流式 tar 提取，
//      不把 200MB 的 tar.xz / 1.5GB 的 tar 读进内存（旧版 Python 的 tarfile 也是流式的）
//   3) 找不到 objdump 时跳过 DLL 闭包并警告（旧版会直接抛 FileNotFoundError 崩掉）
//   4) 复制/写入失败会累计并报错，绝不静默产出残缺包
//
// 编译：
//   Windows (MSYS2 clang64): clang++ -std=c++17 -O2 -o xccrelease.exe xccrelease.cpp -lwinhttp -lz
//   Linux/macOS            : clang++ -std=c++17 -O2 -o xccrelease      xccrelease.cpp -lcurl -lz -pthread

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

#include <zlib.h>
#include <lzma.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <curl/curl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/utsname.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// --------------------------------------------------------------------------
// 基础工具
// --------------------------------------------------------------------------
static fs::path ROOT, SYSROOT, CACHE, DIST;

static void rlog(const std::string& msg) {
    std::printf("[release] %s\n", msg.c_str());
    std::fflush(stdout);
}
static void rlogf(const char* fmt, ...) {
    std::printf("[release] ");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}
[[noreturn]] static void die(const std::string& msg) {
    std::fprintf(stderr, "[release] 失败：%s\n", msg.c_str());
    std::exit(1);
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

static bool write_text_file(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(s.data(), (std::streamsize)s.size());
    return f.good();
}

// --------------------------------------------------------------------------
// 子进程（捕获 stdout+stderr，带超时）
// --------------------------------------------------------------------------
struct ExecResult {
    int code = -1;
    std::string out;
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
#endif

static ExecResult exec_cmd(const std::vector<std::string>& cmd, int timeout_ms) {
    ExecResult r;
    if (cmd.empty()) return r;
#ifdef _WIN32
    std::wstring cl;
    for (size_t i = 0; i < cmd.size(); i++) {
        if (i) cl += L' ';
        cl += quote_arg(cmd[i]);
    }
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
    std::string o;
    std::thread th([&] {
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) o.append(chunk, n);
        CloseHandle(rd);
    });
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (w == WAIT_TIMEOUT) { r.timed_out = true; TerminateProcess(pi.hProcess, 1); }
    th.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = (int)code;
    r.out = std::move(o);
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
    std::string o;
    std::thread th([&] {
        char chunk[4096];
        ssize_t n;
        while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0) o.append(chunk, n);
        close(pipefd[0]);
    });
    int st = 0;
    if (timeout_ms > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            pid_t rr = waitpid(pid, &st, WNOHANG);
            if (rr == pid) break;
            if (rr < 0 && errno != EINTR) break;
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
    th.join();
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    r.out = std::move(o);
#endif
    return r;
}

static std::string which(const std::string& name) {
#ifdef _WIN32
    const char sep = ';';
    const char* exts[] = { "", ".exe", ".cmd", ".bat" };
#else
    const char sep = ':';
    const char* exts[] = { "" };
#endif
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

// --------------------------------------------------------------------------
// HTTP 下载（流式落盘；--fetch-llvm 的资产是几百 MB，不能读进内存）
// --------------------------------------------------------------------------
static std::string env_get(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

// WinHTTP 不读 http_proxy/https_proxy 环境变量（与 curl 不同），必须自己接
static std::string pick_proxy(const std::string& url) {
    bool https = url.rfind("https://", 0) == 0;
    std::string p = https ? env_get("https_proxy") : env_get("http_proxy");
    if (p.empty()) p = https ? env_get("HTTPS_PROXY") : env_get("HTTP_PROXY");
    if (p.empty()) return "";
    size_t s = p.find("://");
    if (s != std::string::npos) p = p.substr(s + 3);
    size_t at = p.rfind('@');
    if (at != std::string::npos) p = p.substr(at + 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

static void http_download(const std::string& url, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary);
    if (!out) die("无法写入 " + pathstr(dest));
    uint64_t total = 0;
    bool ok = false;
    std::string err;

#ifdef _WIN32
    std::wstring w = u8w(url);
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) die("URL 解析失败: " + url);

    std::string proxy = pick_proxy(url);
    if (!proxy.empty()) {
        WINHTTP_PROXY_INFO pi;
        std::wstring wp = u8w(proxy);
        std::vector<wchar_t> pbuf(wp.begin(), wp.end());
        pbuf.push_back(L'\0');
        pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        pi.lpszProxy = pbuf.data();
        pi.lpszProxyBypass = nullptr;
        WinHttpSetDefaultProxyConfiguration(&pi);
    }
    HINTERNET sess = WinHttpOpen(L"xcc-release/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) die("WinHttpOpen 失败");
    WinHttpSetTimeouts(sess, 30000, 60000, 300000, 300000);
    HINTERNET conn = WinHttpConnect(sess, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(sess); die("WinHttpConnect 失败: " + url); }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); die("WinHttpOpenRequest 失败"); }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    if (WinHttpSendRequest(req, WINHTTP_NO_REQUEST_DATA, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status >= 400) {
            err = "HTTP " + std::to_string(status) + ": " + url;
        } else {
            ok = true;
            DWORD avail = 0;
            std::vector<char> chunk(1 << 20);
            while (WinHttpQueryDataAvailable(req, &avail) && avail) {
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
                out.write(chunk.data(), got);
                total += got;
            }
        }
    } else {
        err = "HTTP 请求失败 (GetLastError=" + std::to_string((uint32_t)GetLastError()) + "): " + url;
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
#else
    CURL* c = curl_easy_init();
    if (!c) die("curl_easy_init 失败");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "xcc-release/0.2");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 7200L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                     +[](char* p, size_t n, size_t nm, void* u) -> size_t {
                         auto* f = static_cast<std::ofstream*>(u);
                         f->write(p, (std::streamsize)(n * nm));
                         return f->good() ? n * nm : 0;
                     });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(c);
    long httpcode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpcode);
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &total);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) err = std::string("curl 失败: ") + curl_easy_strerror(rc) + " " + url;
    else if (httpcode >= 400) err = "HTTP " + std::to_string(httpcode) + ": " + url;
    else ok = true;
#endif
    out.close();
    if (!ok) die(err);
    if (total == 0) die("下载内容为空: " + url);
    rlogf("下载完成: %.1f MB", total / 1048576.0);
}

// --------------------------------------------------------------------------
// 流式 xz 解压 + 流式 tar 提取（1.5GB 的 tar 不能进内存）
// --------------------------------------------------------------------------
static void xz_decompress_file(const fs::path& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in || !out) die("无法打开 " + pathstr(src));

    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        die("lzma_stream_decoder 初始化失败");

    std::vector<uint8_t> ibuf(1 << 20), obuf(1 << 20);
    lzma_ret rc = LZMA_OK;
    bool in_eof = false;
    while (rc != LZMA_STREAM_END) {
        if (s.avail_in == 0 && !in_eof) {
            in.read(reinterpret_cast<char*>(ibuf.data()), (std::streamsize)ibuf.size());
            s.next_in = ibuf.data();
            s.avail_in = (size_t)in.gcount();
            if (s.avail_in == 0) in_eof = true;
        }
        s.next_out = obuf.data();
        s.avail_out = obuf.size();
        rc = lzma_code(&s, in_eof ? LZMA_FINISH : LZMA_RUN);
        size_t produced = obuf.size() - s.avail_out;
        if (produced) out.write(reinterpret_cast<const char*>(obuf.data()), (std::streamsize)produced);
        if (rc == LZMA_STREAM_END) break;
        if (rc != LZMA_OK) {
            lzma_end(&s);
            die("xz 解压失败: " + pathstr(src));
        }
        if (in_eof && produced == 0 && s.avail_in == 0) break;
    }
    lzma_end(&s);
    out.close();
    if (!out) die("xz 解压写入失败: " + pathstr(dst));
}

static uint64_t tar_size_field(const uint8_t* h) {
    if (h[124] & 0x80) {                                  // GNU base-256
        uint64_t v = 0;
        for (int i = 4; i < 12; i++) v = (v << 8) | (h[124 + i] & 0xff);
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < 12; i++) {
        char c = (char)h[124 + i];
        if (c == 0 || c == ' ') break;
        if (c < '0' || c > '7') continue;
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}
static std::string tar_str(const uint8_t* p, size_t n) {
    size_t e = 0;
    while (e < n && p[e]) e++;
    return std::string(reinterpret_cast<const char*>(p), e);
}

// 顺序读取 tar 文件并落盘（不把整个 tar 读进内存）
static size_t tar_extract_file(const fs::path& tar, const fs::path& destdir) {
    std::ifstream in(tar, std::ios::binary);
    if (!in) die("无法打开 " + pathstr(tar));
    std::error_code ec;
    fs::create_directories(destdir, ec);

    std::string long_name, pax_path;
    size_t count = 0, failed = 0;
    std::vector<char> hbuf(512);
    while (true) {
        in.read(hbuf.data(), 512);
        if (in.gcount() != 512) break;
        const uint8_t* h = reinterpret_cast<const uint8_t*>(hbuf.data());
        bool empty = true;
        for (int i = 0; i < 512; i++) if (h[i]) { empty = false; break; }
        if (empty) break;                                  // tar 结束块

        std::string name = tar_str(h, 100);
        std::string prefix = tar_str(h + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        char type = (char)h[156];
        std::string link = tar_str(h + 157, 100);
        uint64_t size = tar_size_field(h);
        uint64_t skip = (size + 511) / 512 * 512;

        if (type == 'L') {                                 // GNU 长名
            std::string data(size, '\0');
            in.read(&data[0], (std::streamsize)size);
            in.seekg((std::streamoff)(skip - size), std::ios::cur);
            size_t z = data.find('\0');
            long_name = (z == std::string::npos) ? data : data.substr(0, z);
            continue;
        }
        if (type == 'x' || type == 'g') {                  // pax 扩展头
            std::string data(size, '\0');
            in.read(&data[0], (std::streamsize)size);
            in.seekg((std::streamoff)(skip - size), std::ios::cur);
            size_t p = data.find("path=");
            if (p != std::string::npos) {
                p += 5;
                size_t e = data.find('\n', p);
                pax_path = data.substr(p, e == std::string::npos ? std::string::npos : e - p);
            }
            continue;
        }
        if (!long_name.empty()) { name = long_name; long_name.clear(); }
        if (!pax_path.empty()) { name = pax_path; pax_path.clear(); }

        while (name.rfind("./", 0) == 0) name = name.substr(2);
        bool skip_content = (name.empty() || name == "." ||
                             name.rfind("../", 0) == 0 || name == "..");

        bool is_regular = (type == '0' || type == '\0' || type == '7');
        if (!skip_content && is_regular) {
            fs::path dst = destdir / upath(name);
            fs::create_directories(dst.parent_path(), ec);
            std::ofstream o(dst, std::ios::binary);
            uint64_t left = size;
            if (!o) {
                failed++;
            } else {
                std::vector<char> chunk(1 << 20);
                while (left) {
                    size_t want = (size_t)std::min<uint64_t>(left, chunk.size());
                    in.read(chunk.data(), (std::streamsize)want);
                    std::streamsize got = in.gcount();
                    if (got <= 0) break;
                    o.write(chunk.data(), got);
                    left -= (uint64_t)got;
                }
                o.close();
                if (left == 0 && o) count++; else failed++;
            }
            // 跳过未读完的数据 + 512 对齐填充
            in.seekg((std::streamoff)(left + (skip - size)), std::ios::cur);
        } else if (!skip_content && type == '2') {
            fs::path dst = destdir / upath(name);
            fs::create_directories(dst.parent_path(), ec);
            fs::remove(dst, ec);
            fs::create_symlink(upath(link), dst, ec);      // 失败可容忍（无权限时退化为跳过）
            if (skip) in.seekg((std::streamoff)skip, std::ios::cur);
        } else {
            if (!skip_content && type == '5') {
                fs::create_directories(destdir / upath(name), ec);
            }
            if (skip) in.seekg((std::streamoff)skip, std::ios::cur);
        }
    }
    if (failed) die("tar 提取失败 " + std::to_string(failed) + " 个文件: " + pathstr(tar));
    return count;
}

// --------------------------------------------------------------------------
// zip 写入（deflate + unix 权限位）
// --------------------------------------------------------------------------
struct ZipEntry {
    std::string name;
    uint16_t method = 8;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0;
    uint64_t offset = 0;
    uint32_t ext_attr = 0;
};

class ZipWriter {
public:
    bool open(const fs::path& p) {
        f.open(p, std::ios::binary | std::ios::trunc);
        return f.good();
    }
    bool add_entry(const std::string& name, const fs::path* src, bool is_dir, uint32_t mode) {
        std::string zname = name;
        std::replace(zname.begin(), zname.end(), '\\', '/');
        if (is_dir && !zname.empty() && zname.back() != '/') zname += '/';

        ZipEntry e;
        e.name = zname;
        e.offset = (uint64_t)f.tellp();
        e.ext_attr = (mode << 16) | (is_dir ? 0x10u : 0u);

        uint16_t dtime = 0, ddate = 0;
        dos_time(std::time(nullptr), dtime, ddate);

        if (is_dir) {
            e.method = 0;
            write_local_header(zname, 0, dtime, ddate, 0, 0, 0);
            entries.push_back(e);
            return true;
        }

        std::ifstream in(*src, std::ios::binary);
        if (!in) return false;
        uint64_t usize = fs::file_size(*src, ec_);
        std::vector<uint8_t> raw;
        bool compressed = false;
        if (usize > 0 && usize <= (512ULL << 20)) {          // 超大文件不尝试压缩，直接 store
            compressed = deflate_stream(in, usize, raw, e.crc);
        }
        if (!compressed) {
            in.clear();
            in.seekg(0);
            e.method = 0;
            e.crc = 0;
            std::vector<char> chunk(1 << 20);
            uint64_t left = usize;
            while (left) {
                size_t want = (size_t)std::min<uint64_t>(left, chunk.size());
                in.read(chunk.data(), (std::streamsize)want);
                std::streamsize got = in.gcount();
                if (got <= 0) break;
                e.crc = crc32(e.crc, reinterpret_cast<const Bytef*>(chunk.data()), (uInt)got);
                f.write(chunk.data(), got);
                left -= (uint64_t)got;
            }
            e.csize = e.usize = usize - left;
            if (!f) return false;
        } else if (raw.size() < usize) {
            e.method = 8;
            e.usize = usize;
            e.csize = raw.size();
            f.write(reinterpret_cast<const char*>(raw.data()), (std::streamsize)raw.size());
        } else {                                            // 压了反而更大 → store
            in.clear();
            in.seekg(0);
            e.method = 0;
            e.crc = 0;
            std::vector<char> chunk(1 << 20);
            uint64_t left = usize;
            while (left) {
                size_t want = (size_t)std::min<uint64_t>(left, chunk.size());
                in.read(chunk.data(), (std::streamsize)want);
                std::streamsize got = in.gcount();
                if (got <= 0) break;
                e.crc = crc32(e.crc, reinterpret_cast<const Bytef*>(chunk.data()), (uInt)got);
                f.write(chunk.data(), got);
                left -= (uint64_t)got;
            }
            e.csize = e.usize = usize - left;
            if (!f) return false;
        }
        // 回填 local header 里的 crc / 大小（写完才知道）
        uint64_t pos = (uint64_t)f.tellp();
        f.seekp((std::streamoff)(e.offset + 14));
        put32(e.crc); put32((uint32_t)e.csize); put32((uint32_t)e.usize);
        f.seekp((std::streamoff)pos);
        entries.push_back(e);
        return f.good();
    }
    void close() {
        uint64_t cd_off = (uint64_t)f.tellp();
        uint16_t dtime = 0, ddate = 0;
        dos_time(std::time(nullptr), dtime, ddate);
        for (auto& e : entries) {
            put32(0x02014b50);
            put16(0x031E);            // version made by: UNIX, zip 3.0
            put16(20);
            put16(0);
            put16(e.method);
            put16(dtime);
            put16(ddate);
            put32(e.crc);
            put32((uint32_t)e.csize);
            put32((uint32_t)e.usize);
            put16((uint16_t)e.name.size());
            put16(0); put16(0); put16(0); put16(0);
            put32(e.ext_attr);
            put32((uint32_t)e.offset);
            f.write(e.name.data(), (std::streamsize)e.name.size());
        }
        uint64_t cd_size = (uint64_t)f.tellp() - cd_off;
        put32(0x06054b50);
        put16(0); put16(0);
        put16((uint16_t)entries.size());
        put16((uint16_t)entries.size());
        put32((uint32_t)cd_size);
        put32((uint32_t)cd_off);
        put16(0);
        f.flush();
    }
    size_t count() const { return entries.size(); }

private:
    std::ofstream f;
    std::vector<ZipEntry> entries;
    std::error_code ec_;

    void put16(uint16_t v) { f.put((char)(v & 0xff)); f.put((char)(v >> 8)); }
    void put32(uint32_t v) {
        for (int i = 0; i < 4; i++) f.put((char)((v >> (8 * i)) & 0xff));
    }
    void write_local_header(const std::string& name, uint16_t method,
                            uint16_t dtime, uint16_t ddate,
                            uint32_t crc, uint32_t csize, uint32_t usize) {
        put32(0x04034b50);
        put16(20);
        put16(0);
        put16(method);
        put16(dtime);
        put16(ddate);
        put32(crc);
        put32(csize);
        put32(usize);
        put16((uint16_t)name.size());
        put16(0);
        f.write(name.data(), (std::streamsize)name.size());
    }
    static void dos_time(std::time_t t, uint16_t& dtime, uint16_t& ddate) {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        dtime = (uint16_t)(((tm.tm_hour & 31) << 11) | ((tm.tm_min & 63) << 5) | ((tm.tm_sec / 2) & 31));
        ddate = (uint16_t)((((tm.tm_year + 1900) - 1980) & 127) << 9 |
                           (((tm.tm_mon + 1) & 15) << 5) | (tm.tm_mday & 31));
    }
    static bool deflate_stream(std::ifstream& in, uint64_t usize,
                               std::vector<uint8_t>& out, uint32_t& crc) {
        crc = crc32(0L, Z_NULL, 0);
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
        std::vector<uint8_t> ibuf(1 << 20), obuf(1 << 20);
        uint64_t left = usize;
        bool ok = true;
        while (left) {
            size_t want = (size_t)std::min<uint64_t>(left, ibuf.size());
            in.read(reinterpret_cast<char*>(ibuf.data()), (std::streamsize)want);
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            crc = crc32(crc, ibuf.data(), (uInt)got);
            z.next_in = ibuf.data();
            z.avail_in = (uInt)got;
            left -= (uint64_t)got;
            while (z.avail_in) {
                z.next_out = obuf.data();
                z.avail_out = (uInt)obuf.size();
                int rc = deflate(&z, left ? Z_NO_FLUSH : Z_FINISH);
                size_t produced = obuf.size() - z.avail_out;
                if (produced) out.insert(out.end(), obuf.begin(), obuf.begin() + produced);
                if (rc == Z_STREAM_END) break;
                if (rc != Z_OK) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (ok && left == 0) {
            int rc = Z_OK;
            while (rc != Z_STREAM_END) {
                z.next_out = obuf.data();
                z.avail_out = (uInt)obuf.size();
                rc = deflate(&z, Z_FINISH);
                size_t produced = obuf.size() - z.avail_out;
                if (produced) out.insert(out.end(), obuf.begin(), obuf.begin() + produced);
                if (rc != Z_OK && rc != Z_STREAM_END) { ok = false; break; }
            }
        }
        deflateEnd(&z);
        return ok && left == 0;
    }
};

// --------------------------------------------------------------------------
// host 探测
// --------------------------------------------------------------------------
struct Host { std::string system, tag; };

static Host detect_host() {
#ifdef _WIN32
    return { "windows", "win64" };
#elif defined(__APPLE__)
    std::string tag = "macos-x64";
    struct utsname u;
    if (uname(&u) == 0 && std::string(u.machine) == "arm64") tag = "macos-arm64";
    return { "macos", tag };
#else
    std::string tag = "linux-x64";
    struct utsname u;
    if (uname(&u) == 0 && std::string(u.machine) == "aarch64") tag = "linux-arm64";
    return { "linux", tag };
#endif
}

static std::string exe_suffix_for(const std::string& system) {
    return system == "windows" ? ".exe" : "";
}

// --------------------------------------------------------------------------
// LLVM 底座
// --------------------------------------------------------------------------
static const char* LLVM_VERSION = "22.1.8";

static std::string llvm_fetch_url(const std::string& tag) {
    std::string sys = tag.substr(0, tag.find('-'));
    std::string arch = (tag == "win64") ? "x86_64" : "aarch64";
    if (sys != "windows") return "";
    return std::string("https://github.com/llvm/llvm-project/releases/download/llvmorg-") +
           LLVM_VERSION + "/clang+llvm-" + LLVM_VERSION + "-" + arch + "-pc-windows-msvc.tar.xz";
}

static fs::path find_msys2_clang64() {
    const char* cands[] = {
        "C:/env/Msys2/clang64", "C:/env/Msys2/ucrt64",
        "C:/msys64/clang64", "C:/msys64/ucrt64",
    };
    std::error_code ec;
    for (const char* c : cands) {
        fs::path p = upath(c);
        if (fs::is_regular_file(p / "bin" / "clang.exe", ec)) return p;
    }
    std::string w = which("clang");
    if (!w.empty()) {
        fs::path p = fs::absolute(upath(w), ec);
        return p.parent_path().parent_path();
    }
    return fs::path();
}

static fs::path fetch_llvm(const std::string& tag) {
    std::string url = llvm_fetch_url(tag);
    if (url.empty())
        die("LLVM 官方没有 " + tag + " 的预编译资产；请安装 clang 后传 --llvm-dir");
    fs::path dest = CACHE / ("llvm-" + tag + ".tar.xz");
    if (!fs::is_regular_file(dest)) {
        rlog("下载 LLVM 官方底座: " + url);
        http_download(url, dest);
    } else {
        rlogf("复用缓存: %s", pathstr(dest).c_str());
    }
    fs::path out = CACHE / ("llvm-" + tag);
    std::error_code ec;
    if (!fs::is_regular_file(out / "bin" / "clang.exe", ec)) {
        fs::path tmp = CACHE / ("llvm-" + tag + ".tar");
        rlog("解压 xz ...");
        xz_decompress_file(dest, tmp);
        rlog("提取 tar ...");
        size_t n = tar_extract_file(tmp, out);
        rlogf("提取 %zu 个文件", n);
        fs::remove(tmp, ec);
    }
    for (fs::directory_iterator it(out, ec), end; it != end; it.increment(ec)) {
        if (fs::is_directory(it->path(), ec)) return it->path();
    }
    die("解包后的 LLVM 目录为空: " + pathstr(out));
}

// --------------------------------------------------------------------------
// 复制：LLVM 工具 / DLL 闭包 / 资源头 / sysroot
// --------------------------------------------------------------------------
static const char* LLVM_TOOLS[] = {
    "clang", "clang++", "ld.lld", "lld-link", "wasm-ld", "lld",
    "llvm-ar", "llvm-ranlib", "llvm-nm", "llvm-objcopy", "llvm-strip",
    "llvm-readobj", "llvm-objdump", "llvm-readelf",
};

static const char* DRIVER_ROLES[] = { "xcc", "xcc++", "xcc-c++", "xcc-g++", "xcc-cc", "xcc-gcc" };
static const char* BINUTILS_ROLES[] = { "xcc-ar", "xcc-ranlib", "xcc-nm", "xcc-objcopy",
                                        "xcc-strip", "xcc-dlltool", "xcc-readobj", "xcc-objdump" };

// Windows：递归解析 clang.exe 的 DLL 依赖闭包（objdump -p）
static std::vector<fs::path> dll_closure(const fs::path& src_bin) {
    std::string objdump = which("objdump");
    if (objdump.empty()) {
        rlog("警告：找不到 objdump，跳过 DLL 依赖闭包（包内 clang 可能缺 DLL）");
        return {};
    }
    std::vector<fs::path> todo{ src_bin / "clang.exe" };
    std::vector<fs::path> closure;
    std::vector<std::string> seen;
    std::error_code ec;
    while (!todo.empty()) {
        fs::path cur = todo.back();
        todo.pop_back();
        std::string cs = pathstr(cur);
        if (std::find(seen.begin(), seen.end(), cs) != seen.end()) continue;
        seen.push_back(cs);
        ExecResult r = exec_cmd({ objdump, "-p", cs }, 120000);
        if (!r.spawned) continue;
        for (size_t i = 0;;) {
            size_t p = r.out.find("DLL Name:", i);
            if (p == std::string::npos) break;
            size_t e = r.out.find('\n', p);
            std::string dll = r.out.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
            i = (e == std::string::npos) ? r.out.size() : e + 1;
            while (!dll.empty() && (dll.front() == ' ' || dll.front() == '\r')) dll.erase(dll.begin());
            while (!dll.empty() && (dll.back() == ' ' || dll.back() == '\r')) dll.pop_back();
            if (dll.empty()) continue;
            fs::path dp = src_bin / upath(dll);
            std::string dps = pathstr(dp);
            if (fs::is_regular_file(dp, ec) &&
                std::find(seen.begin(), seen.end(), dps) == seen.end()) {
                closure.push_back(dp);
                todo.push_back(dp);
            }
        }
    }
    std::sort(closure.begin(), closure.end(),
              [](const fs::path& a, const fs::path& b) { return pathstr(a) < pathstr(b); });
    return closure;
}

static void copy_llvm(const fs::path& src, const fs::path& dst_llvm,
                      const std::string& system, const std::string& exe_suffix) {
    std::error_code ec;
    fs::path bin_src = src / "bin", bin_dst = dst_llvm / "bin";
    fs::create_directories(bin_dst, ec);

    std::vector<std::string> copied;
    for (const char* t : LLVM_TOOLS) {
        std::string cand = std::string(t) + exe_suffix;
        fs::path s = bin_src / upath(cand);
        if (!fs::is_regular_file(s, ec)) {
            s = bin_src / upath(t);
            if (!fs::is_regular_file(s, ec)) continue;
        }
        fs::copy_file(s, bin_dst / s.filename(), fs::copy_options::overwrite_existing, ec);
        if (!ec) copied.push_back(pathstr(s.filename()));
    }
    std::sort(copied.begin(), copied.end());
    std::string joined;
    for (size_t i = 0; i < copied.size(); i++) { if (i) joined += ", "; joined += copied[i]; }
    rlog("复制 LLVM 工具 (" + std::to_string(copied.size()) + "): " + joined);

    if (system == "windows") {
        auto closure = dll_closure(bin_src);
        for (const fs::path& d : closure)
            fs::copy_file(d, bin_dst / d.filename(), fs::copy_options::overwrite_existing, ec);
        std::string names;
        for (size_t i = 0; i < closure.size(); i++) {
            if (i) names += ", ";
            names += pathstr(closure[i].filename());
        }
        rlogf("复制 DLL 依赖闭包: %zu 个 (%s)", closure.size(), names.c_str());
    } else {
        size_t extra = 0;
        for (fs::directory_iterator it(bin_src, ec), end; it != end; it.increment(ec)) {
            std::string n = pathstr(it->path().filename());
            if (n.size() >= 3 && (n.rfind(".so") == n.size() - 3 || n.find(".so.") != std::string::npos ||
                                  n.rfind(".dylib") == n.size() - 6)) {
                fs::copy_file(it->path(), bin_dst / it->path().filename(),
                              fs::copy_options::overwrite_existing, ec);
                extra++;
            }
        }
        rlogf("复制共享库: %zu 个", extra);
    }

    // clang 资源头（主机无关）
    fs::path clang_lib = src / "lib" / "clang";
    if (fs::is_directory(clang_lib, ec)) {
        for (fs::directory_iterator it(clang_lib, ec), end; it != end; it.increment(ec)) {
            if (!fs::is_directory(it->path(), ec)) continue;
            fs::path inc = it->path() / "include";
            if (!fs::is_directory(inc, ec)) continue;
            fs::path dst_inc = dst_llvm / "lib" / "clang" / it->path().filename() / "include";
            fs::create_directories(dst_inc.parent_path(), ec);
            size_t n = 0;
            for (fs::recursive_directory_iterator jt(inc, fs::directory_options::follow_directory_symlink, ec), je;
                 jt != je; jt.increment(ec)) {
                if (fs::is_directory(jt->path(), ec)) continue;
                fs::path rel = fs::relative(jt->path(), inc, ec);
                fs::path t = dst_inc / rel;
                fs::create_directories(t.parent_path(), ec);
                fs::copy_file(jt->path(), t, fs::copy_options::overwrite_existing, ec);
                n++;
            }
            rlogf("复制 clang 资源头: lib/clang/%s/include (%zu 个文件)",
                  pathstr(it->path().filename()).c_str(), n);
        }
    }

    fs::path clang_bin = bin_dst / upath(std::string("clang") + exe_suffix);
    std::string ver_line = "unknown";
    ExecResult r = exec_cmd({ pathstr(clang_bin), "--version" }, 60000);
    if (r.spawned && r.code == 0 && !r.out.empty()) {
        size_t e = r.out.find('\n');
        ver_line = r.out.substr(0, e == std::string::npos ? std::string::npos : e);
    }
    write_text_file(dst_llvm / "VERSION.txt", ver_line + "\n");
    rlog("LLVM 底座: " + ver_line);
}

static size_t copy_tree(const fs::path& src, const fs::path& dst, const std::string& label) {
    size_t n = 0, failed = 0;
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, fs::directory_options::follow_directory_symlink, ec), end;
         it != end; it.increment(ec)) {
        if (fs::is_directory(it->path(), ec)) continue;
        fs::path rel = fs::relative(it->path(), src, ec);
        fs::path t = dst / rel;
        fs::create_directories(t.parent_path(), ec);
        fs::path real = it->path();
        std::error_code ec2;
        if (fs::is_symlink(it->path(), ec2)) {
            fs::path r = fs::canonical(it->path(), ec2);
            if (!ec2) real = r;
        }
        if (fs::is_regular_file(real, ec2)) {
            fs::copy_file(real, t, fs::copy_options::overwrite_existing, ec2);
            if (ec2) failed++; else n++;
        }
    }
    if (failed)
        die("复制失败 " + std::to_string(failed) + " 个文件: " + label);
    if (!label.empty()) rlogf("  %-26s %zu 个文件", label.c_str(), n);
    return n;
}

// --------------------------------------------------------------------------
// 原生驱动：14 个角色各自独立编译
// --------------------------------------------------------------------------
static void build_driver(const fs::path& dst_bin, const std::string& system,
                         const std::string& exe_suffix, const fs::path& llvm_base) {
    std::error_code ec;
    fs::create_directories(dst_bin, ec);
    fs::path src = ROOT / "xcc.cpp";
    if (!fs::is_regular_file(src, ec)) die("找不到 " + pathstr(src));

    fs::path clangxx = llvm_base / "bin" / upath(std::string("clang++") + exe_suffix);
    if (!fs::is_regular_file(clangxx, ec)) {
        std::string w = which("clang++");
        if (w.empty()) w = which("clang");
        if (w.empty()) die("找不到 clang++，无法编译驱动");
        clangxx = upath(w);
    }

    std::vector<std::string> flags = { "-std=c++17", "-O2", "-s" };
    if (system != "macos") flags.push_back("-static");

    // 每个角色独立编译到最终名（而非复制）：Windows 上刚落地的 exe 会被杀软
    // 瞬时扫描锁定，复制新鲜 exe 会撞锁；逐个编译只写不读，天然规避。
    std::vector<std::string> roles;
    for (const char* r : DRIVER_ROLES) roles.push_back(r);
    for (const char* r : BINUTILS_ROLES) roles.push_back(r);

    for (const std::string& role : roles) {
        fs::path p = dst_bin / upath(role + exe_suffix);
        bool ok = false;
        std::string last_err;
        for (int attempt = 0; attempt < 10; attempt++) {
            std::vector<std::string> cmd;
            cmd.push_back(pathstr(clangxx));
            cmd.insert(cmd.end(), flags.begin(), flags.end());
            cmd.push_back("-o");
            cmd.push_back(pathstr(p));
            cmd.push_back(pathstr(src));
            ExecResult r = exec_cmd(cmd, 300000);
            if (r.spawned && r.code == 0) { ok = true; break; }
            last_err = r.out;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        if (!ok)
            die("驱动编译失败 (" + role + "):\n" + last_err.substr(0, 2000));
        if (system != "windows") {
            fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
        }
    }
    rlogf("原生驱动角色: %zu 个", roles.size());
}

static void place_wasm_builtins(const fs::path& llvm_dst, const fs::path& sysroot_dst) {
    std::error_code ec;
    fs::path src = sysroot_dst / "wasm32-wasi" / "lib" / "wasm32-wasi" /
                   "libclang_rt.builtins-wasm32.a";
    if (!fs::is_regular_file(src, ec)) return;
    fs::path clang_lib = llvm_dst / "lib" / "clang";
    if (!fs::is_directory(clang_lib, ec)) return;
    for (fs::directory_iterator it(clang_lib, ec), end; it != end; it.increment(ec)) {
        if (!fs::is_directory(it->path(), ec)) continue;
        fs::path dst = it->path() / "lib" / "wasm32-unknown-wasi" / "libclang_rt.builtins.a";
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        rlog("预置 wasm builtins -> lib/clang/" + pathstr(it->path().filename()) +
             "/lib/wasm32-unknown-wasi/");
        break;
    }
}

// --------------------------------------------------------------------------
// 打包
// --------------------------------------------------------------------------
static uint32_t mode_of(const fs::path& p, bool is_dir, const std::string& system) {
#ifdef _WIN32
    (void)system;
    if (is_dir) return 040755;
    std::string n = pathstr(p.filename());
    // Windows 没有 unix mode：bin 下的一律可执行，脚本可执行，其余 0644
    std::string full = pathstr(p);
    std::replace(full.begin(), full.end(), '\\', '/');
    if (full.find("/bin/") != std::string::npos) return 0755;
    if (n.rfind(".sh") == n.size() - 3) return 0755;
    return 0644;
#else
    (void)system;
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return is_dir ? 040755 : 0644;
    return (uint32_t)(st.st_mode & 07777);
#endif
}

static void zip_dir(const fs::path& dir, const fs::path& zippath, const std::string& system) {
    std::error_code ec;
    ZipWriter zw;
    if (!zw.open(zippath)) die("无法创建 " + pathstr(zippath));
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        std::string rel = pathstr(fs::relative(it->path(), dir, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (fs::is_directory(it->path(), ec)) {
            zw.add_entry(rel, nullptr, true, mode_of(it->path(), true, system));
        } else if (fs::is_regular_file(it->path(), ec)) {
            zw.add_entry(rel, &it->path(), false, mode_of(it->path(), false, system));
        }
    }
    zw.close();
    uintmax_t zsz = fs::file_size(zippath, ec);
    rlogf("打包 %zu 个条目: %s (%.1f MB)", zw.count(), pathstr(zippath).c_str(), zsz / 1048576.0);
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
static void usage() {
    std::printf("用法: xccrelease [--fetch-llvm] [--llvm-dir DIR]\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string llvm_dir_arg;
    bool fetch_llvm_flag = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--fetch-llvm") fetch_llvm_flag = true;
        else if (a == "--llvm-dir") { if (i + 1 < argc) llvm_dir_arg = argv[++i]; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { usage(); return 2; }
    }

    // 定位仓库根：优先 XCC_ROOT，其次 exe 上一级的上一级，再次 cwd
    std::error_code ec;
    if (const char* e = std::getenv("XCC_ROOT"); e && *e) {
        ROOT = fs::absolute(upath(e), ec);
    } else {
#ifdef _WIN32
        wchar_t buf[32768];
        DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
        fs::path r = (n > 0) ? fs::path(std::wstring(buf, n)).parent_path().parent_path() : fs::path();
#elif defined(__linux__)
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        fs::path r;
        if (n > 0) { buf[n] = '\0'; r = fs::path(buf).parent_path().parent_path(); }
#else
        fs::path r;
#endif
        if (!r.empty() && fs::is_regular_file(r / "xcc.cpp", ec)) ROOT = r;
        else {
            fs::path cwd = fs::current_path(ec);
            ROOT = fs::is_regular_file(cwd / "xcc.cpp", ec) ? cwd : (r.empty() ? cwd : r);
        }
    }
    SYSROOT = ROOT / "sysroot";
    CACHE = ROOT / "cache";
    DIST = ROOT / "dist";

    Host host = detect_host();
    std::string exe_suffix = exe_suffix_for(host.system);
    rlog("host: " + host.system + " (" + host.tag + ")");

    // ---- LLVM 底座 ----
    fs::path base;
    if (!llvm_dir_arg.empty()) base = fs::absolute(upath(llvm_dir_arg), ec);
    else if (!env_get("XCC_LLVM_DIR").empty()) base = fs::absolute(upath(env_get("XCC_LLVM_DIR")), ec);
    else if (host.system == "windows") {
        base = fetch_llvm_flag ? fs::path() : find_msys2_clang64();
        if (base.empty()) {
            if (!fetch_llvm_flag) rlog("未找到 MSYS2 clang64，回退 --fetch-llvm");
            base = fetch_llvm(host.tag);
        }
    } else {
        die("未指定 LLVM 底座：请传 --llvm-dir 或设置 XCC_LLVM_DIR");
    }
    if (base.empty() || !fs::is_directory(base / "bin", ec))
        die("LLVM 底座无效: " + pathstr(base));
    rlog("LLVM 底座来源: " + pathstr(base));

    // ---- 输出目录 ----
    fs::path out = DIST / upath("xcc-" + host.tag);
    if (fs::exists(out, ec)) fs::remove_all(out, ec);
    fs::create_directories(out, ec);

    rlog("=== 1/4 复制 LLVM 底座 ===");
    fs::path llvm_dst = out / "lib" / "llvm";
    copy_llvm(base, llvm_dst, host.system, exe_suffix);

    rlog("=== 2/4 编译原生驱动 ===");
    fs::path bin_dir = out / "bin";
    build_driver(bin_dir, host.system, exe_suffix, base);

    rlog("=== 3/4 复制 sysroot ===");
    fs::path sysroot_dst = out / "sysroot";
    size_t n = 0;
    if (fs::is_directory(SYSROOT, ec)) {
        std::vector<fs::path> targets;
        for (fs::directory_iterator it(SYSROOT, ec), end; it != end; it.increment(ec)) {
            if (fs::is_directory(it->path(), ec) &&
                fs::is_regular_file(it->path() / "xcc.json", ec))
                targets.push_back(it->path());
        }
        std::sort(targets.begin(), targets.end(),
                  [](const fs::path& a, const fs::path& b) { return pathstr(a) < pathstr(b); });
        for (const fs::path& t : targets) {
            copy_tree(t, sysroot_dst / t.filename(), pathstr(t.filename()));
            n++;
        }
    }
    rlogf("复制 sysroot: %zu 个 target", n);
    place_wasm_builtins(llvm_dst, sysroot_dst);

    rlog("=== 4/4 写入口与压缩 ===");
    if (host.system == "windows") {
        write_text_file(out / "xcc.bat",
                        "@echo off\n"
                        "rem xcc 便捷入口：把 bin 目录加入当前会话 PATH\n"
                        "set PATH=%~dp0bin;%PATH%\n"
                        "xcc.exe %*\n");
    } else {
        fs::path sh = out / "xcc.sh";
        write_text_file(sh,
                        "#!/bin/sh\n"
                        "# xcc 便捷入口：把 bin 目录加入当前会话 PATH\n"
                        "export PATH=\"$(cd \"$(dirname \"$0\")\" && pwd)/bin:$PATH\"\n"
                        "exec xcc \"$@\"\n");
        fs::permissions(sh, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
    }

    write_text_file(out / "README.txt",
        "xcc — 多平台 C/C++ 交叉编译工具链（开箱即用）\n\n"
        "用法（bin/ 已在 PATH 内）：\n"
        "  xcc   -target aarch64-linux-musl  hello.c    -o hello\n"
        "  xcc   -target wasm32-wasi         hello.c    -o hello.wasm\n"
        "  xcc++ -target wasm32-wasi         hello.cpp  -o hello.wasm\n"
        "  xcc -print-targets\n\n"
        "支持: aarch64/x86_64/riscv64 x linux-musl(全静态), wasm32-wasi\n");

    fs::create_directories(DIST, ec);
    fs::path zip_path = DIST / upath("xcc-" + host.tag + ".zip");
    if (fs::exists(zip_path, ec)) fs::remove(zip_path, ec);
    zip_dir(out, zip_path, host.system);

    rlog("完成: " + pathstr(zip_path));
    return 0;
}
