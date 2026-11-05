# BinKey 更新记录

> 这里只写**每个版本改了什么**（改动清单）。
> 问题的原因、修复过程、验证方法都集中在 **[FIXES.md](FIXES.md)**。

## v1.0.1 — 保留上次开关状态（总开关与单独开关分离）
- **总开关与单独开关分离，状态持久化**：`config.cfg` 新增 `enabled`（总开关）。
  托盘左键 / 菜单"禁用全部功能" / `toggle_key` 热键只切总开关，
  重新启用时恢复用户之前的 `socd` / `dks` 单独设定（不会两个都打开）；
  每次切换都会写回 `config.cfg`，所以退出程序再打开也只开启上次那个功能。
  只有两个单独开关都被手动关闭过，"启用全部功能"才会两个都开。
- **新增 `--selftest` 场景 14**（12 项断言）：验证"只开 SOCD → 禁用全部 → 重新启用
  仍只开 SOCD"，以及开关状态写回配置文件后能读回。自检总断言数 57 → 69。
- **新增验证工具 `build\traytest.exe`**：启动真实程序，用 `PostMessage` 模拟托盘左键
  和菜单项，读回 `config.cfg` 检查 `enabled / socd / dks`。
- 版本号：主程序与安装引导都显示 **v1.0.1**（`BinKey.exe --version` → `BinKey v1.0.1`）。
- 详见 [FIXES.md](FIXES.md) 第 16 节。

## v1.0.0 — 正式改名 BinKey，图标换成真实图片
- **改名**：程序从 `DKSKey` 改名为 **BinKey**。可执行文件 `BinKey.exe`、
  安装程序 `BinKeySetup.exe`、窗口类名（`BinKeyTrayWindow` / `BinKeyEditor` /
  `BinKeySetupWizard`）、单实例互斥锁 `BinKeySingletonMutex`、日志 `BinKey.log`、
  注册表自启值 `BinKey`、卸载脚本 `BinKey_uninstall.bat` 全部同步改名。
- **版本号统一为 1.0.0**：主程序和安装引导都显示 `v1.0.0`；新增
  `BinKey.exe --version`（`-v`）输出 `BinKey v1.0.0`。
- **图标换成你提供的 .ico 文件**（`D:\project\1.ico` / `2.ico` / `3.ico`），原封不动内嵌：
  - 程序图标（Explorer / 桌面快捷方式 / 安装引导程序 / 窗口标题栏） = **1.ico**
  - 托盘图标 **开启状态**（SOCD 或 DKS 任一开启） = **3.ico**
  - 托盘图标 **关闭状态**（全部功能关闭） = **2.ico**
  这三个 .ico 每个包含 16 / 24 / 32 / 48 / 64 / 72 / 96 / 128 / 256 九种尺寸，
  `resources.rc` 里 `1 ICON` 生成标准的 `RT_GROUP_ICON`（Explorer 用），
  `2/3 ICOFILE` 以自定义类型 `ICOFILE` 原样嵌入原始 .ico 字节（托盘用，运行时解析）。
  解析失败时回退到内置自绘图标。若以后只有 .jpg，也可以用
  `python src\mkico.py <图片> <输出.ico> 16,32,48,64,128,256` 生成。
- **ICO 解析修正**：`loadEmbeddedIcon()` 之前只对 24bpp 的条目减半 `biHeight`，
  而你的 .ico 是 **32bpp + AND 掩码**（`biHeight = 2 × 真实高度`），按旧逻辑会读出
  64 行 → 图标空白/错位。现在用条目总长度判断有没有掩码，24bpp 和 32bpp 都正确；
  实测渲染出的 32×32 平均 RGB 与源 .ico 完全一致（开启 157/148/167，关闭 177/170/172）。
- **图标显示修复**：安装引导窗口和规则编辑器窗口之前没有窗口图标，所以
  标题栏 / Alt-Tab 里看不到图标——现在两个窗口类都注册了 `hIcon`，创建窗口后
  显式 `WM_SETICON`。托盘注册 (`Shell_NotifyIcon NIM_ADD`) 会记录成功/失败，
  失败时自动用自绘图标重试一次。
