// gatecheck.cpp —— 验证"左键关闭插件后 DKS/SOCD 真的不再生效"
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstdlib>

static const UINT WM_TRAY = WM_APP + 1;

static bool physDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void pressRelease(int vk, long long holdMs) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk; in.ki.dwFlags = 0;              SendInput(1, &in, sizeof(in));
    Sleep((DWORD)holdMs);
    in.ki.dwFlags = KEYEVENTF_KEYUP;                      SendInput(1, &in, sizeof(in));
}

int main(int argc, char** argv) {
    std::wstring exe;
    if (argc > 1) for (char* c = argv[1]; *c; ++c) exe.push_back((wchar_t)*c);
    else exe = L"D:\\project\\BinKey\\build\\BinKey.exe";
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring cfg = std::wstring(tmp) + L"BinKey_gatecheck.cfg";
    FILE* f = _wfopen(cfg.c_str(), L"wb");
    fputs("debug = 0\nsocd = 1\ndks = 1\n"
          "rule = key=A on=release tap=P tap_ms=30 min_hold_ms=40 cond=active\n", f);
    fclose(f);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = exe + L" --config \"" + cfg + L"\"";
    if (!CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("启动失败: %lu  cmd=%ls\n", GetLastError(), cmd.c_str());
        return 1;
    }
    HWND tray = nullptr;
    for (int i = 0; i < 60 && !tray; ++i) { tray = FindWindowW(L"BinKeyTrayWindow", nullptr); Sleep(100); }
    if (!tray) { printf("没有托盘窗口\n"); return 1; }
    Sleep(1200);

    auto slowClick = [&]() {
        PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN); Sleep(600);
        PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);   Sleep(500);
    };

    auto tapFired = [&]() {
        bool any = false;
        for (int i = 0; i < 12; ++i) { if (physDown('P')) any = true; Sleep(10); }
        return any;
    };

    // 1) 功能开启：松 A 应触发 P 轻点
    pressRelease('A', 120);
    bool on1 = tapFired();
    printf("开启状态：松 A 后 P 是否被轻点=%s  -> %s\n", on1 ? "是" : "否",
           on1 ? "DKS 生效" : "FAIL");

    // 2) 左键单击关闭（慢点击）
    slowClick();
    printf("左键单击后：托盘左键 = 关闭\n");
    bool off1 = tapFired();
    pressRelease('A', 120);
    bool off2 = tapFired();
    printf("关闭后连测两次：P=%s / P=%s  -> %s\n", off1 ? "按下" : "未按下", off2 ? "按下" : "未按下",
           (!off1 && !off2) ? "PASS 插件真正关闭" : "FAIL 关闭后仍有 DKS 行为");

    // 3) 再左键单击启用
    slowClick();
    pressRelease('A', 120);
    bool on2 = tapFired();
    printf("再次启用后：P=%s  -> %s\n", on2 ? "按下" : "未按下", on2 ? "PASS 重新生效" : "FAIL");

    PostMessageW(tray, WM_COMMAND, 9, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(cfg.c_str());
    return 0;
}
