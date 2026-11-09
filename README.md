# ClickShowCpp

ClickShow 的原生 C++ / Win32 重写版，并新增**鼠标点击计数**功能。

纯 Win32 + GDI+，不依赖 .NET / WPF / Qt；使用 `/MT` 静态 CRT，输出单个 `ClickShow.exe`。

## 特性

- 全局鼠标 5 键（左、中、右、侧键后退、侧键前进）
- 按下波纹 + 长按/移动后抬起的小波纹，每个按键独立颜色和启用开关
- 鼠标位置浮标，大小/颜色/透明度可调
- 点击计数（5 键 + 合计），支持重置；只在 10 分钟定时、退出、重置时落盘
- 托盘图标，左键打开主窗口，右键菜单退出；关闭按钮/最小化隐藏到托盘
- 开机自动启动（HKCU Run）
- 单实例，重复启动会提示
- 截图排除：`WDA_EXCLUDEFROMCAPTURE`，旧系统退回 `WDA_MONITOR`；按 PrintScreen 时额外隐藏 1 秒
- 截图工具铺满屏幕时自动隐藏浮标（可关），不再挡住选区
- 版本检查（WinHTTP，后台线程）与打开主页
- 错误日志
- Per-monitor DPI：覆盖层按目标显示器 DPI 计算物理尺寸
- 与原版 C# 版的配置/计数文件字节级兼容，可互相读取

## 更新记录

### 1.51

版本号 1.51（版本号逐段按数值比较，`1.51` 比 `1.6` 新，后续版本须 ≥ `1.52`）。

新增：

- 「截图时隐藏浮标」（默认开启）：前台出现铺满屏幕的无边框窗口（PixPin、Snipaste、系统截图等）时自动隐藏浮标，截图工具退出后恢复

修复：

- 注销/关机（`WM_ENDSESSION`）时不再丢失设置与计数
- JSON 写失败后的 INI 兜底以前永远读不回来，现在会打 `[fallback] pending=1` 并在下次启动迁回 JSON
- 区分"配置损坏"与"暂时读不到"：后者不再被默认值覆盖
- 开机自启路径加引号，并校验注册表项确实指向当前 exe
- 更新检查线程改为退出时 `join`，不再留下游离线程
- `SetWindowDisplayAffinity` 失败不再静默：自动退回 `WDA_MONITOR` 并写入 `error.log`

优化：

- 主窗口隐藏时不再刷新计数控件
- 浮标改为缓存静态位图，移动只更新分层窗口，不再每帧重绘
- DPI 按显示器缓存，显示变化时失效
- 截图隐藏改为一次性定时器；忽略光标未移动的 Raw Input 包

维护：

- 鼠标按键常量、控件 ID 表、版本号收拢到单一定义（`src/version.h`）
- 配置测试改为带断言并接入 CTest

### 1.5.0

首个 C++ / Win32 重写版：

- 用 Raw Input 取代 C# 版的低级鼠标钩子
- 新增点击计数
- 新增 `WDA_EXCLUDEFROMCAPTURE` 截图排除与单实例
- 配置与计数文件与原版 C# 版字节级兼容

## 快速开始

要求 Windows 10/11 x64、Visual Studio Build Tools（MSVC + Windows SDK）、CMake 3.20+、Ninja。

```bat
build.bat
build\ClickShow.exe
```

不想用 CMake 时用 `build.cmd`，输出 `build_direct\ClickShow.exe`，只依赖 `cl` + `rc` + Windows SDK。

启动后不显示主窗口，只出现在系统托盘：左键打开设置窗口，右键菜单可打开或退出；窗口的 X / 最小化也是隐藏到托盘。设置修改后 100ms 防抖自动保存并应用。

数据文件：

| 文件 | 内容 |
|---|---|
| `我的文档\ClickShow.setting` | 设置（原版 `DataContractJsonSerializer` 格式） |
| `%LOCALAPPDATA%\ClickShow\ClickShow.counts` | 点击计数 |
| `%LOCALAPPDATA%\ClickShow\error.log` | 错误日志（超过 1MB 轮转） |

## 文档

- [概要设计](doc/概要设计.md) — 模块划分、线程模型、输入处理、渲染与 DPI、持久化、版本机制、构建系统、已知限制

## 来源与许可

本项目从 [cuiliang/ClickShow](https://github.com/cuiliang/ClickShow) **fork** 而来：原项目是用 C# / .NET Framework 4.7.2 + WPF 写的鼠标点击提示工具；本仓库把全部功能用 C++ / Win32 重写，并在此基础上新增了点击计数。

原项目版权归原作者所有。本仓库遵循原项目的 **MIT License**，详见 [LICENSE](LICENSE)：其中保留 `Copyright (c) 2021 CuiLiang` 的原始声明，并补充了 C++ 版的版权行。原始图标来自 IconFont 图标库（见原项目说明）。