- **`BinKeySetup.exe` 图标与主程序一致**：安装引导单独嵌入 `icons\BinKey.ico`
  （就是你给的 `1.ico`，16…256 九种尺寸），资源管理器 / 桌面 / Alt-Tab 里
  引导程序和主程序显示同一个图标。
- **图标不更新的问题定位**：exe 里的图标是正确的，问题在资源管理器的**图标缓存**
  (`%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db`) —— 重启资源管理器不清缓存。
  新增 `fix-icon-cache.bat` 一键清理。原因、实测数据与验证方法见 **FIXES.md** 第 1 节。
- **桌面快捷方式改名为 `BinKey.lnk`**（原先是 `BinKey 动态键程.lnk`）。安装时如果
  桌面上还存在旧名字 `BinKey 动态键程.lnk`，会自动删除，避免桌面出现两个图标；
  卸载脚本也会删除 `BinKey.lnk`。
- 自检新增 4 条图标检查（开启/关闭托盘图标解析、exe 图标资源、真实
  `Shell_NotifyIcon(NIM_ADD)` 注册），共 **57 项全部 PASS**。

> 下面 v2.2.x / v2.1 / v2.0 是改名前（原 DKSKey）的历史记录。

## v2.2.7 — 点"打开规则编辑器"后引导自动关闭
- 完成页点 **打开规则编辑器** 后：先打开编辑器，然后安装引导窗口自动关闭
  （不用再手动点"退出"）。`SendMessage` 是同步的，所以编辑器窗口已经创建好了才关窗口，
  不会出现"引导关了编辑器没开"的情况。
- 如果 BinKey 还没运行，按钮会启动一个带编辑器的实例，同样关闭引导。

## v2.2.6 — 完成页按钮精简 + 规则编辑器按钮修好
- 第 5 屏（完成页）右下角删掉 **"启动 BinKey"** 和 **"打开安装目录"** 两个按钮，
  只留 **"打开规则编辑器"**（左侧的"退出"保留）。对应的 `ID_BTN_START` /
  `ID_BTN_OPENDIR` 控件和处理分支也一并删除。
- **"打开规则编辑器"以前按了没反应**的原因：向导用 `CreateProcessW` 启动
  `BinKey.exe --editor`，但安装完成后 BinKey 已经在运行，第二个实例撞上单实例
  互斥锁（`BinKeySingletonMutex`），打印"已经在运行"后直接退出，编辑器根本没开。
- 现在：第二个实例检测到互斥锁存在时，用 `FindWindowW(L"BinKeyTrayWindow")` 找到
  正在运行的实例，`SendMessage(hwnd, WM_OPEN_EDITOR)`（`WM_APP + 2`，不能用
  `WM_APP + 1`，那是托盘图标消息 `WM_TRAY`），由运行中的实例弹出规则编辑器，
  改动即时生效。没有实例在跑时，仍然直接启动一个带编辑器实例。
- 实测：安装完成页只有"退出"和"打开规则编辑器"两个按钮，点击后
  `BinKeyEditor` 窗口出现（790×560，居中），规则列表 / SOCD 组都正常。

## v2.2.5 — 自检不再真按键盘（彻底解决"adad 输入"）
- 上一版只是去掉了 `cmd.exe` 窗口，但 `--selftest` 仍然用 `SendInput` 真注入 A/D/W/X/Y，
  这些按键会敲进当时前台的窗口 —— 所以点"安装"后还是看到一串 `adadad`。
- 现在自检改成**纯逻辑自检**：`selfTest()` 设 `g_virtual = true`，`injectKey()` 不再调用
  `SendInput`，而是构造 `KBDLLHOOKSTRUCT` 直接调用 `llHook()`，钩子返回 1（被吞）时
  模拟"系统收不到"，放行时更新虚拟系统状态；`check()` 读这个虚拟状态而不是
  `GetAsyncKeyState`。因此自检期间**系统键盘事件为 0**（实测用另一个全局键盘钩子统计：
  整个安装过程 0 次按键事件）。
