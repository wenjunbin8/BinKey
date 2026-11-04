// keyprobe <Key> [Key...] —— 打印系统当前按键状态
#include <windows.h>
#include <cstdio>
#include <string>
#include <unordered_map>

static int parseKeySimple(const std::string& s) {
    if (s.size() == 1) {
        int vk = VkKeyScanA(tolower(s[0]));
        if (vk >= 0) return vk;
    }
    static const std::unordered_map<std::string,int> m = {
        {"space",VK_SPACE},{"enter",VK_RETURN},{"tab",VK_TAB},{"esc",VK_ESCAPE},
        {"left",VK_LEFT},{"right",VK_RIGHT},{"up",VK_UP},{"down",VK_DOWN},
        {"shift",VK_SHIFT},{"ctrl",VK_CONTROL},{"alt",VK_MENU},
        {"f1",VK_F1},{"f2",VK_F2},{"scrolllock",VK_SCROLL}
    };
    std::string t; for (char c : s) t += (char)tolower(c);
    auto it = m.find(t);
    return it == m.end() ? 0 : it->second;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        int vk = parseKeySimple(argv[i]);
        if (!vk) { printf("%s: 无法解析\n", argv[i]); continue; }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        printf("%-10s = %s\n", argv[i], down ? "按下" : "未按下");
    }
    return 0;
}
