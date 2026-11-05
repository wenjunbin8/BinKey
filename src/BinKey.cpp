// ============================================================================
// BinKey —— 让普通(机械)键盘实现 DKS(动态键程) / SOCD(Snap Tap) 的 Windows 工具
//
// 单文件 C++ / Win32，不依赖第三方库，编译成一个 exe，运行后在系统托盘常驻。
//
// 原理：
//   1. 用 WH_KEYBOARD_LL 低级键盘钩子拦截所有键盘事件；
//   2. 自己维护每个"托管键"的物理/逻辑状态；
//   3. 需要释放某个键时，用 SendInput 注入合成的 key-up（带 dwExtraInfo 标记，
//      钩子遇到该标记直接放行，避免自我循环）；
//   4. 需要"轻点"某个键时，注入 key-down，并在 tap_ms 毫秒后注入 key-up。
//
// 功能：
//   - DKS 单键急停：松开 A 时自动轻点 D（目标键与时长可自定义）
//   - DKS 按下触发 / 按住触发 / 按住连发(turbo)
//   - SOCD 清理：snap(last-input) / first / neutral / dominant / off
//   - 托盘图标 + 菜单(启用/禁用/重载配置/打开配置/退出)，可设全局热键切换
// ============================================================================

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>

// MinGW 的头文件里缺少这两个虚拟键码
#ifndef VK_LALT
#define VK_LALT 0xA4
#endif
#ifndef VK_RALT
#define VK_RALT 0xA5
#endif

// 提高 Sleep 的精度（winmm），让 tap_ms 更准 —— timeBeginPeriod/timeEndPeriod 已在 mmsystem.h 中声明

// ---------------------------------------------------------------------------
// 常量 / 类型
// ---------------------------------------------------------------------------
static const char*     kVersion = "1.0.2";        // 版本号（与 BinKeySetup.exe 一致）
static const ULONG_PTR TAG_SELF = 0x444B534BULL; // "DKSK" —— 我们自己注入的事件
static const ULONG_PTR TAG_TEST = 0x54455354ULL; // "TEST" —— selftest 模拟的"物理"事件

enum SocdMode { SOC_OFF = 0, SOC_SNAP = 1, SOC_FIRST = 2, SOC_NEUTRAL = 3, SOC_DOMINANT = 4 };
enum OnKind  { ON_PRESS = 0, ON_RELEASE = 1, ON_HOLD = 2, ON_REPEAT = 3 };
enum Cond    { C_ALWAYS = 0, C_ACTIVE = 1, C_TARGET_UP = 2, C_TARGET_FREE = 3 };

struct Group {
    std::vector<int> keys;
    int mode = SOC_SNAP;
    int dominant = 0;
};

struct Rule {
    int key = 0;
    OnKind on = ON_RELEASE;
    int tap = 0;
    int tapMs = 25;
    int delayMs = 0;
    int minHoldMs = 0;
    int intervalMs = 0;   // repeat 用
    int count = 0;        // repeat 次数，0 = 无限(只要按住)
    Cond cond = C_ACTIVE;
};

struct Config {
    std::wstring path;
    bool debug = false;
    bool autostart = false;
    bool socdEnabled = true;     // 用户对 SOCD 的单独设定（持久化）
    bool dksEnabled = true;      // 用户对 DKS 的单独设定（持久化）
    bool enabled = true;         // 总开关："禁用全部功能"只是关掉总开关，
                                 // 重新启用时恢复上面的单独设定，而不是两个都开
    int toggleKey = 0;
    std::vector<Group> groups;
    std::vector<Rule> rules;
};

struct KeyState {
    bool phys = false;      // 物理是否按下
    bool logical = false;   // 逻辑上是否处于"按下"状态(游戏看到的)
    long long downAt = 0;
};

struct Action {
    long long when = 0;
    int vk = 0;
    int tapMs = 0;
    int source = 0;
    long long interval = 0;
    int remaining = 0;
    bool up = false;
};
struct ActionCmp { bool operator()(const Action& a, const Action& b) const { return a.when > b.when; } };

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static Config g_cfg;
static std::unordered_map<int, KeyState> g_state;
static std::unordered_map<int, int> g_groupOf;
static CRITICAL_SECTION g_logcs;
static std::priority_queue<Action, std::vector<Action>, ActionCmp> g_queue;
static volatile bool g_running = true;
static HHOOK g_hook = nullptr;
static FILE* g_logFile = nullptr;
static std::wstring g_logPath;
static bool g_console = false;
static HWND g_editorWnd = nullptr;      // 规则编辑器窗口（前台时不拦截键盘）

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
static std::string u8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring w16(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, '\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static long long nowMs() {
    static LARGE_INTEGER base;
    static double freq = 0.0;
    static bool init = false;
    if (!init) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart;
        QueryPerformanceCounter(&base);
        init = true;
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (long long)(((double)(t.QuadPart - base.QuadPart) / freq) * 1000.0);
}

static void logf(const char* fmt, ...) {
    if (!g_cfg.debug) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "[%6lldms] ", nowMs());
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logcs);
    if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
    if (g_console)  { printf("%s\n", buf); fflush(stdout); }
    LeaveCriticalSection(&g_logcs);
}

// ---------------------------------------------------------------------------
// 键名解析
// ---------------------------------------------------------------------------
static int parseKey(const std::string& s) {
    if (s.empty()) return 0;
    std::string t = s;
    for (char& c : t) c = (char)tolower(c);
    if (t == "none" || t == "-") return 0;
    bool numeric = true;
    for (char c : t) if (!isdigit((unsigned char)c)) { numeric = false; break; }
    if (numeric) return atoi(t.c_str());

    static const std::unordered_map<std::string, int> names = {
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"tab", VK_TAB}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN},
        {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT}, {"shift", VK_SHIFT},
        {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL}, {"ctrl", VK_CONTROL},
        {"lalt", VK_LALT}, {"ralt", VK_RALT}, {"alt", VK_MENU},
        {"caps", VK_CAPITAL}, {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},
        {"slash", VK_OEM_2}, {"minus", VK_OEM_MINUS}, {"equals", VK_OEM_PLUS},
        {"semicolon", VK_OEM_1}, {"quote", VK_OEM_3},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"backspace", VK_BACK},
        {"insert", VK_INSERT}, {"scrolllock", VK_SCROLL}, {"numlock", VK_NUMLOCK},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8}, {"f9", VK_F9},
        {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
    };
    auto it = names.find(t);
    if (it != names.end()) return it->second;
    if (t.size() == 1) {
        int vk = VkKeyScanA(t[0]);
        if (vk >= 0) return vk;
    }
    return 0;
}

