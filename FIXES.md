# FIXES.md — BinKey 修复与排错说明

这份文档专门记录 **遇到的问题、原因、处理方式，以及怎么验证**。
产品功能与用法在 [README.md](README.md)，版本改动在 [CHANGELOG.md](CHANGELOG.md)。

当前版本：**v1.0.1**

---

## 目录

1. [图标不更新 / 显示成通用图标（资源管理器图标缓存）](#1-图标不更新--显示成通用图标资源管理器图标缓存)
2. [托盘图标：开启 / 关闭状态切换](#2-托盘图标开启--关闭状态切换)
3. [ICO 条目解析：32bpp + AND 掩码](#3-ico-条目解析32bpp--and-掩码)
4. [窗口标题栏 / Alt-Tab 没有图标](#4-窗口标题栏--alt-tab-没有图标)
5. [托盘右键菜单显示成一条细条 / 内容空白](#5-托盘右键菜单显示成一条细条--内容空白)
6. [弹出菜单不消失](#6-弹出菜单不消失)
7. [菜单打开期间 DKS 不生效](#7-菜单打开期间-dks-不生效)
8. [规则编辑器：更新 / 删除总是作用于第一条](#8-规则编辑器更新--删除总是作用于第一条)
9. [规则编辑器：下拉列表选不到靠后的键](#9-规则编辑器下拉列表选不到靠后的键)
10. [config.cfg 第一行被忽略（UTF-8 BOM）](#10-configcfg-第一行被忽略utf-8-bom)
11. [SOCD 覆盖后，另一键松开时不恢复](#11-socd-覆盖后另一键松开时不恢复)
12. [安装引导："打开规则编辑器"点了没反应](#12-安装引导打开规则编辑器点了没反应)
13. [安装引导：切屏后旧控件残留](#13-安装引导切屏后旧控件残留)
14. [桌面快捷方式改名（BinKey.lnk）](#14-桌面快捷方式改名binkkeylnk)
15. [自检会真按键盘（按键泄漏）](#15-自检会真按键盘按键泄漏)
16. [禁用全部功能后重新启用，两个功能都被打开；重启后状态丢失](#16-禁用全部功能后重新启用两个功能都被打开重启后状态丢失)
17. [验证方法一览](#17-验证方法一览)

---

## 1. 图标不更新 / 显示成通用图标（资源管理器图标缓存）

**现象**：`BinKeySetup.exe` 在资源管理器里显示的不是 `1.ico`，改名 / 重启资源管理器也没用。

**先确认图标确实内嵌** —— 用资源管理器自己的图标提取接口
（`IShellFolder.GetUIObjectOf → IExtractIcon.GetIconLocation/Extract`，也就是 Explorer 渲染图标时真正走的那条路径）实测：

| 对象 | 32×32 像素哈希 | 16×16 像素哈希 |
|---|---|---|
| `D:\project\1.ico`（源图标） | `3c418df36cfd93e5` | `f4618eeba4f936ec` |
| `BinKey.exe` | `3c418df36cfd93e5` ✅ | `f4618eeba4f936ec` ✅ |
| `BinKeySetup.exe` | `3c418df36cfd93e5` ✅ | `f4618eeba4f936ec` ✅ |
| 桌面 `BinKey.lnk` | `3c418df36cfd93e5` ✅ | 同上 ✅ |
| `2.ico`（托盘关闭） | `d8d24a102d3a2d8d` | 不同 |
| `3.ico`（托盘开启） | `72a9852a2031e10b` | 不同 |

PE 资源里的 `RT_GROUP_ICON` 是 9 条目（16 / 24 / 32 / 48 / 64 / 72 / 96 / 128 / 256），
大小 1128 / 2440 / 4264 / 9640 / 16936 / 21640 / 38056 / 67624 / 270376 字节 —— 与 `1.ico` 逐字节一致。

**真正的原因**：Windows 把每个路径的图标缓存写在

```
%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_16.db
%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_32.db
%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_48.db
%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_256.db
```

条目按**路径哈希**索引，缓存里存的是上一次那个 exe 的图标位图。
**重启资源管理器不会清除它**，`ie4uinit.exe -ClearIconCache` 也不起作用 ——
所以只要缓存条目还在，Explorer 就会继续渲染旧图标。

**处理**：停止 explorer → 删除缓存 DB → 重启 explorer。项目里已经提供
`fix-icon-cache.bat`，双击即可：

```bat
taskkill /f /im explorer.exe
del /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db"
explorer.exe
```

**验证**：清理后 Explorer 重新生成缓存，在新生成的 `iconcache_16.db` / `iconcache_32.db`
里能找到 `1.ico` 的 16×16 与 32×32 像素数据 —— 说明这次缓存存的就是正确图标。

**省事的做法**：把 exe 改名或复制到新路径。缓存键按路径哈希，新路径必然重新提取。

> 注意：图标缓存是按路径的，同一目录里换图标后老路径的条目不会自动失效；
> 每次换图标后跑一次 `fix-icon-cache.bat` 最稳。

---

## 2. 托盘图标：开启 / 关闭状态切换

**要求**：关闭状态用 `2.ico`，开启状态用 `3.ico`。

**处理**：三个 .ico 原封不动嵌入 —— `1 ICON` 生成标准 `RT_GROUP_ICON`（Explorer / 快捷方式用），
`2 ICOFILE` / `3 ICOFILE` 以自定义资源类型 `ICOFILE` 原样嵌入原始 .ico 字节（托盘用，运行时解析）。
资源 id：**1 = 程序图标（1.ico），2 = 开启状态（3.ico），3 = 关闭状态（2.ico）**。

> 不能用三个 `ICON` 声明：windres 会把多个 `ICON` 合并成同一个 `RT_GROUP_ICON`，
> 后两个 .ico 会丢失。自定义类型 `ICOFILE` 才保持原字节。
> （写 `"ICOFILE"` 带引号是语法错误，必须不带引号。）

运行时 `loadEmbeddedIcon(resId)` 取 ICO 里最接近 32×32 的条目，用
`CreateDIBitmap` + `CreateBitmap(mask)` + `CreateIconIndirect` 生成 HICON。
（实测 `CreateIconFromResourceEx` 在这批 .ico 上失败，gle = 0，所以不用它。）
失败时回退到内置自绘图标。

**验证**：`BinKey.exe --selftest` 中的两条
`[PASS] 托盘图标（开启状态, 内嵌 ICO 解析成功)` / `[PASS] 托盘图标（关闭状态, ...)`；
渲染出的 32×32 平均 RGB 与源 .ico 一致（开启 157/148/167，关闭 177/170/172）。

---

## 3. ICO 条目解析：32bpp + AND 掩码

**症状**：用你给的 .ico（每个都有 9 个尺寸，全部是 **32bpp + AND 掩码**）时，
托盘图标空白 / 错位。

**原因**：带掩码的 BMP 里 `biHeight = 2 × 真实高度`（颜色块 + 掩码块上下排列）。
旧代码只对 24bpp 条目减半高度，32bpp 条目被当成 64 行 → 读出的像素不对。

**处理**：按条目总长度判断有没有掩码（`len == 40 + w*h*4 + w*ceil(h/8)*4`），
32bpp 与 24bpp 都正确；`biHeight` 可能为负，取绝对值；BMP 行步长按
`((w*3+3)/4)*4` 对齐；掩码用 `CreateBitmap(w, h, 1, 1, mask)`。

**验证**：渲染出的 32×32 像素与源 .ico 同尺寸条目完全一致（逐像素比对）。

---

## 4. 窗口标题栏 / Alt-Tab 没有图标

**原因**：安装引导窗口和规则编辑器窗口的窗口类没有注册 `hIcon`，
所以标题栏左上角和 Alt-Tab 里没有图标。

**处理**：两个窗口类都注册 `hIcon`，创建窗口后显式
`SendMessage(hwnd, WM_SETICON, ICON_BIG/ICON_SMALL, ...)`；
失败时自动用自绘图标重试一次。

**验证**：`FindWindowW("BinKeySetupWizard")` / `FindWindowW("BinKeyEditor")`
后读 `WM_GETICON` 返回非空 HICON。

---

## 5. 托盘右键菜单显示成一条细条 / 内容空白

**原因**：菜单用 `CreateMenu()` 创建。`CreateMenu()` 创建的是**菜单栏**，
用 `TrackPopupMenu` 弹出时 Windows 只给出约 6 像素宽的条，所以"弹出来了但看不见内容"。

**处理**：改用 `CreatePopupMenu()`；同时：

* 右键事件兼容 `WM_RBUTTONDOWN` / `WM_RBUTTONUP`（不同 Windows 版本发送的事件不同），
  并加 300 ms 防重复，避免菜单弹出两次或不弹出；
* `WM_COMMAND` 用 `LOWORD(wParam)` 取菜单 id（弹出菜单的 wParam 高 16 位是菜单句柄）；
* 菜单位置 clamp 到屏幕内，靠右侧 / 右下角时不会被截断。

**验证**（无需鼠标）：启动后用 `FindWindowW(L"BinKeyTrayWindow", NULL)` 找到托盘窗口，
`PostMessage(hwnd, WM_APP+1, 0, WM_RBUTTONDOWN)` 弹出菜单；实测菜单窗口（类名 `#32768`）
尺寸 **206×156**，内容正常，且菜单打开期间键盘钩子仍然工作。

---

## 6. 弹出菜单不消失

**处理**：`TrackPopupMenu` 之后调用 `PostMessage(hwnd, WM_NCHITTEST, 0, 0)`
（让弹出菜单正确关闭的经典补丁），并且先 `SetForegroundWindow(托盘窗口)`。

---

## 7. 菜单打开期间 DKS 不生效

**原因**：`TrackPopupMenu` 阻塞主消息循环，排队的注入动作不会执行。

**处理**：托盘窗口挂一个 20 ms 的 `SetTimer`，`WM_TIMER` 里调用 `processDueActions()`。

**验证**：菜单打开时按 A 再松手，日志出现 `tap-down` / `keyup` —— 急停照常生效。

---

## 8. 规则编辑器：更新 / 删除总是作用于第一条

**症状**：点列表里的第 3 条，"更新选中" / "删除选中"却改 / 删第 1 条。

**三个原因，都修了**：

1. **LISTBOX 没有 `LBS_NOTIFY`** —— Windows 的 LISTBOX 只有创建时带 `LBS_NOTIFY`
   才会向父窗口发 `LBN_SELCHANGE`。原来样式是 `WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL`，
   所以窗口永远收不到"用户点了哪一行"，`g_selRule` / `g_selGroup` 停留在 0。
   现在两个列表框都带 `LBS_EXTENDEDSEL | LBS_NOTIFY`，每次选择后把选择集收敛成一条
   （`LB_SETSEL` 在单选列表框上是空操作，实测已验证）。
2. **COMBOBOX 当前值取不到** —— `CBS_DROPDOWNLIST` 没有编辑子控件，
   `GetWindowTextW()` 返回空串，必须用 `CB_GETCURSEL` + `CB_GETLBTEXT`。
   之前 `readRuleFields()` 用 `GetWindowTextW` 读触发键 / 轻点键 → 空串 →
   `parseKey("") == 0` → 直接 return，更新 / 新增静默失败。现在统一走 `comboText()`。
3. **触发方式 / 条件下拉框选项顺序与枚举不一致** —— 枚举是
   `ON_PRESS=0, ON_RELEASE=1, ON_HOLD=2, ON_REPEAT=3`，但下拉框第一项写的是"松开时"，
   于是选"按下时"实际存的是 `ON_RELEASE`。现在顺序与枚举一一对应：
   `按下时 / 松开时 / 按住超过 / 连发`，条件为 `always / active / target_up / target_free`。

**另外**：列表下方新增两行状态栏（"当前选中：第 N 条规则 / 第 N 个 SOCD 组"），
删除后自动选中相邻一条并载入其内容；没有选中时点更新 / 删除会写
`[editor] 没有选中的规则`。

**验证**（真实鼠标点击第 3 行，状态栏显示"当前选中：第 3 条规则"，改 tap_ms 后点"更新选中"）：

```
[editor] 更新规则 2: W 松开时 -> 轻点 X  tap=40ms
[editor] 删除规则 3: S 松开时 -> 轻点 W  tap=20ms
```

配置文件里对应行确实被改 / 删。

---

## 9. 规则编辑器：下拉列表选不到靠后的键

**症状**：键下拉框有 82 个选项，没有 `WS_VSCROLL`，列表被截断且没有滚动条，
选中靠后的键后无法往上滚（选了 P 滚不到 O）。

**处理**：所有 COMBOBOX / LISTBOX 都带 `WS_VSCROLL`，滚轮 / 上下键 / 滚动条都能用；
下拉高度加大到 240 px，窗口改为 **790×560**，下拉框不再被窗口边界裁切。

**验证**：打开触发键下拉框（选中 P，列表起始索引 15）→ 滚轮向上 → 索引回到 0（A），
可以选到 O。

---

## 10. config.cfg 第一行被忽略（UTF-8 BOM）

**原因**：用记事本 / PowerShell 保存 `config.cfg` 时可能加 UTF-8 BOM（`EF BB BF`），
BOM 会让**第一行**的键名变成无效，于是第一行配置（例如 `socd = 0`）被忽略。

**处理**：读配置文件时自动跳过 BOM。

**验证**：`socd = 0 / dks = 1` 四种组合的行为都符合预期。

---

## 11. SOCD 覆盖后，另一键松开时不恢复

**旧行为**：A 被 snap 释放后，即使 D 松开，A 也不再生效，直到你重新按 A。

**新行为**：

```
A 按住 → D 按下  → A 逻辑释放（A 仍物理按着）
           D 松开  → A 立即恢复为"按下"
```

反向同理；`neutral` 模式也如此（两键抵消后，只要其中一个物理松开，剩下的那个立即生效）。
`dominant` 模式下若主键没有按下，则退化为"后按优先"，不会把剩下的键全部取消。

**实现**：每次按键事件后调用 `syncGroup()` 重新计算组内"胜者"，
对需要恢复的键注入带标记的 key-down，对需要释放的键注入带标记的 key-up。

**验证**：`simkey.exe a down` + `simkey.exe d down` → `keyprobe.exe a d` 显示
`a=未按下, d=按下`；`simkey.exe d up`（A 仍按住）→ `a=按下, d=未按下`。

---

## 12. 安装引导："打开规则编辑器"点了没反应

**原因**：`BinKey.exe` 是单实例（命名互斥锁 `BinKeySingletonMutex`），
第二个实例撞锁后直接退出，所以按钮看起来没反应。

**处理**：第二个实例撞锁时，`FindWindowW(L"BinKeyTrayWindow")` +
`SendMessageW(t, WM_OPEN_EDITOR)`（`WM_APP + 2`），让正在运行的实例弹出编辑器。

**同时**：完成页只保留 **退出** 与 **打开规则编辑器** 两个按钮
（"启动 BinKey" / "打开安装目录" 已删 —— 安装完成时 BinKey 通常已经在运行，这两个按钮多余）；
点"打开规则编辑器"后引导窗口自动关闭（`SendMessage` 是同步的，编辑器已经创建好才关窗口）。

**验证**（`build\guiinstall_test.exe` 自动驱动向导）：

```
[test] wizard: 720x400
[test] options page buttons: 开机自动启动(106) 创建桌面快捷方式(107) 安装完成后立即启动(108) 下一步(102) 安装(103) 退出(113)
[test] finish page buttons: 退出(113) 打开规则编辑器(111)
[test] keyboard events during install: 0
[test] editor window: OK
[test] wizard still open: NO (expect closed)
[test] editor size 790x560
[test] PASS (no keystrokes leaked)
```

---

## 13. 安装引导：切屏后旧控件残留

**原因**：切屏时只新建控件，没有擦掉上一屏的客户区。

**处理**：切屏前先 `FillRect` 整块背景。

**验证**：真实屏幕 BitBlt 抓客户区逐像素比对，切屏后旧控件区域未被新控件覆盖的像素
全部等于背景色（**残留 = 0**），文字不再叠在上一屏上。

---

## 14. 桌面快捷方式改名（BinKey.lnk）

**处理**：快捷方式从 `BinKey 动态键程.lnk` 改名为 **`BinKey.lnk`**；
安装时如果桌面上还存在旧名字的 `BinKey 动态键程.lnk`，会自动删除，避免桌面出现两个图标；
`uninstall.bat` 也会删除 `BinKey.lnk`。

**验证**：`BinKey.lnk` 的 target / working directory 都指向安装目录里的 `BinKey.exe`，
图标用 exe 自带图标；用 `IExtractIcon` 读 .lnk 的图标，像素哈希与 `1.ico` 一致。

---

## 15. 自检会真按键盘（按键泄漏）

**旧行为**：安装引导里跑 `--selftest` 时用 `SendInput` 真按 A/D/W/X/Y，
虽然没有命令窗口，但模拟按键会打到前台窗口（看起来像"不断输入 ad"）。

**处理**：改成**虚拟自检** —— 构造 `KBDLLHOOKSTRUCT` 直接喂进 `llHook()`，
用内部状态判断"系统看到的是按下 / 未按下"，完全不真按键。
真实注入链路用 `--selftest-live` 单独验证（那个版本会真按键，偶尔有一两断言不稳定，
日常用默认 `--selftest` 即可）。

**验证**：另开一个进程装全局 `WH_KEYBOARD_LL` 钩子统计真实键盘事件 ——
整个安装过程 45 秒 = **0 次**键盘事件；单独跑 `--selftest` 期间也是 0 次；
而 `--selftest-live` 期间是 131 次。

---

## 16. 禁用全部功能后重新启用，两个功能都被打开；重启后状态丢失

**版本**：v1.0.1

**现象**

* 用户只开启 SOCD（或只开 DKS），用托盘左键"禁用全部功能"，再左键"启用全部功能"
  —— 结果 **SOCD 和 DKS 都被打开**，用户单独关掉的那个功能重新生效了。
* 退出程序再打开，同样两个功能都变成开启，上次设定的状态丢失。

**原因**

原来的实现把"全部功能"和两个单独开关混在同一组变量里：

```cpp
bool on = g_cfg.socdEnabled || g_cfg.dksEnabled;
g_cfg.socdEnabled = g_cfg.dksEnabled = !on;     // 单独设定被覆写
```

"启用全部"字面上把两个开关都置为 `true`，用户"只开 SOCD"的设定被抹掉。
而且托盘菜单里的切换**只改内存里的 `g_cfg`，不写回 `config.cfg`**，
所以重启时读到的仍是文件里的默认值 `socd = 1 / dks = 1`。

**处理**

把"总开关"与"单独开关"分成三层状态：

```cpp
struct Config { bool enabled = true;   // 总开关（"禁用全部功能"）
                bool socdEnabled = true; // 用户单独设定，持久化
                bool dksEnabled  = true; };

static bool socdActive() { return g_cfg.enabled && g_cfg.socdEnabled; }
static bool dksActive()  { return g_cfg.enabled && g_cfg.dksEnabled; }
```

钩子里所有判断（SOCD 组生效、DKS 规则触发、托盘图标颜色、菜单文字）都用
`socdActive()` / `dksActive()`，而不是裸的 `socdEnabled`。

托盘左键、菜单"禁用/启用全部功能"、`toggle_key` 热键只切 `enabled`：

```cpp
static void toggleAll() {
    if (totalEnabled()) g_cfg.enabled = false;      // 单独设定保持不变
    else {
        g_cfg.enabled = true;
        if (!g_cfg.socdEnabled && !g_cfg.dksEnabled) {   // 两个都被用户手动关过
            g_cfg.socdEnabled = true; g_cfg.dksEnabled = true;
        }
    }
    setTrayIcon(); persistSwitches();
}
```

单独开关（菜单项 2 / 3、规则编辑器里的两个复选框）改变 `socdEnabled` / `dksEnabled`，
开启其中一个时会自动把 `enabled` 打开。

`persistSwitches()` 只改写 `config.cfg` 里的 `enabled` / `socd` / `dks` 三行，
其它注释、规则、组定义原样保留（缺少的键追加在文件末尾）；每次切换都调用它，
所以退出后重新打开能恢复上次状态。

**验证**

`build\traytest.exe` 启动真实程序，用 `PostMessage` 模拟托盘左键和菜单项，
再读回配置文件：

```
初始                       enabled=-1 socd=1 dks=1
菜单关闭 DKS 后         enabled=1  socd=1 dks=0
左键禁用全部后          enabled=0  socd=1 dks=0     ← 单独设定保留
左键重新启用后          enabled=1  socd=1 dks=0     ← 只恢复 SOCD，DKS 仍关
两个都单独关闭后        enabled=1  socd=0 dks=0
再左键启用后            enabled=1  socd=1 dks=1     ← 两个都关过 → 全部开启
重启后读回              enabled=1  socd=1 dks=1     ← 与退出前一致
```

`--selftest` 的场景 14 也断言同样的语义（12 项）。

---

## 17. 验证方法一览

| 要验证什么 | 用什么 |
|---|---|
| 逻辑对不对 | `BinKey.exe --selftest`（69 项断言，全部 PASS） |
| 真实注入链路 | `BinKey.exe --selftest-live`，或 `simkey.exe` + `keyprobe.exe` |
| 开关状态是否保留 / 持久化 | `build\traytest.exe`（启动真程序，模拟托盘左键与菜单项，读回 config.cfg） |
| 安装向导流程 / 按钮 | `build\guiinstall_test.exe`（自动点击走完 5 屏，统计键盘事件） |
| exe 图标是不是 `1.ico` | 资源管理器接口 `IExtractIcon.GetIconLocation/Extract`，对比像素哈希；或读 PE 的 `RT_GROUP_ICON` |
| 图标缓存是否已刷新 | 查 `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db` 里有没有 `1.ico` 的像素数据 |
| 安装过程是否泄漏按键 | 全局 `WH_KEYBOARD_LL` 钩子计数 |
| 切屏残留 | BitBlt 抓客户区逐像素比对 |

---

## 附：图标缓存清理脚本 `fix-icon-cache.bat`

```bat
taskkill /f /im explorer.exe
timeout /t 3 /nobreak >nul
del /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db"
start "" explorer.exe
```

它随 `BinKeySetup.exe` 一起释放到安装目录，双击即可运行，不需要管理员权限。
