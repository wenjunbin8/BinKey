// 小工具：模拟真实按键，用于端到端验证 BinKey
// 用法: simkey.exe <key> <hold_ms>      -> 按下并保持后释放
//       simkey.exe <key> down|up        -> 只按下 / 只释放
#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { printf("用法: simkey <key> <hold_ms>  或  simkey <key> down|up\n"); return 1; }
    std::string k = argv[1];
    std::string mode = argc > 2 ? argv[2] : "100";
    int vk = VkKeyScanA(tolower(k[0]));
    if (vk < 0) { printf("无法解析键: %s\n", k.c_str()); return 1; }

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;

    if (mode == "down" || mode == "up") {
        in.ki.dwFlags = (mode == "up") ? KEYEVENTF_KEYUP : 0;
        SendInput(1, &in, sizeof(INPUT));
        printf("已模拟 %s %s\n", k.c_str(), mode.c_str());
        return 0;
    }

    int hold = atoi(mode.c_str());
    int before = argc > 3 ? atoi(argv[3]) : 0;
    Sleep(before);
    in.ki.dwFlags = 0;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(hold);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
    printf("已模拟 %s 按住 %d ms\n", k.c_str(), hold);
    return 0;
}
