# 变更记录

本文件记录 ClickShowCpp 每次功能变更的**排查过程、实测证据与实现取舍**，
供后续维护者复核判断依据。

与 `README.md` 的分工：

| 文件 | 内容 |
|---|---|
| `README.md` | 功能介绍、面向用户的更新记录 |
| `doc/概要设计.md` | 架构、关键实现决策与约束 |
| `change.md` | **每次变更怎么查出来的、凭什么这么改**（本文） |

---

## 1.53 — 忽略远程控制软件接管期间的点击

### 问题现象

使用 KVM / 远程控制软件时，把鼠标移到远程画面里点击，本机 ClickShow
仍然会显示一次点击动画（画在**屏幕正中央**），点击计数也 +1。

### 排查过程

#### 第一步：确认 ClickShow 侧的代码路径

`src/input.cpp` 的 `HandleInput()` 在收到鼠标报文后，位置完全依赖
`GetCursorPos()`：

```cpp
POINT point{};
if (!GetCursorPos(&point)) { return 0; }
```

Raw Input 的鼠标报文里**没有绝对坐标**，只有相对位移 `lLastX/lLastY`。
点击事件没有位移信息，`GetCursorPos()` 给出的就是上次移动后光标停住的
地方。代码里没有任何"这个点击发生在屏幕外吗"的判断，所以只要报文到达，
`src/main.cpp` 的 `g_counts.Add(index)` 就无条件执行。

同时注意到 `src/input.cpp` 已有的合成输入过滤用的是
`input.header.hDevice == nullptr`，但 **KVM 软件通常是虚拟 HID 设备（有
自己的 hDevice），不是 0**，所以现有的合成过滤也拦不住它。

#### 第二步：写探针收集证据

新建 `build/kvm_mouse_probe.py`，注册方式与 ClickShow 完全一致
（mouse usage + `RIDEV_INPUTSINK`），记录每条报文的
`hDevice` / 设备名 / `ulExtraInformation` / `usButtonFlags` /
`lLastX/lLastY` / `GetCursorPos()` / 虚拟屏幕矩形 /
`MonitorFromPoint` / `WindowFromPoint` / `GetCursorInfo()` /
`GetClipCursor()` / `GetForegroundWindow()` / `GetCapture()`，
并以 10ms 采样光标位置，记录它进出虚拟屏幕矩形的状态迁移。

**探针自身的 bug（第一版）**：记的是 `usFlags`（**移动**标志：
`MOUSE_MOVE_ABSOLUTE` 等），不是 `usButtonFlags`（**按键**标志）。
RAWMOUSE 的 `usButtonFlags` 在 offset 4（用 C++ `offsetof` 核对过），
第一版日志因此**一条按键事件都没有**。修正后重跑才拿到关键数据。

#### 第三步：真实 KVM 会话实测

抓到的关键数据（`build/kvm2.log`）：

**光标接管过程**

```
2653ms  KVM 发绝对定位报文（MOUSE_MOVE_ABSOLUTE）
        delta=(32768,32768) → 光标被钉到 (960,540) 屏幕正中，同时隐藏
2653~5890ms  物理鼠标报文照常到达（327 条移动 + 14 条按键），
              但光标纹丝不动（344/345 条报文的 cur 都是 (960,540)），
              位移被转发给远端
5895ms  KVM 恢复光标
```

**本地点击 vs 远程点击的对比**

| 观察点 | 本地点击 | 远程点击 |
|---|---|---|
| `header.hDevice` | `0x1003b`（物理设备） | `0x1003b`（**同样是物理设备**） |
| `ulExtraInformation` | `0` | `0`（**完全相同**） |
| 坐标 | 实际点击处 | 恒为 `(960,540)` |
| `GetCursorInfo()` | `CURSOR_SHOWING` | `CURSOR_HIDDEN` |
| `WH_MOUSE_LL` 钩子 | 能看到完整 down/up | **整个会话期间一条按键都没有** |

**KVM 的报文标记**（只出现在**移动**报文上）

| `ulExtraInformation` | ASCII | 次数 | 用途 |
|---|---|---|---|
| `0x4F494E4B` | `KNIO` | 15 | 空闲心跳（零位移） |
| `0x4F494E53` | `SNIO` | 2 | 绝对定位 |
| `0x00B04E57` / `0x00B14E57` | — | 各 1 | 停泊到中心 / 恢复 |
| `0x012FA8CC` | — | 6 | WGestures（1.52 已处理的那个） |

