// xccsysroot.cpp — xcc sysroot 采集器（原生 C++17，替代 tools/build_sysroot.py）
//
//   xccsysroot --list                     列出 target 与采集状态
//   xccsysroot x86_64-linux-musl          采集（遵守 pins/<target>.json）
//   xccsysroot --all                      采集全部
//   xccsysroot <target> --update          忽略锁定，重新解析上游并刷新 pins
//
// 与 Python 版的行为差异（本次"版本钉死 + 校验和"改造的核心）：
//   1) 每个下载件记入 pins/<target>.json：URL + 文件名 + 版本 + sha256
//   2) 有锁时直接用锁定文件名下载，不再"挑最新"；上游一变就地报错，
//      要跟上上游必须显式 --update（fail loud，不静默漂移）
//   3) 缓存命中也校验 sha256（旧版只看"文件存在且非空"）
//
// 编译：
//   Windows (MSYS2 clang64): clang++ -std=c++17 -O2 -o xccsysroot.exe xccsysroot.cpp -lwinhttp -lz -llzma
//   Linux/macOS            : clang++ -std=c++17 -O2 -o xccsysroot      xccsysroot.cpp -lcurl -lz -llzma
//   zstd 压缩包：运行时调 zstd -dc；也可 -DXCC_USE_LIBZSTD -lzstd 静态链

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

#include <filesystem>
namespace fs = std::filesystem;

#include <zlib.h>
#include <lzma.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <unistd.h>
#  include <curl/curl.h>
#endif

#ifdef XCC_USE_LIBZSTD
#  include <zstd.h>
#endif

// --------------------------------------------------------------------------
// 基础工具
// --------------------------------------------------------------------------
static fs::path ROOT, CACHE, SYSROOT, BUILD, PINS;

static void xlog(const std::string& msg) {
    std::printf("[sysroot] %s\n", msg.c_str());
    std::fflush(stdout);
}
static void xlogf(const char* fmt, ...) {
    std::printf("[sysroot] ");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

[[noreturn]] static void die(const std::string& msg) {
    std::fprintf(stderr, "[sysroot] 失败：%s\n", msg.c_str());
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

static bool write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return f.good();
}
static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// --------------------------------------------------------------------------
// 上游镜像（Alpine 走阿里云国内可达；Debian 官方源直连）
// --------------------------------------------------------------------------
static std::string ALPINE_MIRROR = "https://mirrors.aliyun.com/alpine";
static std::string DEB_MIRROR = "https://deb.debian.org/debian/pool/main";
static std::string WASI_URLS[2] = {
    "https://github.com/WebAssembly/wasi-sdk/releases/download/{ver}/wasi-sysroot-{ver2}.tar.gz",
    "https://gh-proxy.com/https://github.com/WebAssembly/wasi-sdk/releases/download/{ver}/wasi-sysroot-{ver2}.tar.gz",
};

// --------------------------------------------------------------------------
// SHA-256（自实现，避免引入 OpenSSL/BCrypt 依赖）
// --------------------------------------------------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t buf[64] = {0};
    size_t buflen = 0;
    uint64_t total = 0;

    static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            p += 4;
        }
        for (int i = 16; i < 64; i++) {
            uint32_t x15 = w[i-15], x2 = w[i-2];
            uint32_t s0 = rotr(x15, 7) ^ rotr(x15, 18) ^ (x15 >> 3);
            uint32_t s1 = rotr(x2, 17) ^ rotr(x2, 19) ^ (x2 >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3],
                 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            size_t take = (n < 64 - buflen) ? n : (64 - buflen);
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { compress(buf); buflen = 0; }
        }
    }
    std::string hex() {
        uint8_t tail[128];
        size_t tlen = buflen;
        std::memcpy(tail, buf, buflen);
        tail[tlen++] = 0x80;
        while (tlen % 64 != 56) tail[tlen++] = 0;
        uint64_t bits = total * 8;
        for (int i = 0; i < 8; i++) tail[tlen++] = (uint8_t)(bits >> (56 - i * 8));
        for (size_t i = 0; i < tlen; i += 64) compress(tail + i);
        static const char* D = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t v = (h[i] >> (24 - j * 8)) & 0xff;
                out += D[v >> 4]; out += D[v & 0xf];
            }
        }
        return out;
    }
};

static std::string sha256_hex(const std::vector<uint8_t>& d) {
    Sha256 s;
    s.update(d.data(), d.size());
    return s.hex();
}

// --------------------------------------------------------------------------
// HTTP
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

    std::string np = env_get("no_proxy");
    if (np.empty()) np = env_get("NO_PROXY");
    if (!np.empty()) {
        std::string host = url.substr(url.find("://") + 3);
        size_t slash = host.find('/');
        if (slash != std::string::npos) host = host.substr(0, slash);
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        for (size_t i = 0; i <= np.size();) {
            size_t c = np.find(',', i);
            std::string tok = np.substr(i, c == std::string::npos ? std::string::npos : c - i);
            while (!tok.empty() && tok[0] == ' ') tok.erase(tok.begin());
            if (!tok.empty() && (tok == "*" || host == tok ||
                (tok[0] == '.' && host.size() > tok.size() &&
                 host.rfind(tok) == host.size() - tok.size())))
                return "";
            if (c == std::string::npos) break;
            i = c + 1;
        }
    }
    return p;
}

