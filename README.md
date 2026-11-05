# BinKey — 普通机械键盘也能用 DKS(动态键程) / SOCD(Snap Tap)

一个单文件 C++ / Win32 的 Windows 程序，用**低级键盘钩子**实现磁轴键盘（Wooting / Razer / 磁轴厂）的
**DKS（Dynamic Keystroke，动态键程）** 与 **SOCD（Snap Tap）** 功能，让普通机械键盘也能实现
"单键急停"、"按放两段触发"、"连发" 等玩法。

与 [SnapKey](https://github.com/cafali/SnapKey) 的关系：SnapKey 只做 SOCD（同向键互相取消），
**DKS 是它没有的功能**。BinKey 同时提供两者，并且 DKS 的核心 ——
"**按下触发一次、松手再触发一次**" —— 用软件时间语义模拟了磁轴的多触发点。

当前版本：**v1.0.2**

* 用法、配置、原理 → 本文档
* 遇到的问题、图标不更新、菜单显示异常等 → **[FIXES.md](FIXES.md)**（修复与排错说明）
* 每个版本改了什么 → **[CHANGELOG.md](CHANGELOG.md)**

> **不想编译？** 直接双击项目根目录的 **`BinKeySetup.exe`**（单文件安装引导，约 3.8 MB），
> 向导式安装，详见 [第 3 节](#3-安装引导程序-binkkeysetupexe)。

---

## 目录

1. [DKS 到底是什么](#1-dks-到底是什么)
2. [快速开始](#2-快速开始)
3. [安装引导程序 `BinKeySetup.exe`](#3-安装引导程序-binkkeysetupexe)
4. [编译](#4-编译)
5. [运行与命令行](#5-运行与命令行)
6. [托盘图标与菜单](#6-托盘图标与菜单)
7. [图标](#7-图标)
8. [规则编辑器窗口](#8-规则编辑器窗口)
9. [配置语法](#9-配置语法)
10. [预设方案（profiles/）](#10-预设方案profiles)
11. [快速验证（不需要游戏）](#11-快速验证不需要游戏)
12. [调参建议](#12-调参建议)
13. [工作原理](#13-工作原理)
14. [已知限制](#14-已知限制)
15. [常见问题](#15-常见问题)
16. [项目结构](#16-项目结构)
17. [卸载](#17-卸载)

---

## 1. DKS 到底是什么

磁轴键盘能感知按键的**实时深度**，所以一个键可以设多个触发点：

| 硬件语义 | 普通键盘的软件模拟 |
|---|---|
| 按下到 0.5 mm 触发 A | `on=press`（按下瞬间触发） |
| 按下到 1.5 mm 触发 B | `on=hold`（按住超过 N ms 触发） |
| 松手回到 0.5 mm 触发 C | `on=release`（松手瞬间触发） ← 单键急停的关键 |
| 反复快速触发 | `on=repeat`（按住期间每 N ms 轻点一次） |

最常用的场景（无畏契约 / CS 类）：

> 按住 **A** 向左移动 → 松手 **A** 的瞬间，程序自动轻点 **D** 25 ms → 角色被"拽停"。
> 不需要另一只手去点 D，也不依赖 SOCD。

---

## 2. 快速开始

1. 双击 `BinKeySetup.exe` → 选安装目录（默认 `%LOCALAPPDATA%\BinKey`）→ 安装。
2. 安装完 BinKey 已经在运行，系统托盘会有图标。
3. 右键托盘图标 → **规则编辑器**，可视化改规则；改完即时生效并写回 `config.cfg`。
4. 想换预设方案：把 `profiles\` 里的某个文件复制成 `config.cfg`，
   或 `BinKey.exe --config profiles\valorant-stop.cfg`。

不需要管理员权限。

---

## 3. 安装引导程序 `BinKeySetup.exe`

项目根目录的 `BinKeySetup.exe`（约 3.8 MB）是一个**自解压式的单文件安装向导**，
适合直接发给别人，不需要装任何运行环境。

### 怎么生成

```bat
build-installer.bat
```

它会编译 `BinKey.exe` / `simkey.exe` / `keyprobe.exe` / 安装向导 / 打包器，
然后把 10 个文件（程序 + config.cfg + README + CHANGELOG + FIXES + fix-icon-cache.bat
+ 4 个预设方案）追加到向导 exe 尾部，输出 `BinKeySetup.exe`。

原理：PE 文件尾部追加 `[负载数据][清单][u32 清单长度]["DSKSETUP"]`，
向导运行时用 `GetModuleFileNameW` 读自己，解析清单并释放文件。

### 图形向导（双击运行）

| 步骤 | 内容 |
|---|---|
| 1 欢迎 | 说明 DKS / SOCD 是什么，环境要求 |
| 2 安装位置 | 点 **选择安装文件夹...** 用系统文件夹选择器挑一个目录（没有预设列表，也不用手打路径）；不选就装到 `%LOCALAPPDATA%\BinKey` |
| 3 安装选项 | 只有三个：开机自动启动 / 创建桌面快捷方式 / 安装完成后立即启动 BinKey |
| 4 安装结果 | 释放的文件清单 + 自动跑 `--selftest` 的结果 |
| 5 完成 | **打开规则编辑器**（右下角） + 退出；点"打开规则编辑器"后引导窗口自动关闭 |

第 2、3、4 屏右上角都会显示 **安装到: <实际目录>**，装错与否一眼就能确认。

`BinKey.exe` 是单实例（命名互斥锁）。向导点"打开规则编辑器"时：

* 若已有实例在运行 → 第二个实例 `FindWindowW("BinKeyTrayWindow")` +
  `SendMessage(hwnd, WM_APP+2)`，让正在运行的实例弹出编辑器，改动即时生效；
* 若没有实例在运行 → 直接启动一个带编辑器的实例。

Windows 11 的 SmartScreen 可能提示"未知发布者" —— 点"仍要运行"即可；
如果下载后被标记，右键文件 → 属性 → 勾选"解除阻止"。

### 命令行静默安装（适合脚本 / 批量部署）

```bat
BinKeySetup.exe --silent
BinKeySetup.exe --silent --dir D:\BinKey --no-autostart --no-shortcut --launch
BinKeySetup.exe --help
```

| 参数 | 作用 |
|---|---|
| `--silent` | 不弹窗口，直接安装并打印结果 |
| `--dir <路径>` | 安装目录（默认 `%LOCALAPPDATA%\BinKey`） |
| `--no-autostart` | 不写开机自启 |
| `--no-shortcut` | 不创建桌面快捷方式 |
| `--launch` | 装完立刻启动 BinKey |

安装自检在后台直接跑 `BinKey.exe --selftest`，输出重定向到临时文件，**不弹任何命令窗口**，
也不会真的按键（虚拟自检）。想验证真实注入链路，手动跑 `BinKey.exe --selftest-live`。

桌面快捷方式名为 `BinKey.lnk`，指向安装目录里的 `BinKey.exe`，工作目录也设成安装目录；
卸载时会自动删除它。

调试工具（`simkey.exe` / `keyprobe.exe`）和源码不打包进安装文件 —— 源码在 `src\` 里，
需要时自己编译：`g++ -O2 -std=c++17 -static src\simkey.cpp -o build\simkey.exe -luser32`。

---

## 4. 编译

需要 MinGW（msys2）或任何 C++17 编译器：

```bat
build.bat
```

或手动：

```bat
g++ -O2 -std=c++17 -Wall -Wextra -static -mwindows src\BinKey.cpp build\binkey_res.o -o build\BinKey.exe -luser32 -ladvapi32 -lshell32 -lwinmm -lgdi32
```

产物 `build\BinKey.exe` 约 2.3 MB，静态链接，不需要运行时。

---

## 5. 运行与命令行

| 命令 | 说明 |
|---|---|
| `BinKey.exe` | 后台运行，系统托盘图标（左键切换开关，右键菜单） |
| `BinKey.exe --config <路径>` | 指定配置文件（默认找 exe 同目录的 `config.cfg`，再找当前目录） |
| `BinKey.exe --print` | 打印当前解析后的规则，然后退出 |
| `BinKey.exe --editor` | 启动时直接打开**规则编辑器窗口** |
| `BinKey.exe --watch [秒]` | 前台打印每个托管按键的事件与注入动作（默认 20 秒），排错用 |
| `BinKey.exe --selftest` | 内置场景自检（**虚拟自检**：把事件直接喂进钩子，不真按键盘，不会敲进任何窗口） |
| `BinKey.exe --selftest-live` | 同样场景，但用真实 `SendInput` 注入（会真按键，用来验证注入链路） |
| `BinKey.exe --autostart` / `--no-autostart` | 开机自启开关（写注册表 Run 键） |
| `BinKey.exe --version` / `-v` | 打印版本号（当前 **v1.0.2**） |

配置文件不存在时会自动生成一份默认配置。`debug = 1` 时写入 `BinKey.log`。

---

## 6. 托盘图标与菜单

* **左键**：切换**总开关**（启用 / 禁用全部功能，图标颜色随之变化，并写入日志）。
  一次物理点击只切换一次 —— 慢点击（按住几百毫秒再松手）、连点、双击都按真实
  点击次数计数，不会因为防抖而"点了没反应"。
* **右键**：弹出菜单
  1. 禁用全部功能 / 启用全部功能 —— **总开关**
  2. **SOCD(Snap Tap)：开启 / 关闭** —— 单独开关
  3. **DKS(动态键程)：开启 / 关闭** —— 单独开关
  4. **规则编辑器**（图形窗口，自定义 DKS 与 SOCD）
  5. 重新加载配置
  6. 打开配置文件（用系统默认编辑器）
  7. 设为开机自启 / 取消开机自启
  8. 打印当前规则到日志（方便排错）
  9. 退出

### 总开关 vs. 单独开关（重要）

* `socd` / `dks` 是你对两个功能**各自的设定**，会被写回 `config.cfg`。
* 托盘左键、菜单里的"禁用全部功能"、`toggle_key` 热键，只切换**总开关**(`enabled`)，
  **不会改动**两个功能的单独设定。
* 所以：如果你只开 SOCD、关掉 DKS，那么
  * 左键禁用全部 → 再左键启用，**仍然只开 SOCD**，不会把 DKS 也打开；
  * 退出程序再打开，也**只开 SOCD**（设定已写回 `config.cfg`）。
* 只有当你把两个单独开关都手动关闭过，再点"启用全部功能"，才会两个都打开。

> SOCD 与 DKS 是两个**独立开关**：可以只开 SOCD（只做 Snap Tap，不做急停），
> 也可以只开 DKS（只做单键急停，不做 SOCD 清理），或两个都开。
> 总开关关闭，或两个单独开关都关闭时，程序完全放行键盘，等于不存在。

`toggle_key` 热键（默认无，可在配置里设成 `F1` 等）切换总开关。

---

## 7. 图标

图标是内嵌的真实 .ico 文件（`icons\` 目录，由 `src\resources.rc` / `src\icon.rc` 嵌入）：

| 用途 | 文件 | 来源 | 内嵌尺寸 |
|---|---|---|---|
| 程序图标（Explorer / 快捷方式 / 任务栏 / 安装引导窗口标题栏） | `icons\BinKey.ico` | `1.ico` | 16 / 24 / 32 / 48 / 64 / 72 / 96 / 128 / 256 |
| 托盘图标 — **开启**状态（SOCD 或 DKS 任一开启） | `icons\tray_on.ico` | `3.ico` | 同上 |
| 托盘图标 — **关闭**状态（全部功能关闭） | `icons\tray_off.ico` | `2.ico` | 同上 |

换图标只需要两步：

```bat
:: 1) 把新的 .ico 覆盖 icons\ 里的同名文件（多尺寸 32bpp 最好）
copy /Y D:\project\1.ico      icons\BinKey.ico
copy /Y D:\project\3.ico      icons\tray_on.ico
copy /Y D:\project\2.ico      icons\tray_off.ico
:: 2) 重新运行 build-installer.bat
```

如果只有 .jpg，可以用 `src\mkico.py` 转成多尺寸 .ico：

```bat
python src\mkico.py 1.jpg icons\BinKey.ico   16,32,48,64,128,256
python src\mkico.py 3.jpg icons\tray_on.ico  16,32,48,64
python src\mkico.py 2.jpg icons\tray_off.ico 16,32,48,64
```

没有 `icons\` 目录时程序回退到内置自绘图标，程序照样有图标。
安装引导窗口和规则编辑器窗口都注册了 `hIcon`，所以**标题栏左上角、Alt-Tab**都会显示程序图标。

> 资源管理器里图标不更新（显示旧图标 / 通用图标）是 Windows 图标缓存的问题，
> 处理方法见 **[FIXES.md](FIXES.md)**。

---

## 8. 规则编辑器窗口（菜单 → 规则编辑器，或 `BinKey.exe --editor`）

一个纯 Win32 窗口，不需要联网、不需要额外运行时：

| 区块 | 作用 |
|---|---|
| 左上 **DKS 规则列表** | 列出所有 `key → tap` 规则；选中后下方字段会填充 |
| 触发键 / 轻点键 | 下拉选择任意字母、数字、方向键、F1-F12 等 |
| 触发方式 | `按下时`(press) / `松开时`(release) / `按住超过`(hold) / `连发`(repeat) |
| tap_ms / 延迟 / min_hold / 连发间隔 / 次数 | 数值参数 |
| 条件 | `always` / `active` / `target_up` / `target_free` |
| 新增 / 更新选中 / 删除选中 | 立即生效（改完立刻生效，无需重启） |
| 右上 **SOCD 组列表** | 组内按键（空格分隔）+ 模式 + 主键(dominant) |
| 启用 SOCD / 启用 DKS | 两个独立开关，等同于托盘菜单里的 2、3 项 |
| 写回配置文件 / 重置为示例规则 | 保存到 `config.cfg` |

* 任何改动都**即时生效**并自动写回 `config.cfg`。
* 编辑器窗口处于前台时，键盘钩子**放行所有按键**（否则你在编辑框里打字会被规则干扰）。
  所以编辑器打开时不要同时玩游戏；关掉窗口后规则照常生效。
* 窗口下方有状态栏，显示当前选中第几条规则 / 第几个 SOCD 组。

`build\simkey.exe` 是一个模拟按键的小工具，用来验证注入效果（不需要真键盘）：

```bat
simkey.exe a 150          # 按下 A 150ms 后松开
simkey.exe a down         # 只按下
simkey.exe a up           # 只松开
```

---

## 9. 配置语法

每行一条 `键 = 值`，`#` / `;` 之后是注释。

### 全局

```ini
debug        = 0          # 写日志
autostart    = 0          # 开机自启
toggle_key   = F1        # 全局热键：按下切换"总开关"开/关（默认无）
enabled      = 1          # 总开关：0 = 全部功能暂时禁用（socd / dks 的设定保持不变）
```

`enabled`、`socd`、`dks` 三个值都会被程序自动写回 `config.cfg`（只改这三行，
其它注释和内容不动），所以重启程序后能恢复上次的状态。

### SOCD 组

```ini
socd = 1
group = A D mode=snap
group = W S mode=snap
```

| mode | 含义 |
|---|---|
| `snap` | **后输入优先**（Razer Snap Tap / Wooting "Last Input"）：按住 A 再按 D，A 自动释放 |
| `first` | 先输入优先：同按时后按的键被忽略 |
| `neutral` | 同按则两个都取消（Wooting "Neutral"） |
| `dominant` | 指定键优先：`group = A D mode=dominant dominant=A` |
| `off` | 关闭 |

**关键行为：释放后的"恢复"**（这是多数 DKS / Snap Tap 硬件的做法）：

```
按住 A  →  按 D   →  A 被释放（D 生效，A 仍物理按着）
按 D 松开       →  A 仍然物理按着 → A 重新生效（系统重新按下 A）
```

即：**同组中某键松开时，若组内仍有其它键物理按着，那个键会被恢复为"按下"状态。**
反向同理（先按 D 再按 A，松开 A 后 D 恢复）。`neutral` 模式也如此：两键抵消后
只要其中一个物理松开，剩下的那个键立即生效。

`dominant` 模式下若主键不在被按，则退化为"后按优先"，不会把剩下的键全部取消。

### DKS 规则

```ini
rule = key=A on=release tap=D tap_ms=25 min_hold_ms=40 cond=active
```

| 字段 | 含义 |
|---|---|
| `key` | 触发键 |
| `on` | `press` 按下瞬间 / `release` 松手瞬间 / `hold` 按住超过 `min_hold_ms` / `repeat` 按住期间连发 |
| `tap` | 要"轻点"的目标键 |
| `tap_ms` | 目标键按下时长（建议 ≥ 10 ms，太小游戏可能识别不到） |
| `delay_ms` | 触发后延时 |
| `min_hold_ms` | `release`：按住多久才算（快速连点不算）；`hold`/`repeat`：开始触发前的时间 |
| `interval_ms` | `repeat` 的间隔 |
| `count` | `repeat` 次数，0 = 只要按住就一直连发 |
| `cond` | `always` / `active` / `target_up` / `target_free` |

`cond` 的作用：

* `active`（推荐）：只有触发键此刻仍是"有效输入"（没有被同组 SOCD 覆盖）才触发。
  这样 SOCD 已经把 A 释放时，松手 A 不会重复触发反向键，避免"罚站"。
* `target_up`：目标键没有被按住时才触发。
* `target_free`：目标键既没被按住也没处于按下状态才触发。

键名写法：`A`–`Z`、`0`–`9`、`Space Enter Tab Esc Left Right Up Down LShift RShift LCtrl RCtrl
LAlt RAlt Caps Comma Period Minus Equals F1..F12 Home End PageUp PageDown Backspace Insert`，
或直接写虚拟键码数字（如 `65` = A）。

---

## 10. 预设方案（profiles/）

* `valorant-stop.cfg` — 单键急停（WASD 四个键各自反向轻点），SOCD 关闭，最稳。
* `snap-tap.cfg` — 纯 SOCD snap（等价 SnapKey），无 DKS 规则。
* `dkssocd-mix.cfg` — 单键急停 + snap 混用，适合"松手急停派"想更激进的人。
* `typing.cfg` — 全部关闭，只保留热键，打字 / 办公时用。

切换方式：复制对应文件为 `config.cfg`，或 `BinKey.exe --config profiles\valorant-stop.cfg`，
托盘菜单里也有"重新加载配置"。

---

## 11. 快速验证（不需要游戏）

打开记事本（或任意文本框），运行 `BinKey.exe --config config.cfg`，然后：

1. 按住 `A` 150 ms 再松手 → 记事本里应出现 `ad`（`a` 是你按的，`d` 是自动轻点的）。
2. 快速连点 `A` → 只出现一串 `a`，不会触发 `d`（`min_hold_ms` 生效）。
3. 想要看到每一毫秒的动作，把配置里 `debug = 1`，然后看 `BinKey.log`。

也可以用自带的模拟按键工具（不需要真键盘）：

```bat
build\simkey.exe a 150      :: 模拟按下 A 150ms 再松开
build\simkey.exe a down     :: 只按下
build\simkey.exe a up       :: 只松开
build\keyprobe.exe a d      :: 打印系统当前认为 A / D 是否处于按下状态（验证 SOCD）
```

内置的 `--selftest` 会自动跑 **17 组场景 / 90 项断言**（SOCD 四种模式、snap 释放后的
"恢复"、只开 SOCD / 只开 DKS 的组合、松手急停的延迟与时长、连点不触发、
按住目标键时不重复注入、全局热键开关、托盘与程序图标资源等），全部 PASS 才说明逻辑正确。

验证 snap 的"恢复"行为（打开记事本，让 `debug = 1` 以便看日志）：

```
1. 按住 A 不放
2. 按 D            → 日志 [socd-release] A ；记事本里 A 停止、D 出现
3. 松 D（A 仍按着）→ 日志 [socd-restore] A ；A 重新生效
4. 松 A            → A 停止
```

---

## 12. 调参建议

1. **先只用 WASD**。规则越多越容易误触发。
2. `tap_ms` 从 **25 ms** 起步；太小（<10 ms）很多游戏不识别，太大会导致角色"倒退"。
3. `min_hold_ms` 设 **30–60 ms**，避免快速连点时每次都触发反向轻点。
4. 出现"走两步卡一下 / 罚站"：先把 `tap_ms` 调大、`min_hold_ms` 调大，或把 `cond` 改成 `active`。
5. 感觉急停不够快：把 `delay_ms` 保持 0，`tap_ms` 降到 15–20 ms。

---

## 13. 工作原理

1. `SetWindowsHookEx(WH_KEYBOARD_LL)` 拦截所有键盘事件。
2. 程序维护每个"托管键"的**物理状态**（你真的按着吗）与**逻辑状态**（系统 / 游戏认为它按着吗）。
3. 需要释放某个键时，用 `SendInput` 注入合成的 key-up（带 `dwExtraInfo` 标记，钩子见到该标记直接放行，避免自我循环）。
4. 需要"轻点"某个键时，注入 key-down，并在 `tap_ms` 之后注入 key-up。所有注入都在主线程的消息循环里完成
   ——实测在部分环境里子线程调用 `SendInput` 会被系统阻塞约 1 秒，所以主线程驱动更可靠。
5. 键盘的自动重复（按住时的 repeat）会被正确处理：如果某键已被 SOCD 逻辑释放，
   它的重复 key-down 会被吞掉，否则 Snap Tap 会失效（这是很多同类工具翻车的原因）。
6. 需要"恢复"一个被 SOCD 覆盖的键时，注入带标记的 key-down；每次按键事件后调用 `syncGroup()`
   重新计算组内"胜者"。

---

## 14. 已知限制

* 普通键盘没有"行程深度"，所以 DKS 只能用**时间**语义（按下 / 松手 / 按住多久）模拟多触发点，不能模拟"按到一半"。
* 轻点的最小时长受系统调度影响，通常能稳定到 10–15 ms；要更精细需要磁轴硬件。
* 钩子方案对绝大多数游戏（DirectInput / Raw Input / XInput 轮询）有效，但个别反作弊严格的游戏可能不生效或禁止此类工具。
  **CS2 官方明确禁止 Snap Tap / SOCD 类输入，使用可能被封。** 责任自负。
* 程序是单实例（命名互斥锁），重复运行不会叠加。

---

## 15. 常见问题

**Q：托盘图标不显示？**
Windows 10/11 默认把第三方托盘图标收进"隐藏托盘"。点击任务栏右下角的 `˗` / 向上的箭头，
把 BinKey 图标拖到可见区域。

**Q：游戏里没有效果？**
1. 先确认 `BinKey.exe --print` 输出的规则是你想要的；
2. 用 `--watch 30` 观察事件是否被识别；
3. 部分游戏用 Raw Input，依然能被钩子拦截，但某些反作弊（如 CS2）明确禁止 SOCD / Snap Tap。

**Q：需要管理员权限吗？**
不需要。全局键盘钩子和注册表 Run 键都是普通用户权限。

**Q：规则编辑器打开时 DKS 没生效？**
编辑器窗口处于**前台**时，钩子会放行所有按键（否则你在编辑框里打字会被规则干扰）。
把窗口关掉、或切到游戏窗口，规则立即恢复生效。

**Q：资源管理器里 exe 显示的是旧图标 / 通用图标？**
图标已经内嵌，问题在 Windows 的图标缓存 —— 见 **[FIXES.md](FIXES.md)**。

---

## 16. 项目结构

```
BinKey/
├── BinKeySetup.exe        单文件安装向导（分发用，约 3.8 MB）
├── build.bat              编译主程序 + 调试工具
├── build-installer.bat    编译 + 打包成 BinKeySetup.exe
├── fix-icon-cache.bat     清除资源管理器图标缓存（见 FIXES.md）
├── README.md              本文档：用法 / 配置 / 原理
├── FIXES.md               修复与排错说明（问题 → 原因 → 处理 → 验证）
├── CHANGELOG.md           版本改动记录
├── config.cfg             默认配置
├── icons/                 内嵌图标（BinKey.ico / tray_on.ico / tray_off.ico）
├── profiles/              4 个预设方案
├── src/                   源码（BinKey.cpp / BinKeySetup.cpp / packer.cpp / simkey.cpp / keyprobe.cpp / resources.rc ...）
└── build/                 编译产物与测试工具
```

---

## 17. 卸载

安装目录里有 `uninstall.bat`，双击即可：
终止进程 → 删除注册表 Run 项 → 删除桌面快捷方式 → 删除安装目录。
（它先把自己复制到 `%TEMP%` 再执行，否则删目录时会把自己也删掉。）