#### 第四步：确定真因

**Raw Input 与 `WH_MOUSE_LL` 是两条独立通路**。低级钩子吞掉点击后，本地
窗口收不到（所以远端能收到、本地没有反应），但 Raw Input 依然会投递
该报文 —— ClickShow 于是照单全收。

**"显示在屏幕中央"** 也解释清楚了：KVM 把光标钉在 (960,540)，
`GetCursorPos()` 返回的就是这个被停泊的位置。

#### 第五步：排除的方案

| 方案 | 为什么不行 |
|---|---|
| 按 `hDevice == 0` 过滤 | 远程点击是**物理设备** `0x1003b`，过滤不掉 |
| 按 `ulExtraInformation` 过滤 | 远程点击 `extra = 0`，与本地点击完全相同；KVM 的标记只出现在移动报文上 |
| 按坐标是否在屏幕内 | `(960,540)` 在屏幕正中，完全合法 |
| 装 `WH_MOUSE_LL` 钩子 | 钩子能看到"被吞掉"这件事，但与项目"不用低级钩子"的设计相悖，且钩子链顺序不可控 |

唯一可行的是**行为特征**：光标不再跟随鼠标。

#### 第六步：设计判据

```
系统光标冻结不动 + 原始报文位移持续累积  →  鼠标被外部程序接管
```

实测数据支撑：远程会话时 150ms 内累积到 `(715,109)`；本地正常操作时
光标始终跟随。

**假阳性与排除**

| 假阳性场景 | 排除方式 |
|---|---|
| 光标推到屏幕边缘，光标也不动 | `IsPushingOutwards()`：位移方向朝屏幕外时不计入累积 |
| 绝对定位报文被误当位移 | `usFlags & MOUSE_MOVE_ABSOLUTE` 时 `lLastX/lLastY` 是归一化坐标，不计入累积 |
| 光标短暂滞后于报文投递 | 阈值要求**持续** 150ms 且位移累积 ≥40 |

### 实现

`src/input.cpp` 新增两个常量与两个成员函数：

```cpp
constexpr ULONGLONG kParkedFreezeMs = 150;   // 光标冻结时长
constexpr LONG      kParkedMinTravel = 40;   // 累积位移阈值
```

- `RawInput::IsPushingOutwards()` —— 判断位移是否朝虚拟屏幕外
- `RawInput::TrackRemoteControl()` —— 维护「上次光标位置 + 累积位移 +
  冻结起点」，每次鼠标报文到达时更新，返回当前是否处于被接管状态

`HandleInput()` 里在 `deliver` 之前取得判定结果：

```cpp
const bool remoteControl =
    TrackRemoteControl(point, mouse.lLastX, mouse.lLastY, absolute);
```

被接管时**按下不回调**，但**抬起照常回调**。理由：`src/main.cpp` 靠
`if (!state.down) return;`（`main.cpp:310`）维护每按钮的按下状态，
抬起回调是唯一能清理该状态的途径。若按下被抑制而抬起不回调，
`state.down` 会残留 true，下一次抬起会误触发一次波纹。反过来
（按下被抑制、抬起回调）时 `state.down` 本就是 false，
`main.cpp` 自己的守卫会忽略这个孤立的抬起 —— 两侧状态保持平衡。

`RawInput::Stop()` 里重置全部接管状态。

### 验证

| 验证方式 | 工具 | 结果 |
|---|---|---|
| 真实 KVM 日志离线回放 | `build/replay_parked.py` | 8 次本地点击**全计数**，7 次远程点击**全抑制** |
| 端到端（低级钩子复现接管） | `build/verify_kvm_e2e.py` | 3 场景 × 5 轮稳定 PASS |
| 边界合成场景 | `build/replay_edge_cases.py` | 7 种场景零误判，反例 160ms 触发 |

**离线回放结果**

```
RAW 报文总数 : 766
计数的按下   : 8      <- 本地点击全部保留
被抑制的按下 : 7      <- 远程点击全部拦下

  2802.5ms  TAKEOVER  travel=831 frozen=153ms at (960,540)
  3306.7ms  SUPPRESS  0001 at (960,540)
  ...
  5895.9ms  RELEASE
```