static std::string keyName(int vk) {
    if (!vk) return "none";
    static const std::unordered_map<int, std::string> special = {
        {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_TAB, "Tab"}, {VK_ESCAPE, "Esc"},
        {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_UP, "Up"}, {VK_DOWN, "Down"},
        {VK_LSHIFT, "LShift"}, {VK_RSHIFT, "RShift"}, {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"},
        {VK_LALT, "LAlt"}, {VK_RALT, "RAlt"}, {VK_CAPITAL, "Caps"}, {VK_OEM_COMMA, "Comma"},
        {VK_OEM_PERIOD, "Period"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"},
        {VK_BACK, "Backspace"}, {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F4, "F4"},
        {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F8, "F8"}, {VK_F9, "F9"},
        {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
        {VK_SCROLL, "ScrollLock"}, {VK_NUMLOCK, "NumLock"},
    };
    auto it = special.find(vk);
    if (it != special.end()) return it->second;
    for (char c = 'a'; c <= 'z'; ++c) if (VkKeyScanA(c) == vk) return std::string(1, (char)toupper(c));
    for (char c = '0'; c <= '9'; ++c) if (VkKeyScanA(c) == vk) return std::string(1, c);
    return "vk" + std::to_string(vk);
}

// ---------------------------------------------------------------------------
// 注入按键
// ---------------------------------------------------------------------------
static LRESULT CALLBACK llHook(int code, WPARAM wParam, LPARAM lParam);   // 前向声明

// 虚拟自检模式：不真按键盘，而是把事件直接喂进钩子 —— 自检期间不会产生任何真实按键
static bool g_virtual = false;
static std::unordered_map<int, bool> g_virtDown;   // 钩子放行后"系统实际看到"的按键状态

static bool virtualEvent(int vk, bool down, ULONG_PTR tag) {
    KBDLLHOOKSTRUCT ks{};
    ks.vkCode     = (DWORD)vk;
    ks.dwExtraInfo = tag;
    LRESULT r = llHook(HC_ACTION, down ? WM_KEYDOWN : WM_KEYUP, (LPARAM)&ks);
    if (r == 1) { /* 被钩子吞掉，系统不会收到这个事件 */ }
    else        g_virtDown[vk] = down;
    return true;
}

// 系统/游戏实际看到的按键状态
static bool systemDown(int vk) {
    if (g_virtual) {
        auto it = g_virtDown.find(vk);
        return it != g_virtDown.end() && it->second;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool injectKey(int vk, bool down, ULONG_PTR tag = TAG_SELF) {
    if (g_virtual) return virtualEvent(vk, down, tag);
    long long t0 = nowMs();
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwExtraInfo = tag;
    if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
    bool ok = SendInput(1, &in, sizeof(INPUT)) == 1;
    long long took = nowMs() - t0;
    if (took > 20) logf("[SLOW SendInput] %s %s took %lld ms (ok=%d)", keyName(vk).c_str(), down ? "down" : "up", took, ok ? 1 : 0);
    return ok;
}

static bool physDown(int vk) {
    auto it = g_state.find(vk);
    if (it != g_state.end() && it->second.phys) return true;
    return systemDown(vk);
}

// "用户是否真的物理按着这个键" —— 只看物理状态，不包含我们自己注入的合成按键
static bool userHolding(int vk) {
    auto it = g_state.find(vk);
    return it != g_state.end() && it->second.phys;
}

static bool logicalDown(int vk) {
    auto it = g_state.find(vk);
    if (it != g_state.end()) return it->second.logical;
    return systemDown(vk);
}

// ---------------------------------------------------------------------------
// 动作队列（主线程驱动）—— tap 的 "松手" 需要延时，所以用带时间戳的队列
// ---------------------------------------------------------------------------
static void pushAction(const Action& a) {
    logf("[push]   when=%lld now=%lld vk=%d up=%d", a.when, nowMs(), a.vk, a.up ? 1 : 0);
    g_queue.push(a);
}

static bool socdActive();
static bool dksActive();

static void doAction(const Action& a) {
    if (a.up) {
        // 若用户此刻真的物理按着这个键，就不释放它（避免把用户的按压误释放）
        if (userHolding(a.vk)) {
            logf("[skip-up]   %s 用户物理仍按住", keyName(a.vk).c_str());
        } else {
            injectKey(a.vk, false);
            auto it = g_state.find(a.vk);
            if (it != g_state.end()) it->second.logical = false;
            logf("[keyup]     %s", keyName(a.vk).c_str());
        }
        return;
    }

    if (!a.vk) return;
    if (!dksActive()) { logf("[skip-tap]  %s 总开关/ DKS 已关闭，队列中的轻点不再执行", keyName(a.vk).c_str()); return; }
    if (a.source && !userHolding(a.source)) return;   // 源键已经松开了，连发停止
    if (userHolding(a.vk)) {
        logf("[skip-down] %s 用户已经按住", keyName(a.vk).c_str());
        return;
    }
    if (logicalDown(a.vk)) injectKey(a.vk, false);    // 上一次轻点还没释放，先释放
    injectKey(a.vk, true);
    auto it = g_state.find(a.vk);
    if (it != g_state.end()) { it->second.logical = true; it->second.phys = false; }
    logf("[tap-down]  %s (%d ms)", keyName(a.vk).c_str(), a.tapMs);

    Action up{};
    up.when = nowMs() + (a.tapMs > 5 ? a.tapMs : 5);
    up.vk = a.vk;
    up.up = true;
    g_queue.push(up);

    if (a.interval > 0 && (a.remaining == 0 || a.remaining > 1)) {
        Action nxt{};
        nxt.when = nowMs() + a.interval;
        nxt.vk = a.vk;
        nxt.tapMs = a.tapMs;
        nxt.source = a.source;
        nxt.interval = a.interval;
        nxt.remaining = a.remaining > 1 ? a.remaining - 1 : 0;
        g_queue.push(nxt);
    }
}

// 所有注入都在主线程完成：实测子线程 SendInput 会被系统阻塞约 1 秒。
static void processDueActions() {
    long long now = nowMs();
    while (!g_queue.empty() && g_queue.top().when <= now) {
        Action a = g_queue.top();
        g_queue.pop();
        doAction(a);
    }
}

static void pumpUntil(long long ms) {
    long long end = nowMs() + ms;
    MSG msg;
    while (g_running) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        processDueActions();
        if (ms > 0 && nowMs() >= end) break;
        Sleep(1);
    }
}

// ---------------------------------------------------------------------------
// 条件判断
// ---------------------------------------------------------------------------
static bool condOk(const Rule& r, bool keyActive) {
    switch (r.cond) {
        case C_ALWAYS:      return true;
        case C_ACTIVE:      return keyActive;
        case C_TARGET_UP:   return !physDown(r.tap);
        case C_TARGET_FREE: return !physDown(r.tap) && !logicalDown(r.tap);
    }
    return true;
}

static int groupIndexOf(int vk) {
    auto it = g_groupOf.find(vk);
    return it == g_groupOf.end() ? -1 : it->second;
}

static void setTrayIcon();       // 前向声明
static bool persistSwitches();   // 前向声明（定义在 saveConfig 附近）

// 开关语义：
//   socdEnabled / dksEnabled = 用户对两个功能各自的设定（会被写回 config.cfg）
//   enabled = 总开关。"禁用全部功能 / 左键托盘 / toggle_key" 只切总开关；
//             重新启用时恢复用户之前的单独设定（例如只开 SOCD），而不是两个都开。
static bool socdActive() { return g_cfg.enabled && g_cfg.socdEnabled; }
static bool dksActive()  { return g_cfg.enabled && g_cfg.dksEnabled; }
static bool totalEnabled() { return socdActive() || dksActive(); }

static void setFeature(int which, bool on) {   // 2 = SOCD, 3 = DKS
    if (which == 2) g_cfg.socdEnabled = on; else g_cfg.dksEnabled = on;
    if (on) g_cfg.enabled = true;              // 单独开启某个功能时总开关自动打开
    setTrayIcon();
    persistSwitches();
    logf("[switch] %s = %s", which == 2 ? "SOCD" : "DKS", on ? "开启" : "关闭");
}

static void toggleAll() {
    if (totalEnabled()) {
        g_cfg.enabled = false;                  // 单独设定保持不变，重新启用时恢复
    } else {
        g_cfg.enabled = true;
        // 两个单独开关都被用户关掉了 —— 这时"启用全部"就是字面意义上的全部开启
        if (!g_cfg.socdEnabled && !g_cfg.dksEnabled) {
            g_cfg.socdEnabled = true;
            g_cfg.dksEnabled  = true;
        }
    }
    setTrayIcon();
    persistSwitches();
    logf("[switch] 全部功能 %s（单独设定: SOCD=%s DKS=%s）",
         totalEnabled() ? "启用" : "禁用",
         g_cfg.socdEnabled ? "开" : "关", g_cfg.dksEnabled ? "开" : "关");
}

static void releaseLogical(int vk) {
    auto it = g_state.find(vk);
    if (it == g_state.end() || !it->second.logical) return;
    it->second.logical = false;
    injectKey(vk, false);
    logf("[socd-release] %s", keyName(vk).c_str());
}

// 组内"胜者"：物理按下的键中，按当前 mode 应该生效的那一个（0 = 都不生效）
static int groupWinner(const Group& g) {
    std::vector<int> held;
    for (int vk : g.keys) {
        auto it = g_state.find(vk);
        if (it != g_state.end() && it->second.phys) held.push_back(vk);
    }
    if (held.empty()) return 0;
    if (held.size() == 1) return held[0];

    switch (g.mode) {
        case SOC_SNAP: {                       // 后按下的优先
            int best = held[0];
            for (int vk : held) if (g_state[vk].downAt > g_state[best].downAt) best = vk;
            return best;
        }
        case SOC_FIRST: {                      // 先按下的优先
            int best = held[0];
            for (int vk : held) if (g_state[vk].downAt < g_state[best].downAt) best = vk;
            return best;
        }
        case SOC_NEUTRAL: return 0;            // 互相抵消
        case SOC_DOMINANT:
            // 主键按下时主键优先；主键不在时退化为"后按优先"
            for (int vk : held) if (vk == g.dominant) return vk;
            return held.back();
        default: return held[0];               // SOC_OFF：不做清理，交给调用处
    }
}

// 把组内除 vkNow 之外所有键的逻辑状态同步到"胜者"规则
static void syncGroup(int gi, int vkNow) {
    const Group& g = g_cfg.groups[gi];
    int winner = groupWinner(g);
    for (int o : g.keys) {
        if (o == vkNow) continue;
        KeyState& st = g_state[o];
        if (!st.phys) continue;               // 物理已松开的键由它自己的 key-up 处理
        bool want = (o == winner);
        if (want && !st.logical) {
            st.logical = true;
            injectKey(o, true);               // 恢复：这个键仍然被物理按着，重新置为"按下"
            logf("[socd-restore] %s", keyName(o).c_str());
        } else if (!want && st.logical) {
            releaseLogical(o);
        }
    }
}

// ---------------------------------------------------------------------------
// 键盘钩子
// ---------------------------------------------------------------------------
static void setTrayIcon();   // 前向声明

static LRESULT CALLBACK llHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wParam, lParam);
    // 规则编辑器处于前台时放行所有按键，避免编辑时被 SOCD/DKS 规则干扰
    if (g_editorWnd && GetForegroundWindow() == g_editorWnd)
        return CallNextHookEx(g_hook, code, wParam, lParam);
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
    int vk = (int)k->vkCode;

    // 我们自己注入的合成事件：放行，不做任何处理
    if (k->dwExtraInfo == TAG_SELF) return CallNextHookEx(g_hook, code, wParam, lParam);

    // wParam 就是 WM_KEYDOWN / WM_KEYUP（这是最可靠的判断方式）
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!down && !up) return CallNextHookEx(g_hook, code, wParam, lParam);

    // 全局切换热键（即使功能被禁用也能切回来）
    if (g_cfg.toggleKey && vk == g_cfg.toggleKey) {
        KeyState& st = g_state[g_cfg.toggleKey];
        if (down) {
            if (!st.phys) {
                st.phys = true; st.downAt = nowMs();
                toggleAll();
            }
        } else st.phys = false;
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    auto it = g_state.find(vk);
    if (it == g_state.end()) return CallNextHookEx(g_hook, code, wParam, lParam);
    logf("[event]    %s %s", keyName(vk).c_str(), down ? "down" : "up");
    KeyState& st = it->second;

    if (down) {
        if (st.phys) {
            // 自动重复：如果该键已被 SOCD 逻辑释放，必须吞掉重复事件，
            // 否则系统会把键状态重新置为"按下"，Snap Tap 就失效了。
            if (!st.logical) { logf("[swallow]   %s (repeat, 已被 SOCD 释放)", keyName(vk).c_str()); return 1; }
            return CallNextHookEx(g_hook, code, wParam, lParam);
        }
        st.phys = true;
        st.downAt = nowMs();

        bool passed = true;
        int gi = groupIndexOf(vk);
        if (gi >= 0 && socdActive() && g_cfg.groups[gi].mode != SOC_OFF) {
            int winner = groupWinner(g_cfg.groups[gi]);
            st.logical = (vk == winner);
            passed = st.logical;
            syncGroup(gi, vk);                 // 组内其它键：该释放的释放，该恢复的恢复
        } else {
            st.logical = true;
        }

        if (passed && dksActive()) {
            for (const Rule& r : g_cfg.rules) {
                if (r.key != vk || r.tap == 0) continue;
                if (r.on == ON_RELEASE) continue;
                if (!condOk(r, st.logical)) continue;
                Action a{};
                a.vk = r.tap;
                a.tapMs = r.tapMs;
                a.source = vk;
                if (r.on == ON_PRESS) {
                    a.when = st.downAt + r.delayMs;
                } else if (r.on == ON_HOLD) {
                    int mh = r.minHoldMs > 1 ? r.minHoldMs : 1;
                    a.when = st.downAt + mh + r.delayMs;
                } else { // ON_REPEAT
                    int mh = r.minHoldMs > 0 ? r.minHoldMs : 0;
                    a.when = st.downAt + mh + r.delayMs;
                    a.interval = std::max(r.intervalMs, std::max(r.tapMs, 1));
                    a.remaining = r.count;
                }
                pushAction(a);
            }
        }
        return passed ? CallNextHookEx(g_hook, code, wParam, lParam) : 1;
    }

    // ---- key up ----
    st.phys = false;
    bool wasLogical = st.logical;
    st.logical = false;

    // 先让系统处理这个 key-up（如果它逻辑上是按下），再同步组内其它键：
    // 例如 A 被 snap 释放后仍物理按着，此时 D 松手 → A 应当恢复为"按下"
    int gi = groupIndexOf(vk);
    bool socdOn = (gi >= 0 && socdActive() && g_cfg.groups[gi].mode != SOC_OFF);
    LRESULT ret;
    if (wasLogical) {
        ret = CallNextHookEx(g_hook, code, wParam, lParam);
    } else {
        logf("[swallow]   %s (keyup, 已被 SOCD 释放)", keyName(vk).c_str());
        ret = 1;
    }
    if (socdOn) syncGroup(gi, vk);

    if (dksActive()) {
        long long held = nowMs() - st.downAt;
        for (const Rule& r : g_cfg.rules) {
            if (r.key != vk || r.tap == 0 || r.on != ON_RELEASE) continue;
            if (r.minHoldMs > 0 && held < r.minHoldMs) continue;
            if (!condOk(r, wasLogical)) continue;
            Action a{};
            a.when = nowMs() + r.delayMs;
            a.vk = r.tap;
            a.tapMs = r.tapMs;
            pushAction(a);
            logf("[dks-release] %s -> tap %s %dms (held %lldms)",
                 keyName(vk).c_str(), keyName(r.tap).c_str(), r.tapMs, held);
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------
// 配置解析
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static bool loadConfig(const std::wstring& path, std::string& err) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { err = "配置文件不存在: " + u8(path); return false; }
    std::vector<std::string> lines;
    char buf[1024];
    bool first = true;
    while (fgets(buf, sizeof(buf), f)) {
        std::string s = buf;
        // 去掉 UTF-8 BOM（记事本/PowerShell 保存时可能加上，否则第一行会被当成无效键)
        if (first && s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s = s.substr(3);
        first = false;
        lines.push_back(s);
    }
    fclose(f);

    Config c; c.path = path;
    for (const std::string& raw : lines) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '/') continue;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '#' || line[i] == ';') { line = trim(line.substr(0, i)); break; }
        }
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        std::vector<std::string> toks;
        std::string cur;
        for (char ch : val) {
            if (isspace((unsigned char)ch)) { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
            else cur.push_back(ch);
        }
        if (!cur.empty()) toks.push_back(cur);

        auto getTok = [&](const std::string& name, const std::string& def) -> std::string {
            for (const std::string& t : toks) {
                size_t p = t.find('=');
                if (p != std::string::npos && trim(t.substr(0, p)) == name) return trim(t.substr(p + 1));
            }
            return def;
        };

        if (key == "debug") c.debug = (!toks.empty() && toks[0] != "0");
        else if (key == "autostart") c.autostart = (!toks.empty() && toks[0] != "0");
        else if (key == "toggle_key") c.toggleKey = parseKey(toks.empty() ? "" : toks[0]);
        else if (key == "enabled" || key == "master" || key == "总开关")
            c.enabled = toks.empty() || toks[0] != "0";
        else if (key == "socd") c.socdEnabled = toks.empty() || toks[0] != "0";
        else if (key == "dks") c.dksEnabled = toks.empty() || toks[0] != "0";
        else if (key == "group") {
            Group g; g.mode = SOC_SNAP;
            for (const std::string& t : toks) {
                if (t.find('=') != std::string::npos) {
                    std::string nm = trim(t.substr(0, t.find('=')));
                    std::string vv = trim(t.substr(t.find('=') + 1));
                    if (nm == "mode") {
                        if (vv == "snap") g.mode = SOC_SNAP;
                        else if (vv == "first") g.mode = SOC_FIRST;
                        else if (vv == "neutral") g.mode = SOC_NEUTRAL;
                        else if (vv == "dominant") g.mode = SOC_DOMINANT;
                        else g.mode = SOC_OFF;
                    } else if (nm == "dominant") g.dominant = parseKey(vv);
                    continue;
                }
                int vk = parseKey(t);
                if (vk) g.keys.push_back(vk);
            }
            if (!g.keys.empty()) c.groups.push_back(g);
        }
        else if (key == "rule") {
            Rule r;
            std::string onS = getTok("on", "release");
            if (onS == "press") r.on = ON_PRESS;
            else if (onS == "hold") r.on = ON_HOLD;
            else if (onS == "repeat") r.on = ON_REPEAT;
            else r.on = ON_RELEASE;
            r.key = parseKey(getTok("key", ""));
            r.tap = parseKey(getTok("tap", ""));
            r.tapMs = atoi(getTok("tap_ms", "25").c_str());
            r.delayMs = atoi(getTok("delay_ms", "0").c_str());
            r.minHoldMs = atoi(getTok("min_hold_ms", "0").c_str());
            r.intervalMs = atoi(getTok("interval_ms", "0").c_str());
            r.count = atoi(getTok("count", "0").c_str());
            std::string condS = getTok("cond", "active");
            if (condS == "always") r.cond = C_ALWAYS;
            else if (condS == "target_up") r.cond = C_TARGET_UP;
            else if (condS == "target_free") r.cond = C_TARGET_FREE;
            else r.cond = C_ACTIVE;
            if (!r.key || !r.tap) { err += "规则缺少 key 或 tap，已忽略: " + line + "\n"; }
            else if (r.key == r.tap) { err += "规则 key 与 tap 相同，已忽略: " + keyName(r.key) + "\n"; }
            else c.rules.push_back(r);
        }
    }
    g_cfg = c;
    return true;
}

static void buildManagedKeys() {
    g_groupOf.clear();
    for (size_t i = 0; i < g_cfg.groups.size(); ++i)
        for (int vk : g_cfg.groups[i].keys) g_groupOf[vk] = (int)i;

    // 保留当前物理状态（改规则时不应把正在按着的键"重置"）
    std::unordered_map<int, KeyState> fresh;
    auto addKey = [&](int vk) {
        if (!vk) return;
        if (fresh.count(vk)) return;
        auto it = g_state.find(vk);
        fresh[vk] = (it != g_state.end()) ? it->second : KeyState{};
    };
    for (auto& kv : g_groupOf) addKey(kv.first);
    for (const Rule& r : g_cfg.rules) { addKey(r.key); addKey(r.tap); }
    if (g_cfg.toggleKey) addKey(g_cfg.toggleKey);

    // 不再托管的键：若是我们注入的"按下"，先释放，避免卡键
    for (auto& kv : g_state)
        if (!fresh.count(kv.first) && kv.second.logical && !kv.second.phys) injectKey(kv.first, false);

    g_state = std::move(fresh);
}

static void printConfig() {
    if (!g_console) return;
    printf("配置文件: %s\n", u8(g_cfg.path).c_str());
    printf("debug=%d autostart=%d toggle_key=%s enabled=%d socd=%d dks=%d\n",
           g_cfg.debug ? 1 : 0, g_cfg.autostart ? 1 : 0, keyName(g_cfg.toggleKey).c_str(),
           g_cfg.enabled ? 1 : 0, g_cfg.socdEnabled ? 1 : 0, g_cfg.dksEnabled ? 1 : 0);
    printf("实际生效: SOCD=%s DKS=%s\n", socdActive() ? "开" : "关", dksActive() ? "开" : "关");
    for (const Group& g : g_cfg.groups) {
        printf("SOCD 组:");
        for (int vk : g.keys) printf(" %s", keyName(vk).c_str());
        const char* m = g.mode == SOC_SNAP ? "snap" : g.mode == SOC_FIRST ? "first"
                      : g.mode == SOC_NEUTRAL ? "neutral" : g.mode == SOC_DOMINANT ? "dominant" : "off";
        printf("  mode=%s%s\n", m, g.dominant ? (" dominant=" + keyName(g.dominant)).c_str() : "");
    }
    for (const Rule& r : g_cfg.rules) {
        const char* on = r.on == ON_PRESS ? "press" : r.on == ON_RELEASE ? "release"
                       : r.on == ON_HOLD ? "hold" : "repeat";
        const char* cd = r.cond == C_ALWAYS ? "always" : r.cond == C_ACTIVE ? "active"
                       : r.cond == C_TARGET_UP ? "target_up" : "target_free";
        printf("规则: %s %s -> %s  tap_ms=%d delay=%d min_hold=%d interval=%d count=%d cond=%s\n",
               keyName(r.key).c_str(), on, keyName(r.tap).c_str(), r.tapMs, r.delayMs,
               r.minHoldMs, r.intervalMs, r.count, cd);
    }
}

// ---------------------------------------------------------------------------
// 托盘图标
// ---------------------------------------------------------------------------
// 图标来自内嵌资源（见 src/resources.rc + icons/*.ico）：
//   2 = 开启状态图标, 3 = 关闭状态图标
// 资源不存在时回退到程序自绘的图标，保证在任何情况下都有图标。
static HICON g_iconOn = nullptr, g_iconOff = nullptr;

// 从内嵌的 .ico 原始字节手工构造 HICON（CreateIconFromResourceEx 对
// 多尺寸 / 非标准排列的 ICO 不够可靠，这里直接读 XOR 位图 + AND 掩码）。
static HICON loadEmbeddedIcon(int resId) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), L"ICOFILE");
    if (!r) return nullptr;
    DWORD sz = SizeofResource(nullptr, r);
    HGLOBAL h = LoadResource(nullptr, r);
    if (!h) return nullptr;
    const BYTE* data = (const BYTE*)LockResource(h);
    if (!data || sz < 22) return nullptr;

    WORD count = *(const WORD*)(data + 4);
    int bestOff = -1, bestLen = 0, bestScore = 1 << 30;
    for (int i = 0; i < count; ++i) {
        const BYTE* e = data + 6 + i * 16;
        int w = e[0] ? e[0] : 256, hh = e[1] ? e[1] : 256;
        DWORD len = (DWORD)e[8] | ((DWORD)e[9] << 8) | ((DWORD)e[10] << 16) | ((DWORD)e[11] << 24);
        DWORD off = (DWORD)e[12] | ((DWORD)e[13] << 8) | ((DWORD)e[14] << 16) | ((DWORD)e[15] << 24);
        if (off + 40 > sz || off + len > sz) continue;
        int score = (w - 32) * (w - 32) + (hh - 32) * (hh - 32);   // 托盘图标标准是 32x32
        if (score < bestScore) { bestScore = score; bestOff = (int)off; bestLen = (int)len; }
    }
    if (bestOff < 0) return nullptr;

    const BYTE* dib = data + bestOff;
    int w = (int)*(const DWORD*)(dib + 4);
    int hFull = (int)*(const DWORD*)(dib + 8);
    int bpp = (int)*(const WORD*)(dib + 14);
    if (w <= 0 || hFull <= 0 || w > 256 || hFull > 512 || bpp < 1) return nullptr;

    // ICO 里的 BMP 通常带 AND 掩码：biHeight = 2 * 真实高度。
    // 用条目总长度判断到底有没有掩码，比只看 bpp 更可靠（24/32bpp 都可能带掩码）。
    int rowBytes = ((w + 31) / 32) * 4;
    int hh = hFull;
    bool hasMask = false;
    if (40 + (size_t)w * (size_t)hFull * (bpp / 8) != (size_t)bestLen && hFull % 2 == 0) {
        int hHalf = hFull / 2;
        if ((size_t)bestLen == 40 + (size_t)w * (size_t)hHalf * (bpp / 8) + (size_t)rowBytes * (size_t)hHalf) {
            hh = hHalf;
            hasMask = true;
        }
    }
    if (hh <= 0) return nullptr;

    BITMAPINFOHEADER bh{};
    bh.biSize = sizeof(bh);
    bh.biWidth = w; bh.biHeight = hh;                  // 自底向上
    bh.biPlanes = 1; bh.biBitCount = (WORD)bpp; bh.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBitmap(screen, &bh, CBM_INIT, dib + 40, (const BITMAPINFO*)&bh, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    std::vector<unsigned char> mask((size_t)rowBytes * (size_t)hh, 0xFF);   // 全不透明
    if (hasMask) {
        const BYTE* m = dib + 40 + (size_t)w * (size_t)hh * (bpp / 8);
        memcpy(mask.data(), m, mask.size());
    }
    HBITMAP hmask = CreateBitmap(w, hh, 1, 1, mask.data());
    ICONINFO ii{}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = hmask;
    HICON icon = CreateIconIndirect(&ii);
    if (color) DeleteObject(color);
    if (hmask) DeleteObject(hmask);
    return icon;
}

