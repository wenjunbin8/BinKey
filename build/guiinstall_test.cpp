// guiinstall_test.cpp —— 自动驱动安装引导向导，验证流程与按钮
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static HHOOK g_hook = nullptr;
static volatile long g_keyEvents = 0;

static LRESULT llHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) ++g_keyEvents;
    return CallNextHookEx(g_hook, code, w, l);
}

static std::vector<HWND> controls(HWND h, const wchar_t* cls) {
    std::vector<HWND> out;
    for (HWND c = GetWindow(h, GW_CHILD); c; c = GetNextWindow(c, GW_HWNDNEXT)) {
        wchar_t cl[64] = {0};
        GetClassNameW(c, cl, 63);
        if (lstrcmpiW(cl, cls) == 0) out.push_back(c);
    }
    return out;
}

static std::string textOf(HWND h) {
    wchar_t t[512] = {0};
    GetWindowTextW(h, t, 511);
    char u[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, t, -1, u, 1023, nullptr, nullptr);
    return u;
}

static int idOf(HWND h) { return (int)(intptr_t)GetWindowLongPtrW(h, GWLP_ID); }

static bool click(HWND w, int id) {
    for (HWND b : controls(w, L"BUTTON"))
        if (idOf(b) == id) { PostMessageW(b, BM_CLICK, 0, 0); return true; }
    return false;
}

static HWND waitForWnd(const wchar_t* cls, int ms) {
    for (int i = 0; i < ms / 100; ++i) {
        HWND h = FindWindowW(cls, nullptr);
        if (h) return h;
        Sleep(100);
    }
    return nullptr;
}

int main(int argc, char** argv) {
    std::wstring exe = L"D:\\project\\BinKey\\BinKeySetup.exe";
    (void)argc; (void)argv;

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, llHook, nullptr, 0);
    g_keyEvents = 0;

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, (LPWSTR)exe.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("cannot start installer (code %lu)\n", GetLastError());
        return 1;
    }
    printf("[test] installer pid=%lu\n", pi.dwProcessId);

    HWND w = waitForWnd(L"BinKeySetupWizard", 8000);
    if (!w) { printf("[test] FAIL: wizard window not found\n"); return 1; }
    RECT rc; GetWindowRect(w, &rc);
    printf("[test] wizard: %dx%d\n", rc.right - rc.left, rc.bottom - rc.top);

    // 欢迎 -> 位置 -> 选项
    for (int i = 0; i < 3; ++i) { Sleep(700); click(w, 101); }
    Sleep(700);
    auto btns = controls(w, L"BUTTON");
    printf("[test] options page buttons: ");
    for (HWND b : btns) printf("%s(id=%d) ", textOf(b).c_str(), idOf(b));
    printf("\n");
    click(w, 103);   // 安装

    // 安装结果页（含 完成/104），然后进入完成页
    HWND done = nullptr;
    for (int i = 0; i < 800; ++i) {
        Sleep(100);
        if (!IsWindow(w)) break;
        auto list = controls(w, L"BUTTON");
        bool hasDone = false, hasEditor = false, hasExit = false;
        for (HWND b : list) {
            if (idOf(b) == 104) hasDone = true;
            if (idOf(b) == 111) hasEditor = true;
            if (idOf(b) == 113) hasExit = true;
        }
        if (hasDone) { click(w, 104); Sleep(400); continue; }
        if (hasEditor && hasExit && list.size() == 2) { done = w; break; }
    }
    if (!done) { printf("[test] FAIL: never reached the 2-button finish page\n"); return 1; }

    btns = controls(done, L"BUTTON");
    printf("[test] finish page buttons: ");
    for (HWND b : btns) printf("%s(id=%d) ", textOf(b).c_str(), idOf(b));
    printf("\n");

    long duringInstall = g_keyEvents;
    printf("[test] keyboard events during install: %ld\n", duringInstall);

    click(done, 111);   // 打开规则编辑器
    Sleep(1500);
    HWND ed = FindWindowW(L"BinKeyEditor", nullptr);
    printf("[test] editor window: %s\n", ed ? "OK" : "FAIL");
    printf("[test] wizard still open: %s (expect closed)\n", IsWindow(w) ? "YES" : "NO");

    if (ed) {
        RECT er; GetWindowRect(ed, &er);
        printf("[test] editor size %dx%d\n", er.right - er.left, er.bottom - er.top);
        PostMessageW(ed, WM_CLOSE, 0, 0);
    }
    for (int i = 0; i < 100 && WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT; ++i) {}
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (g_hook) UnhookWindowsHookEx(g_hook);
    printf("[test] total keyboard events: %ld\n", g_keyEvents);
    printf("[test] %s\n", duringInstall == 0 ? "PASS (no keystrokes leaked)" : "FAIL (keystrokes leaked)");
    return 0;
}
