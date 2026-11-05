// clicktest.cpp —— 模拟真实托盘点击消息序列，检查每次点击是否恰好切换一次
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

// 用真实键盘事件验证"总开关关闭后键盘完全放行"
static void keyboardCheck(const char* label) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = 'A'; in.ki.dwFlags = 0;            // KEYDOWN
    SendInput(1, &in, sizeof(in));                // A down
    Sleep(120);
    in.ki.wVk = 'D'; in.ki.dwFlags = 0;
    SendInput(1, &in, sizeof(in));                // D down
    Sleep(200);
    bool a = physDown('A'), d = physDown('D');
    printf("  %-26s A=%s D=%s  -> %s\n", label, a ? "按下" : "未按下", d ? "按下" : "未按下",
           (!a && d) ? "SOCD 生效" : (a && d) ? "全部放行(插件已关闭)" : "异常");
    in.ki.wVk = 'D'; in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    Sleep(120);
    in.ki.wVk = 'A'; in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    Sleep(200);
}

int main() {
    std::wstring exe = L"D:\\project\\BinKey\\build\\BinKey.exe";
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring cfg = std::wstring(tmp) + L"BinKey_clicktest.cfg";
    FILE* f = _wfopen(cfg.c_str(), L"wb");
    fputs("debug = 1\nautostart = 0\nsocd = 1\ngroup = A D mode=snap\ndks = 1\n"
          "rule = key=A on=release tap=P tap_ms=25 min_hold_ms=40 cond=active\n", f);
    fclose(f);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = std::wstring(exe) + L" --config \"" + cfg + L"\"";
    if (!CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("cannot start BinKey\n"); return 1;
    }
    HWND tray = nullptr;
    for (int i = 0; i < 50 && !tray; ++i) { tray = FindWindowW(L"BinKeyTrayWindow", nullptr); Sleep(100); }
    if (!tray) { printf("FAIL: no tray window\n"); return 1; }
    Sleep(800);

    auto state = [&]() {
        std::string s = readAll(cfg);
        return std::string("enabled=") + std::to_string(keyOf(s, "enabled")) +
               " socd=" + std::to_string(keyOf(s, "socd")) +
               " dks=" + std::to_string(keyOf(s, "dks"));
    };
    printf("初始: %s\n", state().c_str());

    // A) 一次完整点击：DOWN 然后 UP（相隔 40 ms）
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(40);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(700);
    printf("点击 1 (DOWN+UP, 40ms): %s\n", state().c_str());
    keyboardCheck("点击1 后键盘行为");

    // A2) 慢一点点击：按住 400 ms 再松（真实用户常见）
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(400);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(700);
    printf("点击 2 (DOWN+UP, 400ms): %s   -> 期望只翻转一次\n", state().c_str());
    keyboardCheck("点击2 后键盘行为");

    // B) 快速连点两次（每次 DOWN+UP，间隔 150 ms）
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(30);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(150);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(30);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(700);
    printf("点击 3+4 (快速连点两次): %s   -> 期望翻转两次(回到原状态)\n", state().c_str());

    // C) 只发 UP（某些外壳只发 UP）
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(700);
    printf("点击 5 (只有 UP): %s\n", state().c_str());

    // D) 只发 DOWN
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(700);
    printf("点击 6 (只有 DOWN): %s\n", state().c_str());

    // E) 双击消息
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDBLCLK); Sleep(700);
    printf("点击 7 (DBLCLK): %s\n", state().c_str());

    // F) 右键：慢速点击（DOWN + UP 相隔 400ms）应该只弹一次菜单
    PostMessageW(tray, WM_TRAY, 0, WM_RBUTTONDOWN); Sleep(400);
    PostMessageW(tray, WM_TRAY, 0, WM_RBUTTONUP);   Sleep(500);
    printf("右键慢速点击后: %s   -> 期望不变\n", state().c_str());

    PostMessageW(tray, WM_COMMAND, 9, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(cfg.c_str());
    return 0;
}
