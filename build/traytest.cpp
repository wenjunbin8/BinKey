// traytest.cpp —— 启动 BinKey，用消息模拟托盘操作，验证开关持久化
#include <windows.h>
#include <cstdio>
#include <string>
#include <fstream>

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_OPEN_EDITOR = WM_APP + 2;

static std::string readAll(const std::wstring& p) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return "";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, buf + n);
    fclose(f);
    return s;
}

static int findKey(const std::string& s, const std::string& key) {
    size_t i = 0;
    while (i < s.size()) {
        size_t nl = s.find('\n', i);
        std::string line = (nl == std::string::npos) ? s.substr(i) : s.substr(i, nl - i);
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t' || k.back() == '\r')) k.pop_back();
            size_t a = k.find_first_not_of(" \t");
            if (a != std::string::npos) k = k.substr(a);
            if (k == key) {
                std::string v = line.substr(eq + 1);
                size_t h = v.find('#');
                if (h != std::string::npos) v = v.substr(0, h);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
                return v.empty() ? 1 : atoi(v.c_str());
            }
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return -1;
}

static void dump(const char* label, const std::wstring& cfg) {
    std::string s = readAll(cfg);
    int en = findKey(s, "enabled"), so = findKey(s, "socd"), dk = findKey(s, "dks");
    printf("%-28s enabled=%d socd=%d dks=%d\n", label, en, so, dk);
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring cfg = std::wstring(tmp) + L"BinKey_traytest.cfg";
    {
        FILE* f = _wfopen(cfg.c_str(), L"wb");
        fputs("debug = 0\nautostart = 0\nsocd = 1\ngroup = A D mode=snap\ndks = 1\n"
              "rule = key=A on=release tap=D tap_ms=25 min_hold_ms=40 cond=active\n", f);
        fclose(f);
    }
    printf("配置文件: %ls\n", cfg.c_str());

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"D:\\project\\BinKey\\build\\BinKey.exe --config \"" + cfg + L"\"";
    if (!CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("cannot start BinKey (code %lu)\n", GetLastError());
        return 1;
    }
    HWND tray = nullptr;
    for (int i = 0; i < 50 && !tray; ++i) { tray = FindWindowW(L"BinKeyTrayWindow", nullptr); Sleep(100); }
    if (!tray) { printf("FAIL: 找不到托盘窗口\n"); return 1; }
    printf("托盘窗口: %p\n", tray);
    Sleep(800);
    dump("初始", cfg);

    // 1) 菜单项 3：关闭 DKS（SOCD 保持开启）
    PostMessageW(tray, WM_COMMAND, (WPARAM)3, 0);
    Sleep(600);
    dump("菜单关闭 DKS 后", cfg);

    // 2) 托盘左键：禁用全部功能
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN);
    Sleep(600);
    dump("左键禁用全部后", cfg);

    // 3) 托盘左键：重新启用 —— 应只恢复 SOCD，不是两个都开
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN);
    Sleep(600);
    dump("左键重新启用后", cfg);

    int ok = (findKey(readAll(cfg), "enabled") == 1 &&
              findKey(readAll(cfg), "socd") == 1 &&
              findKey(readAll(cfg), "dks") == 0);
    printf("[%s] 重新启用只恢复用户单独设定 (enabled=1 socd=1 dks=0)\n", ok ? "PASS" : "FAIL");

    // 4) 两个单独开关都手动关闭，再左键 —— 这时"启用全部"就是字面意义的全部开启
    PostMessageW(tray, WM_COMMAND, (WPARAM)2, 0);   // 关闭 SOCD
    Sleep(600);
    dump("两个都单独关闭后", cfg);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN);
    Sleep(600);
    dump("再左键启用后", cfg);
    std::string s2 = readAll(cfg);
    printf("[%s] 单独开关都关过时，启用全部 = 两个都开 (enabled=1 socd=1 dks=1)\n",
           (findKey(s2, "enabled") == 1 && findKey(s2, "socd") == 1 && findKey(s2, "dks") == 1) ? "PASS" : "FAIL");

    PostMessageW(tray, WM_COMMAND, (WPARAM)9, 0);   // 退出
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    // 5) 重新启动，确认读回的是上次的设定
    CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    tray = nullptr;
    for (int i = 0; i < 50 && !tray; ++i) { tray = FindWindowW(L"BinKeyTrayWindow", nullptr); Sleep(100); }
    if (tray) {
        Sleep(800);
        dump("重启后读回", cfg);
        std::string s = readAll(cfg);
        int ok2 = (findKey(s, "enabled") == 1 && findKey(s, "socd") == 1 && findKey(s, "dks") == 1);
        printf("[%s] 重启后读回与退出前一致\n", ok2 ? "PASS" : "FAIL");
        PostMessageW(tray, WM_COMMAND, (WPARAM)9, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    DeleteFileW(cfg.c_str());
    return 0;
}