// 程序主图标（资源 1 = icons/BinKey.ico），用于窗口标题栏 / Alt-Tab。
static HICON g_appIcon = nullptr;

static HICON appIcon() {
    if (!g_appIcon) {
        g_appIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                      IMAGE_ICON, 32, 32, 0);
        if (!g_appIcon)
            g_appIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                          IMAGE_ICON, 0, 0, 0);
    }
    return g_appIcon;
}

static HICON makeIcon(bool enabled) {
    const int W = 32, H = 32;
    std::vector<uint32_t> px(W * H, 0);
    auto setPx = [&](int x, int y, uint32_t c) { if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = c; };
    auto fillRect = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) setPx(x, y, c);
    };
    auto insideTri = [&](int x, int y, int ax, int ay, int bx, int by, int cx, int cy) {
        auto sgn = [&](int p1x, int p1y, int q1x, int q1y, int r1x, int r1y) {
            return (long long)(q1x - p1x) * (r1y - p1y) - (long long)(q1y - p1y) * (r1x - p1x);
        };
        long long d1 = sgn(x, y, ax, ay, bx, by), d2 = sgn(x, y, bx, by, cx, cy), d3 = sgn(x, y, cx, cy, ax, ay);
        return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
    };
    auto rounded = [&](int x, int y, int x0, int y0, int x1, int y1, int r) {
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        int cx = x < x0 + r ? x0 + r : (x > x1 - r ? x1 - r : x);
        int cy = y < y0 + r ? y0 + r : (y > y1 - r ? y1 - r : y);
        int dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    };

    uint32_t bg     = enabled ? 0xFF33598Cu : 0xFF6A7079u;   // ARGB
    uint32_t accent = enabled ? 0xFF3EC8B0u : 0xFFAAB0B8u;

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (rounded(x, y, 1, 1, 30, 30, 7)) setPx(x, y, bg);

    fillRect(8, 15, 23, 17, accent);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        if (insideTri(x, y, 4, 16, 10, 10, 10, 22)) setPx(x, y, accent);
        if (insideTri(x, y, 27, 16, 21, 10, 21, 22)) setPx(x, y, accent);
    }
    fillRect(15, 12, 16, 20, 0xFFFFFFFFu);

    // 32 位颜色位图：用 DIB 而不是 CreateBitmap(…,32,1,…)（后者在 32bpp 下不可靠）
    BITMAPINFOHEADER bh{};
    bh.biSize = sizeof(bh); bh.biWidth = W; bh.biHeight = H;
    bh.biPlanes = 1; bh.biBitCount = 32; bh.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HBITMAP hColor = CreateCompatibleBitmap(screen, W, H);
    SetDIBits(screen, hColor, 0, H, px.data(), (BITMAPINFO*)&bh, 1);   // 1 = DIB_TOP_DOWN
    ReleaseDC(nullptr, screen);

    // 蒙版全 1（不透明），真正的透明靠 32bpp 的 alpha 通道决定；
    // 这样即使某些环境忽略 alpha，图标也不会变成空白。
    std::vector<uint8_t> maskBytes((size_t)W * H / 8, 0xFF);
    HBITMAP hMask = CreateBitmap(W, H, 1, 1, maskBytes.data());

    ICONINFO ii{}; ii.fIcon = TRUE; ii.hbmMask = hMask; ii.hbmColor = hColor;
    HICON icon = CreateIconIndirect(&ii);
    if (hColor) DeleteObject(hColor);
    if (hMask) DeleteObject(hMask);
    return icon;
}

// ---------------------------------------------------------------------------
// 托盘窗口
// ---------------------------------------------------------------------------
static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_OPEN_EDITOR = WM_APP + 2;   // 外部进程请求：打开规则编辑器
static HWND g_trayWnd = nullptr;