判定在 2802.5ms 成立，比第一次远程点击（3306.7ms）早 504ms，无漏网。

**端到端验证的三个场景**

| 场景 | 期望 | 结果 |
|---|---|---|
| 本地点击 | 计数（`remote=0`） | PASS |
| KVM 停泊期间点击 | 抑制（`remote=1`） | PASS |
| 恢复后点击 | 计数（`remote=0`） | PASS |

验证方式：用 `WH_MOUSE_LL` 钩子吞掉鼠标移动 —— 这正是真实 KVM 的做法
（钩子吞移动 → 光标停住，Raw Input 仍收到带位移的报文）。

**边界场景（零误判）**

- 左上角持续朝外推 5 秒
- 右下角持续朝外推 5 秒
- 左边缘向右下斜推（光标会移动）
- 中间快速抖动（光标跟动）
- 完全不动鼠标
- 底边贴边，dy 朝外
- 顶边（x=1000）dy 朝外

反例（光标冻在中心、位移持续）应在 160ms 触发 ✓

### 踩过的坑

1. **探针读错字段** —— `usFlags`（移动标志）vs `usButtonFlags`（按键标志），
   两者在 RAWMOUSE 里偏移不同（0 vs 4）。第一版日志因此没有任何按键数据，
   一度以为是环境问题。用 C++ `offsetof` 核对后修正。
2. **`SendInput` 相对移动会移动光标** —— 一度以为不会（某次测试中光标
   恰好没动），导致模拟器反复失败、误判为产品代码有 bug。实测确认
   `SendInput(MOUSEEVENTF_MOVE, dx, dy)` 会正常移动光标。
3. **`RI_MOUSE_MOVE_ABSOLUTE` 这个常量不存在** —— Raw Input 的移动标志
   用的是 `MOUSE_MOVE_ABSOLUTE`（winuser.h:15092，值 1）。名字相似但
   不是同一个头文件里的定义。
4. **`GetRawInputDeviceList` 在本机返回 `ERROR_INVALID_PARAMETER`** ——
   C++ 和 Python 都一样，是环境限制。设备名取不到，但 `hDevice` 本身
   够用，不影响排查。
5. **PowerShell 控制台按 GBK 显示中文会抛 `UnicodeEncodeError`** ——
   `error.log` 内容是**正确的 UTF-8**（`E6 A3 80 E6 B5 8B ...`），
   乱码只是控制台显示问题。排查时一度误以为编码有问题。

### 已知限制

- 阈值（150ms / 40）是按本机实测定的固定宏值，不做自适应。
- 若某款 KVM 软件**不**停泊光标（让本地光标照常跟随），本判据不生效。
- 若某款软件把光标停在**屏幕边缘**且位移方向朝内，累积仍会发生
  （`IsPushingOutwards` 只排除朝外），可能误判。
- 判定有 150ms 的延迟：进入远程会话后最初 150ms 内的点击仍会被计数。
- 物理鼠标仍在使用（不是 KVM 接管）时，如果光标被某个程序卡住不动
  （例如被 `ClipCursor` 限制），也可能触发。

### 相关文件

| 文件 | 说明 |
|---|---|
| `src/input.cpp` | `TrackRemoteControl()` / `IsPushingOutwards()` |
| `src/input.h` | 接管状态成员与方法声明 |
| `build/kvm_mouse_probe.py` | Raw Input 探针（本次新增，未纳入 git） |
| `build/replay_parked.py` | 真实日志离线回放验证 |
| `build/replay_edge_cases.py` | 边界场景合成测试 |
| `build/verify_kvm_e2e.py` | 端到端验证（低级钩子复现接管） |
| `build/kvm2.log` | 真实 KVM 会话原始日志 |

### 验证状态

- `CMake` + MSVC 19.51 + Ninja 构建通过，`/W4` 无告警
- `build.cmd`（cl + rc 直编）构建通过
- `ctest` 全绿（配置兼容测试 1/1）
- 两个 exe 的 `FileVersion` 均为 `1.53.0.0`
- 离线回放、端到端、边界场景三项验证全部通过
- **仍需在真实 KVM 软件下人工验收**（模拟器复现的是"钩子吞移动 +
  光标停泊"，不同 KVM 软件行为可能不同）