static std::vector<uint8_t> http_get(const std::string& url) {
#ifdef _WIN32
    std::wstring w = u8w(url);
    URL_COMPONENTSW uc;
    std::memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc))
        throw std::runtime_error("URL 解析失败: " + url);

    std::string proxy = pick_proxy(url);
    if (!proxy.empty()) {
        // 走进程级默认代理配置，请求仍用 path（HTTPS 隧道内不能发完整 URL）
        WINHTTP_PROXY_INFO pi;
        std::wstring wp = u8w(proxy);
        std::vector<wchar_t> pbuf(wp.begin(), wp.end());
        pbuf.push_back(L'\0');
        pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        pi.lpszProxy = pbuf.data();
        pi.lpszProxyBypass = nullptr;
        WinHttpSetDefaultProxyConfiguration(&pi);
    }
    HINTERNET sess = WinHttpOpen(L"xcc-sysroot/0.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) throw std::runtime_error("WinHttpOpen 失败");
    WinHttpSetTimeouts(sess, 30000, 30000, 300000, 300000);
    HINTERNET conn = WinHttpConnect(sess, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(sess); throw std::runtime_error("WinHttpConnect 失败: " + url); }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    // 镜像/CDN 常回 302，WinHTTP 默认不跟随，必须显式开启
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); throw std::runtime_error("WinHttpOpenRequest 失败"); }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_REQUEST_DATA, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    std::vector<uint8_t> out;
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status >= 400) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
            throw std::runtime_error("HTTP " + std::to_string(status) + ": " + url);
        }
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail) {
            std::vector<uint8_t> chunk(avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
            out.insert(out.end(), chunk.begin(), chunk.begin() + got);
        }
    } else {
        DWORD err = GetLastError();
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
        throw std::runtime_error("HTTP 请求失败 (GetLastError=" + std::to_string(err) + "): " + url);
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
    if (out.empty()) throw std::runtime_error("HTTP 返回空内容: " + url);
    return out;
#else
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init 失败");
    std::vector<uint8_t> out;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "xcc-sysroot/0.4");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                     +[](char* p, size_t n, size_t nm, void* u) -> size_t {
                         auto* v = static_cast<std::vector<uint8_t>*>(u);
                         v->insert(v->end(), p, p + n * nm);
                         return n * nm;
                     });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(c);
    long httpcode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpcode);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("curl 失败: ") + curl_easy_strerror(rc) + " " + url);
    if (httpcode >= 400) throw std::runtime_error("HTTP " + std::to_string(httpcode) + ": " + url);
    return out;
#endif
}

// --------------------------------------------------------------------------
// 解压：gzip / xz / zstd
// --------------------------------------------------------------------------
struct GzResult {
    std::vector<uint8_t> out;
    size_t consumed = 0;
    bool ok = false;
};

// 单段 gzip 解压（apk 是多段拼接，需要精确的 consumed）
static GzResult gz_decompress_part(const uint8_t* data, size_t n) {
    GzResult r;
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 31) != Z_OK) return r;
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = (uInt)n;
    std::vector<uint8_t> buf(1 << 20);
    while (true) {
        z.next_out = buf.data();
        z.avail_out = (uInt)buf.size();
        int rc = inflate(&z, Z_NO_FLUSH);
        size_t produced = buf.size() - z.avail_out;
        r.out.insert(r.out.end(), buf.begin(), buf.begin() + produced);
        if (rc == Z_STREAM_END) { r.ok = true; break; }
        if (rc != Z_OK) break;
        // avail_in 归零不代表结束：zlib 会把输入吃进内部状态后继续吐数据，
        // 只有"既没产出又没输入"才是真的到底
        if (produced == 0 && z.avail_in == 0) { r.ok = true; break; }
    }
    r.consumed = n - z.avail_in;
    inflateEnd(&z);
    return r;
}

// 拼接所有 gzip 段（Alpine 的 APKINDEX.tar.gz 是"签名段 + 数据段"两段拼接，
// Python 的 gzip/tarfile 会自动处理，这里必须显式拼起来才能拿到完整 tar）
static bool gz_decompress(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    size_t off = 0, count = 0;
    while (off + 2 <= n && data[off] == 0x1f && data[off + 1] == 0x8b) {
        GzResult r = gz_decompress_part(data + off, n - off);
        if (!r.ok || r.out.empty()) break;
        out.insert(out.end(), r.out.begin(), r.out.end());
        if (r.consumed == 0) break;
        off += r.consumed;
        count++;
    }
    return count > 0 && !out.empty();
}

static bool xz_decompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return false;
    s.next_in = in;
    s.avail_in = n;
    std::vector<uint8_t> buf(1 << 20);
    bool ok = false;
    while (true) {
        s.next_out = buf.data();
        s.avail_out = buf.size();
        lzma_ret rc = lzma_code(&s, LZMA_RUN);
        size_t produced = buf.size() - s.avail_out;
        out.insert(out.end(), buf.begin(), buf.begin() + produced);
        if (rc == LZMA_STREAM_END) { ok = true; break; }
        if (rc != LZMA_OK) break;
        if (produced == 0 && s.avail_in == 0) { ok = true; break; }
    }
    lzma_end(&s);
    return ok;
}

static bool zstd_decompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
#ifdef XCC_USE_LIBZSTD
    unsigned long long cap = ZSTD_getFrameContentSize(in, n);
    if (cap == ZSTD_CONTENTSIZE_ERROR || cap == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    out.resize((size_t)cap);
    size_t rc = ZSTD_decompress(out.data(), out.size(), in, n);
    return !ZSTD_isError(rc);
#else
    // 无 libzstd：调外部 zstd -dc。找不到就明确报错，不静默跳过
    fs::path tmp = CACHE / "zstd-in.tmp";
    std::error_code ec;
    fs::create_directories(CACHE, ec);
    if (!write_file(tmp, std::vector<uint8_t>(in, in + n))) return false;
    std::string cmd = "zstd -dc \"" + pathstr(tmp) + "\"";
    FILE* p =
#ifdef _WIN32
        _popen(cmd.c_str(), "rb");
#else
        popen(cmd.c_str(), "r");
#endif
    if (!p) {
        fs::remove(tmp, ec);
        die("解压 data.tar.zst 需要 zstd：请安装 zstd，或用 -DXCC_USE_LIBZSTD -lzstd 重新编译采集器");
    }
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), p)) > 0)
        out.insert(out.end(), chunk, chunk + got);
    int rc =
#ifdef _WIN32
        _pclose(p);
#else
        pclose(p);
#endif
    fs::remove(tmp, ec);
    return rc == 0 && !out.empty();
#endif
}

static bool decompress_any(const std::string& name, const std::vector<uint8_t>& body,
                           std::vector<uint8_t>& out) {
    if (name.find(".xz") != std::string::npos) return xz_decompress(body.data(), body.size(), out);
    if (name.find(".gz") != std::string::npos) return gz_decompress(body.data(), body.size(), out);
    if (name.find(".zst") != std::string::npos) return zstd_decompress(body.data(), body.size(), out);
    return false;
}