static void setTrayIcon() {
    if (!g_trayWnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = totalEnabled() ? g_iconOn : g_iconOff;
    swprintf(nid.szTip, sizeof(nid.szTip) / sizeof(WCHAR),
             L"BinKey - SOCD:%s DKS:%s%s",
             socdActive() ? L"开" : L"关", dksActive() ? L"开" : L"关",
             g_cfg.enabled ? L"" : L" (总开关已关闭)");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void reloadConfig() {
    std::string err;
    if (loadConfig(g_cfg.path, err)) buildManagedKeys();
    logf("[reload] 配置已重新加载");
}

static void setAutostart(bool on);   // 前向声明（定义在托盘部分之后）
static void openEditor();            // 前向声明（定义在后面）

static void showMenu() {
    // 必须用 CreatePopupMenu：CreateMenu 创建的是"菜单栏"，用 TrackPopupMenu
    // 弹出时只有几像素宽，表现为"菜单显示异常"（实测 6x101 vs 170x103）。
    HMENU menu = CreatePopupMenu();
    // 总开关的标签：关闭时提示"恢复上次设定"，避免用户以为重新启用会把两个都打开
    std::wstring allLabel;
    if (totalEnabled()) allLabel = L"禁用全部功能";
    else if (g_cfg.socdEnabled || g_cfg.dksEnabled) {
        std::wstring saved;
        if (g_cfg.socdEnabled && g_cfg.dksEnabled) saved = L"SOCD + DKS";
        else if (g_cfg.socdEnabled) saved = L"仅 SOCD";
        else saved = L"仅 DKS";
        allLabel = L"启用全部功能（恢复上次设定: " + saved + L"）";
    } else {
        allLabel = L"启用全部功能";
    }
    AppendMenuW(menu, MF_STRING, 1, allLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, socdActive() ? L"SOCD(Snap Tap)：开启  [点击关闭]"
                                                 : L"SOCD(Snap Tap)：关闭  [点击开启]");
    AppendMenuW(menu, MF_STRING, 3, dksActive() ? L"DKS(动态键程)：开启  [点击关闭]"
                                                : L"DKS(动态键程)：关闭  [点击开启]");
    AppendMenuW(menu, MF_STRING, 4, L"规则编辑器 (自定义 DKS / SOCD 规则)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"重新加载配置");
    AppendMenuW(menu, MF_STRING, 6, L"打开配置文件");
    AppendMenuW(menu, MF_STRING, 7, g_cfg.autostart ? L"取消开机自启" : L"设为开机自启");
    AppendMenuW(menu, MF_STRING, 8, L"打印当前规则到日志");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"退出");

    // 菜单位置：优先用托盘图标附近的光标位置；若菜单会超出屏幕右边界则左移。
    POINT p; GetCursorPos(&p);
    int w = GetSystemMetrics(SM_CXSCREEN);
    const int estWidth = 340;
    if (p.x + estWidth > w) p.x = w - estWidth;
    if (p.x < 0) p.x = 0;

    SetForegroundWindow(g_trayWnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, g_trayWnd, nullptr);
    // 让菜单在点击别处时正确关闭（经典托盘菜单技巧）
    PostMessageW(g_trayWnd, WM_NCHITTEST, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// 托盘点击配对（一次物理点击只触发一次动作）
// ---------------------------------------------------------------------------
// 一次物理点击通常收到 WM_LBUTTONDOWN 然后 WM_LBUTTONUP（有的外壳只发其中一个）。
// 用"按下 → 松手"配对状态机，而不是时间防抖：时间防抖会把"按下后超过防抖窗口
// 才松手"的 DOWN 和 UP 当成两次点击，于是切换两次 —— 用户看到的就是
// "左键单击没反应 / 关不掉插件"。
struct TrayClick {
    int  lastEvent   = 0;     // 上一条托盘事件
    long long lastMs = 0;
    bool downPending = false; // 有一次"按下"还没有对应的"松手"
    long long downMs = 0;
};

static TrayClick g_leftClick{}, g_rightClick{};

static bool newTrayClick(TrayClick& c, int ev, long long now) {
    const long long pairWindow = 5000;   // 按下与松手属于同一点击的最大间隔
    const long long dupWindow  = 200;    // 外壳重发同一事件（真正的重复消息）
    bool isDown = (ev == WM_LBUTTONDOWN || ev == WM_RBUTTONDOWN || ev == WM_LBUTTONDBLCLK);

    if (c.lastEvent == ev && now - c.lastMs < dupWindow) return false;   // 重复消息

    if (isDown) {
        c.downPending = true; c.downMs = now;
        c.lastEvent = ev; c.lastMs = now;
        return true;                       // 按下（含双击的第二次按下）就是一次点击
    }
    // UP：若它对应一次尚未关闭的"按下"，只是同一次点击的收尾，不重复触发
    if (c.downPending && now - c.downMs < pairWindow) {
        c.downPending = false;
        c.lastEvent = ev; c.lastMs = now;
        return false;
    }
    // 没有对应的按下 —— 有的外壳只发 UP，把这条 UP 当作一次完整点击
    c.downPending = false;
    c.lastEvent = ev; c.lastMs = now;
    return true;
}

static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TRAY) {
        long long now = nowMs();
        if (l == WM_RBUTTONDOWN || l == WM_RBUTTONUP) {
            if (newTrayClick(g_rightClick, (int)l, now)) { logf("[tray] 右键：弹出菜单"); showMenu(); }
            return 0;
        }
        if (l == WM_LBUTTONDOWN || l == WM_LBUTTONUP || l == WM_LBUTTONDBLCLK) {
            if (newTrayClick(g_leftClick, (int)l, now)) {
                toggleAll();
                logf("[tray] 左键：全部功能 %s", totalEnabled() ? "启用" : "禁用");
            }
            return 0;
        }
        return 0;
    }
    // 外部程序（安装引导 / 第二个实例）请求打开规则编辑器
    if (msg == WM_OPEN_EDITOR) { openEditor(); return 0; }

    if (msg == WM_COMMAND) {
        // 弹出菜单的 WM_COMMAND：wParam 低 16 位是菜单项 id，高 16 位是菜单句柄
        switch (LOWORD(w)) {
            case 1: toggleAll(); break;
            case 2: setFeature(2, !socdActive()); break;
            case 3: setFeature(3, !dksActive()); break;
            case 4: openEditor(); break;
            case 5: reloadConfig(); break;
            case 6: ShellExecuteW(nullptr, L"open", g_cfg.path.c_str(), nullptr, nullptr, SW_SHOW); break;
            case 7:
                g_cfg.autostart = !g_cfg.autostart;
                setAutostart(g_cfg.autostart);
                logf("[tray] 开机自启 = %s", g_cfg.autostart ? "开启" : "关闭");
                break;
            case 8: {
                g_cfg.debug = true;
                if (!g_logFile) g_logFile = _wfopen(g_logPath.c_str(), L"a");
                logf("--- 当前规则 ---");
                for (const Group& g : g_cfg.groups) {
                    std::string ks;
                    for (int vk : g.keys) ks += keyName(vk) + " ";
                    logf("  SOCD 组 [%s] mode=%s%s", ks.c_str(),
                         g.mode == SOC_SNAP ? "snap" : g.mode == SOC_FIRST ? "first"
                         : g.mode == SOC_NEUTRAL ? "neutral" : g.mode == SOC_DOMINANT ? "dominant" : "off",
                         g.mode == SOC_DOMINANT ? ("  dominant=" + keyName(g.dominant)) : "");
                }
                for (const Rule& r : g_cfg.rules) {
                    logf("  %s %s -> %s tap_ms=%d min_hold=%d delay=%d interval=%d count=%d",
                         keyName(r.key).c_str(),
                         r.on == ON_PRESS ? "press" : r.on == ON_RELEASE ? "release"
                         : r.on == ON_HOLD ? "hold" : "repeat",
                         keyName(r.tap).c_str(), r.tapMs, r.minHoldMs, r.delayMs, r.intervalMs, r.count);
                }
                break;
            }
            case 9: g_running = false; break;
        }
        return 0;
    }
    if (msg == WM_TIMER) { processDueActions(); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, w, l);
}

static void installTray() {
    g_appIcon = appIcon();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = trayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BinKeyTrayWindow";
    wc.hIcon = g_appIcon;          // 窗口/Alt-Tab 图标
    wc.hIconSm = g_appIcon;
    RegisterClassExW(&wc);
    g_trayWnd = CreateWindowExW(0, wc.lpszClassName, L"BinKey", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);

    g_iconOn  = loadEmbeddedIcon(2); if (!g_iconOn)  g_iconOn  = makeIcon(true);
    g_iconOff = loadEmbeddedIcon(3); if (!g_iconOff) g_iconOff = makeIcon(false);
    logf("[tray] 图标: 开启=%s 关闭=%s 程序=%s",
         g_iconOn  ? "内嵌 tray_on.ico"  : "自绘(回退)",
         g_iconOff ? "内嵌 tray_off.ico" : "自绘(回退)",
         g_appIcon ? "内嵌 BinKey.ico"   : "无");

    // 定时器：即使主循环被 TrackPopupMenu 阻塞（弹出菜单打开期间），
    // 队列里的注入动作仍会被执行，避免"菜单打开时 DKS 不生效"。
    SetTimer(g_trayWnd, 1, 20, nullptr);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = totalEnabled() ? g_iconOn : g_iconOff;
    swprintf(nid.szTip, sizeof(nid.szTip) / sizeof(WCHAR),
             L"BinKey - SOCD:%s DKS:%s", socdActive() ? L"开" : L"关",
             dksActive() ? L"开" : L"关");
    BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    logf("[tray] NIM_ADD: %s", added ? "成功" : "失败");
    if (!added) {
        // 回退：用自绘图标再试一次，确保托盘一定有图标
        nid.hIcon = makeIcon(totalEnabled());
        added = Shell_NotifyIconW(NIM_ADD, &nid);
        logf("[tray] NIM_ADD(回退): %s", added ? "成功" : "失败");
    }
}

static void removeTray() {
    if (!g_trayWnd) return;
    KillTimer(g_trayWnd, 1);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------------------------------------------------------------------------
// 开机自启
// ---------------------------------------------------------------------------
static void setAutostart(bool on) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    if (on) RegSetValueExW(hk, L"BinKey", 0, REG_SZ, (const BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
    else RegDeleteValueW(hk, L"BinKey");
    RegCloseKey(hk);
}

// ---------------------------------------------------------------------------
// 保存配置（规则编辑器写回 config.cfg）
// ---------------------------------------------------------------------------
static std::wstring toW(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static bool saveConfig() {
    FILE* f = _wfopen(g_cfg.path.c_str(), L"wb");
    if (!f) return false;
    fputs("# BinKey 配置文件 —— 由 BinKey 规则编辑器保存\n", f);
    fputs("# 手动编辑也可以，格式见 README.md\n", f);
    fputs("# enabled = 总开关。禁用全部功能只关掉总开关，重新启用时\n", f);
    fputs("#           恢复 socd / dks 各自的设定（不会两个都打开）\n", f);
    fprintf(f, "debug = %d\n", g_cfg.debug ? 1 : 0);
    fprintf(f, "autostart = %d\n", g_cfg.autostart ? 1 : 0);
    fprintf(f, "toggle_key = %s\n", keyName(g_cfg.toggleKey).c_str());
    fprintf(f, "enabled = %d\n", g_cfg.enabled ? 1 : 0);
    fprintf(f, "socd = %d\n", g_cfg.socdEnabled ? 1 : 0);
    fprintf(f, "dks = %d\n", g_cfg.dksEnabled ? 1 : 0);
    fputs("\n# ---- SOCD(Snap Tap) 组 ----\n", f);
    if (g_cfg.groups.empty()) fputs("# (无)\n", f);
    for (const Group& g : g_cfg.groups) {
        std::string mode = g.mode == SOC_SNAP ? "snap" : g.mode == SOC_FIRST ? "first"
                         : g.mode == SOC_NEUTRAL ? "neutral" : g.mode == SOC_DOMINANT ? "dominant" : "off";
        fprintf(f, "group =");
        for (int vk : g.keys) fprintf(f, " %s", keyName(vk).c_str());
        fprintf(f, " mode=%s", mode.c_str());
        if (g.mode == SOC_DOMINANT && g.dominant) fprintf(f, " dominant=%s", keyName(g.dominant).c_str());
        fputc('\n', f);
    }
    fputs("\n# ---- DKS(动态键程) 规则 ----\n", f);
    fputs("# on = release(松开时) | press(按下时) | hold(按住超过 min_hold_ms) | repeat(连发)\n", f);
    fputs("# cond = active(默认) | always | target_up | target_free\n", f);
    if (g_cfg.rules.empty()) fputs("# (无)\n", f);
    for (const Rule& r : g_cfg.rules) {
        const char* on = r.on == ON_PRESS ? "press" : r.on == ON_RELEASE ? "release"
                       : r.on == ON_HOLD ? "hold" : "repeat";
        const char* cd = r.cond == C_ALWAYS ? "always" : r.cond == C_ACTIVE ? "active"
                       : r.cond == C_TARGET_UP ? "target_up" : "target_free";
        fprintf(f, "rule = key=%s on=%s tap=%s tap_ms=%d delay_ms=%d min_hold_ms=%d interval_ms=%d count=%d cond=%s\n",
                keyName(r.key).c_str(), on, keyName(r.tap).c_str(), r.tapMs, r.delayMs,
                r.minHoldMs, r.intervalMs, r.count, cd);
    }
    fclose(f);
    logf("[save] 配置已写入 %s", u8(g_cfg.path).c_str());
    return true;
}

// 只更新 config.cfg 里的开关行（保留其它内容与注释），
// 用于托盘 / 热键切换的持久化，这样重启后能恢复上次的单独设定。
static bool persistSwitches() {
    if (g_cfg.path.empty()) return false;
    FILE* f = _wfopen(g_cfg.path.c_str(), L"rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, buf + n);
    fclose(f);

    std::vector<std::string> lines;
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        if (nl == std::string::npos) { lines.push_back(data.substr(i)); break; }
        lines.push_back(data.substr(i, nl - i));
        i = nl + 1;
    }

    bool haveSocd = false, haveDks = false, haveEnabled = false;
    for (std::string& line : lines) {
        std::string s = line;
        size_t cr = 0;
        while (!s.empty() && s.back() == '\r') { s.pop_back(); ++cr; }
        std::string t = trim(s);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        if (key != "socd" && key != "dks" && key != "enabled") continue;
        std::string val = trim(t.substr(eq + 1));
        size_t hash = val.find('#');
        std::string comment = (hash == std::string::npos) ? "" : "  " + trim(val.substr(hash));
        if (key == "socd")      { val = g_cfg.socdEnabled ? "1" : "0"; haveSocd = true; }
        else if (key == "dks")  { val = g_cfg.dksEnabled ? "1" : "0";  haveDks = true; }
        else                    { val = g_cfg.enabled ? "1" : "0";     haveEnabled = true; }
        line = key + " = " + val + comment + (cr ? std::string(cr, '\r') : std::string());
    }
    // 缺少的开关行追加到文件末尾（解析器不关心顺序）
    if (!haveEnabled) lines.push_back("enabled = " + std::string(g_cfg.enabled ? "1" : "0"));
    if (!haveSocd)    lines.push_back("socd = " + std::string(g_cfg.socdEnabled ? "1" : "0"));
    if (!haveDks)     lines.push_back("dks = " + std::string(g_cfg.dksEnabled ? "1" : "0"));

    std::string out;
    for (size_t k = 0; k < lines.size(); ++k) { out += lines[k]; if (k + 1 < lines.size()) out += "\n"; }
    out += "\n";

    FILE* w = _wfopen(g_cfg.path.c_str(), L"wb");
    if (!w) return false;
    fwrite(out.data(), 1, out.size(), w);
    fclose(w);
    logf("[save] 开关状态已写入 %s (enabled=%d socd=%d dks=%d)",
         u8(g_cfg.path).c_str(), g_cfg.enabled ? 1 : 0,
         g_cfg.socdEnabled ? 1 : 0, g_cfg.dksEnabled ? 1 : 0);
    return true;
}

// ---------------------------------------------------------------------------
// DKS / SOCD 规则编辑器窗口
// ---------------------------------------------------------------------------
static HWND g_ruleList = nullptr, g_edKey = nullptr, g_edTap = nullptr;
static HWND g_edOn = nullptr, g_edCond = nullptr;
static HWND g_edTapMs = nullptr, g_edDelay = nullptr, g_edMinHold = nullptr;
static HWND g_edInterval = nullptr, g_edCount = nullptr;
static HWND g_socdList = nullptr, g_edGroup = nullptr, g_edGroupMode = nullptr, g_edDominant = nullptr;
static HWND g_chkSocd = nullptr, g_chkDks = nullptr;
static HWND g_statusRule = nullptr, g_statusGroup = nullptr;
static std::vector<int> g_keyChoices;

enum {
    ID_RULE_LIST = 101, ID_KEY = 102, ID_TAP = 103, ID_ON = 104, ID_COND = 105,
    ID_TAPMS = 106, ID_DELAY = 107, ID_MINHOLD = 108, ID_INTERVAL = 109, ID_COUNT = 110,
    ID_BTN_ADD = 111, ID_BTN_UPDATE = 112, ID_BTN_DELETE = 113,
    ID_SOCD_LIST = 121, ID_SOCD_KEYS = 122, ID_SOCD_MODE = 123, ID_SOCD_DOM = 124,
    ID_BTN_SOCD_ADD = 125, ID_BTN_SOCD_UPDATE = 126, ID_BTN_SOCD_DELETE = 127,
    ID_BTN_SAVE = 130, ID_BTN_CLOSE = 131, ID_BTN_DEFAULT = 132,
    ID_CHK_SOC = 133, ID_CHK_DKS = 134,
    ID_STATUS_RULE = 140, ID_STATUS_GROUP = 141
};

// 模式 combo 顺序: snap / first / neutral / dominant / off
static int modeToCombo(int mode) {
    switch (mode) {
        case SOC_SNAP:     return 0;
        case SOC_FIRST:    return 1;
        case SOC_NEUTRAL:  return 2;
        case SOC_DOMINANT: return 3;
        default:           return 4;   // SOC_OFF
    }
}
static int comboToMode(int idx) {
    switch (idx) {
        case 0: return SOC_SNAP;
        case 1: return SOC_FIRST;
        case 2: return SOC_NEUTRAL;
        case 3: return SOC_DOMINANT;
        default: return SOC_OFF;
    }
}

static void initKeyChoices() {
    g_keyChoices.clear();
    for (char c = 'a'; c <= 'z'; ++c) { int vk = VkKeyScanA(c); if (vk >= 0) g_keyChoices.push_back(vk); }
    for (char c = '0'; c <= '9'; ++c) { int vk = VkKeyScanA(c); if (vk >= 0) g_keyChoices.push_back(vk); }
    static const int sp[] = {
        VK_SPACE, VK_RETURN, VK_TAB, VK_ESCAPE, VK_BACK,
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LALT, VK_RALT,
        VK_CAPITAL, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_INSERT,
        VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_1, VK_OEM_2, VK_OEM_3,
        VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_SCROLL, VK_NUMLOCK,
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12
    };
    for (int vk : sp) g_keyChoices.push_back(vk);
}

static int keyComboIndex(HWND combo, int vk) {
    if (!vk) return -1;
    for (size_t i = 0; i < g_keyChoices.size(); ++i) if (g_keyChoices[i] == vk) return (int)i;
    g_keyChoices.push_back(vk);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)toW(keyName(vk)).c_str());
    return (int)g_keyChoices.size() - 1;
}

static HWND mkCtl(HWND parent, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int h,
                  DWORD style = WS_CHILD | WS_VISIBLE, int id = 0) {
    HWND c = CreateWindowExW(0, cls, text, style, x, y, w, h, parent,
                             (HMENU)(uintptr_t)id, GetModuleHandleW(nullptr), nullptr);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static std::string ctlText(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 255);
    return u8(buf);
}
// COMBOBOX(CBS_DROPDOWNLIST) 没有窗口文本，必须用 CB_GETCURSEL + CB_GETLBTEXT
static std::string comboText(HWND h) {
    int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i < 0) return "";
    wchar_t buf[256] = {};
    SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)buf);
    return u8(buf);
}
static int ctlInt(HWND h, int def) {
    std::string s = ctlText(h);
    if (s.empty()) return def;
    return atoi(s.c_str());
}
static int comboSel(HWND h) {
    int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : i;
}