- 自检也不安装真实的 WH_KEYBOARD_LL 钩子（`installHook()` 在虚拟模式下直接返回），
  避免自检那两秒里拦截用户真按键。
- 新增 `BinKey.exe --selftest-live`：想验证真实的 `SendInput` 链路时用这个，
  它会真按键（会敲进前台窗口，自己注意）。
- 顺带清理冗余代码：`build-installer.bat` 不再编译不打包的 `simkey.exe` / `keyprobe.exe`；
  `runCapture()` 里 `si.hStdInput` 在 GUI 程序里可能是无效句柄，改为只在句柄有效时传入。
- 版本号 `kVer` 升到 2.2.5。

## v2.2.4 — 安装选项精简 + 不再弹命令窗口
- **安装选项只剩三个**：开机自动启动 / **创建桌面快捷方式**（新增） / 安装完成后立即启动 BinKey。
  "附带调试工具"、"附带源码"两个复选框删掉，`simkey.exe` / `keyprobe.exe` / `src\` / `build.bat`
  也不再打包进安装文件 —— 安装文件从 3.5 MB 降到 2.1 MB。
- **新增桌面快捷方式**：用 Shell 的 `IShellLinkW` + `IPersistFile` 生成
  `BinKey.lnk`，指向安装目录里的 `BinKey.exe`，工作目录设成安装目录，图标用 exe 自带图标。
  卸载脚本会自动删除这个 `.lnk`。
- **点"安装"后弹出的黑色命令窗口**：自检以前用
  `cmd /c chcp 65001 && BinKey.exe --selftest > 文件`，`cmd.exe` 是控制台程序，
  会闪一个窗口；而 selftest 用 `SendInput` 注入 A/D 按键，这些按键敲进那个窗口，
  所以看起来"不断输入 ad"，进程结束窗口自动关闭。
  现在直接 `CreateProcessW("BinKey.exe --selftest")`，标准输出重定向到一个临时文件句柄
  （`SetHandleInformation(fh, HANDLE_FLAG_INHERIT, 1)` 让子进程能继承该句柄），
  没有 `cmd.exe`，没有任何窗口。
- 静默模式参数改为 `--silent [--dir <路径>] [--no-autostart] [--no-shortcut] [--launch]`。

## v2.2.3 — 安装位置只靠文件夹选择器
- 第 2 屏删掉预设下拉列表（用户目录 / 程序所在目录 / 主目录 / D 盘 / 自选），
  只留一个 **选择安装文件夹...** 按钮 + 只读的路径显示框。
  以前"选了文件夹却还是装到用户目录"的困惑来自预设列表：列表选中项会覆盖
  `浏览文件夹...` 的结果，现在没有列表，选择器返回什么就装什么。
- 选择器打开时默认停在当前路径（`IFileOpenDialog` 的 `SetFolder`）。
- 第 3 / 4 屏右上角显示 **安装到: <目录>**，装到哪一目了然。

## v2.2.2 — 安装引导的两处体验修复
- **切屏时旧文字残留（图层重叠）**：向导窗口类没有背景刷，切屏时只销毁旧控件，
  旧控件绘制的文字还留在客户区，看起来像"上一步的提示没关掉"。
  现在窗口类带背景刷，并且每次 `buildStep()` 在重建控件后
  `InvalidateRect(win, NULL, TRUE) + UpdateWindow(win)` 强制重绘整屏。
  （像素级验证：切屏前后对比，旧控件区域里未被新控件覆盖的像素全部被重绘为背景，
  残留像素 = 0；旧版本同一测试残留数千像素。）
- **安装位置改成"选文件夹"而不是手打路径**：第 2 屏现在是
  - 预设下拉框：用户目录 (推荐) / 安装程序所在目录 / 用户主目录 / D 盘 / 自选文件夹；
  - `浏览文件夹...` 按钮：调用系统文件夹选择器（优先 `IFileOpenDialog` + `FOS_PICKFOLDERS`，
    失败时退回 `SHBrowseForFolderW`），选完自动填入路径；
  - 只读的路径显示框（不能再手打，避免打错）。
  选"自选文件夹"时直接弹出选择器；取消则保持原选择。

## v2.2.1 — 单文件安装引导 `BinKeySetup.exe`
- 新增 `src/BinKeySetup.cpp`：一个 3.5 MB 的**自解压式单文件安装向导**，
  把 BinKey.exe / config.cfg / README / CHANGELOG / 4 个预设方案 / 调试工具 / 源码 / build.bat
  全部嵌在 exe 尾部（PE + payload + 清单 + `"DSKSETUP"` 签名），运行时读自己并释放文件。
- 5 屏向导：欢迎 → 安装位置 → 安装选项 → 安装结果 → 完成。
  安装后自动跑 `--selftest` 并在结果页显示 `53 PASS / 0 FAIL`。
- 复选框：开机自启（写 `HKCU\...\Run`）、附带调试工具、附带源码、安装后立即启动。
- 自动生成 `uninstall.bat`（先复制到 `%TEMP%` 再执行，避免删自己时删掉自己）。
- 命令行静默模式：`BinKeySetup.exe --silent --dir <路径> [--no-autostart] [--no-tools] [--no-src] [--launch]`。
- 新增 `build-installer.bat`：一键编译 + 打包成 `BinKeySetup.exe`；新增 `src/packer.cpp`。
- 修复：复选框样式用 `BS_AUTOCHECKBOX`（用 `BS_CHECKBOX` 时点击不会切换）。

## v2.2 — 编辑器选中修复
- **规则/SOCD 列表选中失效**（点第 3 条，更新/删除却作用于第 1 条）
  - 根因：LISTBOX 缺少 `LBS_NOTIFY`，Windows 不会向父窗口发送 `LBN_SELCHANGE`，
    所以"选中了哪一行"的消息根本没到达编辑器。
  - 现在两个列表框都是 `LBS_EXTENDEDSEL | LBS_NOTIFY`，每次选择后收敛成一条选中。
- **下拉框的值取不到**：`CBS_DROPDOWNLIST` 没有编辑子控件，`GetWindowTextW` 返回空串，
  导致"更新选中/新增"静默失败。现在用 `CB_GETCURSEL` + `CB_GETLBTEXT` 读取。
- **触发方式 / 条件 下拉框顺序与枚举不一致**：选"按下时"实际存的是 `release`。已对齐。
- 新增状态栏："当前选中：第 N 条规则 / 第 N 个 SOCD 组"。
- 配置文件解析跳过 UTF-8 BOM（记事本/PowerShell 保存时会加 BOM，导致第一行失效）。

## v2.1 — 滚动与托盘
- 所有下拉框/列表框加 `WS_VSCROLL`，选项多也能滚（之前选 P 后滚不到 O）。
- 托盘菜单打开时 `TrackPopupMenu` 阻塞主循环，排队的注入动作不执行；
  给托盘窗口挂 20 ms 定时器，`WM_TIMER` 里 `processDueActions()`。
- `dominant` 模式：主键按着时胜者应是主键，而不是无胜者。

## v2.0 — 独立开关 + 图形编辑器
- SOCD 与 DKS 成为两个独立开关（托盘菜单 + 编辑器复选框 + `toggle_key` 热键）。
- 规则编辑器窗口：增删改规则与 SOCD 组，改动即时生效并自动写回 config.cfg。
- SOCD 逻辑修正：被 snap 压制的键，在另一键松开后恢复为按下状态（双向都成立）。

## v1.0
- 键盘钩子 + SOCD(snap/first/neutral/dominant) + DKS(松开时轻点 / 按住触发 / 连发)。
- 托盘图标、`--selftest` 自检、`--print` 规则打印。