// --------------------------------------------------------------------------
// tar（ustar + GNU 长名 + pax path=）
// --------------------------------------------------------------------------
struct TarMember {
    std::string name;
    char type = '0';
    std::string link;
    std::vector<uint8_t> data;
    bool is_file = false;
    bool is_symlink = false;
};

static uint64_t tar_size(const uint8_t* h) {
    if (h[124] & 0x80) {                       // GNU base-256
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

static void tar_parse(const std::vector<uint8_t>& blob,
                      const std::function<void(const TarMember&)>& cb) {
    size_t off = 0;
    std::string long_name, pax_path;
    if (std::getenv("XCC_TAR_DEBUG"))
        std::fprintf(stderr, "[tar] blob=%zu bytes\n", blob.size());
    while (off + 512 <= blob.size()) {
        const uint8_t* h = blob.data() + off;
        bool empty = true;
        for (int i = 0; i < 512; i++) if (h[i]) { empty = false; break; }
        if (empty) break;

        std::string name = tar_str(h, 100);
        std::string prefix = tar_str(h + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        char type = (char)h[156];
        std::string link = tar_str(h + 157, 100);
        uint64_t size = tar_size(h);
        off += 512;

        std::vector<uint8_t> data;
        if (size && off + size <= blob.size())
            data.assign(blob.begin() + off, blob.begin() + off + size);
        off += (size_t)((size + 511) / 512 * 512);

        if (type == 'L') {                       // GNU 长名
            long_name = std::string(reinterpret_cast<const char*>(data.data()),
                                    strnlen(reinterpret_cast<const char*>(data.data()), data.size()));
            continue;
        }
        if (type == 'x' || type == 'g') {        // pax 扩展头
            std::string txt(reinterpret_cast<const char*>(data.data()), data.size());
            std::smatch m;
            if (std::regex_search(txt, m, std::regex("path=([^\n]*)\n"))) pax_path = m[1];
            continue;
        }
        if (!long_name.empty()) { name = long_name; long_name.clear(); }
        if (!pax_path.empty()) { name = pax_path; pax_path.clear(); }

        while (name.rfind("./", 0) == 0) name = name.substr(2);
        if (name.empty() || name == "." || name.rfind("../", 0) == 0 || name == "..") continue;

        TarMember mem;
        mem.name = name;
        mem.type = type;
        mem.link = link;
        mem.is_symlink = (type == '2');
        mem.is_file = (type == '0' || type == '\0' || type == '7');
        if (mem.is_file || mem.is_symlink) mem.data = std::move(data);
        if (std::getenv("XCC_TAR_DEBUG"))
            std::fprintf(stderr, "[tar] %s type=%c size=%llu got=%zu\n",
                         name.c_str(), type, (unsigned long long)size, mem.data.size());
        cb(mem);
    }
}

// --------------------------------------------------------------------------
// ar（deb 容器）
// --------------------------------------------------------------------------
static void ar_members(const std::vector<uint8_t>& buf,
                       const std::function<void(const std::string&, const uint8_t*, size_t)>& cb) {
    if (buf.size() < 8 || std::memcmp(buf.data(), "!<arch>\n", 8) != 0)
        die("不是合法的 ar 归档");
    size_t off = 8;
    while (off + 60 <= buf.size()) {
        const uint8_t* h = buf.data() + off;
        std::string name = tar_str(h, 16);
        while (!name.empty() && name.back() == '/') name.pop_back();
        std::string sizef = tar_str(h + 48, 10);
        if (sizef.empty()) break;
        size_t size = (size_t)std::strtoull(sizef.c_str(), nullptr, 10);
        const uint8_t* body = buf.data() + off + 60;
        if (off + 60 + size > buf.size()) break;
        cb(name, body, size);
        off += 60 + size;
        if (off % 2) off++;
    }
}

// --------------------------------------------------------------------------
// 锁定文件 pins/<target>.json
// --------------------------------------------------------------------------
struct PinItem {
    std::string key;      // 逻辑标识：包名 / deb 角色 / wasi tarball
    std::string url;
    std::string file;
    std::string version;
    std::string sha256;
};

struct Pins {
    std::string branch;                 // Alpine 分支
    std::vector<PinItem> items;
    bool loaded = false;
    bool update = false;                // --update：忽略锁定，解析最新后覆盖
    fs::path path;

    PinItem* find(const std::string& key) {
        if (update) return nullptr;
        for (auto& it : items) if (it.key == key) return &it;
        return nullptr;
    }
    void set(const std::string& key, const std::string& url, const std::string& file,
             const std::string& version, const std::string& sha) {
        for (auto& it : items) {
            if (it.key == key) {
                it.url = url; it.file = file; it.version = version; it.sha256 = sha;
                return;
            }
        }
        items.push_back({key, url, file, version, sha});
    }
};

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

static std::string pins_to_json(const Pins& p) {
    std::string s = "{\n";
    s += "  \"branch\": \"" + json_escape(p.branch) + "\",\n";
    s += "  \"items\": [\n";
    for (size_t i = 0; i < p.items.size(); i++) {
        const auto& it = p.items[i];
        s += "    {\"key\": \"" + json_escape(it.key) +
             "\", \"url\": \"" + json_escape(it.url) +
             "\", \"file\": \"" + json_escape(it.file) +
             "\", \"version\": \"" + json_escape(it.version) +
             "\", \"sha256\": \"" + it.sha256 + "\"}";
        s += (i + 1 < p.items.size()) ? ",\n" : "\n";
    }
    s += "  ]\n}\n";
    return s;
}

static void pins_save(const Pins& p) {
    std::error_code ec;
    fs::create_directories(p.path.parent_path(), ec);
    std::ofstream f(p.path, std::ios::binary);
    f << pins_to_json(p);
}

// 极简解析：只需要 branch 与 items 数组
static void pins_load(Pins& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p.path, ec)) return;
    std::ifstream pf(p.path, std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());

    // 从 pos 处找 "key": "value"，返回 value（pos 指向 key 的起始）
    auto field = [](const std::string& s, size_t pos) -> std::string {
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos) return "";
        size_t i = s.find('"', colon);
        if (i == std::string::npos) return "";
        size_t j = i + 1;
        std::string out;
        while (j < s.size()) {
            if (s[j] == '\\') { out += s[j + 1]; j += 2; continue; }
            if (s[j] == '"') break;
            out += s[j++];
        }
        return out;
    };
    size_t b = src.find("\"branch\"");
    if (b != std::string::npos) p.branch = field(src, b);

    size_t ia = src.find("\"items\"");
    if (ia == std::string::npos) { p.loaded = true; return; }
    size_t pos = ia;
    while (true) {
        size_t o = src.find('{', pos);
        if (o == std::string::npos) break;
        size_t close = src.find('}', o);
        if (close == std::string::npos) break;
        std::string obj = src.substr(o, close - o);
        auto get = [&](const char* k) -> std::string {
            size_t kp = obj.find(std::string("\"") + k + "\"");
            if (kp == std::string::npos) return "";
            size_t q = obj.find('"', kp + std::strlen(k) + 2);
            if (q == std::string::npos) return "";
            size_t e = q + 1;
            std::string out;
            while (e < obj.size()) {
                if (obj[e] == '\\') { out += obj[e + 1]; e += 2; continue; }
                if (obj[e] == '"') break;
                out += obj[e++];
            }
            return out;
        };
        PinItem it;
        it.key = get("key");
        it.url = get("url");
        it.file = get("file");
        it.version = get("version");
        it.sha256 = get("sha256");
        if (!it.key.empty()) p.items.push_back(it);
        pos = close + 1;
        if (src.find(']', pos) != std::string::npos && src.find('{', pos) == std::string::npos) break;
    }
    p.loaded = true;
}

