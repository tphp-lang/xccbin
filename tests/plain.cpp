// 无异常 C++ 样例 —— wasm32-wasi 的 libc++abi 是 no-exception 变体，
// 异常代码在 wasi 上不可链接（__cxa_throw 未提供），ELF 目标用 vector.cpp。
#include <vector>
#include <string>
#include <cstdio>

static int sum_squares(int n) {
    std::vector<int> v;
    for (int i = 1; i <= n; ++i)
        v.push_back(i);
    int s = 0;
    for (int x : v)
        s += x * x;
    return s;
}

int main() {
    std::string msg = "plain-cpp";
    msg += ":";
    int s = sum_squares(10);          // 385
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", s);
    msg += buf;
    msg += (s == 385) ? " ok" : " bad";
    printf("%s\n", msg.c_str());
    return (s == 385) ? 0 : 1;
}
