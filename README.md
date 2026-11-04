# BinKey — 普通机械键盘也能用 DKS(动态键程) / SOCD(Snap Tap)

一个单文件 C++ / Win32 的 Windows 程序，用**低级键盘钩子**实现磁轴键盘（Wooting / Razer / 磁轴厂）的
**DKS（Dynamic Keystroke，动态键程）** 与 **SOCD（Snap Tap）** 功能，让普通机械键盘也能实现
"单键急停"、"按放两段触发"、"连发" 等玩法。

与 [SnapKey](https://github.com/cafali/SnapKey) 的关系：SnapKey 只做 SOCD（同向键互相取消），
**DKS 是它没有的功能**。BinKey 同时提供两者，并且 DKS 的核心 ——
"**按下触发一次、松手再触发一次**" —— 用软件时间语义模拟了磁轴的多触发点。

> **不想编译？** 直接双击项目根目录的 **`BinKeySetup.exe`**（单文件安装引导，约 3.8 MB），
> 向导式安装，详见 [第 15 节](#15-安装引导程序-binkkeysetupexe)。

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

## 2. 编译

需要 MinGW（msys2）或任何 C++17 编译器：

```bat
build.bat
```

或手动：

```bat
g++ -O2 -std=c++17 -static -mwindows src\BinKey.cpp -o build\BinKey.exe -lwinmm -lgdi32 -luser32
```

产物 `build\BinKey.exe` 约 1 MB，静态链接，不需要运行时。

---

## 3. 运行

| 命令 | 说明 |
|---|---|
| `BinKey.exe` | 后台运行，系统托盘图标（左键切换开关，右键菜单） |
| `BinKey.exe --config <路径>` | 指定配置文件（默认找 exe 同目录的 `config.cfg`，再找当前目录） |
| `BinKey.exe --print` | 打印当前解析后的规则，然后退出 |
| `BinKey.exe --editor` | 启动时直接打开**规则编辑器窗口**（可视化编辑 DKS / SOCD） |
| `BinKey.exe --watch [秒]` | 前台打印每个托管按键的事件与注入动作（默认 20 秒），排错用 |
| `BinKey.exe --selftest` | 内置 13 组场景自检（**虚拟自检**：把事件直接喂进钩子，不真按键盘，不会敲进任何窗口） |
| `BinKey.exe --selftest-live` | 同样的 13 组场景，但用真实 `SendInput` 注入（会真按键，用来验证注入链路） |
| `BinKey.exe --autostart` / `--no-autostart` | 开机自启开关（写注册表 Run 键） |
| `BinKey.exe --version` / `-v` | 打印版本号（当前 **v1.0.0**） |

配置文件不存在时会自动生成一份默认配置。`debug = 1` 时写入 `BinKey.log`。

### 托盘图标

* **左键**：启用 / 禁用全部功能（图标颜色随之变化，并写入日志）。
* **右键**：弹出菜单
  1. 启用全部功能 / 禁用全部功能
  2. **SOCD(Snap Tap)：开启 / 关闭** —— 单独开关
  3. **DKS(动态键程)：开启 / 关闭** —— 单独开关
  4. **规则编辑器**（图形窗口，自定义 DKS 与 SOCD）
  5. 重新加载配置
  6. 打开配置文件（用系统默认编辑器）
  7. 设为开机自启 / 取消开机自启
  8. 打印当前规则到日志（方便排错）
  9. 退出

> SOCD 与 DKS 是两个**独立开关**：可以只开 SOCD（只做 Snap Tap，不做急停），
> 也可以只开 DKS（只做单键急停，不做 SOCD 清理），或两个都开。
> 两个都关闭时，程序完全放行键盘，等于不存在。

> 排错提示：托盘菜单必须用 `CreatePopupMenu()` 而不是 `CreateMenu()`。
> `CreateMenu()` 创建的是"菜单栏"，用 `TrackPopupMenu` 弹出时只有几像素宽，
> 表现就是"右键后菜单显示异常 / 菜单变成一条细条"。本程序已修正。

### 图标

图标是内嵌的真实 .ico 文件（`icons\` 目录，由 `src\resources.rc` / `src\icon.rc` 嵌入），
它们就是你提供的 `1.ico / 2.ico / 3.ico`，原封不动地放进 `icons\`：

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
:: 2) 重新 build-installer.bat
```

如果只有 .jpg，可以用 `src\mkico.py` 转成多尺寸 .ico：

```bat
python src\mkico.py 1.jpg icons\BinKey.ico   16,32,48,64,128,256
python src\mkico.py 3.jpg icons\tray_on.ico  16,32,48,64
python src\mkico.py 2.jpg icons\tray_off.ico 16,32,48,64
```

`loadEmbeddedIcon()` 解析内嵌的 ICO 条目（选最接近 32×32 的那张，并且正确处理 AND 掩码），
失败时回退到内置自绘图标，所以即使没有 `icons\` 目录程序也有图标。

安装引导窗口和规则编辑器窗口也都注册了 `hIcon`（并在创建后显式
`WM_SETICON`），所以**标题栏左上角、Alt-Tab**都会显示程序图标。

> **如果资源管理器里 exe 仍然显示旧图标 / 通用图标** —— 图标已经内嵌，问题出在
> Windows 的**图标缓存**：缓存保存在
> `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_16/32/48/256.db`，
> **重启资源管理器并不会清除它**。
> 运行项目里的 `fix-icon-cache.bat`（它会停止 explorer.exe、删除缓存 DB、再重启
> explorer.exe），或手动：
>
> ```bat
> taskkill /f /im explorer.exe
> del /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db"
> explorer.exe
> ```
>
> 也可以把文件改名 / 复制到新路径，缓存键是按路径哈希的，新路径一定会重新提取。


### 规则编辑器窗口（菜单 → 规则编辑器，或 `BinKey.exe --editor`）

一个纯 Win32 窗口，不需要联网、不需要额外运行时：

| 区块 | 作用 |
|---|---|
| 左上 **DKS 规则列表** | 列出所有 `key → tap` 规则；选中后下方字段会填充 |
| 触发键 / 轻点键 | 下拉选择任意字母、数字、方向键、F1-F12 等 |
| 触发方式 | `松开时`(release) / `按下时`(press) / `按住超过`(hold) / `连发`(repeat) |
| tap_ms / 延迟 / min_hold / 连发间隔 / 次数 | 数值参数 |
| 条件 | `active` / `always` / `target_up` / `target_free` |
| 新增 / 更新选中 / 删除选中 | 立即生效（改完立刻生效，无需重启） |
| 右上 **SOCD 组列表** | 组内按键（空格分隔）+ 模式 + 主键(dominant) |
| 启用 SOCD / 启用 DKS | 两个独立开关，等同于托盘菜单里的 2、3 项 |
| 写回配置文件 / 重置为示例规则 | 保存到 `config.cfg` |

* 任何改动都**即时生效**并自动写回 `config.cfg`。
* 编辑器窗口处于前台时，键盘钩子**放行所有按键**（否则你在编辑框里打字会被规则干扰）。
  所以编辑器打开时不要同时玩游戏；关掉窗口后规则照常生效。

`build\simkey.exe` 是一个模拟按键的小工具，用来验证注入效果（不需要真键盘）：

```bat
simkey.exe a 150          # 按下 A 150ms 后松开
simkey.exe a down         # 只按下
simkey.exe a up           # 只松开
```

---

## 4. 配置语法

每行一条 `键 = 值`，`#` / `;` 之后是注释。

### 全局

```ini
debug        = 0          # 写日志
autostart    = 0          # 开机自启
toggle_key   = F1        # 全局热键：按下切换 DKS/SOCD 开/关（默认无）
```

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

**关键行为：释放后的"恢复"**（这是多数 DKS/Snap Tap 硬件的做法）：

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

## 5. 预设方案（profiles/）

* `valorant-stop.cfg` — 单键急停（WASD 四个键各自反向轻点），SOCD 关闭，最稳。
* `snap-tap.cfg` — 纯 SOCD snap（等价 SnapKey），无 DKS 规则。
* `dkssocd-mix.cfg` — 单键急停 + snap 混用，适合"松手急停派"想更激进的人。
* `typing.cfg` — 全部关闭，只保留热键，打字/办公时用。

切换方式：复制对应文件为 `config.cfg`，或 `BinKey.exe --config profiles\valorant-stop.cfg`，
托盘菜单里也有"重新加载配置"。

---

## 6. 快速验证（不需要游戏）

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

内置的 `--selftest` 会自动跑 **13 组场景 / 53 项断言**（SOCD 四种模式、snap 释放后的
"恢复"、只开 SOCD / 只开 DKS 的组合、松手急停的延迟与时长、连点不触发、
按住目标键时不重复注入、全局热键开关等），全部 PASS 才说明逻辑正确。

验证 snap 的"恢复"行为（打开记事本，让 `debug = 1` 以便看日志）：

```
1. 按住 A 不放
2. 按 D            → 日志 [socd-release] A ；记事本里 A 停止、D 出现
3. 松 D（A 仍按着）→ 日志 [socd-restore] A ；A 重新生效
4. 松 A            → A 停止
```

---

## 7. 调参建议

1. **先只用 WASD**。规则越多越容易误触发。
2. `tap_ms` 从 **25 ms** 起步；太小（<10 ms）很多游戏不识别，太大会导致角色"倒退"。
3. `min_hold_ms` 设 **30–60 ms**，避免快速连点时每次都触发反向轻点。
4. 出现"走两步卡一下 / 罚站"：先把 `tap_ms` 调大、`min_hold_ms` 调大，或把 `cond` 改成 `active`。
5. 感觉急停不够快：把 `delay_ms` 保持 0，`tap_ms` 降到 15–20 ms。

---

## 8. 原理（为什么普通键盘也能做）

1. `SetWindowsHookEx(WH_KEYBOARD_LL)` 拦截所有键盘事件。
2. 程序维护每个"托管键"的**物理状态**（你真的按着吗）与**逻辑状态**（系统/游戏认为它按着吗）。
3. 需要释放某个键时，用 `SendInput` 注入合成的 key-up（带 `dwExtraInfo` 标记，钩子见到该标记直接放行，避免自我循环）。
4. 需要"轻点"某个键时，注入 key-down，并在 `tap_ms` 之后注入 key-up。所有注入都在主线程的消息循环里完成
   ——实测在部分环境里子线程调用 `SendInput` 会被系统阻塞约 1 秒，所以主线程驱动更可靠。
5. 键盘的自动重复（按住时的 repeat）会被正确处理：如果某键已被 SOCD 逻辑释放，
   它的重复 key-down 会被吞掉，否则 Snap Tap 会失效（这是很多同类工具翻车的原因）。

---

## 9. 已知限制
* 普通键盘没有"行程深度"，所以 DKS 只能用**时间**语义（按下/松手/按住多久）模拟多触发点，不能模拟"按到一半"。
* 轻点的最小时长受系统调度影响，通常能稳定到 10–15 ms；要更精细需要磁轴硬件。
* 钩子方案对绝大多数游戏（DirectInput / Raw Input / XInput 轮询）有效，但个别反作弊严格的游戏可能不生效或禁止此类工具。
  **CS2 官方明确禁止 Snap Tap / SOCD 类输入，使用可能被封。** 责任自负。
* 程序是单实例（命名互斥锁），重复运行不会叠加。

---

## 10. 常见问题

**Q：托盘图标不显示？**
Windows 10/11 默认把第三方托盘图标收进"隐藏托盘"。点击任务栏右下角的 `˗` / 向上的箭头，
把 BinKey 图标拖到可见区域。

**Q：exe / 快捷方式在资源管理器里还是通用图标？**
图标确实已经内嵌（`BinKey.exe --selftest` 会打印 `程序图标资源（1 组，Explorer/快捷方式可用）`
和 `托盘注册 Shell_NotifyIcon(NIM_ADD) PASS`），而且用资源管理器自己的图标提取接口
（`IExtractIcon`）实测 `BinKeySetup.exe` 提取到的就是 `1.ico` 的像素。
资源管理器显示旧图标是 **图标缓存**的问题：缓存写在
`%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_16/32/48/256.db`，
**重启资源管理器不会清除它**（我实测 `ie4uinit.exe -ClearIconCache` 也不起作用）。
正确做法是删掉这些缓存文件再重启 explorer：

```bat
taskkill /f /im explorer.exe
del /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db"
explorer.exe
```

项目里已经提供 `fix-icon-cache.bat`，双击即可完成上面三步。
另一个更省事的做法：把 exe 改名或复制到新路径 —— 缓存键是按路径哈希的，新路径必然重新提取。

**Q：右键菜单显示成一条细条 / 菜单内容空白？**
这是 `CreateMenu()` 与 `CreatePopupMenu()` 的区别：前者是"菜单栏"，用 `TrackPopupMenu`
弹出时宽度只有几像素。当前代码使用 `CreatePopupMenu()`，实测弹出尺寸约 206x156 像素。

**Q：菜单弹出不消失？**
`TrackPopupMenu` 之后调用 `PostMessage(hwnd, WM_NCHITTEST, 0, 0)`（代码已包含），
这是让弹出菜单正确关闭的经典补丁。另外 `SetForegroundWindow(托盘窗口)` 必须先调用。

**Q：游戏里没有效果？**
1. 先确认 `BinKey.exe --print` 输出的规则是你想要的；
2. 用 `--watch 30` 观察事件是否被识别；
3. 部分游戏用 Raw Input，依然能被钩子拦截，但某些反作弊（如 CS2）明确禁止 SOCD/Snap Tap。

**Q：需要管理员权限吗？**
不需要。全局键盘钩子和注册表 Run 键都是普通用户权限。

**Q：规则编辑器打开时 DKS 没生效？**
编辑器窗口处于**前台**时，钩子会放行所有按键（否则你在编辑框里打字会被规则干扰）。
把窗口关掉、或切到游戏窗口，规则立即恢复生效。

---

## 11. 修复记录

* **托盘右键菜单"显示异常"（菜单弹出来只有几像素宽 / 内容看不见）**
  根因：菜单用 `CreateMenu()` 创建。`CreateMenu()` 创建的是**菜单栏**，
  用 `TrackPopupMenu` 弹出时 Windows 只给出约 6 像素宽的条；
  正确做法是 `CreatePopupMenu()`。修复后实测弹出尺寸 206×156 像素，内容正常。
* 右键事件同时兼容 `WM_RBUTTONDOWN` / `WM_RBUTTONUP`（不同 Windows 版本发送的事件不同），
  并加了 300 ms 防重复，避免菜单弹出两次或不弹出。
* `WM_COMMAND` 用 `LOWORD(wParam)` 取菜单 id（弹出菜单的 wParam 高 16 位是菜单句柄），
  之前直接 `switch (w)` 在个别环境下会匹配失败。
* 菜单位置会 clamp 到屏幕内，避免靠右侧/右下角时菜单被截断。
* 托盘图标改为 `CreateCompatibleBitmap + SetDIBits` 生成 32bpp 图标，比 `CreateBitmap(...,32,1,...)` 更可靠。
* 新增菜单项：**设为开机自启 / 取消开机自启**。

验证方式（无需鼠标）：程序启动后，用 `FindWindowW(L"BinKeyTrayWindow", NULL)` 找到托盘窗口，
`PostMessage(hwnd, WM_APP+1, 0, WM_RBUTTONDOWN)` 即可弹出菜单；实测菜单窗口（类名 `#32768`）
尺寸为 **206×156**，且键盘钩子在菜单打开期间仍然正常工作。

---

## 12. 本次更新（v2）

1. **SOCD 与 DKS 成为两个独立开关**
   托盘菜单新增 `SOCD(Snap Tap)：开启/关闭` 和 `DKS(动态键程)：开启/关闭` 两项，
   可以只开其中一个，也可以两个都开；两个都关时键盘完全放行。
   编辑器窗口里也有同样的两个复选框。`toggle_key` 热键则同时切换两者。

2. **DKS / SOCD 规则编辑器窗口**
   菜单 → "规则编辑器"，或 `BinKey.exe --editor`。
   可视化增删改规则（触发键、轻点键、触发方式、tap_ms、延迟、min_hold、连发间隔与次数、条件），
   以及 SOCD 组（键列表 + 模式 + dominant 主键）。改动即时生效并自动写回 `config.cfg`。

3. **SOCD 逻辑修正：被覆盖的键在另一键松开后恢复**
   旧行为：A 被 snap 释放后，即使 D 松开，A 也不再生效，直到你重新按 A。
   新行为：
   ```
   A 按住 → D 按下  → A 逻辑释放（A 仍物理按着）
              D 松开  → A 立即恢复为"按下"
   ```
   反向同理；`neutral` 模式也如此（两键抵消后，只要其中一个物理松开，剩下的那个立即生效）。
   `dominant` 模式下若主键没有按下，则退化为"后按优先"，不会把剩下的键全部取消。
   实现方式：每次按键事件后调用 `syncGroup()` 重新计算组内"胜者"，
   对需要恢复的键注入 key-down、需要释放的键注入 key-up

---

## 13. 修复

**编辑器里的选项无法滚动（选了 P 滚不到 O）**

* 键下拉框有 82 个选项，创建时没有 `WS_VSCROLL`，列表被截断且没有滚动条，
  选中靠后的键后无法往上滚。现在所有 COMBOBOX / LISTBOX 都带 `WS_VSCROLL`，
  滚轮、上下键、滚动条都能用；下拉高度加大到 240 px，窗口改为 790×560，
  下拉框不再被窗口边界裁切。

**托盘菜单打开时 DKS 暂时不生效**

* `TrackPopupMenu` 阻塞主消息循环，排队的注入动作不会执行。
  现在给托盘窗口挂了一个 20 ms 的 `SetTimer`，`WM_TIMER` 里调用 `processDueActions()`，
  菜单打开期间急停/轻点依然正常（实测：菜单打开时按 A 松手，日志出现 tap-down / keyup）。

实测：打开触发键下拉框（选中 P，列表起始索引 15）→ 鼠标滚轮向上 → 索引回到 0（A），可以选到 O。（日志中对应 `[socd-restore]` / `[socd-release]`）。

---

## 14. 修复："更新选中 / 删除选中"永远作用于第一条

**症状**：规则编辑器里点列表中的某一条，"更新选中"和"删除选中"却总是改/删第 1 条；选中状态看起来也不生效。

**原因有三个，都修了：**

1. **LISTBOX 没有 `LBS_NOTIFY`**
   Windows 的 LISTBOX 只有在创建时带 `LBS_NOTIFY` 才会向父窗口发送 `LBN_SELCHANGE`。
   原来两个列表框的样式是 `WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL`，没有 `LBS_NOTIFY`，
   所以编辑器窗口永远收不到"用户点了哪一行"的通知，`g_selRule` / `g_selGroup` 停留在打开时的 0。
   现在两个列表框都带 `LBS_EXTENDEDSEL | LBS_NOTIFY`，每次选择后把选择集收敛成一条
   （`LB_SETSEL` 只在多选列表框上有效，单选列表框上它是空操作，实测已验证）。

2. **COMBOBOX 的当前值取不到**
   `CBS_DROPDOWNLIST` 类型的下拉框没有编辑子控件，`GetWindowTextW()` 返回空字符串，
   必须用 `CB_GETCURSEL` + `CB_GETLBTEXT`。之前 `readRuleFields()` 用 `GetWindowTextW` 读
   触发键 / 轻点键，拿到空串 → `parseKey("") == 0` → 直接 return，更新/新增静默失败。
   现在统一走 `comboText()`。

3. **触发方式 / 条件下拉框的选项顺序与枚举不一致**
   枚举是 `ON_PRESS=0, ON_RELEASE=1, ON_HOLD=2, ON_REPEAT=3`，但下拉框里第一项写的是
   "松开时"，于是选"按下时"实际存的是 `ON_RELEASE`。现在下拉框顺序与枚举一一对应：
   `按下时 / 松开时 / 按住超过 / 连发`，条件为 `always / active / target_up / target_free`。

**另外**：列表下方新增了两行状态栏（"当前选中：第 N 条规则 / 第 N 个 SOCD 组"），
选中哪一条一目了然；删除后自动选中相邻的一条并把其内容载入字段；没有选中时点更新/删除
会在日志里写 `[editor] 没有选中的规则`。

实测（用真实鼠标点击第 3 行，状态栏显示 "当前选中：第 3 条规则"；改 tap_ms 后点"更新选中"）：
```
[editor] 更新规则 2: W 松开时 -> 轻点 X  tap=40ms
[editor] 删除规则 3: S 松开时 -> 轻点 W  tap=20ms
```
配置文件里对应行确实被改/删。新增 / 删除 / SOCD 组的新增、更新、删除也都按选中行生效。

**顺带修了一个配置文件解析的坑**

用记事本 / PowerShell 保存 `config.cfg` 时可能会加 UTF-8 BOM（EF BB BF），
原来 BOM 会让**第一行**的键名变成无效，导致第一行配置（例如 `socd = 0`）被忽略。
现在读取配置文件时会自动跳过 BOM。实测：`socd = 0 / dks = 1` 四种组合的行为都符合预期。

---

## 15. 安装引导程序 `BinKeySetup.exe`

项目根目录的 `BinKeySetup.exe`（约 3.8 MB）是一个**自解压式的单文件安装向导**，
适合直接发给别人，不需要装任何运行环境。

### 怎么生成

```bat
build-installer.bat
```

它会编译 `BinKey.exe` / `simkey.exe` / `keyprobe.exe` / 安装向导 / 打包器，
然后把 8 个文件（程序 + config.cfg + README + CHANGELOG + 4 个预设方案）
追加到向导 exe 尾部，输出 `BinKeySetup.exe`。

原理很简单：PE 文件尾部追加
`[负载数据][清单][u32 清单长度]["DSKSETUP"]`，
向导运行时用 `GetModuleFileNameW` 读自己，解析清单并释放文件。

### 图形向导（双击运行）

| 步骤 | 内容 |
|---|---|
| 1 欢迎 | 说明 DKS / SOCD 是什么，环境要求 |
| 2 安装位置 | 点 **选择安装文件夹...** 用系统文件夹选择器挑一个目录（没有预设列表，也不用手打路径）；不选就装到 `%LOCALAPPDATA%\BinKey` |
| 3 安装选项 | 只有三个：开机自动启动 / 创建桌面快捷方式 / 安装完成后立即启动 BinKey |
| 4 安装结果 | 释放的文件清单 + 自动跑 `--selftest` 的结果（应为 53 PASS / 0 FAIL） |
| 5 完成 | **打开规则编辑器**（右下角，唯一的操作按钮） + 退出；点"打开规则编辑器"后引导窗口自动关闭 |

"启动 BinKey" / "打开安装目录" 两个按钮已删掉 —— 安装完成时 BinKey 通常已经在运行，
这两个按钮多余；要改规则直接点 **打开规则编辑器**。

按钮的实现：向导调用 `BinKey.exe --editor`。
- 如果 BinKey 已经在运行（单实例互斥锁已存在），第二个实例会 `FindWindowW("BinKeyTrayWindow")`
  并 `SendMessage(hwnd, WM_APP+2)`，让正在运行的实例弹出规则编辑器 —— 编辑器窗口属于
  那个实例，改动即时生效。
- 如果没有实例在运行，则直接启动一个带编辑器的实例。
（以前这个按钮"按了没反应"，就是因为第二个实例撞上单实例锁后直接退出了。）

第 2、3、4 屏右上角都会显示 **安装到: <实际目录>**，装错与否一眼就能确认。

不需要管理员权限（写的是 `HKCU` 注册表项，不是系统级驱动）。
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

调试工具（`simkey.exe` / `keyprobe.exe`）和源码不再打包进安装文件 —— 源码在
`src\` 里，需要自己编译时用 `g++ -O2 -std=c++17 -static src\simkey.cpp -o build\simkey.exe -luser32`。

自检是在后台 `CreateProcessW` 直跑 `BinKey.exe --selftest`，输出重定向到临时文件，
**不会弹出任何命令窗口**。而且从 v2.2.5 起自检是**虚拟自检**：不再用 `SendInput`
真按 A/D/W/X/Y，而是把构造的 `KBDLLHOOKSTRUCT` 直接喂进 `llHook()`，用内部状态
判断"系统看到的是按下/未按下"。所以安装过程中**不会有任何按键敲进你正在用的窗口**
（旧版本虽然也没有命令窗口，但模拟按键会打到前台窗口，看起来像"不断输入 ad"）。

想验证真实的按键注入链路，手动跑 `BinKey.exe --selftest-live`（那个版本会真按键，
会敲进前台窗口；因为涉及真实按键时序，偶尔会有一两断言不稳定，日常用默认的
`--selftest` 即可）。

桌面快捷方式名为 `BinKey.lnk`，指向安装目录里的 `BinKey.exe`，
工作目录也设成安装目录；卸载时会自动删除它。

### 卸载

安装目录里有 `uninstall.bat`，双击即可：
终止进程 → 删除注册表 Run 项 → 删除桌面快捷方式 → 删除安装目录。
（它先把自己复制到 `%TEMP%` 再执行，否则删目录时会把自己也删掉。）

### 实测记录

- 图形向导：真实鼠标点击走完 5 屏，用系统文件夹选择器选目录安装成功，
  结果页显示 10 条（8 个文件 + 桌面快捷方式 + uninstall.bat）+ `自检结果: 53 PASS / 0 FAIL`；
  全程没有任何命令窗口弹出来（只有终端自己的 PseudoConsoleWindow）。
- **自检不产生按键**：另开一个进程装全局 `WH_KEYBOARD_LL` 钩子统计真实键盘事件，
  整个安装过程 45 秒 = **0 次**键盘事件；单独跑 `--selftest` 期间也是 0 次，
  而 `--selftest-live` 期间是 131 次 —— 说明虚拟自检确实不真按键盘。
- 安装选项只有三个复选框，默认全勾选；取消"创建桌面快捷方式"后桌面确实不生成 `.lnk`。
- **完成页只有两个按钮**：实测枚举控件，第 5 屏只有 `退出` 和 `打开规则编辑器`；
  点击后 `BinKeyEditor` 窗口出现（790×560，42 个控件，规则列表 / SOCD 组列表都有内容）。
- 编辑器打开后 DKS/SOCD 仍正常：用 `simkey` 真按键 + `keyprobe` 读系统状态 ——
  A 按住、按 D → 系统 `a=未按下, d=按下`；松 D（A 仍按住）→ `a=按下, d=未按下`。
- 静默模式：`--silent --dir ... --no-autostart` 释放 8 个文件，
  `profiles\` 子目录正确创建（不是平铺到根目录）。
- 桌面快捷方式：`BinKey.lnk`（2055 B），target / working directory 都指向安装目录，
  双击确实拉起 BinKey 托盘；
- 卸载：`uninstall.bat` 执行后目录消失、`HKCU\...\Run\BinKey` 被删除、桌面 `.lnk` 被删除。
- 切屏残留：用真实屏幕 BitBlt 抓客户区逐像素比对，切屏后旧控件区域未被新控件覆盖的像素
  全部等于背景色（残留 = 0），文字不再叠在上一屏上。
- 装完直接运行 `BinKey.exe`，SOCD 与 DKS 都生效（A+D 互相取消、W 松手自动轻点 S）。