// --------------------------------------------------------------------------
// 下载（带缓存 + sha256 校验 + 锁定）；key 为空表示不参与锁定
// --------------------------------------------------------------------------
static fs::path download(const std::string& url, const fs::path& dest, const std::string& note,
                         Pins& pins, const std::string& key = "", const std::string& version = "") {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::string fname = pathstr(dest.filename());
    PinItem* pin = key.empty() ? nullptr : pins.find(key);

    if (fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) > 0) {
        xlog("  校验缓存 " + fname + " ...");
        std::string sha = sha256_hex(read_file(dest));   // 始终从文件实算，不信任旧 sidecar
        std::ofstream(upath(pathstr(dest) + ".sha256")) << sha << "\n";
        sha = sha.substr(0, 64);
        if (pin && !pin->sha256.empty() && pin->sha256 != sha)
            die("缓存文件与锁定不符：" + fname + "\n  锁定 sha256 " + pin->sha256 +
                "\n  实际 sha256 " + sha + "\n  请删除该文件后重试，或用 --update 接受新内容");
        if (pin && pin->url != url)
            xlog("  注意：上游已有新版本（锁定 " + pin->version + " → 现有 " + version +
                 "），仍按锁定采集；--update 可跟上");
        if (!key.empty()) { pins.set(key, url, fname, version, sha); pins_save(pins); }
        xlogf("缓存命中 %s%s", fname.c_str(), note.empty() ? "" : (" (" + note + ")").c_str());
        return dest;
    }

    xlogf("下载 %s%s", fname.c_str(), note.empty() ? "" : (" (" + note + ")").c_str());
    std::vector<uint8_t> data = http_get(url);
    xlogf("  -> %.2f MB", data.size() / 1048576.0);
    std::string sha = sha256_hex(data);
    if (pin && !pin->sha256.empty() && pin->sha256 != sha)
        die("下载内容与锁定 sha256 不符：" + fname + "\n  锁定 " + pin->sha256 + "\n  实际 " + sha +
            "\n  上游包已变更或被替换；确认无误后用 --update 刷新锁定");
    if (!write_file(dest, data)) die("写入失败: " + pathstr(dest));
    std::ofstream(upath(pathstr(dest) + ".sha256")) << sha << "\n";
    if (!key.empty()) {
        pins.set(key, url, fname, version, sha);
        pins_save(pins);
    }
    return dest;
}

