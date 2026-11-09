# ClickShowCpp

ClickShow 的原生 C++ / Win32 重写版，并新增**鼠标点击计数**功能。

纯 Win32 + GDI+，不依赖 .NET / WPF / Qt；使用 `/MT` 静态 CRT，输出单个 `ClickShow.exe`。

## 来源与许可

本项目从 [cuiliang/ClickShow](https://github.com/cuiliang/ClickShow) **fork** 而来：

- 原项目是用 C# / .NET Framework 4.7.2 + WPF 写的鼠标点击提示工具；
- 本仓库把全部功能用 C++ / Win32 重写，并在此基础上**新增了鼠标点击计数功能**（5 键分别计数、合计、重置、持久化）；
- 配置和计数文件与原版保持字节级兼容，两个版本可以互相读取配置。

原项目版权归原作者所有。本仓库遵循原项目的 **MIT License**，详见 [LICENSE](LICENSE)：其中保留 `Copyright (c) 2021 CuiLiang` 的原始声明，并补充了 C++ 版的版权行。原始图标来自 IconFont 图标库（见原项目说明）。

## 本仓库新增/改动

相对原版 C# 实现的主要变化：

| 项目 | 说明 |
|---|---|
| 语言与运行时 | C# + WPF → C++ 20 + Win32 + GDI+，单个 exe，无 .NET 依赖 |
| 输入 | `WH_MOUSE_LL` → Raw Input（`RIDEV_INPUTSINK`），不再受低级钩子超时限制 |
| 点击计数 | **新增**：左/中/右/侧键1/侧键2 + 合计，支持重置，10 分钟定时与退出落盘 |
| 截图排除 | 新增 `WDA_EXCLUDEFROMCAPTURE`，截图工具里不再出现波纹和浮标 |
| 单实例 | 新增命名 Mutex |
| 配置 | 沿用原版 `我的文档\ClickShow.setting`（JSON），读写格式字节级一致 |

## 功能

- 全局鼠标 5 键（左、中、右、侧键后退、侧键前进）
- 按下波纹 + 长按/移动后抬起的小波纹，每个按键独立颜色和启用开关
- 鼠标位置浮标，大小/颜色/透明度可调
- 点击计数（5 键 + 合计），支持重置；只在 10 分钟定时、退出、重置时落盘
- 托盘图标、左键打开主窗口、右键菜单退出；关闭按钮/最小化隐藏到托盘
- 开机自动启动（HKCU Run）
- 单实例，重复启动会提示
- 截图排除：优先 `WDA_EXCLUDEFROMCAPTURE`；旧系统回退到 PrintScreen 时隐藏 1 秒
- 版本检查（WinHTTP，后台线程）和打开主页
- 反馈与更新链接指向 <https://github.com/loginux/ClickShowCpp>
- 错误日志
- Per-monitor DPI（覆盖层按目标显示器 DPI 计算物理尺寸）

## 实现概要

### 模块划分

| 模块 | 职责 |
|---|---|
| `main.cpp` | 主对话框、托盘、定时器、生命周期、开机自启、更新检查 |
| `input.cpp` | Raw Input（鼠标 5 键 + 键盘截图键），解析后回调给上层 |
| `overlay.cpp` | 波纹/浮标两个分层窗口、GDI+ 绘制、16ms 合帧定时器 |
| `settings.cpp` / `counts.cpp` | 与原版兼容的 JSON 读写、旧 INI 迁移与兜底 |
| `json.cpp` | 最小 JSON 解析/转义，带嵌套深度上限 |
| `util.cpp` | 路径、原子写、日志、DPI、ARGB、版本比较 |

### 线程模型

- **UI 线程**是唯一拥有窗口的线程：Raw Input 的 message-only 窗口、`WM_TIMER`、`WM_COMMAND` 都在这里分发，所以设置、计数、覆盖层状态全部单线程访问，**不需要加锁**。
- **更新检查线程**启动后等 10 秒（可被取消事件提前唤醒）再发 WinHTTP 请求。结果放进互斥量保护的槽，`PostMessage` 只当信号用、不携带指针；退出时先 `SetEvent` 再 `join`，不会留游离线程在静态 CRT 拆卸期运行。
- 覆盖层动画由挂在主窗口上的 **16ms 定时器**驱动：有待处理内容才启动，空闲即停；鼠标移动期间保持存活，避免每个输入包都创建/销毁定时器。

### 配置加载与持久化

加载结果分三种，只有一种会覆盖文件：

| 结果 | 含义 | 行为 |
|---|---|---|
| `Ok` | 读到并解析成功 | 使用文件内容 |
| `Unavailable` | 打不开/被占用/权限不足 | 内存里用默认值，**禁止任何写回**（含退出保存） |
| `Corrupt` | 读到但不是合法 JSON | 用默认值、弹窗提示、立即保存 |

兜底链路：JSON 写入失败 → 写 `*.ini` 并置 `[fallback] pending=1` → 下次启动优先读回 INI、迁回 JSON 后清除标记。否则旧 JSON 还在，兜底数据永远读不回来。

### 渲染与 DPI

- 覆盖层窗口 = `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST`，内容画在 32bpp DIB 上，用 `UpdateLayeredWindow` 呈现。
- 波纹每帧重绘；浮标是静态图，只在尺寸/颜色/透明度变化时重绘，移动只更新分层窗口。
- DPI 按光标所在显示器取，物理尺寸 = `MulDiv(逻辑尺寸, dpi, 96)`；结果按显示器缓存，`WM_DISPLAYCHANGE` / `WM_DPICHANGED` 时失效。

### 版本

`src/version.h` 是唯一真源，CMake 与 `build.cmd` 解析它生成 `app.rc` 和 `app.manifest`。这两处都要求 4 段版本号，脚本会自动补零（`1.51` → `1.51.0.0`）。

## 构建

### 要求

- Windows 10 / 11 x64
- Visual Studio Build Tools（MSVC + Windows SDK）
- CMake 3.20+
- Ninja（`build.bat` 默认使用）

### 一键构建

在仓库根目录双击或运行：

```bat
build.bat
```

脚本会：

1. 如果 PATH 里没有 `cl`，通过 `vswhere` 找到 VS BuildTools 并调用 `vcvars64.bat`；
2. 用 CMake + Ninja 配置 `build/`（缓存里的路径失效时自动清空重配）；
3. 编译输出 `build\ClickShow.exe`。

### 直接编译（不经过 CMake）

如果只想用 MSVC 直接出 exe：

```bat
build.cmd
```

输出 `build_direct\ClickShow.exe`。只依赖 Visual Studio Build Tools（`cl` + `rc` + Windows SDK），不需要 CMake/Ninja。脚本会自动查找 `vcvars64.bat`，并从 `res\app.rc.in` / `res\app.manifest` 生成资源脚本和清单（版本号取自 `src\version.h`）。

### 手动构建

在 Visual Studio Developer Command Prompt 中：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

如果你更想用 Visual Studio 生成器：

```bat
cmake -S . -B build-vs -G "Visual Studio 18 2026" -A x64
cmake --build build-vs --config Release
```

输出在 `build-vs\Release\ClickShow.exe`。

> 备注：CMake 4.4.0-rc3 的 `NMake Makefiles` 生成器在当前机器上会生成 `$(MAKE) ... -$(MAKEFLAGS)`；`MAKEFLAGS` 为空时 `nmake` 会报“无效选项 -”。所以脚本默认用 Ninja，而不是 NMake。

## 运行

```bat
build\ClickShow.exe
```

- 启动后不显示主窗口，只出现在系统托盘。
- 左键点击托盘图标打开设置窗口；右键菜单可打开或退出。
- 窗口的 X / 最小化会隐藏到托盘；“退出”按钮或托盘菜单退出程序。
- 设置修改后 100ms 防抖自动保存并应用（先保存、再应用，和原版一致）。

### 数据文件（与原版 C# 完全一致）

| 文件 | 内容 | 说明 |
|---|---|---|
| `我的文档\ClickShow.setting` | 设置 | 原版 `DataContractJsonSerializer` 格式；C++ 写出的内容与原版字节级一致 |
| `%LOCALAPPDATA%\ClickShow\ClickShow.counts` | 点击计数 | 原版 JSON |
| `%LOCALAPPDATA%\ClickShow\error.log` | 错误日志 | 超过 1MB 轮转 |

- 按钮颜色和 `HoverDotFill` 都按 `#AARRGGBB` 读写，alpha 会保留。
- `settings.ini` / `counts.ini` 只是第一版 C++ 的旧路径：原版 JSON 不存在时读取一次做迁移；JSON 写入失败时作为兜底写入，并打上 `[fallback] pending=1` 标记，下次启动优先读回 INI、迁回 JSON 后清除标记（不会因旧 JSON 还在而丢失修改）。正常保存都写原版 JSON。
- 配置文件"读得到但解析失败"和"根本读不到"是分开处理的：只有真正损坏才恢复默认值并立即保存；瞬时读取失败（文件被占用等）只在内存里用默认值，且不会写回覆盖原文件。
- C# 版和 C++ 版可以互相读取配置，但**不要同时运行两个版本**：两边各自在内存里维护计数和设置，后保存的会覆盖先保存的。

## 配置写入时机（与原版对齐）

磁盘写入频率很低：设置只在 100ms 防抖后写一次，计数只在 10 分钟定时/退出/重置时写。

| 触发 | 原版 C# | C++ 版 |
|---|---|---|
| 任意设置属性变化 | 100ms 防抖（ApplicationIdle）→ `SaveSettings()` → `ApplySettings()` | 100ms `WM_TIMER` → 保存设置 JSON → 应用覆盖层 |
| 更新提示 | 先写入原始版本字符串 `LastNotifiedVersion` 并保存，再弹窗 | 相同；保存 `raw`（含 `|url`），弹窗前写入 |
| 启动加载 | 文件有效不写；损坏时弹窗 + 默认值 + 立即保存 | 相同；只有真正损坏才弹窗并保存，读不到时用默认值但不写回 |
| 关闭窗口/最小化 | 只隐藏，不保存 | 相同 |
| 退出（托盘/按钮） | `OnClosed`：先保存设置，再停钩子，最后保存计数 | `WM_DESTROY`：同样的顺序 |
| 注销/关机 | 无 | `WM_ENDSESSION`：立即保存设置与计数（系统可能不再发 `WM_DESTROY`） |
| 点击计数 | 只在内存 `++`，不落盘 | 相同 |
| 计数定时落盘 | 10 分钟 DispatcherTimer | 10 分钟 `WM_TIMER` |
| 重置计数 | 确认后 Reset + Save | 相同 |
| 开机自启 | 只写 HKCU Run，不写设置文件 | 相同 |

有意的差异（不影响兼容性）：

- C++ 用带 PID 的临时文件 + 同名替换的原子写入，避免半截 JSON，也避免两个实例撞同一个临时文件；不调用 `FlushFileBuffers`、不使用 `MOVEFILE_WRITE_THROUGH`，配置文件很小，按普通缓存写即可，不会每次保存都强制物理落盘。时机不变。
- C++ 在启动输入前加载计数，避免原版启动瞬间丢一次点击。
- Win32 `ChooseColor` 只能改 RGB，不能编辑 alpha；C++ 会保留原文件里的 per-button/hover alpha（原版 ColorPicker 可编辑 alpha）。
- JSON 写入失败时回退到旧 INI 兜底保存（含 `#AARRGGBB` 颜色与独立 alpha 键），原版只提示保存失败。
- 版本号只在 `src/version.h` 定义；CMake 和 `build.cmd` 都从这里生成版本资源与 manifest。

写入频率实测（本机）：

| 场景 | 写入次数 |
|---|---|
| 模拟 2 秒内 40 次滑块变化 | 1 次（停手后） |
| 20 次快速复选框连点 | 1 次（停手后） |
| 正常点击鼠标 | 0 次（仅在内存累加） |

## 与 C# 版的实现差异

| 方面 | C# 版 | C++ 版 |
|---|---|---|
| 输入 | `WH_MOUSE_LL` 低级钩子，跑在 UI 线程 | Raw Input（`RIDEV_INPUTSINK`），无钩子超时问题 |
| 截图 | PrintScreen 后隐藏 1 秒 | `WDA_EXCLUDEFROMCAPTURE` + PrintScreen 回退 |
| 设置 | JSON（`MyDocuments`） | 相同 JSON（`MyDocuments`），字节级兼容 |
| 计数 | JSON（`ClickShow.counts`） | 相同 JSON |
| 渲染 | WPF 透明窗口 | Win32 layered window + GDI+ |
| 运行时 | .NET Framework 4.7.2 + WPF | 静态 CRT，单个 exe |
| 单实例 | 无 | 命名 Mutex |

## 已知限制

- 目标为 Windows 10/11 x64。`WDA_EXCLUDEFROMCAPTURE` 需要 Windows 10 2004+；更老的系统只走 PrintScreen 隐藏回退。
- 设置窗口没有做完整的 per-monitor 重排（移动到不同 DPI 显示器时不会按新 DPI 重新缩放控件）；覆盖层（波纹/浮标）是按 DPI 计算的。
- `ReleaseElevated`/UIAccess 不在此版本中；默认以普通用户运行。
- 版本号按点分段做**数值**比较：`1.51` 的次版本号是 51，因此它比 `1.6` 新。升到 1.51 之后的版本号必须 ≥ `1.52`，否则更新检查会认为比当前版本旧。
- 未做代码签名，SmartScreen 可能提示。
- 更新检查只打开浏览器，不自动下载安装。
- Win32 颜色对话框不编辑 alpha；per-button/hover 的 alpha 会保留原值。
- 不要同时运行 C# 版和 C++ 版，否则设置/计数会互相覆盖。

## 目录

```text
ClickShowCpp/
  CMakeLists.txt
  build.bat     CMake + Ninja 增量构建
  build.cmd     仅用 MSVC 直接编译（不经过 CMake）
  src/
    version.h      版本号唯一来源（CMake / build.cmd 从这里生成资源）
    main.cpp       窗口、托盘、设置、计数、更新、生命周期
    input.cpp      Raw Input
    overlay.cpp    波纹/浮标窗口与 GDI+ 渲染
    json.cpp       轻量 JSON 解析/转义
    settings.cpp   原版 ClickShow.setting JSON（+ 旧 INI 迁移）
    counts.cpp     原版 ClickShow.counts JSON（+ 旧 INI 迁移）
    util.cpp       路径、文件读写、日志、DPI、ARGB、版本比较
  tests/
    config_test.cpp  配置兼容测试（带断言，可跑 CTest；读原版文件、写临时 JSON）
  res/
    app.rc.in      对话框、图标、版本资源、manifest 引用
    app.manifest   PerMonitorV2 + comctl32 v6
    resource.h     控件 ID
    clickshow.ico  图标
```

## 验证状态

- `CMake` + MSVC 19.51 + Ninja 构建通过，输出 `build\ClickShow.exe`。
- `build.cmd` 仅用 `cl` + `rc` 直接编译通过，输出 `build_direct\ClickShow.exe`。
- 3 秒启动冒烟测试通过（首版提交时验证；后续改动需在真机重跑）。
- 配置兼容测试通过：读取原版真实配置可无损往返，同一份设置写出的内容字节稳定（与原版逐字节一致由首版提交时人工比对）。
- 写盘频率测试通过：滑块拖动、复选框连点均只写 1 次；点击鼠标 0 次写盘。
- 视觉与交互（波纹、浮标、托盘、计数、截图、多屏 DPI）需要在实际桌面上手动验收。

构建并运行配置测试：

```bat
build.bat -DCLICKSHOW_BUILD_TESTS=ON
ctest --test-dir build --output-on-failure
```

测试自带断言（设置/计数往返、原版 JSON 格式、alpha 保留、INI 兜底恢复、JSON 嵌套深度与代理对），失败会返回非零退出码。除打印外，它只读原版 `ClickShow.setting` / `ClickShow.counts`，临时文件写在 `build_direct\config_test\`，不会改动原配置。
