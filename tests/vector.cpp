#include <vector>
#include <string>
#include <cstdio>

int main() {
    std::vector<std::string> v{"xcc", "cross", "c++"};
    for (const auto &s : v) {
        std::printf("%s\n", s.c_str());
    }
    std::printf("size=%zu\n", v.size());
    return 0;
}