// --------------------------------------------------------------------------
// Debian 目录页挑选
// --------------------------------------------------------------------------
static std::vector<std::string> ver_parts(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    bool digit = false;
    for (char c : v) {
        bool d = (c >= '0' && c <= '9');
        if (!cur.empty() && d != digit) { out.push_back(cur); cur.clear(); }
        cur += c;
        digit = d;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
static bool version_less(const std::string& a, const std::string& b) {
    auto pa = ver_parts(a), pb = ver_parts(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; i++) {
        if (i >= pa.size()) return true;
        if (i >= pb.size()) return false;
        bool da = std::isdigit((unsigned char)pa[i][0]) != 0;
        bool db = std::isdigit((unsigned char)pb[i][0]) != 0;
        if (da && db) {
            long long ia = std::strtoll(pa[i].c_str(), nullptr, 10);
            long long ib = std::strtoll(pb[i].c_str(), nullptr, 10);
            if (ia != ib) return ia < ib;
        } else if (pa[i] != pb[i]) {
            return pa[i] < pb[i];
        }
    }
    return false;
}

static std::string deb_pick(Pins& pins, const std::string& key, const std::string& deb_dir,
                            const std::string& pattern, const std::string& arch) {
    if (PinItem* pin = pins.find(key))
        if (pin->url.rfind(deb_dir + "/", 0) == 0) return pin->url;

    std::string html = [&] {
        std::vector<uint8_t> hb = http_get(deb_dir + "/");
        return std::string(reinterpret_cast<const char*>(hb.data()), hb.size());
    }();
    std::string pat = pattern;
    size_t ph = pat.find("%s");
    if (ph != std::string::npos) pat.replace(ph, 2, arch);
    std::regex re(pat);
    std::map<std::string, std::string, bool (*)(const std::string&, const std::string&)> cands(&version_less);
    std::regex href("href=\"([^\"]+?\\.deb)\"");
    for (std::sregex_iterator it(html.begin(), html.end(), href), end; it != end; ++it) {
        std::string name = (*it)[1];
        size_t sl = name.rfind('/');
        if (sl != std::string::npos) name = name.substr(sl + 1);
        std::smatch m;
        if (std::regex_search(name, m, re) && m.size() > 1) cands[m[1]] = name;
    }
    if (cands.empty()) die("Debian 目录页未找到匹配包: " + deb_dir + " (pattern " + pat + ")");
    return deb_dir + "/" + cands.rbegin()->second;
}

static void tar_extract_to(const std::vector<uint8_t>& tar, const fs::path& destdir) {
    std::error_code ec;
    fs::create_directories(destdir, ec);
    size_t fails = 0;
    std::string first_fail;
    tar_parse(tar, [&](const TarMember& m) {
        fs::path dst = destdir / upath(m.name);
        if (m.is_symlink) {
            fs::create_directories(dst.parent_path(), ec);
            fs::remove(dst, ec);
            fs::create_symlink(upath(m.link), dst, ec);
        } else if (m.is_file) {
            // 写失败必须暴露：静默缺文件只会在几小时后的链接期变成诡异的 not found
            if (!write_file(dst, m.data)) {
                fails++;
                if (first_fail.empty()) first_fail = pathstr(dst);
            }
        }
    });
    if (fails)
        die("解包写入失败 " + std::to_string(fails) + " 个文件（例如 " + first_fail +
            "）：检查磁盘权限或安全软件拦截");
}

static void deb_extract(const fs::path& deb_path, const fs::path& destdir) {
    std::vector<uint8_t> buf = read_file(deb_path);
    std::vector<uint8_t> tar_bytes;
    std::string tname;
    ar_members(buf, [&](const std::string& name, const uint8_t* body, size_t size) {
        if (tar_bytes.empty() && name.rfind("data.tar", 0) == 0) {
            std::vector<uint8_t> in(body, body + size);
            if (decompress_any(name, in, tar_bytes)) tname = name;
        }
    });
    if (tname.empty()) die("deb 中没有可解压的 data.tar: " + pathstr(deb_path));
    tar_extract_to(tar_bytes, destdir);
}

// --------------------------------------------------------------------------
// Alpine
// --------------------------------------------------------------------------
struct ApkPkg { std::string version; };

static std::map<std::string, ApkPkg> alpine_index(const std::string& arch, const std::string& branch) {
    std::string url = ALPINE_MIRROR + "/" + branch + "/main/" + arch + "/APKINDEX.tar.gz";
    fs::path dest = CACHE / ("APKINDEX-" + arch + ".tar.gz");
    Pins dummy;                      // 索引本身不锁定（内容随上游滚动）
    dummy.path = PINS / "_index.json";
    download(url, dest, "alpine index", dummy);

    std::vector<uint8_t> gz = read_file(dest), tar;
    if (!gz_decompress(gz.data(), gz.size(), tar)) die("APKINDEX 解压失败");
    std::string raw;
    tar_parse(tar, [&](const TarMember& m) {
        if (m.name == "APKINDEX" && raw.empty())
            raw.assign(reinterpret_cast<const char*>(m.data.data()), m.data.size());
    });
    if (raw.empty()) die("APKINDEX 为空");

    std::map<std::string, ApkPkg> pkgs;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t end = raw.find("\n\n", pos);
        std::string rec = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        std::string P, V, A = arch;
        size_t rp = 0;
        while (rp <= rec.size()) {
            size_t le = rec.find('\n', rp);
            std::string line = rec.substr(rp, le == std::string::npos ? std::string::npos : le - rp);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon), v = line.substr(colon + 1);
                if (k == "P" && P.empty()) P = v;
                else if (k == "V" && V.empty()) V = v;
                else if (k == "A" && A.empty()) A = v;
            }
            if (le == std::string::npos) break;
            rp = le + 1;
        }
        if (!P.empty() && !V.empty() && A == arch) pkgs[P] = ApkPkg{V};
        if (end == std::string::npos) break;
        pos = end + 2;
    }
    return pkgs;
}

static void apk_extract(const fs::path& apk_path, const fs::path& destdir) {
    std::vector<uint8_t> buf = read_file(apk_path);
    std::vector<uint8_t> last;
    size_t off = 0;
    while (off + 2 <= buf.size() && buf[off] == 0x1f && buf[off + 1] == 0x8b) {
        GzResult r = gz_decompress_part(buf.data() + off, buf.size() - off);
        if (!r.ok || r.out.empty()) break;
        last = std::move(r.out);
        off += r.consumed;
        if (r.consumed == 0) break;
    }
    if (last.empty())
        die("apk 格式异常（不是有效的 gzip 流；可能是下载被截断或代理返回了错误页）: " + pathstr(apk_path));
    tar_extract_to(last, destdir);
}

// --------------------------------------------------------------------------
// 文件系统辅助
// --------------------------------------------------------------------------
static std::vector<fs::path> list_dir(const fs::path& d) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(d, ec)) return out;
    for (fs::directory_iterator it(d, ec), end; it != end; it.increment(ec))
        out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

static size_t copy_tree(const fs::path& src, const fs::path& dst, const std::string& label) {
    size_t n = 0, fails = 0;
    std::string first_fail;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, ec), end; it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (fs::is_directory(p, ec)) continue;
        fs::path rel = fs::relative(p, src, ec);
        fs::path t = dst / rel;
        fs::create_directories(t.parent_path(), ec);
        fs::path real = p;
        if (fs::is_symlink(p, ec)) {
            fs::path r = fs::canonical(p, ec);
            if (!ec) real = r;
        }
        if (fs::is_regular_file(real, ec)) {
            std::error_code ec2;
            fs::copy_file(real, t, fs::copy_options::overwrite_existing, ec2);
            if (ec2) {
                fails++;
                if (first_fail.empty()) first_fail = pathstr(t);
            } else {
                n++;
            }
        }
    }
    if (!label.empty()) xlogf("  %-26s %zu 个文件", label.c_str(), n);
    if (fails)
        die("复制失败 " + std::to_string(fails) + " 个文件（例如 " + first_fail +
            "）：检查磁盘权限或安全软件拦截");
    return n;
}

static bool wild_match(const std::string& pat, const std::string& s) {
    size_t pi = 0, si = 0, star = std::string::npos, ss = 0;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) { pi++; si++; }
        else if (pi < pat.size() && pat[pi] == '*') { star = pi++; ss = si; }
        else if (star != std::string::npos) { pi = star + 1; si = ++ss; }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '*') pi++;
    return pi == pat.size();
}