static std::string ruleText(const Rule& r) {
    const char* on = r.on == ON_PRESS ? "按下时" : r.on == ON_RELEASE ? "松开时"
                   : r.on == ON_HOLD ? "按住超过" : "连发";
    std::string s = keyName(r.key) + " " + on + " -> 轻点 " + keyName(r.tap);
    if (r.on == ON_HOLD) s += " " + std::to_string(r.minHoldMs) + "ms";
    if (r.on == ON_REPEAT) s += " 每 " + std::to_string(r.intervalMs) + "ms" +
                              (r.count ? " x" + std::to_string(r.count) : " (无限)");
    s += "  tap=" + std::to_string(r.tapMs) + "ms";
    if (r.delayMs) s += " delay=" + std::to_string(r.delayMs) + "ms";
    if (r.cond != C_ACTIVE) s += std::string(" cond=") + (r.cond == C_ALWAYS ? "always" :
        r.cond == C_TARGET_UP ? "target_up" : "target_free");
    return s;
}

static std::string groupText(const Group& g) {
    std::string s;
    for (int vk : g.keys) s += keyName(vk) + " ";
    s += "[";
    s += g.mode == SOC_SNAP ? "snap" : g.mode == SOC_FIRST ? "first"
         : g.mode == SOC_NEUTRAL ? "neutral" : g.mode == SOC_DOMINANT ? "dominant" : "off";
    if (g.mode == SOC_DOMINANT && g.dominant) s += " 主=" + keyName(g.dominant);
    s += "]";
    return s;
}

static int g_selRule = -1, g_selGroup = -1;

// 只有带 LBS_NOTIFY 的 LISTBOX 才会向父窗口发送 LBN_SELCHANGE；而 LB_SETSEL
// 又只在"多选"列表框上有效（单选列表框调用它完全无效，实测确认）。
// 所以用 EXTENDEDSEL + NOTIFY，并在每次选择后把选择集收敛成一条。
static int listSel(HWND lb) {
    int i = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (i >= 0) return i;
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int k = n - 1; k >= 0; --k)
        if (SendMessageW(lb, LB_GETSEL, k, 0) > 0) return k;
    return -1;
}

static void listPick(HWND lb, int i) {
    SendMessageW(lb, LB_SETSEL, FALSE, -1);
    if (i >= 0) SendMessageW(lb, LB_SETSEL, TRUE, (LPARAM)i);
}

static void updateStatus() {
    wchar_t buf[128];
    if (g_statusRule) {
        if (g_selRule < 0) wcscpy(buf, L"当前选中：无 (先点左侧列表里的某一条)");
        else swprintf(buf, 128, L"当前选中：第 %d 条规则", g_selRule + 1);
        SetWindowTextW(g_statusRule, buf);
    }
    if (g_statusGroup) {
        if (g_selGroup < 0) wcscpy(buf, L"当前选中：无 (先点右侧列表里的某一组)");
        else swprintf(buf, 128, L"当前选中：第 %d 个 SOCD 组", g_selGroup + 1);
        SetWindowTextW(g_statusGroup, buf);
    }
}

static void refreshRuleList() {
    SendMessageW(g_ruleList, LB_RESETCONTENT, 0, 0);
    for (const Rule& r : g_cfg.rules) SendMessageW(g_ruleList, LB_ADDSTRING, 0, (LPARAM)toW(ruleText(r)).c_str());
    if (g_selRule >= (int)g_cfg.rules.size()) g_selRule = (int)g_cfg.rules.size() - 1;
    listPick(g_ruleList, g_selRule);
    updateStatus();
}

static void refreshGroupList() {
    SendMessageW(g_socdList, LB_RESETCONTENT, 0, 0);
    for (const Group& g : g_cfg.groups) SendMessageW(g_socdList, LB_ADDSTRING, 0, (LPARAM)toW(groupText(g)).c_str());
    if (g_selGroup >= (int)g_cfg.groups.size()) g_selGroup = (int)g_cfg.groups.size() - 1;
    listPick(g_socdList, g_selGroup);
    updateStatus();
}

static void fillRuleFields(const Rule& r) {
    SendMessageW(g_edKey, CB_SETCURSEL, keyComboIndex(g_edKey, r.key), 0);
    SendMessageW(g_edTap, CB_SETCURSEL, keyComboIndex(g_edTap, r.tap), 0);
    SendMessageW(g_edOn, CB_SETCURSEL, r.on, 0);
    SendMessageW(g_edCond, CB_SETCURSEL, r.cond, 0);
    wchar_t b[32];
    swprintf(b, 32, L"%d", r.tapMs);      SetWindowTextW(g_edTapMs, b);
    swprintf(b, 32, L"%d", r.delayMs);    SetWindowTextW(g_edDelay, b);
    swprintf(b, 32, L"%d", r.minHoldMs);  SetWindowTextW(g_edMinHold, b);
    swprintf(b, 32, L"%d", r.intervalMs); SetWindowTextW(g_edInterval, b);
    swprintf(b, 32, L"%d", r.count);      SetWindowTextW(g_edCount, b);
}

static bool readRuleFields(Rule& r) {
    std::string ks = comboText(g_edKey), ts = comboText(g_edTap);
    r.key = parseKey(ks);
    r.tap = parseKey(ts);
    if (!r.key) { logf("[editor] 触发键无效: %s", ks.c_str()); return false; }
    if (!r.tap) { logf("[editor] 轻点键无效: %s", ts.c_str()); return false; }
    if (r.key == r.tap) { logf("[editor] 触发键与轻点键相同"); return false; }
    r.on = (OnKind)comboSel(g_edOn);
    r.cond = (Cond)comboSel(g_edCond);
    r.tapMs = ctlInt(g_edTapMs, 25);
    if (r.tapMs < 5) r.tapMs = 5;
    if (r.tapMs > 500) r.tapMs = 500;
    r.delayMs = ctlInt(g_edDelay, 0);
    if (r.delayMs < 0) r.delayMs = 0;
    r.minHoldMs = ctlInt(g_edMinHold, 0);
    if (r.minHoldMs < 0) r.minHoldMs = 0;
    r.intervalMs = ctlInt(g_edInterval, 0);
    if (r.intervalMs < 0) r.intervalMs = 0;
    r.count = ctlInt(g_edCount, 0);
    if (r.count < 0) r.count = 0;
    if (r.on == ON_REPEAT && r.intervalMs <= 0) r.intervalMs = std::max(r.tapMs, 10);
    return true;
}

static void applyChange() {
    buildManagedKeys();
    refreshRuleList();
    refreshGroupList();
    saveConfig();
}

static void editorAddRule() {
    Rule r;
    if (!readRuleFields(r)) return;
    g_cfg.rules.push_back(r);
    g_selRule = (int)g_cfg.rules.size() - 1;
    logf("[editor] 新增规则 %d: %s", g_selRule, ruleText(r).c_str());
    applyChange();
}
static void editorUpdateRule() {
    if (g_selRule < 0) g_selRule = listSel(g_ruleList);
    if (g_selRule < 0 || g_selRule >= (int)g_cfg.rules.size()) { logf("[editor] 没有选中的规则"); return; }
    Rule r;
    if (!readRuleFields(r)) return;
    g_cfg.rules[g_selRule] = r;
    logf("[editor] 更新规则 %d: %s", g_selRule, ruleText(r).c_str());
    applyChange();
}
static void editorDeleteRule() {
    if (g_selRule < 0) g_selRule = listSel(g_ruleList);
    if (g_selRule < 0 || g_selRule >= (int)g_cfg.rules.size()) { logf("[editor] 没有选中的规则"); return; }
    logf("[editor] 删除规则 %d: %s", g_selRule, ruleText(g_cfg.rules[g_selRule]).c_str());
    g_cfg.rules.erase(g_cfg.rules.begin() + g_selRule);
    if (g_selRule >= (int)g_cfg.rules.size()) g_selRule = (int)g_cfg.rules.size() - 1;
    if (g_selRule >= 0) fillRuleFields(g_cfg.rules[g_selRule]);
    applyChange();
}

