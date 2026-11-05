// BinKeySetup.cpp —— BinKey 单文件安装引导程序
//
// 特点:
//   * 一个 exe 里嵌入了 BinKey.exe / config.cfg / README / CHANGELOG / FIXES / fix-icon-cache.bat / 4 个预设方案
//   * 无需管理员权限，无需安装运行环境（静态链接）
//   * 向导式: 欢迎 -> 位置(文件夹选择器) -> 选项 -> 安装 -> 完成
//   * 选项只有三个: 开机自动启动 / 桌面快捷方式 / 装完立即启动
//   * 自动跑自检(后台直跑，不弹命令窗口)，并写出 uninstall.bat
//   * 命令行模式: BinKeySetup.exe --silent [--dir D:\BinKey] [--no-autostart] [--no-shortcut]
//
// 编译:  g++ -O2 -std=c++17 -static -mwindows src\BinKeySetup.cpp -o build\BinKeySetup_stub.exe -luser32 -ladvapi32 -lshell32
// 打包:  build\packer.exe build\BinKeySetup_stub.exe BinKeySetup.exe  ...   (见 build-installer.bat)
#define INITGUID
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <objidl.h>
#include <direct.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static const wchar_t* kCls    = L"BinKeySetupWizard";
static const char*    kMagic  = "DSKSETUP";
static const char*    kVer    = "1.0.1";

// ---- 控件 ID ----
enum {
    ID_NEXT = 101, ID_BACK = 102, ID_INSTALL = 103, ID_DONE = 104,
    ID_DIR  = 105, ID_CHK_AUTO = 106, ID_CHK_SHORTCUT = 107, ID_CHK_LAUNCH = 108,
    ID_BTN_EDITOR = 111, ID_EXIT = 113,
    ID_PICKDIR = 114, ID_BROWSE = 115,
    ID_TITLE = 200, ID_BODY = 201, ID_LIST = 202
};

struct Entry { std::string name; uint32_t off, size; };

static HWND  g_win;
static HFONT g_font, g_fontBig;
static int   g_step = 0;                       // 0 欢迎 1 位置 2 选项 3 结果 4 完成
static std::vector<Entry> g_entries;
static std::string g_blob;
static std::wstring g_dir;
static bool g_autostart = true, g_shortcut = true, g_launch = true;
static std::vector<std::pair<std::string, std::string>> g_written;
static std::string g_log;
static bool g_installed = false;

static HWND byId(int id);

// ---------------- 通用工具 ----------------
static std::wstring toW(const std::string& s) {
    wchar_t b[4096] = {};
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), b, 4095);
    return b;
}
static std::string u8(const wchar_t* s) {
    char b[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, s, -1, b, 4095, nullptr, nullptr);
    return b;
}
static std::string u8(const std::wstring& s) { return u8(s.c_str()); }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
static std::string human(uint32_t n) {
    wchar_t b[64];
    if (n < 1024) swprintf(b, 64, L"%u B", n);
    else if (n < 1048576u) swprintf(b, 64, L"%.1f KB", n / 1024.0);
    else swprintf(b, 64, L"%.2f MB", n / 1048576.0);
    return u8(b);
}

static void printLine(const std::string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static bool readAllW(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, buf + n);
    fclose(f);
    return true;
}

// 读取自身 exe 尾部的清单: [payload][manifest][u32 manifestSize]["DSKSETUP"]
static bool loadPayload() {
    wchar_t exePath[2048] = {};
    GetModuleFileNameW(nullptr, exePath, 2047);
    if (!readAllW(exePath, g_blob)) return false;
    if (g_blob.size() < 16) return false;
    if (g_blob.compare(g_blob.size() - 8, 8, kMagic) != 0) return false;
    uint32_t msz = 0;
    memcpy(&msz, g_blob.data() + g_blob.size() - 12, 4);
    size_t start = g_blob.size() - 12 - msz;
    const char* p = g_blob.data() + start;
    const char* end = g_blob.data() + g_blob.size() - 12;
    uint32_t n = 0; memcpy(&n, p, 4); p += 4;
    for (uint32_t i = 0; i < n && p + 8 <= end; ++i) {
        uint32_t l = 0; memcpy(&l, p, 4); p += 4;
        if (p + l + 8 > end) break;
        std::string name(p, l); p += l;
        uint32_t off, sz; memcpy(&off, p, 4); memcpy(&sz, p + 4, 4); p += 8;
        g_entries.push_back({name, off, sz});
    }
    return !g_entries.empty();
}