static void fix_linker_scripts(const fs::path& libdir) {
    std::error_code ec;
    for (const fs::path& f : list_dir(libdir)) {
        if (f.extension() != ".so") continue;
        std::vector<uint8_t> raw = read_file(f);
        if (raw.empty()) continue;
        std::string txt(reinterpret_cast<const char*>(raw.data()), raw.size());
        if (txt.find("GROUP") == std::string::npos &&
            txt.find("OUTPUT_FORMAT") == std::string::npos) continue;
        std::string orig = txt;
        txt = std::regex_replace(txt,
            std::regex("/(?:usr/)?lib\\d*/[^ )]*/([A-Za-z0-9_.+-]+\\.(?:a|so[.0-9]*))"), "$1");
        txt = std::regex_replace(txt,
            std::regex("/(?:usr/)?lib\\d*/(ld-musl-[A-Za-z0-9_-]+\\.so\\.[0-9.]+)"), "$1");
        txt = std::regex_replace(txt,
            std::regex("/(?:usr/)?lib\\d*/(ld-linux-[A-Za-z0-9_-]+\\.so\\.[0-9.]+)"), "$1");
        txt = std::regex_replace(txt,
            std::regex("/(?:usr/)?lib\\d*/(libc\\.musl-[A-Za-z0-9_-]+\\.so\\.1)"), "$1");
        if (txt != orig) {
            write_file(f, std::vector<uint8_t>(txt.begin(), txt.end()));
            xlog("  修正链接脚本 " + pathstr(f.filename()));
        }
    }
}

static std::string join_names(const std::vector<std::string>& v) {
    std::vector<std::string> s = v;
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        out += "\"" + json_escape(s[i]) + "\"";
        if (i + 1 < s.size()) out += ", ";
    }
    return out;
}

static std::vector<std::string> lib_names(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::path& p : list_dir(dir))
        if (fs::is_regular_file(p, ec) && p.extension() == ".a") out.push_back(pathstr(p.filename()));
    return out;
}

static void write_manifest(const fs::path& out, const std::string& body) {
    write_file(out / "xcc.json", std::vector<uint8_t>(body.begin(), body.end()));
}

// --------------------------------------------------------------------------
// target 配方
// --------------------------------------------------------------------------
struct Recipe {
    std::string family;
    std::string alpine_arch, deb_arch, triplet, mw_arch, version;
};
static std::map<std::string, Recipe> RECIPES = {
    {"aarch64-linux-musl", {"musl", "aarch64", "arm64", "aarch64-linux-musl", "", ""}},
    {"x86_64-linux-musl",  {"musl", "x86_64", "amd64", "x86_64-linux-musl", "", ""}},
    {"riscv64-linux-musl", {"musl", "riscv64", "riscv64", "riscv64-linux-musl", "", ""}},
    {"wasm32-wasi",        {"wasi", "", "", "wasm32-wasi", "", "wasi-sdk-25"}},
};

