// clicknow.cpp —— 给正在运行的 BinKey 实例模拟一次真实托盘点击（可配置按下时长）
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstdlib>

static const UINT WM_TRAY = WM_APP + 1;

int main(int argc, char** argv) {
    long long hold = (argc > 1) ? atoll(argv[1]) : 500;      // 按下保持时间(ms)
    HWND tray = FindWindowW(L"BinKeyTrayWindow", nullptr);
    if (!tray) { printf("找不到托盘窗口（BinKey 未运行）\n"); return 1; }
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONDOWN);
    Sleep((DWORD)hold);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);
    Sleep(400);
    printf("已模拟一次左键点击（按下 %lld ms 后松手）\n", hold);
    return 0;
}