static bool writeFile(const std::wstring& path, const char* data, size_t len) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    if (len) fwrite(data, 1, len, f);
    fclose(f);
    return true;
}
static void ensureDir(const std::wstring& path) {
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == L'\\') _wmkdir(cur.c_str());
    }
    _wmkdir(path.c_str());
}
static bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring defaultDir() {
    wchar_t tmp[2048] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", tmp, 2047);
    std::wstring base = tmp[0] ? tmp : L"C:\\Users\\Public";
    return base + L"\\BinKey";
}

// 标准 Windows 文件夹选择对话框（现代 Explorer 风格；失败时退回经典 SHBrowseForFolder）
static const GUID kClsidFileOpenDialog =
    {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const GUID kIidIFileOpenDialog =
    {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const GUID kIidIShellItem =
    {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};

static bool modernPickFolder(HWND owner, const std::wstring& startAt, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (CoCreateInstance(kClsidFileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                         kIidIFileOpenDialog, (void**)&dlg) != S_OK || !dlg)
        return false;
    dlg->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择 BinKey 安装文件夹");
    dlg->SetOkButtonLabel(L"选择这个文件夹");
    if (!startAt.empty() && dirExists(startAt)) {
        IShellItem* si = nullptr;
        if (SHCreateItemFromParsingName(startAt.c_str(), nullptr, kIidIShellItem, (void**)&si) == S_OK && si) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    HRESULT hr = dlg->Show(owner);
    bool ok = false;
    if (hr == S_OK) {
        IShellItem* item = nullptr;
        if (dlg->GetResult(&item) == S_OK) {
            wchar_t* path = nullptr;
            if (item->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path) {
                out = path;
                ok = !out.empty();
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool classicPickFolder(HWND owner, const std::wstring& startAt, std::wstring& out) {
    (void)startAt;   // 经典对话框不支持起始目录，只是兜底
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择 BinKey 安装文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, path) && path[0];
    CoTaskMemFree(pidl);
    if (ok) out = path;
    return ok;
}

static bool browseFolder(HWND owner, const std::wstring& startAt, std::wstring& out) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = modernPickFolder(owner, startAt, out) || classicPickFolder(owner, startAt, out);
    CoUninitialize();
    return ok;
}

// 把清单里的相对路径(用 '/' 分隔)展开成安装目录下的绝对路径，并创建中间目录
static std::wstring resolvePath(const std::wstring& root, const std::string& name) {
    std::vector<std::wstring> parts;
    std::string cur;
    for (char ch : name) {
        if (ch == '/' || ch == '\\') { if (!cur.empty()) { parts.push_back(toW(cur)); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (!cur.empty()) parts.push_back(toW(cur));
    if (parts.empty()) return root;
    std::wstring p = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) { p += L"\\" + parts[i]; ensureDir(p); }
    return p + L"\\" + parts.back();
}

static bool extractEntry(const Entry& e, const std::wstring& root) {
    return writeFile(resolvePath(root, e.name), g_blob.data() + e.off, e.size);
}

// 桌面快捷方式
static std::wstring g_shortcutPath;
static bool createShortcut(const std::wstring& target, const std::wstring& working, const std::wstring& linkPath) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link) != S_OK || !link) {
        CoUninitialize();
        return false;
    }
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(working.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"BinKey —— 动态键程 / SOCD 规则编辑器");

    bool saved = false;
    IPersistFile* pf = nullptr;
    if (link->QueryInterface(IID_IPersistFile, (void**)&pf) == S_OK && pf) {
        saved = (pf->Save(linkPath.c_str(), TRUE) == S_OK);
        pf->Release();
    }
    link->Release();
    CoUninitialize();
    return saved;
}

static std::wstring desktopPath() {
    wchar_t p[2048] = {};
    if (SHGetSpecialFolderPathW(nullptr, p, 16 /* CSID_DESKTOPDIRECTORY */, TRUE))
        return p;
    wchar_t up[2048] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", up, 2047) && up[0]) return std::wstring(up) + L"\\Desktop";
    return L"C:\\Users\\Public\\Desktop";
}

static uint32_t writeUninstall(const std::wstring& root) {
    std::wstring un =
        L"@echo off\r\n"
        L"rem BinKey 卸载脚本 —— 关闭程序、清除开机自启、删除桌面快捷方式、删除安装目录\r\n"
        L"set T=%TEMP%\\BinKey_uninstall.bat\r\n"
        L"if \"%~0\"==\"%T%\" goto do\r\n"
        L"copy /y \"%~0\" \"%T%\" >nul\r\n"
        L"call \"%T%\"\r\n"
        L"del \"%T%\" >nul 2>&1\r\n"
        L"exit /b\r\n"
        L":do\r\n"
        L"cd /d \"%TEMP%\"\r\n"
        L"taskkill /f /im BinKey.exe >nul 2>&1\r\n"
        L"timeout /t 2 >nul\r\n"
        L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\" /v BinKey /f >nul 2>&1\r\n";
    if (!g_shortcutPath.empty())
        un += L"del /q \"" + g_shortcutPath + L"\" >nul 2>&1\r\n";
    un += L"rmdir /s /q \"" + root + L"\"\r\n"
        L"echo BinKey 已卸载。\r\n"
        L"pause\r\n";
    std::string s = u8(un);
    writeFile(root + L"\\uninstall.bat", s.data(), s.size());
    return (uint32_t)s.size();
}

static void setAutostart(bool on, const std::wstring& exe) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) {
        g_log += "无法写入开机自启注册表项（不影响使用）\n";
        return;
    }
    if (on)
        RegSetValueExW(hk, L"BinKey", 0, REG_SZ, (const BYTE*)exe.c_str(),
                       (DWORD)((exe.size() + 1) * sizeof(wchar_t)));
    else
        RegDeleteValueW(hk, L"BinKey");
    RegCloseKey(hk);
}

// 运行子进程并把输出重定向到临时文件 —— 不经过 cmd，所以不会弹出命令窗口
static bool runCapture(const std::wstring& exe, const std::wstring& args, std::string& out) {
    wchar_t tmp[2048] = {};
    GetTempPathW(2048, tmp);
    std::wstring o = std::wstring(tmp) + L"dkkey_selftest.txt";

    HANDLE fh = CreateFileW(o.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    SetHandleInformation(fh, HANDLE_FLAG_INHERIT, 1);

    std::wstring cmd = exe;
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = fh;
    si.hStdError  = fh;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdInput  = (in && in != INVALID_HANDLE_VALUE) ? in : nullptr;

    PROCESS_INFORMATION pi;
    bool started = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(fh);
    if (!started) { DeleteFileW(o.c_str()); return false; }

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    bool ok = readAllW(o, out);
    DeleteFileW(o.c_str());
    return ok;
}

// ---------------- 界面 ----------------
static HWND mk(HWND parent, const wchar_t* cls, const std::wstring& text,
               int x, int y, int w, int h, DWORD style, int id) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(intptr_t)id, GetModuleHandleW(nullptr), nullptr);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
static void clearChildren(HWND p) {
    HWND c = FindWindowExW(p, nullptr, nullptr, nullptr);
    while (c) { HWND nx = FindWindowExW(p, c, nullptr, nullptr); DestroyWindow(c); c = nx; }
}
static void setCheck(int id, bool on) {
    HWND c = byId(id);
    if (c) SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
static bool getCheck(int id) {
    HWND c = byId(id);
    return c && SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
static std::wstring textOf(HWND c) {
    wchar_t b[2048] = {};
    if (c) SendMessageW(c, WM_GETTEXT, 2047, (LPARAM)b);
    return b;
}
static void setDir(const std::wstring& d) {
    g_dir = d;
    HWND e = byId(ID_DIR);
    if (e) SendMessageW(e, WM_SETTEXT, 0, (LPARAM)g_dir.c_str());
}

static void buildStep() {
    clearChildren(g_win);
    // 窗口没有背景刷时，销毁旧控件后残留的文字像素不会被重绘 -> 切屏时文字叠在一起
    InvalidateRect(g_win, nullptr, TRUE);
    UpdateWindow(g_win);
    const int W = 720;
    HWND t = mk(g_win, L"STATIC", L"BinKey 安装引导  v" + toW(kVer) + L"    DKS(动态键程) + SOCD(Snap Tap)",
                16, 12, W - 32, 26, SS_CENTER, ID_TITLE);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_fontBig, TRUE);

    switch (g_step) {
        case 0: {
            std::wstring body =
                L"BinKey 让普通机械键盘也能实现 DKS(动态键程) 和 SOCD(Snap Tap)。\n"
                L"不装驱动、不改硬件，只是一个 Windows 键盘钩子，随时可卸载。\n\n"
                L"  - SOCD: 同时按 A 和 D 时只保留后按的那个，另一键松开后被压制的键自动恢复\n"
                L"  - DKS : 单键急停 —— 松开 A 时自动轻点 D，让\"松手\"变成一次点击\n"
                L"  - 所有按键 / 模式 / 时间都在图形编辑器里改，改完自动写回 config.cfg\n\n"
                L"环境要求: Windows 10 / 11 (64 位)。不需要管理员权限。\n"
                L"下一步选安装文件夹；不选则默认装到 " + defaultDir() + L"\n\n"
                L"点 \"下一步\" 开始。";
            mk(g_win, L"STATIC", body, 16, 46, W - 32, 250, SS_LEFT, ID_BODY);
            mk(g_win, L"BUTTON", L"下一步", 590, 320, 110, 32, 0, ID_NEXT);
            mk(g_win, L"BUTTON", L"退出", 16, 320, 80, 32, 0, ID_EXIT);
            break;
        }
        case 1: {
            mk(g_win, L"STATIC", L"安装位置", 16, 46, 300, 20, SS_LEFT, ID_TITLE);

            mk(g_win, L"BUTTON", L"选择安装文件夹...", 16, 74, 240, 32, 0, ID_BROWSE);
            HWND shown = mk(g_win, L"EDIT", g_dir, 268, 74, W - 32 - 268, 32,
                            WS_BORDER | ES_READONLY, ID_DIR);
            SendMessageW(shown, WM_SETFONT, (WPARAM)g_fontBig, TRUE);

            mk(g_win, L"STATIC",
                L"- 点左边的按钮，在系统文件夹选择器里挑一个文件夹，选完路径会显示在右边（不手打路径）。\n"
                L"- 不选也行：默认装到 " + defaultDir() + L"\n"
                L"- 该目录会被创建并写入 BinKey.exe、config.cfg、README.md、profiles 等。\n"
                L"- 如果目录里已有 BinKey.exe，会被覆盖(旧 config.cfg 会被替换，可先备份)。",
                16, 122, W - 32, 110, SS_LEFT, ID_BODY);
            mk(g_win, L"BUTTON", L"上一步", 460, 320, 110, 32, 0, ID_BACK);
            mk(g_win, L"BUTTON", L"下一步", 590, 320, 110, 32, 0, ID_NEXT);
            mk(g_win, L"BUTTON", L"退出", 16, 320, 80, 32, 0, ID_EXIT);
            break;
        }
        case 2: {
            mk(g_win, L"STATIC", L"安装选项", 16, 46, 300, 20, SS_LEFT, ID_TITLE);
            mk(g_win, L"STATIC", L"安装到:  " + g_dir, 340, 46, W - 340 - 16, 20, SS_RIGHT, ID_PICKDIR);
            mk(g_win, L"BUTTON", L"开机自动启动 (开机后自动运行 BinKey)", 16, 72, 420, 24, BS_AUTOCHECKBOX, ID_CHK_AUTO);
            mk(g_win, L"BUTTON", L"创建桌面快捷方式 (BinKey.lnk)", 16, 100, 420, 24, BS_AUTOCHECKBOX, ID_CHK_SHORTCUT);
            mk(g_win, L"BUTTON", L"安装完成后立即启动 BinKey", 16, 128, 420, 24, BS_AUTOCHECKBOX, ID_CHK_LAUNCH);
            mk(g_win, L"STATIC",
                L"开机自启写入 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run，\n"
                L"托盘菜单里也能随时关闭；卸载时会自动清理（连同桌面快捷方式）。",
                16, 170, W - 32, 60, SS_LEFT, ID_BODY);
            mk(g_win, L"BUTTON", L"上一步", 460, 320, 110, 32, 0, ID_BACK);
            mk(g_win, L"BUTTON", L"安装", 590, 320, 110, 32, 0, ID_INSTALL);
            mk(g_win, L"BUTTON", L"退出", 16, 320, 80, 32, 0, ID_EXIT);
            break;
        }
        case 3: {
            mk(g_win, L"STATIC", L"安装结果", 16, 46, 300, 20, SS_LEFT, ID_TITLE);
            mk(g_win, L"STATIC", L"安装到:  " + g_dir, 340, 46, W - 340 - 16, 20, SS_RIGHT, ID_PICKDIR);
            std::wstring list;
            for (auto& kv : g_written) list += L"  " + toW(kv.first) + L"  (" + toW(kv.second) + L")\n";
            if (!g_log.empty()) list += L"\n" + toW(g_log);
            mk(g_win, L"STATIC", list, 16, 70, W - 32, 230, SS_LEFT, ID_LIST);
            mk(g_win, L"BUTTON", L"完成", 590, 320, 110, 32, 0, ID_DONE);
            mk(g_win, L"BUTTON", L"退出", 16, 320, 80, 32, 0, ID_EXIT);
            break;
        }
        default: {
            std::wstring body =
                L"已安装到  " + g_dir + L"\n\n"
                L"接下来:\n"
                L"  1. 任务栏右下角找到 BinKey 图标 (Windows 11 可能藏在 ^ 溢出区里)\n"
                L"  2. 右键图标 -> \"规则编辑器\" -> 增删改规则，改动即时生效并自动保存\n"
                L"  3. 想直接改配置: 右键 -> \"打开配置文件\"; profiles 里有现成方案\n"
                L"  4. 排查问题: 把 config.cfg 里的 debug 改成 1，会生成 BinKey.log\n\n"
                L"卸载: 运行 " + g_dir + L"\\uninstall.bat (会自动清除开机自启)。";
            mk(g_win, L"STATIC", body, 16, 46, W - 32, 240, SS_LEFT, ID_BODY);
            mk(g_win, L"BUTTON", L"退出", 16, 320, 80, 32, 0, ID_EXIT);
            mk(g_win, L"BUTTON", L"打开规则编辑器", 544, 320, 160, 32, 0, ID_BTN_EDITOR);
            break;
        }
    }
    setCheck(ID_CHK_AUTO, g_autostart);
    setCheck(ID_CHK_SHORTCUT, g_shortcut);
    setCheck(ID_CHK_LAUNCH, g_launch);
}

static void launch(const std::wstring& exe, const std::wstring& args) {
    PROCESS_INFORMATION pi; STARTUPINFOW si = {}; si.cb = sizeof(si);
    std::wstring cmd = args.empty() ? exe : exe + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static void doInstall() {
    std::wstring d = g_dir;
    if (d.empty()) d = defaultDir();
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    g_dir = d;

    g_written.clear();
    g_log.clear();

    ensureDir(g_dir);
    if (!dirExists(g_dir)) {
        g_log = "无法创建目录: " + u8(g_dir);
        g_step = 3; buildStep(); return;
    }

    for (const Entry& e : g_entries) {
        if (extractEntry(e, g_dir))
            g_written.push_back({e.name, human(e.size)});
        else
            g_log += "写入失败: " + e.name + "\n";
    }

    std::wstring exe = g_dir + L"\\BinKey.exe";
    setAutostart(g_autostart, exe);

    g_shortcutPath.clear();
    if (g_shortcut) {
        std::wstring desk = desktopPath();
        ensureDir(desk);
        g_shortcutPath = desk + L"\\BinKey.lnk";
        // 旧版本用过 "BinKey 动态键程.lnk"，改名后清理掉，避免桌面上出现两个快捷方式
        std::wstring legacy = desk + L"\\BinKey 动态键程.lnk";
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
            DeleteFileW(legacy.c_str());
        if (createShortcut(exe, g_dir, g_shortcutPath))
            g_written.push_back({"桌面快捷方式 BinKey.lnk", "link"});
        else
            g_log += "创建桌面快捷方式失败 (不影响使用)\n";
    }

    std::string s;
    if (runCapture(exe, L"--selftest", s)) {
        size_t pass = 0, fail = 0;
        for (size_t i = 0; i + 4 <= s.size(); ++i) {
            if (s.compare(i, 4, "PASS") == 0) ++pass;
            if (s.compare(i, 4, "FAIL") == 0) ++fail;
        }
        if (pass || fail) g_log += "自检结果: " + std::to_string(pass) + " PASS / " + std::to_string(fail) + " FAIL\n";
        else g_log += "自检未能读取输出\n";
    } else {
        g_log += "自检未能启动 (不影响使用)\n";
    }

    uint32_t unSz = writeUninstall(g_dir);
    g_written.push_back({"uninstall.bat", human(unSz)});

    g_installed = true;
    g_step = 3;
    buildStep();

    if (g_launch) launch(exe, L"");
}

static LRESULT CALLBACK wproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(w);
            switch (id) {
                case ID_DIR:                                   // 只读路径框，由选择结果写入
                    if (HIWORD(w) == EN_CHANGE) {
                        std::wstring d = toW(trim(u8(textOf(byId(ID_DIR)))));
                        if (!d.empty()) g_dir = d;
                    }
                    break;
                case ID_BROWSE: {
                    std::wstring p;
                    if (browseFolder(h, g_dir, p)) {
                        while (!p.empty() && p.back() == L'\\') p.pop_back();
                        setDir(p);
                    }
                    break;
                }
                case ID_NEXT:
                    if (g_step == 1) {
                        std::wstring d = toW(trim(u8(textOf(byId(ID_DIR)))));
                        if (!d.empty()) g_dir = d;
                    }
                    if (g_step < 4) { ++g_step; buildStep(); }
                    break;
                case ID_BACK:
                    if (g_step > 0) { --g_step; buildStep(); }
                    break;
                case ID_INSTALL: doInstall(); break;
                case ID_DONE:    g_step = 4; buildStep(); break;
                case ID_EXIT:        DestroyWindow(h); break;
                case ID_BTN_EDITOR:  launch(g_dir + L"\\BinKey.exe", L"--editor");
                                     Sleep(200);          // 让编辑器窗口先创建出来
                                     DestroyWindow(h);    // 引导自动关闭
                                     break;
                case ID_CHK_AUTO:     g_autostart = getCheck(ID_CHK_AUTO); break;
                case ID_CHK_SHORTCUT: g_shortcut = getCheck(ID_CHK_SHORTCUT); break;
                case ID_CHK_LAUNCH:   g_launch = getCheck(ID_CHK_LAUNCH); break;
            }
            return 0;
        }
        case WM_CLOSE:   DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static HWND byId(int id) {
    for (HWND c = FindWindowExW(g_win, nullptr, nullptr, nullptr); c; c = FindWindowExW(g_win, c, nullptr, nullptr))
        if ((int)(uintptr_t)GetWindowLongPtrW(c, GWLP_ID) == id) return c;
    return nullptr;
}

// 用 GetCommandLineW 解析参数，这样带空格 / 中文的路径不会被 ANSI 代码页破坏
static std::vector<std::wstring> wideArgs() {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuote = false;
    LPWSTR p = GetCommandLineW();
    for (; *p; ++p) {
        if (*p == L'"') { inQuote = !inQuote; continue; }
        if (*p == L' ' && !inQuote) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(*p);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static int silentInstall(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!loadPayload()) { printLine("安装程序无内嵌负载，请用 build-installer.bat 重新打包"); return 1; }
    std::wstring dir = defaultDir();
    bool autostart = true, shortcut = true, launchAfter = false;
    std::vector<std::wstring> a = wideArgs();
    for (size_t i = 1; i < a.size(); ++i) {
        const std::wstring& s = a[i];
        if (s == L"--silent") continue;
        if (s == L"--dir" && i + 1 < a.size()) { dir = a[++i]; }
        else if (s == L"--no-autostart") autostart = false;
        else if (s == L"--no-shortcut") shortcut = false;
        else if (s == L"--launch") launchAfter = true;
        else if (s == L"--help" || s == L"-h") {
            printLine("用法: BinKeySetup.exe [--silent] [--dir <路径>] [--no-autostart] [--no-shortcut] [--launch]");
            return 0;
        }
    }
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    g_dir = dir;
    g_shortcut = shortcut;
    ensureDir(dir);
    printLine("安装到 " + u8(dir));
    for (const Entry& e : g_entries) {
        if (extractEntry(e, dir)) printLine("  " + e.name + "  (" + human(e.size) + ")");
        else printLine("  写入失败: " + e.name);
    }
    setAutostart(autostart, dir + L"\\BinKey.exe");
    printLine(std::string("开机自动启动: ") + (autostart ? "已开启" : "已关闭"));
    if (shortcut) {
        std::wstring desk = desktopPath();
        ensureDir(desk);
        g_shortcutPath = desk + L"\\BinKey.lnk";
        std::wstring legacy = desk + L"\\BinKey 动态键程.lnk";
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
            DeleteFileW(legacy.c_str());
        printLine(std::string("桌面快捷方式: ") + (createShortcut(dir + L"\\BinKey.exe", dir, g_shortcutPath) ? u8(g_shortcutPath) : "创建失败"));
    }
    writeUninstall(dir);

    std::string s;
    if (runCapture(dir + L"\\BinKey.exe", L"--selftest", s)) {
        size_t pass = 0, fail = 0;
        for (size_t i = 0; i + 4 <= s.size(); ++i) {
            if (s.compare(i, 4, "PASS") == 0) ++pass;
            if (s.compare(i, 4, "FAIL") == 0) ++fail;
        }
        printLine("自检: " + std::to_string(pass) + " PASS / " + std::to_string(fail) + " FAIL");
    } else {
        printLine("自检未能启动 (不影响使用)");
    }
    printLine("完成。运行 " + u8(dir) + "\\BinKey.exe，或双击 uninstall.bat 卸载。");
    if (launchAfter) launch(dir + L"\\BinKey.exe", L"");
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--silent") == 0) return silentInstall(argc, argv);
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printLine(std::string("BinKey 安装引导  v") + kVer);
            printLine("  直接双击 / 不带参数  = 图形向导");
            printLine("  --silent [--dir <路径>] [--no-autostart] [--no-shortcut] [--launch]");
            printLine("  默认安装到 " + u8(defaultDir()));
            return 0;
        }
    }

    if (!loadPayload()) {
        MessageBoxW(nullptr, L"这个安装程序没有内嵌文件。\n请运行 build-installer.bat 生成 BinKeySetup.exe。",
                    L"BinKey 安装引导", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_dir = defaultDir();
    g_font = CreateFontW(-14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, L"Microsoft YaHei");
    g_fontBig = CreateFontW(-18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, L"Microsoft YaHei");

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wproc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);   // 没有背景刷 -> 切屏时旧文字残留
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                   IMAGE_ICON, 32, 32, 0);
    wc.hIcon = icon; wc.hIconSm = icon;              // 标题栏 / Alt-Tab 图标
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_win = CreateWindowExW(0, kCls, L"BinKey 安装引导", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            (sw - 720) / 2, (sh - 400) / 2, 720, 400, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!g_win) { printf("窗口创建失败\n"); return 1; }
    SendMessageW(g_win, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    SendMessageW(g_win, WM_SETICON, ICON_BIG,   (LPARAM)icon);
    buildStep();
    ShowWindow(g_win, SW_SHOW);
    UpdateWindow(g_win);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