// --------------------------------------------------------------------------
// musl
// --------------------------------------------------------------------------
static void build_musl(const std::string& target, const Recipe& rc, Pins& pins) {
    const std::string& arch = rc.alpine_arch;
    const std::string& triplet = rc.triplet;
    std::string gnu_triplet = triplet;
    size_t mp = gnu_triplet.find("-musl");
    if (mp != std::string::npos) gnu_triplet.replace(mp, 5, "-gnu");

    fs::path out = SYSROOT / target;
    std::error_code ec;
    if (fs::exists(out, ec)) fs::remove_all(out, ec);
    fs::create_directories(out / "include", ec);
    fs::create_directories(out / "lib", ec);

    fs::path stage = BUILD / "stage" / target;
    if (fs::exists(stage, ec)) fs::remove_all(stage, ec);

    // 1) Alpine：musl 头 / libc.a / CRT / C++ 运行时
    //    注意：各架构的 apk 文件名相同，缓存必须按架构加前缀隔离，否则串包
    std::map<std::string, ApkPkg> index;
    bool have_index = false;
    auto apk_url = [&](const std::string& name) -> std::pair<std::string, std::string> {
        if (PinItem* pin = pins.find(name)) return {pin->version, pin->version};
        if (!have_index) { index = alpine_index(arch, pins.branch); have_index = true; }
        auto it = index.find(name);
        if (it == index.end()) die("Alpine 索引中未找到包: " + name);
        return {it->second.version, it->second.version};
    };
    for (const std::string& name : {"musl-dev", "musl", "libstdc++-dev"}) {
        std::string ver = apk_url(name).first;
        std::string url = ALPINE_MIRROR + "/" + pins.branch + "/main/" + arch + "/" +
                          name + "-" + ver + ".apk";
        fs::path apk = download(url, CACHE / (arch + "-" + name + "-" + ver + ".apk"), ver,
                                pins, name, ver);
        apk_extract(apk, stage / name);
        xlog("解包 " + pathstr(apk.filename()) + " (" + ver + ")");
    }

    if (fs::is_directory(stage / "musl-dev" / "usr" / "include", ec))
        copy_tree(stage / "musl-dev" / "usr" / "include", out / "include", "musl 头文件");

    for (const char* pat : {"musl/lib/ld-musl-*.so.1", "musl/usr/lib/ld-musl-*.so.1",
                            "musl/lib/libc.musl-*.so.1", "musl/usr/lib/libc.musl-*.so.1"}) {
        fs::path base = stage / fs::path(pat).parent_path();
        std::string fname = fs::path(pat).filename().string();
        for (const fs::path& f : list_dir(base)) {
            if (fs::is_regular_file(f, ec) && wild_match(fname, pathstr(f.filename()))) {
                fs::copy_file(f, out / "lib" / f.filename(), fs::copy_options::overwrite_existing, ec);
                xlogf("  %-26s %s", "动态链接器", pathstr(f.filename()).c_str());
            }
        }
    }

    size_t nlib = 0;
    fs::path musllib = stage / "musl-dev" / "usr" / "lib";
    if (fs::is_directory(musllib, ec)) {
        for (const fs::path& f : list_dir(musllib)) {
            std::string fn = pathstr(f.filename());
            if (!fs::is_regular_file(f, ec)) continue;
            if (f.extension() == ".a" || f.extension() == ".o" ||
                fn.rfind(".so", fn.size() - 3) != std::string::npos || fn.find(".so.") != std::string::npos) {
                fs::copy_file(f, out / "lib" / f.filename(), fs::copy_options::overwrite_existing, ec);
                nlib++;
            }
        }
    }
    xlogf("  %-26s %zu 个文件", "musl 库/CRT", nlib);

    // 2) libstdc++-dev：C++ 头 + 静态库
    fs::path cxx_stage = stage / "libstdc++-dev";
    std::string cxx_ver;
    fs::path cxx_inc = cxx_stage / "usr" / "include" / "c++";
    if (fs::is_directory(cxx_inc, ec)) {
        for (const fs::path& v : list_dir(cxx_inc)) {
            if (!fs::is_directory(v, ec)) continue;
            cxx_ver = pathstr(v.filename());
            copy_tree(v, out / "include" / "c++" / v.filename(), "C++ 头文件 (" + cxx_ver + ")");
            for (const fs::path& sub : list_dir(v)) {
                std::string sn = pathstr(sub.filename());
                if (!fs::is_directory(sub, ec) || sn == triplet || sn.find('-') == std::string::npos) continue;
                std::string cpu = sn.substr(0, sn.find('-'));
                if (cpu == "aarch64" || cpu == "x86_64" || cpu == "riscv64" ||
                    cpu == "armv7" || cpu == "ppc64le" || cpu == "s390x" || cpu == "i586")
                    copy_tree(sub, out / "include" / "c++" / v.filename() / triplet,
                              "C++ 平台子头 -> " + triplet);
            }
        }
    }
    if (cxx_ver.empty()) xlog("  警告：未拿到 C++ 头文件，C++ 将不可用");

    for (const char* f : {"libstdc++.a", "libstdc++.so", "libstdc++exp.a", "libstdc++fs.a", "libsupc++.a"}) {
        fs::path p = cxx_stage / "usr" / "lib" / f;
        if (fs::is_regular_file(p, ec))
            fs::copy_file(p, out / "lib" / f, fs::copy_options::overwrite_existing, ec);
    }
    if (!cxx_ver.empty()) xlogf("  %-26s %s", "C++ 静态库", "libstdc++.a (musl 原生)");

    // 3) gcc 包：crtbegin/crtend/libgcc（必须 musl 原生，Debian 的引用 glibc 专有符号）
    for (const std::string& name : {"libgcc-static", "gcc"}) {
        std::string ver = apk_url(name).first;
        std::string url = ALPINE_MIRROR + "/" + pins.branch + "/main/" + arch + "/" +
                          name + "-" + ver + ".apk";
        fs::path apk = download(url, CACHE / (arch + "-" + name + "-" + ver + ".apk"), ver,
                                pins, name, ver);
        apk_extract(apk, stage / name);
        xlog("解包 " + pathstr(apk.filename()) + " (" + ver + ")");
    }

    const char* gcc_files[] = {"crtbegin.o", "crtend.o", "crtbeginS.o", "crtendS.o",
                               "crtbeginT.o", "crtbeginD.o", "crtendD.o", "libgcc.a", "libgcc_eh.a"};
    std::string gcc_ver;
    for (const fs::path& cand : list_dir(stage)) {          // stage/<pkg>/usr/lib/gcc/<triplet>/<ver>
        fs::path gdir = cand / "usr" / "lib" / "gcc";
        if (!fs::is_directory(gdir, ec)) continue;
        for (const fs::path& t : list_dir(gdir)) {
            for (const fs::path& gd : list_dir(t)) {
                if (!fs::is_directory(gd, ec)) continue;
                gcc_ver = pathstr(gd.filename());
                fs::path gdst = out / "lib" / "gcc" / gnu_triplet / gd.filename();
                fs::create_directories(gdst, ec);
                size_t n = 0;
                for (const char* f : gcc_files) {
                    fs::path p = gd / f;
                    if (fs::is_regular_file(p, ec)) {
                        fs::copy_file(p, gdst / f, fs::copy_options::overwrite_existing, ec);
                        fs::copy_file(p, out / "lib" / f, fs::copy_options::overwrite_existing, ec);
                        n++;
                    }
                }
                if (n) xlogf("  %-26s %zu 个文件 (gcc %s)", "gcc CRT/运行时", n, gcc_ver.c_str());
            }
        }
    }
    fix_linker_scripts(out / "lib");

    std::string ci = cxx_ver.empty() ? "" : "include/c++/" + cxx_ver;
    std::string cti = cxx_ver.empty() ? "" : "include/c++/" + cxx_ver + "/" + triplet;
    std::string body =
        "{\n  \"target\": \"" + target + "\",\n  \"triplet\": \"" + triplet +
        "\",\n  \"family\": \"musl\",\n  \"cxx_include\": " + (ci.empty() ? "null" : "\"" + ci + "\"") +
        ",\n  \"cxx_triplet_include\": " + (cti.empty() ? "null" : "\"" + cti + "\"") +
        ",\n  \"gcc_version\": " + (gcc_ver.empty() ? "null" : "\"" + gcc_ver + "\"") +
        ",\n  \"alpine_arch\": \"" + arch + "\",\n  \"deb_arch\": \"" + rc.deb_arch +
        "\",\n  \"runtime\": \"libgcc\",\n  \"libs\": [" + join_names(lib_names(out / "lib")) + "]\n}\n";
    write_manifest(out, body);

    xlog("完成 sysroot: " + pathstr(out));
    xlog(std::string("  C++: ") + (cxx_ver.empty() ? "不可用" : "可用 (" + cxx_ver + ")"));
}