static void editorAddGroup() {
    std::string s = ctlText(g_edGroup);
    Group g; g.mode = (SocdMode)comboToMode(comboSel(g_edGroupMode));
    std::string cur;
    for (char ch : s) {
        if (isspace((unsigned char)ch)) { if (!cur.empty()) { int vk = parseKey(cur); if (vk) g.keys.push_back(vk); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (!cur.empty()) { int vk = parseKey(cur); if (vk) g.keys.push_back(vk); }
    if (g.keys.size() < 2) { logf("[editor] SOCD 组至少需要两个键"); return; }
    g.dominant = parseKey(comboText(g_edDominant));
    if (g.mode == SOC_DOMINANT && !g.dominant) g.dominant = g.keys[0];
    g_cfg.groups.push_back(g);
    g_selGroup = (int)g_cfg.groups.size() - 1;
    applyChange();
}
static void editorUpdateGroup() {
    if (g_selGroup < 0) g_selGroup = listSel(g_socdList);
    if (g_selGroup < 0 || g_selGroup >= (int)g_cfg.groups.size()) { logf("[editor] 没有选中的 SOCD 组"); return; }
    Group g = g_cfg.groups[g_selGroup];
    std::string s = ctlText(g_edGroup);
    g.mode = (SocdMode)comboToMode(comboSel(g_edGroupMode));
    std::vector<int> keys; std::string cur;
    for (char ch : s) {
        if (isspace((unsigned char)ch)) { if (!cur.empty()) { int vk = parseKey(cur); if (vk) keys.push_back(vk); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (!cur.empty()) { int vk = parseKey(cur); if (vk) keys.push_back(vk); }
    if (keys.size() >= 2) g.keys = keys;
    g.dominant = parseKey(comboText(g_edDominant));
    if (g.mode == SOC_DOMINANT && !g.dominant) g.dominant = g.keys[0];
    g_cfg.groups[g_selGroup] = g;
    applyChange();
}
static void editorDeleteGroup() {
    if (g_selGroup < 0) g_selGroup = listSel(g_socdList);
    if (g_selGroup < 0 || g_selGroup >= (int)g_cfg.groups.size()) { logf("[editor] 没有选中的 SOCD 组"); return; }
    g_cfg.groups.erase(g_cfg.groups.begin() + g_selGroup);
    if (g_selGroup >= (int)g_cfg.groups.size()) g_selGroup = (int)g_cfg.groups.size() - 1;
    if (g_selGroup >= 0) {
        const Group& g = g_cfg.groups[g_selGroup];
        std::string s;
        for (int vk : g.keys) s += keyName(vk) + " ";
        SetWindowTextW(g_edGroup, toW(s).c_str());
        SendMessageW(g_edGroupMode, CB_SETCURSEL, modeToCombo(g.mode), 0);
        SendMessageW(g_edDominant, CB_SETCURSEL, keyComboIndex(g_edDominant, g.dominant), 0);
    }
    applyChange();
}

static LRESULT CALLBACK editorWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(w), notif = HIWORD(w);
            switch (id) {
                case ID_RULE_LIST:
                    // LBN_DOUBLECLICK(0x0002) 在 MinGW 头文件里没有定义，用字面值
                    if (notif == LBN_SELCHANGE || notif == 0x0002) {
                        int i = listSel(g_ruleList);
                        if (i >= 0 && i < (int)g_cfg.rules.size()) {
                            g_selRule = i;
                            listPick(g_ruleList, i);          // 只保留一条选中
                            fillRuleFields(g_cfg.rules[i]);
                            updateStatus();
                        }
                    }
                    break;
                case ID_BTN_ADD:    editorAddRule();    break;
                case ID_BTN_UPDATE: editorUpdateRule(); break;
                case ID_BTN_DELETE: editorDeleteRule(); break;
                case ID_SOCD_LIST:
                    if (notif == LBN_SELCHANGE || notif == 0x0002) {
                        int i = listSel(g_socdList);
                        if (i >= 0 && i < (int)g_cfg.groups.size()) {
                            g_selGroup = i;
                            listPick(g_socdList, i);
                            const Group& g = g_cfg.groups[i];
                            std::string s;
                            for (int vk : g.keys) s += keyName(vk) + " ";
                            SetWindowTextW(g_edGroup, toW(s).c_str());
                            SendMessageW(g_edGroupMode, CB_SETCURSEL, modeToCombo(g.mode), 0);
                            SendMessageW(g_edDominant, CB_SETCURSEL, keyComboIndex(g_edDominant, g.dominant), 0);
                            updateStatus();
                        }
                    }
                    break;
                case ID_BTN_SOCD_ADD:    editorAddGroup();    break;
                case ID_BTN_SOCD_UPDATE: editorUpdateGroup(); break;
                case ID_BTN_SOCD_DELETE: editorDeleteGroup(); break;
                case ID_BTN_SAVE:   saveConfig(); break;
                case ID_CHK_SOC:
                    setFeature(2, (SendMessageW(g_chkSocd, BM_GETCHECK, 0, 0) == BST_CHECKED));
                    saveConfig();
                    break;
                case ID_CHK_DKS:
                    setFeature(3, (SendMessageW(g_chkDks, BM_GETCHECK, 0, 0) == BST_CHECKED));
                    saveConfig();
                    break;
                case ID_BTN_DEFAULT: {
                    g_cfg.rules.clear();
                    g_cfg.groups.clear();
                    Group ga; ga.keys = {parseKey("A"), parseKey("D")}; ga.mode = SOC_SNAP;
                    Group gw; gw.keys = {parseKey("W"), parseKey("S")}; gw.mode = SOC_SNAP;
                    g_cfg.groups.push_back(ga); g_cfg.groups.push_back(gw);
                    auto mkRule = [&](const char* k, OnKind on, const char* tap, int tapMs, int minHold) {
                        Rule r; r.key = parseKey(k); r.tap = parseKey(tap); r.on = on;
                        r.tapMs = tapMs; r.minHoldMs = minHold; r.cond = C_ACTIVE; g_cfg.rules.push_back(r);
                    };
                    mkRule("A", ON_RELEASE, "D", 25, 40);
                    mkRule("D", ON_RELEASE, "A", 25, 40);
                    mkRule("W", ON_RELEASE, "S", 20, 40);
                    mkRule("S", ON_RELEASE, "W", 20, 40);
                    g_selRule = -1; g_selGroup = -1;
                    applyChange();
                    break;
                }
                case ID_BTN_CLOSE:  DestroyWindow(h); break;
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: g_editorWnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void openEditor() {
    if (g_editorWnd) { ShowWindow(g_editorWnd, SW_SHOW); SetForegroundWindow(g_editorWnd); return; }
    initKeyChoices();

    static const wchar_t* kEditorCls = L"BinKeyEditor";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = editorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kEditorCls;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = appIcon();
        wc.hIconSm = appIcon();
        RegisterClassExW(&wc);
        registered = true;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int W = 790, H = 560;
    g_editorWnd = CreateWindowExW(0, kEditorCls, L"BinKey 规则编辑器 (SOCD + DKS)",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                                  (sw - W) / 2, (sh - H) / 2, W, H, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!g_editorWnd) { logf("[editor] 窗口创建失败"); return; }
    SendMessageW(g_editorWnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon());
    SendMessageW(g_editorWnd, WM_SETICON, ICON_BIG,   (LPARAM)appIcon());

    // ---- 左：DKS 规则 ----
    mkCtl(g_editorWnd, L"STATIC", L"DKS(动态键程) 规则列表", 12, 10, 340, 18);
    g_ruleList = mkCtl(g_editorWnd, L"LISTBOX", L"", 12, 30, 340, 190,
                       WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOTIFY, ID_RULE_LIST);
    g_statusRule = mkCtl(g_editorWnd, L"STATIC", L"当前选中：无", 12, 342, 340, 20, WS_CHILD | WS_VISIBLE, ID_STATUS_RULE);

    mkCtl(g_editorWnd, L"STATIC", L"触发键", 12, 228, 80, 16);
    g_edKey = mkCtl(g_editorWnd, L"COMBOBOX", L"", 12, 246, 80, 240,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_KEY);
    mkCtl(g_editorWnd, L"STATIC", L"轻点键", 100, 228, 80, 16);
    g_edTap = mkCtl(g_editorWnd, L"COMBOBOX", L"", 100, 246, 80, 240,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_TAP);
    mkCtl(g_editorWnd, L"STATIC", L"触发方式", 188, 228, 90, 16);
    g_edOn = mkCtl(g_editorWnd, L"COMBOBOX", L"", 188, 246, 110, 180,
                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_ON);
    mkCtl(g_editorWnd, L"STATIC", L"条件", 306, 228, 46, 16);
    g_edCond = mkCtl(g_editorWnd, L"COMBOBOX", L"", 306, 246, 46, 180,
                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_COND);

    for (int vk : g_keyChoices) {
        SendMessageW(g_edKey, CB_ADDSTRING, 0, (LPARAM)toW(keyName(vk)).c_str());
        SendMessageW(g_edTap, CB_ADDSTRING, 0, (LPARAM)toW(keyName(vk)).c_str());
    }
    // 顺序必须与 OnKind 枚举一致：ON_PRESS/ON_RELEASE/ON_HOLD/ON_REPEAT
    for (const wchar_t* s : {L"按下时", L"松开时", L"按住超过", L"连发"}) SendMessageW(g_edOn, CB_ADDSTRING, 0, (LPARAM)s);
    // 顺序必须与 Cond 枚举一致：C_ALWAYS/C_ACTIVE/C_TARGET_UP/C_TARGET_FREE
    for (const wchar_t* s : {L"always", L"active", L"target_up", L"target_free"}) SendMessageW(g_edCond, CB_ADDSTRING, 0, (LPARAM)s);

    mkCtl(g_editorWnd, L"STATIC", L"tap_ms", 12, 292, 60, 16);
    g_edTapMs = mkCtl(g_editorWnd, L"EDIT", L"25", 12, 310, 60, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_TAPMS);
    mkCtl(g_editorWnd, L"STATIC", L"延迟ms", 78, 292, 60, 16);
    g_edDelay = mkCtl(g_editorWnd, L"EDIT", L"0", 78, 310, 60, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_DELAY);
    mkCtl(g_editorWnd, L"STATIC", L"min_hold", 144, 292, 66, 16);
    g_edMinHold = mkCtl(g_editorWnd, L"EDIT", L"40", 144, 310, 66, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_MINHOLD);
    mkCtl(g_editorWnd, L"STATIC", L"连发间隔", 216, 292, 66, 16);
    g_edInterval = mkCtl(g_editorWnd, L"EDIT", L"0", 216, 310, 66, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_INTERVAL);
    mkCtl(g_editorWnd, L"STATIC", L"连发次数", 288, 292, 62, 16);
    g_edCount = mkCtl(g_editorWnd, L"EDIT", L"0", 288, 310, 62, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_COUNT);

    mkCtl(g_editorWnd, L"BUTTON", L"新增", 12, 366, 80, 26, WS_CHILD | WS_VISIBLE, ID_BTN_ADD);
    mkCtl(g_editorWnd, L"BUTTON", L"更新选中", 98, 366, 100, 26, WS_CHILD | WS_VISIBLE, ID_BTN_UPDATE);
    mkCtl(g_editorWnd, L"BUTTON", L"删除选中", 204, 366, 100, 26, WS_CHILD | WS_VISIBLE, ID_BTN_DELETE);

    // ---- 右：SOCD 组 ----
    mkCtl(g_editorWnd, L"STATIC", L"SOCD(Snap Tap) 组列表", 372, 10, 370, 18);
    g_socdList = mkCtl(g_editorWnd, L"LISTBOX", L"", 372, 30, 370, 190,
                       WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOTIFY, ID_SOCD_LIST);
    g_statusGroup = mkCtl(g_editorWnd, L"STATIC", L"当前选中：无", 372, 342, 370, 20, WS_CHILD | WS_VISIBLE, ID_STATUS_GROUP);
    mkCtl(g_editorWnd, L"STATIC", L"组内按键(空格分隔)", 372, 228, 160, 16);
    g_edGroup = mkCtl(g_editorWnd, L"EDIT", L"A D", 372, 246, 160, 22, WS_CHILD | WS_VISIBLE | WS_BORDER, ID_SOCD_KEYS);
    mkCtl(g_editorWnd, L"STATIC", L"模式", 540, 228, 90, 16);
    g_edGroupMode = mkCtl(g_editorWnd, L"COMBOBOX", L"", 540, 246, 110, 220, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_SOCD_MODE);
    for (const wchar_t* s : {L"snap(后按优先)", L"first(先按优先)", L"neutral(抵消)", L"dominant(主键)", L"off(关闭)"})
        SendMessageW(g_edGroupMode, CB_ADDSTRING, 0, (LPARAM)s);
    mkCtl(g_editorWnd, L"STATIC", L"主键(dominant)", 660, 228, 84, 16);
    g_edDominant = mkCtl(g_editorWnd, L"COMBOBOX", L"", 660, 246, 84, 240, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_SOCD_DOM);
    for (int vk : g_keyChoices) SendMessageW(g_edDominant, CB_ADDSTRING, 0, (LPARAM)toW(keyName(vk)).c_str());

    mkCtl(g_editorWnd, L"BUTTON", L"新增组", 372, 366, 80, 26, WS_CHILD | WS_VISIBLE, ID_BTN_SOCD_ADD);
    mkCtl(g_editorWnd, L"BUTTON", L"更新选中", 460, 366, 100, 26, WS_CHILD | WS_VISIBLE, ID_BTN_SOCD_UPDATE);
    mkCtl(g_editorWnd, L"BUTTON", L"删除选中", 566, 366, 100, 26, WS_CHILD | WS_VISIBLE, ID_BTN_SOCD_DELETE);

    // ---- 底部 ----
    mkCtl(g_editorWnd, L"BUTTON", L"写回配置文件", 12, 412, 130, 26, WS_CHILD | WS_VISIBLE, ID_BTN_SAVE);
    mkCtl(g_editorWnd, L"BUTTON", L"关闭窗口", 150, 412, 100, 26, WS_CHILD | WS_VISIBLE, ID_BTN_CLOSE);
    mkCtl(g_editorWnd, L"BUTTON", L"重置为示例规则", 260, 412, 140, 26, WS_CHILD | WS_VISIBLE, ID_BTN_DEFAULT);
    g_chkSocd = mkCtl(g_editorWnd, L"BUTTON", L"启用 SOCD", 410, 412, 110, 26,
                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, ID_CHK_SOC);
    g_chkDks  = mkCtl(g_editorWnd, L"BUTTON", L"启用 DKS",  530, 412, 110, 26,
                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, ID_CHK_DKS);
    SendMessageW(g_chkSocd, BM_SETCHECK, g_cfg.socdEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_chkDks,  BM_SETCHECK, g_cfg.dksEnabled  ? BST_CHECKED : BST_UNCHECKED, 0);
    mkCtl(g_editorWnd, L"STATIC",
          L"改动即时生效并自动写回 config.cfg。编辑此窗口时键盘规则不生效(避免干扰打字)。",
          12, 450, 730, 30);

    refreshRuleList();
    refreshGroupList();
    // 默认值：A 松开时轻点 D
    SendMessageW(g_edKey, CB_SETCURSEL, 0, 0);
    SendMessageW(g_edTap, CB_SETCURSEL, 3, 0);
    SendMessageW(g_edOn, CB_SETCURSEL, ON_RELEASE, 0);
    SendMessageW(g_edCond, CB_SETCURSEL, C_ACTIVE, 0);
    SendMessageW(g_edGroupMode, CB_SETCURSEL, 0, 0);
    if (!g_cfg.rules.empty()) { g_selRule = 0; listPick(g_ruleList, 0); fillRuleFields(g_cfg.rules[0]); }
    if (!g_cfg.groups.empty()) { g_selGroup = 0; listPick(g_socdList, 0); }
    updateStatus();

    ShowWindow(g_editorWnd, SW_SHOW);
    SetForegroundWindow(g_editorWnd);
    logf("[editor] 规则编辑器已打开");
}

// ---------------------------------------------------------------------------
// 自检 / watch
// ---------------------------------------------------------------------------
static void installHook() {
    if (g_virtual) { g_hook = nullptr; return; }   // 虚拟自检不需要真实钩子
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, llHook, GetModuleHandleW(nullptr), 0);
}

static void injectTest(int vk, bool down) { injectKey(vk, down, TAG_TEST); }

static void clearStuckKeys() {
    // 退出时释放所有由我们注入但尚未释放的键，避免"卡键"
    for (auto& kv : g_state)
        if (kv.second.logical && !kv.second.phys) injectKey(kv.first, false);
}

static void setScenario(std::vector<Group> groups, std::vector<Rule> rules) {
    g_cfg.groups = groups;
    g_cfg.rules = rules;
    g_cfg.socdEnabled = true;
    g_cfg.dksEnabled = true;
    g_cfg.toggleKey = 0;
    buildManagedKeys();
}

static void check(const char* label, int vk, bool expectDown) {
    bool got = systemDown(vk);
    printf("  %-38s 系统状态=%s 期望=%s  %s\n", label, got ? "按下" : "未按下",
           expectDown ? "按下" : "未按下", got == expectDown ? "PASS" : "FAIL");
}

static void selfTest(bool live = false) {
    g_cfg.debug = true;
    g_console = true;
    g_virtual = !live;   // 默认虚拟自检：不真按键盘，把事件直接喂进钩子，不会打进任何窗口
    g_virtDown.clear();
    if (live) printf("(live 模式：真按键注入，会敲进当前前台窗口)\n");
    int A = VkKeyScanA('a'), D = VkKeyScanA('d'), W = VkKeyScanA('w'), X = VkKeyScanA('x'), Y = VkKeyScanA('y');

    printf("=== 场景 1: SOCD snap（后输入优先） + 单键急停 ===\n");
    {
        Group g1; g1.keys = {A, D}; g1.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = D; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g1}, {r});
        installHook();
        if (!g_virtual && !g_hook) { printf("钩子安装失败\n"); return; }

        injectTest(A, true); pumpUntil(120);
        check("按住 A 120ms", A, true);
        injectTest(A, true); pumpUntil(30);
        check("A 的自动重复(不应改变状态)", A, true);
        injectTest(D, true); pumpUntil(30);
        check("按 D 后 A 应被释放", A, false);
        check("按 D 后 D 应生效", D, true);
        injectTest(A, true); pumpUntil(30);
        check("A 的自动重复被吞掉(不应复活)", A, false);
        injectTest(A, false); pumpUntil(5);
        check("A 松手：已被 snap 释放，不触发急停", D, true);
        injectTest(D, false); pumpUntil(60);
        check("D 最终释放", D, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 2: 单键急停延迟与时长测量 (tap_ms=20) ===\n");
    {
        Group g1; g1.keys = {A, D}; g1.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = D; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g1}, {r});
        installHook();
        injectTest(A, true); pumpUntil(150);
        injectTest(A, false);
        pumpUntil(4);   check("A 松手后 +4ms", D, true);
        pumpUntil(10);  check("A 松手后 ~14ms", D, true);
        pumpUntil(30);  check("A 松手后 ~44ms", D, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 3: SOCD neutral / first / dominant ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_NEUTRAL;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(50);
        injectTest(D, true); pumpUntil(20);
        check("neutral: A 应取消", A, false);
        check("neutral: D 应取消", D, false);
        injectTest(D, false); injectTest(A, false); pumpUntil(20);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();

        g.mode = SOC_FIRST;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(50);
        injectTest(D, true); pumpUntil(20);
        check("first: A 保持", A, true);
        check("first: D 被忽略", D, false);
        injectTest(D, false); injectTest(A, false); pumpUntil(20);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();

        g.mode = SOC_DOMINANT; g.dominant = D;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(50);
        injectTest(D, true); pumpUntil(20);
        check("dominant=D: A 被释放", A, false);
        check("dominant=D: D 生效", D, true);
        injectTest(D, false); injectTest(A, false); pumpUntil(20);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 4: on=press 二段触发 & on=repeat 连发 ===\n");
    {
        Rule rp; rp.key = W; rp.on = ON_PRESS; rp.tap = X; rp.tapMs = 15; rp.cond = C_TARGET_FREE;
        Rule rr; rr.key = W; rr.on = ON_REPEAT; rr.tap = Y; rr.tapMs = 15; rr.intervalMs = 50; rr.count = 3;
        setScenario({}, {rp, rr});
        installHook();
        injectTest(W, true); pumpUntil(10);
        check("press 触发 X", X, true);
        pumpUntil(40);
        check("X 已释放", X, false);
        pumpUntil(250);   // 连发 3 次大约 150ms 内完成
        printf("  (日志中应看到 3 次 tap-down Y 与 3 次 keyup Y)\n");
        injectTest(W, false); pumpUntil(60);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 5: 用户物理按住目标键时不重复注入 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_OFF;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = D; r.tapMs = 20; r.cond = C_ALWAYS;
        setScenario({g}, {r});
        installHook();
        injectTest(D, true); pumpUntil(50);
        injectTest(A, true); pumpUntil(100);
        injectTest(A, false); pumpUntil(30);
        check("D 仍由用户按住（未被轻点干扰）", D, true);
        injectTest(D, false); pumpUntil(30);
        check("D 释放后系统状态", D, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 6: 全局热键禁用后不再注入 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = D; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        g_cfg.toggleKey = VK_SCROLL;
        buildManagedKeys();
        installHook();
        injectTest(VK_SCROLL, true); pumpUntil(20); injectTest(VK_SCROLL, false); pumpUntil(20);
        injectTest(A, true); pumpUntil(120);
        injectTest(A, false); pumpUntil(40);
        check("禁用后松手 A 不触发 D", D, false);
        injectTest(VK_SCROLL, true); pumpUntil(20); injectTest(VK_SCROLL, false); pumpUntil(20);
        injectTest(A, true); pumpUntil(120);
        injectTest(A, false); pumpUntil(4);
        check("重新启用后松手 A 触发 D", D, true);
        pumpUntil(40);
        check("D 轻点后释放", D, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 7: snap 恢复 —— A 按住,按 D 释放 A,D 松手后 A 恢复 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(100);
        check("先按住 A", A, true);
        injectTest(D, true); pumpUntil(30);
        check("按 D 后 A 被释放", A, false);
        check("按 D 后 D 生效", D, true);
        injectTest(D, false); pumpUntil(30);
        check("D 松手后 A 恢复(物理仍按住)", A, true);
        check("D 已释放", D, false);
        injectTest(A, true); pumpUntil(30);
        check("恢复后 A 的自动重复不改变状态", A, true);
        injectTest(A, false); pumpUntil(20);
        check("最后 A 释放", A, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 8: neutral 恢复 —— 两键抵消,释放其一后另一键生效 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_NEUTRAL;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(50);
        injectTest(D, true); pumpUntil(20);
        check("neutral: A 取消", A, false);
        check("neutral: D 取消", D, false);
        injectTest(D, false); pumpUntil(30);
        check("D 松手后 A 恢复", A, true);
        injectTest(A, false); pumpUntil(20);
        check("A 松手后全部释放", A, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 9: first 恢复 —— 先按的优先,先按的松手后另一键接管 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_FIRST;
        setScenario({g}, {});
        installHook();
        injectTest(A, true); pumpUntil(50);
        injectTest(D, true); pumpUntil(20);
        check("first: A 保持", A, true);
        check("first: D 被忽略", D, false);
        injectTest(A, false); pumpUntil(30);
        check("A 松手后 D(仍物理按住)接管", D, true);
        injectTest(D, false); pumpUntil(20);
        check("D 最终释放", D, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 10: snap 恢复 + 单键急停联动 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = X; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        installHook();
        injectTest(A, true); pumpUntil(100);
        injectTest(D, true); pumpUntil(30);
        check("A 被 snap 释放(不触发急停)", X, false);
        injectTest(D, false); pumpUntil(30);
        check("D 松手后 A 恢复", A, true);
        injectTest(A, false); pumpUntil(5);
        check("A 真正松手 -> 轻点 X", X, true);
        pumpUntil(40);
        check("X 轻点结束", X, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 11: 只开 SOCD（DKS 关闭）—— snap 生效但不注入 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = X; r.tapMs = 20; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        g_cfg.dksEnabled = false;
        installHook();
        injectTest(A, true); pumpUntil(100);
        injectTest(D, true); pumpUntil(30);
        check("只开 SOCD: A 被释放", A, false);
        check("只开 SOCD: D 生效", D, true);
        injectTest(D, false); pumpUntil(30);
        check("D 松手后 A 恢复", A, true);
        injectTest(A, false); pumpUntil(60);
        check("A 松手后不触发轻点 X", X, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 12: 只开 DKS（SOCD 关闭）—— 不做 Snap Tap 但单键急停生效 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = X; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        g_cfg.socdEnabled = false;
        installHook();
        injectTest(A, true); pumpUntil(60);
        injectTest(D, true); pumpUntil(30);
        check("只开 DKS: A 不被释放(SOCD 关闭)", A, true);
        injectTest(A, false); pumpUntil(5);
        check("A 松手 -> 轻点 X", X, true);
        pumpUntil(40);
        check("X 轻点结束", X, false);
        injectTest(D, false); pumpUntil(30);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 13: dominant 主键未按下时，另一键仍生效 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_DOMINANT; g.dominant = A;
        setScenario({g}, {});
        installHook();
        injectTest(D, true); pumpUntil(50);
        check("主键 A 未按，D 仍生效", D, true);
        injectTest(A, true); pumpUntil(20);
        check("A(主键)按下后 D 被释放", D, false);
        check("A(主键)生效", A, true);
        injectTest(A, false); pumpUntil(20);
        check("A 松手后 D 仍物理按着 -> D 恢复", D, true);
        injectTest(D, false); pumpUntil(20);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 14: 总开关保留单独设定（禁用全部 -> 重新启用） ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = X; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        g_cfg.socdEnabled = true;    // 用户只开 SOCD
        g_cfg.dksEnabled  = false;   // DKS 关闭
        g_cfg.enabled     = true;
        buildManagedKeys();
        installHook();

        // 把配置文件指向一个临时文件，避免自检改动用户真实的 config.cfg
        wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
        std::wstring savedPath = g_cfg.path;
        std::wstring tmpPath = std::wstring(tmpDir) + L"BinKey_selftest.cfg";
        {
            FILE* tf = _wfopen(tmpPath.c_str(), L"wb");
            if (tf) {
                fputs("debug = 0\nsocd = 1\ndks = 0\n"
                      "rule = key=A on=release tap=X tap_ms=20 min_hold_ms=30 cond=active\n", tf);
                fclose(tf);
            }
        }
        g_cfg.path = tmpPath;

        auto checkFlag = [&](const char* label, bool got, bool want) {
            printf("  %-36s 实际=%s 期望=%s  %s\n", label, got ? "开" : "关",
                   want ? "开" : "关", got == want ? "PASS" : "FAIL");
        };

        checkFlag("只开 SOCD: socdActive", socdActive(), true);
        checkFlag("只开 SOCD: dksActive", dksActive(), false);
        injectTest(A, true); pumpUntil(60);
        injectTest(D, true); pumpUntil(30);
        check("只开 SOCD: A 被 snap 释放", A, false);
        injectTest(A, false); pumpUntil(60);
        check("DKS 关闭: 松 A 不触发轻点 X", X, false);

        toggleAll();                                   // 禁用全部功能
        checkFlag("禁用全部: 总开关关闭", !g_cfg.enabled, true);
        checkFlag("禁用全部: 单独设定保留 SOCD", g_cfg.socdEnabled, true);
        checkFlag("禁用全部: 单独设定保留 DKS(关)", g_cfg.dksEnabled, false);
        injectTest(D, false); pumpUntil(30);
        injectTest(A, true); pumpUntil(60);
        check("禁用全部: A 不被释放(SOCD 不生效)", A, true);

        toggleAll();                                   // 重新启用 -> 只恢复 SOCD
        checkFlag("重新启用: SOCD 仍开", socdActive(), true);
        checkFlag("重新启用: DKS 仍关(不是两个都开)", dksActive(), false);
        injectTest(D, true); pumpUntil(30);
        check("重新启用: SOCD 仍然生效(A 被释放)", A, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();

        // 持久化：把开关写回配置文件，再读回 —— 模拟"退出程序后重新打开"
        persistSwitches();
        std::string err2;
        bool reloaded = loadConfig(tmpPath, err2);
        bool ok = reloaded && g_cfg.enabled && g_cfg.socdEnabled && !g_cfg.dksEnabled;
        printf("  %-36s 实际=%d/%d/%d 期望=1/1/0  %s\n", "重启后读回 enabled/socd/dks",
               g_cfg.enabled ? 1 : 0, g_cfg.socdEnabled ? 1 : 0, g_cfg.dksEnabled ? 1 : 0,
               ok ? "PASS" : "FAIL");
        g_cfg.path = savedPath;
        DeleteFileW(tmpPath.c_str());
    }

    printf("=== 场景 15: 托盘左键点击配对（一次物理点击 = 一次切换） ===\n");
    {
        struct Ev { int msg; long long gap; };   // gap = 与上一条消息的间隔(ms)
        auto countToggles = [&](const Ev* evs, int n) {
            TrayClick c{}; int toggles = 0; long long t = 0;
            for (int i = 0; i < n; ++i) {
                t += evs[i].gap;
                if (newTrayClick(c, evs[i].msg, t)) ++toggles;
            }
            return toggles;
        };
        struct Case { const char* name; Ev evs[6]; int n; int want; };
        Case cases[] = {
            {"快速点击 DOWN+UP(50ms)",      {{WM_LBUTTONDOWN,0},{WM_LBUTTONUP,50}}, 2, 1},
            {"慢点击 DOWN+UP(700ms)",       {{WM_LBUTTONDOWN,0},{WM_LBUTTONUP,700}}, 2, 1},
            {"按住 3 秒再松手",             {{WM_LBUTTONDOWN,0},{WM_LBUTTONUP,3000}}, 2, 1},
            {"只发 UP 的外壳",              {{WM_LBUTTONUP,0}}, 1, 1},
            {"只发 DOWN 的外壳",            {{WM_LBUTTONDOWN,0}}, 1, 1},
            {"只发 UP 的外壳连点两次",      {{WM_LBUTTONUP,0},{WM_LBUTTONUP,300}}, 2, 2},
            {"连点两次",                    {{WM_LBUTTONDOWN,0},{WM_LBUTTONUP,50},{WM_LBUTTONDOWN,200},{WM_LBUTTONUP,50}}, 4, 2},
            {"双击 DOWN+UP+DBLCLK+UP",      {{WM_LBUTTONDOWN,0},{WM_LBUTTONUP,50},{WM_LBUTTONDBLCLK,150},{WM_LBUTTONUP,50}}, 4, 2},
            {"只发 DOWN 的外壳连点两次",    {{WM_LBUTTONDOWN,0},{WM_LBUTTONDOWN,300}}, 2, 2},
        };
        for (const Case& c : cases) {
            int got = countToggles(c.evs, c.n);
            printf("  %-30s 切换次数=%d 期望=%d  %s\n", c.name, got, c.want, got == c.want ? "PASS" : "FAIL");
        }
        // 右键同理：一次右键只弹一次菜单
        {
            TrayClick c{}; int opens = 0;
            if (newTrayClick(c, WM_RBUTTONDOWN, 0)) ++opens;
            if (newTrayClick(c, WM_RBUTTONUP, 600)) ++opens;
            printf("  %-30s 弹出次数=%d 期望=1  %s\n", "右键慢速点击", opens, opens == 1 ? "PASS" : "FAIL");
        }
    }

    printf("=== 场景 16: 总开关关闭后，钩子的每一条路径都放行 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_SNAP;
        Rule r; r.key = A; r.on = ON_RELEASE; r.tap = X; r.tapMs = 20; r.minHoldMs = 30; r.cond = C_ACTIVE;
        setScenario({g}, {r});
        g_cfg.socdEnabled = true;
        g_cfg.dksEnabled  = true;
        g_cfg.enabled     = true;
        buildManagedKeys();
        installHook();

        auto checkFlag = [&](const char* label, bool got, bool want) {
            printf("  %-36s 实际=%s 期望=%s  %s\n", label, got ? "开" : "关",
                   want ? "开" : "关", got == want ? "PASS" : "FAIL");
        };

        // 先确认功能开启时确实生效
        injectTest(A, true); pumpUntil(120);
        injectTest(D, true); pumpUntil(30);
        check("开启: 按 D 后 A 被 snap 释放", A, false);
        injectTest(D, false); pumpUntil(30);
        injectTest(A, true); pumpUntil(60);
        injectTest(A, false); pumpUntil(10);
        check("开启: 松 A 触发急停轻点 X", X, true);
        pumpUntil(40);
        check("开启: X 轻点后释放", X, false);

        // 总开关关闭（单独设定仍然保留）
        g_cfg.enabled = false;
        checkFlag("关闭: socdActive=false", socdActive(), false);
        checkFlag("关闭: dksActive=false",  dksActive(),  false);
        injectTest(A, true); pumpUntil(60);
        injectTest(D, true); pumpUntil(30);
        check("关闭: A 不被 snap 释放", A, true);
        check("关闭: D 也正常放行", D, true);
        injectTest(D, false); pumpUntil(30);
        injectTest(A, true); pumpUntil(120);
        injectTest(A, false); pumpUntil(10);
        check("关闭: 松 A 不触发轻点 X", X, false);
        pumpUntil(60);
        check("关闭: 之后 X 仍不按下", X, false);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    printf("=== 场景 17: 总开关关闭时，队列里已排好的连发也不再执行 ===\n");
    {
        Group g; g.keys = {A, D}; g.mode = SOC_OFF;
        Rule r; r.key = A; r.on = ON_REPEAT; r.tap = X; r.tapMs = 40; r.minHoldMs = 30;
        r.intervalMs = 60; r.count = 0; r.cond = C_ALWAYS;
        setScenario({g}, {r});
        g_cfg.socdEnabled = true; g_cfg.dksEnabled = true; g_cfg.enabled = true;
        buildManagedKeys();
        installHook();

        injectTest(A, true); pumpUntil(200);            // 连发已经开始（占空比约 40/60）
        bool fired = false;
        for (int i = 0; i < 20 && !fired; ++i) { if (systemDown(X)) fired = true; pumpUntil(10); }
        printf("  %-36s 实际=%s 期望=%s  %s\n", "开启: A 按住期间 X 连发", fired ? "开" : "关", "开",
               fired ? "PASS" : "FAIL");

        g_cfg.enabled = false;                          // 左键关闭插件
        pumpUntil(150);                                 // 已经在飞行中的轻点收尾（keyup 仍放行）
        int leaks = 0;
        for (int i = 0; i < 12; ++i) { if (systemDown(X)) ++leaks; pumpUntil(40); }
        printf("  %-36s 采样命中=%d 期望=0  %s\n", "关闭: 队列中的连发不再执行", leaks,
               leaks == 0 ? "PASS" : "FAIL");

        injectTest(A, false); pumpUntil(60);
        UnhookWindowsHookEx(g_hook); g_hook = nullptr; clearStuckKeys();
    }

    // --- 图标自检 ---
    {
        HICON on  = loadEmbeddedIcon(2);
        HICON off = loadEmbeddedIcon(3);
        printf("[%s] 托盘图标（开启状态, 内嵌 ICO 解析成功)\n", on ? "PASS" : "FAIL");
        printf("[%s] 托盘图标（关闭状态, 内嵌 ICO 解析成功)\n", off ? "PASS" : "FAIL");
        if (on) DestroyIcon(on);
        if (off) DestroyIcon(off);
        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        HICON big[4] = {nullptr}, small[4] = {nullptr};
        int groups = ExtractIconExW(exePath, -1, big, small, 4);
        printf("[%s] 程序图标资源（%d 组，Explorer/快捷方式可用)\n", groups > 0 ? "PASS" : "FAIL", groups);
        for (int i = 0; i < groups; ++i) {
            if (big[i]) DestroyIcon(big[i]);
            if (small[i]) DestroyIcon(small[i]);
        }

        // 真实注册一次托盘图标，验证 Shell_NotifyIcon 是否接受（会短暂出现在任务栏）
        HICON probe = loadEmbeddedIcon(2);
        if (!probe) probe = makeIcon(true);
        if (probe) {
            WNDCLASSEXW pc{};
            pc.cbSize = sizeof(pc);
            pc.lpfnWndProc = DefWindowProcW;
            pc.hInstance = GetModuleHandleW(nullptr);
            pc.lpszClassName = L"BinKeySelftestTray";
            RegisterClassExW(&pc);
            HWND pw = CreateWindowExW(0, pc.lpszClassName, L"BinKeySelftestTray", 0, 0, 0, 1, 1,
                                      nullptr, nullptr, nullptr, nullptr);
            NOTIFYICONDATAW nd{};
            nd.cbSize = sizeof(nd);
            nd.hWnd = pw;
            nd.uID = 9;
            nd.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nd.uCallbackMessage = WM_TRAY;
            nd.hIcon = probe;
            swprintf(nd.szTip, sizeof(nd.szTip) / sizeof(WCHAR), L"BinKey 自检图标");
            BOOL added = Shell_NotifyIconW(NIM_ADD, &nd);
            printf("[%s] 托盘注册 Shell_NotifyIcon(NIM_ADD)\n", added ? "PASS" : "FAIL");
            if (added) Shell_NotifyIconW(NIM_DELETE, &nd);
            if (pw) DestroyWindow(pw);
            UnregisterClassW(pc.lpszClassName, GetModuleHandleW(nullptr));
            DestroyIcon(probe);
        }
    }

    printf("=== 自检结束 ===\n");
}

static BOOL consoleCtrl(DWORD sig) { (void)sig; g_running = false; return TRUE; }

// ---------------------------------------------------------------------------
// 主程序
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    timeBeginPeriod(1);
    InitializeCriticalSection(&g_logcs);

    std::wstring cfgPath;
    bool doWatch = false, doSelfTest = false, doHelp = false, doPrint = false, doEditor = false;
    bool selfTestLive = false;
    int watchSec = 20;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) cfgPath = w16(argv[++i]);
        else if (a == "--editor") { doEditor = true; AttachConsole(ATTACH_PARENT_PROCESS); }
        else if (a == "--watch") {
            doWatch = true;
            AttachConsole(ATTACH_PARENT_PROCESS);
            if (i + 1 < argc) {
                int v = atoi(argv[i + 1]);
                if (v > 0) { watchSec = v; ++i; }
            }
        }
        else if (a == "--selftest") { doSelfTest = true; AttachConsole(ATTACH_PARENT_PROCESS); }
        else if (a == "--selftest-live") { doSelfTest = true; selfTestLive = true; AttachConsole(ATTACH_PARENT_PROCESS); }
        else if (a == "--print") { doPrint = true; AttachConsole(ATTACH_PARENT_PROCESS); }
        else if (a == "--autostart") { setAutostart(true); return 0; }
        else if (a == "--no-autostart") { setAutostart(false); return 0; }
        else if (a == "--help" || a == "-h") { doHelp = true; AttachConsole(ATTACH_PARENT_PROCESS); }
        else if (a == "--version" || a == "-v") {
            AttachConsole(ATTACH_PARENT_PROCESS);
            printf("BinKey v%s\n", kVersion);
            return 0;
        }
        else if (a == "--seconds" && i + 1 < argc) watchSec = atoi(argv[++i]);
    }

    if (doHelp) {
        printf("BinKey v%s - 普通键盘的 DKS(动态键程) / SOCD 工具\n\n", kVersion);
        printf("  BinKey.exe                      后台运行(系统托盘)\n");
        printf("  BinKey.exe --config <path>      指定配置文件\n");
        printf("  BinKey.exe --watch [N]          前台打印按键事件 N 秒(默认 20)\n");
        printf("  BinKey.exe --selftest           模拟按键验证逻辑\n");
        printf("  BinKey.exe --print              打印当前配置规则后退出\n");
        printf("  BinKey.exe --editor             启动时打开规则编辑器窗口\n");
        printf("  BinKey.exe --autostart / --no-autostart   开机自启开关\n");
        return 0;
    }

    // 单实例（只对后台/托盘模式生效，诊断模式允许多开）
    HANDLE mutex = nullptr;
    if (!doWatch && !doSelfTest && !doPrint) {
        mutex = CreateMutexW(nullptr, TRUE, L"BinKeySingletonMutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            printf("BinKey 已经在运行。\n");
            // 第二个实例带 --editor：让正在运行的实例打开规则编辑器
            if (doEditor) {
                HWND t = FindWindowW(L"BinKeyTrayWindow", nullptr);
                if (t) {
                    SendMessageW(t, WM_OPEN_EDITOR, 0, 0);
                    printf("已请求正在运行的实例打开规则编辑器。\n");
                } else {
                    printf("找不到正在运行的 BinKey 托盘窗口。\n");
                }
            }
            return 0;
        }
        (void)mutex;   // 进程生命周期内保持持有，退出时系统自动释放
    }

    wchar_t exePathW[MAX_PATH]; GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring dir = exePathW;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);

    if (cfgPath.empty()) {
        std::wstring cand = dir + L"\\config.cfg";
        std::wstring cand2 = L"config.cfg";
        FILE* t = _wfopen(cand.c_str(), L"rb");
        if (t) { fclose(t); cfgPath = cand; }
        else {
            t = _wfopen(cand2.c_str(), L"rb");
            if (t) { fclose(t); cfgPath = cand2; }
        }
        if (cfgPath.empty()) {
            FILE* w = _wfopen(cand.c_str(), L"wb");
            if (w) {
                fputs("# BinKey 默认配置（自动生成）\n"
                      "debug = 0\n"
                      "enabled = 1\n"
                      "socd = 1\n"
                      "group = A D mode=snap\n"
                      "group = W S mode=snap\n"
                      "dks = 1\n"
                      "rule = key=A on=release tap=D tap_ms=25 min_hold_ms=40 cond=active\n"
                      "rule = key=D on=release tap=A tap_ms=25 min_hold_ms=40 cond=active\n", w);
                fclose(w);
            }
            cfgPath = cand;
        }
    }
    g_cfg.path = cfgPath;

    std::string err;
    if (!loadConfig(cfgPath, err)) { printf("%s\n", err.c_str()); return 1; }
    buildManagedKeys();
    if (!err.empty()) printf("%s", err.c_str());

    g_logPath = dir + L"\\BinKey.log";
    if (g_cfg.debug) {
        g_logFile = _wfopen(g_logPath.c_str(), L"a");
        if (g_logFile && ftell(g_logFile) == 0) fwrite("\xEF\xBB\xBF", 1, 3, g_logFile);  // UTF-8 BOM，记事本直接 readable
    }

    if (doSelfTest) { selfTest(selfTestLive); DeleteCriticalSection(&g_logcs); timeEndPeriod(1); return 0; }
    if (doPrint) { g_console = true; printConfig(); DeleteCriticalSection(&g_logcs); timeEndPeriod(1); return 0; }

    if (doWatch) {
        SetConsoleCtrlHandler(consoleCtrl, TRUE);
        g_cfg.debug = true;
        g_console = true;
        installHook();
        printf("[watch] 监听 %d 秒，Ctrl+C 结束\n", watchSec);
        pumpUntil((long long)watchSec * 1000LL);
        g_running = false;
        clearStuckKeys();
        if (g_hook) UnhookWindowsHookEx(g_hook);
        DeleteCriticalSection(&g_logcs);
        timeEndPeriod(1);
        return 0;
    }

    installTray();
    if (g_cfg.autostart) setAutostart(true);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, llHook, GetModuleHandleW(nullptr), 0);
    if (!g_hook) { printf("键盘钩子安装失败。\n"); return 1; }

    if (doEditor) openEditor();

    // 主循环：处理托盘消息 + 执行到期的"轻点"动作
    pumpUntil(0);

    g_running = false;
    clearStuckKeys();
    if (g_hook) UnhookWindowsHookEx(g_hook);
    removeTray();
    DeleteCriticalSection(&g_logcs);
    if (g_logFile) fclose(g_logFile);
    timeEndPeriod(1);
    return 0;
}
