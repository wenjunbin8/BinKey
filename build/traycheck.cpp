// traycheck.cpp —— 对正在运行的 BinKey 实例模拟真实托盘点击并验证效果
#include <windows.h>
#include <cstdio>
#include <string>

static const UINT WM_TRAY = WM_APP + 1;

static std::string readAll(const std::wstring& p) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return "";
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, buf + n);
    fclose(f);
    return s;
}
static int keyOf(const std::string& s, const std::string& key) {
    size_t i = 0;
    while (i < s.size()) {
        size_t nl = s.find('\n', i);
        std::string line = (nl == std::string::npos) ? s.substr(i) : s.substr(i, nl - i);
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t' || k.back() == '\r')) k.pop_back();
            size_t a = k.find_first_not_of(" \t"); if (a != std::string::npos) k = k.substr(a);
            if (k == key) {
                std::string v = line.substr(eq + 1);
                size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
                return v.empty() ? 1 : atoi(v.c_str());
            }
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return -1;
}
static bool physDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void keyboardProbe(const char* label) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = 'A'; in.ki.dwFlags = 0; SendInput(1, &in, sizeof(in)); Sleep(120);
    in.ki.wVk = 'D'; in.ki.dwFlags = 0; SendInput(1, &in, sizeof(in)); Sleep(200);
    bool a = physDown('A'), d = physDown('D');
    printf("  %-24s A=%s D=%s -> %s\n", label, a ? "按下" : "未按下", d ? "按下" : "未按下",
           (!a && d) ? "SOCD 生效" : (a && d) ? "放行（插件关闭）" : "异常");
    in.ki.wVk = 'D'; in.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &in, sizeof(in)); Sleep(120);
    in.ki.wVk = 'A'; in.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &in, sizeof(in)); Sleep(200);
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring cfg = std::wstring(tmp) + L"BinKey_traycheck.cfg";
    FILE* f = _wfopen(cfg.c_str(), L"wb");
    fputs("debug = 0\nsocd = 1\ngroup = A D mode=snap\ndks = 1\n"
          "rule = key=A on=release tap=P tap_ms=25 min_hold_ms=40 cond=active\n", f);
    fclose(f);

    auto st = [&]() {
        std::string s = readAll(cfg);
        return "enabled=" + std::to_string(keyOf(s, "enabled")) +
               " socd=" + std::to_string(keyOf(s, "socd")) +
               " dks=" + std::to_string(keyOf(s, "dks"));
    };

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"D:\\project\\BinKey\\build\\BinKey.exe --config \"" + cfg + L"\"";
    if (!CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("启动失败\n"); return 1;
    }
    HWND tray = nullptr;
    for (int i = 0; i < 60 && !tray; ++i) { tray = FindWindowW(L"BinKeyTrayWindow", nullptr); Sleep(100); }
    if (!tray) { printf("没有托盘窗口\n"); return 1; }
    Sleep(900);
    printf("初始: %s\n", st().c_str());

    // 真实用户最常见的点击方式：按下 ~500ms 后松手
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(500);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(600);
    printf("左键单击(按住500ms): %s  [%s]\n", st().c_str(), keyOf(readAll(cfg), "enabled") == 0 ? "PASS 已关闭" : "FAIL");
    keyboardProbe("关闭后键盘行为");

    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(500);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(600);
    printf("左键再单击(按住500ms): %s  [%s]\n", st().c_str(), keyOf(readAll(cfg), "enabled") == 1 ? "PASS 已启用" : "FAIL");
    keyboardProbe("启用后键盘行为");

    // 菜单里的"禁用全部功能"
    PostMessageW(tray, WM_COMMAND, 1, 0); Sleep(500);
    printf("菜单\"禁用全部功能\": %s  [%s]\n", st().c_str(), keyOf(readAll(cfg), "enabled") == 0 ? "PASS" : "FAIL");
    keyboardProbe("关闭后键盘行为");

    PostMessageW(tray, WM_COMMAND, 1, 0); Sleep(500);
    printf("菜单\"启用全部功能\": %s  [%s]\n", st().c_str(), keyOf(readAll(cfg), "enabled") == 1 ? "PASS" : "FAIL");

    PostMessageW(tray, WM_COMMAND, 9, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(cfg.c_str());
    return 0;
}