// --------------------------------------------------------------------------
// wasi
// --------------------------------------------------------------------------
static void build_wasi(const std::string& target, const Recipe& rc, Pins& pins) {
    std::string ver = rc.version;                       // wasi-sdk-25
    std::string ver2 = ver.substr(ver.rfind('-') + 1) + ".0";
    fs::path out = SYSROOT / target;
    std::error_code ec;
    if (fs::exists(out, ec)) fs::remove_all(out, ec);
    fs::create_directories(out, ec);

    fs::path tarball = CACHE / ("wasi-sysroot-" + ver2 + ".tar.gz");
    if (!fs::is_regular_file(tarball, ec)) {
        bool got = false;
        std::string last_err;
        for (const std::string& tpl : WASI_URLS) {
            std::string url = tpl;
            size_t p = url.find("{ver2}");
            if (p != std::string::npos) url.replace(p, 6, ver2);
            p = url.find("{ver}");
            if (p != std::string::npos) url.replace(p, 5, ver);
            try {
                download(url, tarball, "wasi-sysroot", pins, "wasi-sysroot", ver);
                got = true;
                break;
            } catch (const std::exception& e) {
                last_err = e.what();
                std::error_code ec2;
                fs::remove(tarball, ec2);
                fs::remove(upath(pathstr(tarball) + ".sha256"), ec2);
            }
        }
        if (!got) die("wasi-sysroot 全部源失败: " + last_err);
    } else {
        xlogf("缓存命中 %s", pathstr(tarball.filename()).c_str());
    }

    fs::path stage = BUILD / "stage" / target;
    if (fs::exists(stage, ec)) fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    xlog("解包 " + pathstr(tarball.filename()));
    std::vector<uint8_t> tar, gz = read_file(tarball);
    if (!gz_decompress(gz.data(), gz.size(), tar)) die("wasi-sysroot 解压失败");
    tar_extract_to(tar, stage);

    fs::path src;
    for (const fs::path& d : list_dir(stage))
        if (fs::is_directory(d, ec)) { src = d; break; }
    if (src.empty()) die("wasi-sysroot 解包后未找到顶层目录");
    copy_tree(src / "include", out / "include", "wasi 头文件 (含 C++/v1)");
    copy_tree(src / "lib" / "wasm32-wasi", out / "lib" / "wasm32-wasi", "wasi 库/CRT");

    // compiler-rt builtins（clang 链接 wasm 硬性要求）
    std::string url = deb_pick(pins, "wasm-builtins",
                               DEB_MIRROR + "/l/llvm-toolchain-22",
                               "libclang-rt-22-dev-wasm32_([\\d.]+[-~\\w.+]*?)_%s\\.deb", "all");
    std::string fname = url.substr(url.rfind('/') + 1);
    fs::path deb = download(url, CACHE / fname, "wasm builtins", pins, "wasm-builtins");
    fs::path bdir = stage / "builtins";
    deb_extract(deb, bdir);
    bool found = false;
    for (fs::recursive_directory_iterator it(bdir, ec), end; it != end; it.increment(ec)) {
        if (pathstr(it->path().filename()) == "libclang_rt.builtins-wasm32.a") {
            fs::path dst = out / "lib" / "wasm32-wasi" / "libclang_rt.builtins-wasm32.a";
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
            xlogf("  %-26s %s (%.0f KB)", "wasm builtins", "libclang_rt.builtins-wasm32.a",
                  fs::file_size(dst, ec) / 1024.0);
            found = true;
            break;
        }
    }
    if (!found) die("builtins 包中没有 libclang_rt.builtins-wasm32.a");

    std::string cxx_include;
    if (fs::is_directory(out / "include" / "wasm32-wasi" / "c++" / "v1", ec))
        cxx_include = "include/wasm32-wasi/c++/v1";
    bool builtins = fs::is_regular_file(out / "lib" / "wasm32-wasi" / "libclang_rt.builtins-wasm32.a", ec);
    std::string body =
        "{\n  \"target\": \"" + target + "\",\n  \"triplet\": \"wasm32-wasi\",\n  \"family\": \"wasi\",\n"
        "  \"cxx_include\": " + (cxx_include.empty() ? "null" : "\"" + cxx_include + "\"") +
        ",\n  \"cxx_triplet_include\": null,\n  \"lib_dirs\": [\"lib/wasm32-wasi\"],\n"
        "  \"runtime\": \"compiler-rt\",\n  \"has_builtins\": " + (builtins ? "true" : "false") +
        ",\n  \"libs\": [" + join_names(lib_names(out / "lib" / "wasm32-wasi")) + "]\n}\n";
    write_manifest(out, body);

    xlog("完成 sysroot: " + pathstr(out));
    xlog(std::string("  C++: ") + (cxx_include.empty() ? "不可用" : "可用 (libc++)") +
         " | builtins: " + (builtins ? "自带" : "缺失(按需补)"));
}

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
static void print_list() {
    std::printf("支持的 target:\n");
    for (const auto& kv : RECIPES) {
        std::error_code ec;
        bool ok = fs::is_regular_file(SYSROOT / kv.first / "xcc.json", ec);
        std::printf("  %-22s %-8s %s\n", kv.first.c_str(), kv.second.family.c_str(),
                    ok ? "已安装" : "未采集");
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    ROOT = fs::current_path();
    CACHE = ROOT / "cache";
    SYSROOT = ROOT / "sysroot";
    BUILD = ROOT / "build";
    PINS = ROOT / "pins";

    std::vector<std::string> args(argv + 1, argv + argc);
    bool list = false, all = false, update = false;
    std::string target, branch;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--list") list = true;
        else if (args[i] == "--all") all = true;
        else if (args[i] == "--update") update = true;
        else if (args[i].rfind("--branch=", 0) == 0) branch = args[i].substr(9);
        else if (args[i].rfind("-", 0) != 0) target = args[i];
    }

    if (list || (target.empty() && !all)) { print_list(); return 0; }

    std::vector<std::string> targets;
    if (all) for (const auto& kv : RECIPES) targets.push_back(kv.first);
    else targets.push_back(target);

    try {
        std::error_code ec;
        fs::create_directories(BUILD, ec);
        for (const std::string& t : targets) {
            auto it = RECIPES.find(t);
            if (it == RECIPES.end()) { xlog("错误：未知 target '" + t + "'（--list 查看）"); return 1; }
            Pins pins;
            pins.path = PINS / (t + ".json");
            pins.update = update;
            pins_load(pins);
            if (!branch.empty()) pins.branch = branch;
            if (pins.branch.empty()) pins.branch = "latest-stable";
            if (!pins.loaded || !branch.empty()) pins_save(pins);

            xlog("=== " + t + " ===");
            if (it->second.family == "musl") build_musl(t, it->second, pins);
            else if (it->second.family == "wasi") build_wasi(t, it->second, pins);
            else { xlog("错误：未实现的 family '" + it->second.family + "'"); return 1; }
        }
    } catch (const std::exception& e) {
        xlog(std::string("失败：") + e.what() + "  [" + typeid(e).name() + "]");
        return 1;
    }
    return 0;
}
