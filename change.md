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

## 移除联网检查（2.00，按用户要求）

用户明确要求不需要联网。核查发现唯一的网络入口是 `FetchUpdate()` 的 WinHTTP 请求，
由启动时的后台线程延迟 10 秒触发；托盘菜单「启动时检查更新」只是该线程的开关。

处理：

- 删除 `WinHTTP` 请求、后台线程、结果消息、托盘开关与 `enableUpdateCheck` 设置项；
  构建脚本同步去掉 `winhttp.lib`。
- 主窗口「反馈更新」改为「项目主页」，只在用户点击时打开浏览器，程序自身不联网。
- `lastNotifiedVersion` 字段保留，兼容旧配置文件与 C# 版；不再参与任何逻辑。

图标：此前把 ICO 四帧改为无损 PNG，体积从 90,022 B 降到 5,582 B，但用户实测显示异常。
已恢复 `build/clickshow_original.ico`；体积收益不足以抵显示正确性，此优化撤销。

验收：`build/` 与 `build_direct/` 两套 EXE 的导入表均不含 `WINHTTP.dll` 或任何 `WinHttp*` API。

## 代码审查发现（1.60 → 1.61 修复）

对 `src/`、`res/`、`tests/` 与构建脚本做了一次全面审查。下列为**交叉验证确认**的问题，
按严重级别排序；每条附触发条件与依据。带「取舍」标记的是设计决策，本次不改。
用户确认：修复第 1~3 条（高/功能性缺陷），其余取舍项与低优先级项保留记录、本次不动。

**高 — 数据丢失风险（✅ 已在 1.61 修复）**

1. `KeyCountStore::Load()`（`counts.cpp:228-231`）：文件存在但读失败时直接 `return`，
   不置 `loadFailed_`。对比 `CountStore::Load()`（`counts.cpp:64-79`）会检查 `exists()`
   并在读失败时置位。后果：文件被占用/权限变更/超大时，10 分钟定时保存会用全 0
   覆盖原文件，历史键盘计数永久丢失。
2. `CountStore::Save()`（`counts.cpp:89-90`）与 `SettingsStore::Save()`（`settings.cpp:135-136`）：
   INI 兜底置 `pending=1` 后，若后续 `SaveJson()` 成功，未清除该标记。下次启动
   `Load()` 命中 pending，读回旧 INI 快照覆盖较新的 JSON，丢失两次保存之间的计数/设置。

**中 — 功能性缺陷（✅ 已在 1.61 修复第 3 条）**

3. `IDC_BTN_DEFAULT`（`main.cpp:1146-1161`）：重置"默认值"时漏掉
   `enableClickCircle`、`enableHoverDot`、`enableClickCount` 三个开关，与
   `Settings::Defaults()` 不一致；同时却重置了相邻的 `enableKeyboardCount`，行为矛盾。
4. `Parser::ParseNumber()`（`json.cpp:252-283`）：只判断 `position_ != start` 即接受为
   数字，未校验负号/小数点/指数后至少一位数字。`-`、`1.`、`1e` 会被当作合法数字，
   使损坏配置绕过"解析错误→默认值+弹窗"的恢复路径，被静默改写。
5. `settings.cpp:183,186`：`std::lround(value->AsDouble(...))` 未限制输入范围，
   配置写入极大数时 `lround` 结果超出 `long` 范围为未定义行为（Windows 上 `long` 32 位）。
6. 【取舍】合成回放过滤（`input.cpp:330-343`）对全部 5 键生效，而 1.52 的问题只针对
   右键 WGestures 回放。合法注入的 ≤20Hz 连点可能被误过滤。是否收窄到右键需定口径。
7. 【取舍】`keyDown_`（`input.cpp:404-411`）无运行期恢复：锁屏/UAC/热插拔导致丢失
   `WM_KEYUP` 后，该键会卡住并漏掉下一次按下。需要决定在 `WM_KILLFOCUS`/`WM_ACTIVATEAPP`
   时清空，还是维持现状。
8. 【取舍】`KeyCountStore` 无 INI 兜底（与 `CountStore` 及 §6.2 描述不一致）：写失败
   即丢本次会话键盘计数。是否补齐兜底链路需定口径。

**低 — 资源/边界**

9. 主窗口大/小图标（`main.cpp:1081-1092`）：`LoadImageW` 未用 `LR_SHARED`，且从未
   `DestroyIcon`，每进程泄漏 2 个 USER 句柄（有界，退出回收）。
10. 退出时 `StopUpdateCheck()`（`main.cpp:452-463`）：`SetEvent` 无法中断已在进行中的
    WinHTTP 请求，四个独立 3 秒超时最坏累计约 12 秒，与"不卡退出"的意图不符。
11. INI 计数解析（`counts.cpp:163`）：`_wcstoui64` 对畸形数值无校验，可写入 `ULLONG_MAX`。

**已排除（误报）**：`Format()`（`util.cpp:160-169`）在 `_TRUNCATE` 下截断返回 `-1`，
随后 `written < 0` 提前返回，不会用超长长度构造 `wstring`，无越界读。

---

## 2.02 — 修复「设置或计数保存失败」

### 现象

程序运行一段时间后弹出「设置或计数保存失败，本次改动可能不会保留。」，确认只有一个
进程在跑。`%LOCALAPPDATA%\ClickShow\` 下只有 `error.log` 和两个计数文件，**没有
`ClickShow.setting`**。

### 日志证据

`error.log` 中每隔一段时间成组出现（10:41 之后的运行）：

```
[11:45:47.221] WriteAtomic.Dir
拒绝访问。 | C:\Users\admin\AppData\Local\ClickShow\ClickShow.counts
[11:45:47.222] Counts.SaveJson
写入 ... 失败，回退到 counts.ini
[11:45:47.222] Counts.SaveIni.Dir
拒绝访问。 | C:\Users\admin\AppData\Local\ClickShow\counts.ini
```

设置、键盘计数同理——**所有**写入都倒在同一个函数 `WriteAtomic.Dir` 上。

### 根因

`EnsureParentDirectoryExists`（2.01 为替代 C++17 filesystem 新写的）逐级遍历路径、
对每个前缀直接调 `CreateDirectoryW`，只把 `ERROR_ALREADY_EXISTS` 当作「已存在」放过。

实测各级目录的返回码：

```
'C:\'                                             ok=0 err=5    (ERROR_ACCESS_DENIED)
'C:\Users\'                                       ok=0 err=183  (ERROR_ALREADY_EXISTS)
'C:\Users\admin\AppData\Local\ClickShow\'         ok=0 err=183
```

**驱动器根 `C:\` 返回的是 5 而不是 183**，于是函数在第一个前缀就 return false，
整个保存链路（JSON → INI 回退）全部失败。10:00 时还能写，是因为那时跑的还是用
`std::filesystem` 的旧版本；换新实现后 10:41 起所有保存都没成功过。

### 修复

创建前先用 `GetFileAttributesW` 探测：已是目录就跳过；是文件则返回失败；
只有明确 `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` 才真正 `CreateDirectoryW`。
「存在但读不到（拒绝访问）」按存在处理，让后续真正的写操作报错，与 `PathExists`
的语义保持一致。

### 验证

- 新增回归测试 `TestEnsureParentDirectory`：用当前盘符的绝对路径
  (`<cwd>\build_direct\config_test\parent_probe\nested\probe.json`) 准备目录，
  正好覆盖「驱动器根」这个前缀；`ctest` 1/1 通过。
- 实机：修复前 `ClickShow.setting` 不存在；修复后切换一次开关，文件生成（531 B），
  `error.log` 无任何新增写入错误，开关状态回到原值。

---

## 2.01 — 功能键行与主键区对齐

### 现象

计数弹窗里功能键行的 F12 右边缘与下一行 Backspace 的右边缘差出小半个键，看起来像两排
没对齐。

### 根因（两处叠加）

1. 功能键行总共 13 个键（Esc + F1..F12）+ 三个 0.5u 间隙 = **14.5 键位**，
   而主键区是 **15 键位**，天然少 0.5u。
2. 隐藏更深的一处：主键区绘制是 `int x; x += (int)(unitWidth * w)`——每个键都先截断
   成整数再加，58.72px 的键各丢 0.72px，**13 个键累积少约 9px**。所以 Backspace 的
   实际右端比理论值（15u）还靠左，单把功能键行排满 15u 反而会超过它。

### 修复

- 功能键行：Esc + F1..F12 按 15 键位排布，余下 2 键位**均分给三个组间隙**
  （Esc 后、F4 后、F8 后各 2/3u），既让 F12 落到 15u，也顺带把用户嫌小的组间距加宽。
- 主键区、导航簇、小键盘、系统键的 x 累加全部改为 `double`，消除逐键截断的累积误差。

### 验证

- 150% DPI、弹窗客户区 1400px 宽、unit=58.72px 下，功能键行与数字行在 x=886
  同为灰色边框像素，x≥887 同为背景色——两行右端完全重合。
- 截图核对：Esc/F4/F8 后间隙均匀放大，功能键三组分明，无重叠、无裁切。

---

## 2.00 — 版本号跨大版本

用户要求把版本号升到 2.00。功能与 1.74 完全一致，没有代码行为变化。

### 为什么 2.00 不会被判成「比 1.74 旧」

`CompareVersions`（`src/util.cpp:236`）是把版本串按 `.` 分段、每段按数值比较，
**第一段权重最高**：

- `1.74` vs `2.00` → 第一段 `1` vs `2`，`2 > 1`，直接返回「2.00 更新」。
- 之前「`1.51` 比 `1.6` 新」的坑只发生在**同一大版本内**（次版本号 51 > 6），
  与大版本跨越无关。

所以老用户从 1.74 升级到 2.00 时更新检查正常。

注意后续升版规则：
- 同大版本内仍按「次版本号递增」判断，2.00 之后要写 `2.01`、`2.02`……
  （次版本号 1 > 0），不能停在 `2.00`。
- 不要把 `2.10` 简写成 `2.1`：两者数值相等，更新检查会认为没有新版本。

### 版本资源

`src/version.h` 改为 `L"2.00"`；`gen_direct_rc.py` 与 CMake 都把它补成四段
`2.00.0.0` / `2,00,0,0`，两套 exe 的 `FileVersion` 实测均为 `2.00.0.0`。

---

## 1.63 ~ 1.73 — 计数弹窗瘦身与主窗体三分组重排

这一段是连续多轮 UI 迭代，过程曲折，关键决策记录如下。

### 1.63 ~ 1.66：弹窗缩放与键盘热力图重排

- 2K + 150% 缩放下原弹窗控件互相重叠：`IDD_COUNT_INFO` 由 1040×212 逐步收到
  800×190 DLU，绘制面板按区域等比缩放（`unitWidth = 宽/20.5`、`keyHeight = 高/7`），
  字号随格子高度 `clamp(keyHeight/2, 10, 13)` 自适应，避免 7 行文字跨行溢出。
- 键盘热力图改为按真实 104 键布局：主键区 6 行 `rowSpan` 定位，编辑键区与方向键
  （倒 T）单独走 `clusterX`，小键盘 4 列独立区域。F 键按标准 **4 键一组**
  （F1-F4 / F5-F8 / F9-F12）排列——曾误排成 F1-F3 一组，是错的。
- 鼠标区域去掉拟物化，改成按键次数表格；表格随窗口等比缩放。
- **踩坑**：`ViewImage` 反复读到会话缩略图而非真实截图，判断失真。改为
  `ctypes` 枚举控件坐标 + `capture_window.py`（纯 GDI `PrintWindow` 截图）+
  像素扫描核对，才拿到可信结果。

### 1.67 ~ 1.69：计数集中到主窗体，弹窗只留关闭

- 用户要求把总计数、两个重置按钮全部移到主窗体，弹窗「只需要一个关闭按钮」。
  最终 `IDD_COUNT_INFO` 只剩 `PUSHBUTTON "关闭", IDCLOSE, 714, 6, 60, 17` 与热力图。
- `UpdateCountTexts(HWND)` 改为作用在主窗体上（`IDC_TXT_KEYTOTAL` /
  `IDC_TXT_MOUSETOTAL` / 5 个 `IDC_TXT_BTN_*`），主窗体新增 1 秒刷新定时器，
  重置逻辑移入 `MainDlgProc`。

### 1.70 ~ 1.72：三分组重排、对齐与紧凑化

- 主窗体重排为三个 groupbox：**软件设置**（只留开机自动启动）、**鼠标监控**
  （点击特效/位置浮标/鼠标计数开关 + 波纹大小 + 5 行「复选框+颜色+计数」+
  鼠标计数 + 位置浮标 + 浮标颜色）、**键盘监控**（启用键盘计数 + 键盘计数）。
  鼠标相关功能归鼠标监控、键盘相关归键盘监控，窗体由 280×410 收到 **280×368 DLU**。
- 对齐要点：5 个按键标签统一短名（「侧键1/侧键2」取代「侧键(后退)/(前进)」，
  文字宽度不一致会导致颜色按钮错位），标签控件统一 36 DLU 宽，颜色按钮统一
  `x=58, 40×13`，计数文本统一 `x=102`；行距 18→16，滑块高 18→16。
- 计数随开关显隐：`UpdateCountsUi()` 按 `enableClickCount` / `enableKeyboardCount`
  对整组计数控件 `ShowWindow(SW_SHOW/SW_HIDE)`，末尾调 `UpdateCountTexts`。
  **必须在 `IDC_CHK_COUNT` / `IDC_CHK_KEYCOUNT` 的 WM_COMMAND 分支里补调
  `UpdateCountsUi()`**，否则开关不联动。
- **踩坑**：验证显隐时 `IsWindowVisible` 全 false——托盘程序主窗口启动时是隐藏的，
  父窗口隐藏时子控件一律不可见。测试前先 `ShowWindow(main, SW_RESTORE)`；
  发 `BM_SETCHECK` 必须早于 `WM_COMMAND`，因为处理里用 `IsDlgButtonChecked` 回读。

### 1.73：计数只显示数字

- 五个按键行已有复选框标明按键名，计数标签再带「左键: 」前缀会挤占颜色块旁空间。
  改为只 `SetDlgItemTextW(..., std::to_wstring(g_counts.Get(i)))`，纯数字。
- 计数文本 `x` 由 102 → **110 DLU**、宽 70 → 64，与颜色按钮右边界（98 DLU）之间
  由 4 DLU 拉到 12 DLU（150% DPI 实测 21px）。

---

## 1.74 — 计数值右对齐、按 8 位数字预留

### 需求

用户要求五键次数右对齐、按 8 位数字宽度留位，键盘计数同样处理；位数变化时右端不跑。

### 处理

- 资源（`res/app.rc.in`）：五个 `IDC_TXT_BTN_*` 与两个总数由 `LTEXT` 改 `RTEXT`，
  x 统一 110 DLU、**宽 40 DLU**——40 DLU 在 150% DPI 下约 70px，8 位数字实测约
  65px（6 位 `269744` 占 49px，每位 ≈ 8.2px），留约 5px 余量。七个值右边界同为 150 DLU。
- 总数行原先是一个整串「鼠标计数：283105」，整串右对齐会让前缀跟着数字漂移。
  拆成两个控件：`LTEXT "鼠标计数："`（新 ID `IDC_LBL_MOUSETOTAL`=1059）
  + `RTEXT` 数字（`IDC_TXT_MOUSETOTAL`）；键盘同理（`IDC_LBL_KEYTOTAL`=1065）。
- 代码（`src/main.cpp`）：`UpdateCountTexts` 对两个总数只写数字，不再拼前缀；
  `UpdateCountsUi` 的显隐列表加入两个新标签，开关计数时标签与数字一起显隐
  （原来只藏数字会留下一个孤零零的「键盘计数：」）。

### 验证

- `EnumChildWindows`：7 个值控件均为 `x=201px, w=70px`（右边界 271px），
  截图核对右端对齐成一条线，无裁切、无重叠。
- 显隐矩阵：初始全显示 → 关鼠标（标签+数字+5 键计数全隐藏，键盘仍显示）→
  关键盘（键盘标签+数字隐藏）→ 恢复（全显示，两个复选框回到 checked=1）。
- `ctest` 1/1 通过；两套 exe 版本资源 1.74.0.0。

---

## 1.62 — 「计数信息」弹窗缩小

### 需求

用户反馈按键统计弹窗太大，要求缩小。

### 处理

- `res/app.rc.in` 的 `IDD_COUNT_INFO`：宽度 1040→800 DLU，高度 212→190 DLU；
  顶部「关闭」「重置鼠标计数」「重置键盘计数」按钮与「累计按键/点击」标签坐标相应左移。
- 两个自绘面板本身按区域等比缩放（键盘 `unitWidth = 宽/20.5`、`keyHeight = 高/7`；
  鼠标 `scale = min(宽/168, 高/172)`），缩小窗口即整体缩小，无需改绘制逻辑。
- `DrawKeyboardHeatmap` 的标签字体从固定 `-13` 改为随 `keyHeight` 自适应
  （`clamp(keyHeight/2, 10, 13)`），避免高度缩到 190 DLU 后 7 行文字溢出格子、跨行重叠。

### 版本

次版本号递增到 1.62（UI 变更，非补丁）。`app.rc` 经 `gen_direct_rc.py` 重新生成，
两套 exe 均重建。

---

## 1.61 — 键盘计数开关切换时重置按键去抖状态

### 问题现象

键盘计数开启状态下按住某键不放，`input.cpp` 的 `keyDown_[code]` 去抖标记会置
true 且直到抬起才清除。此时若用户关闭「启用键盘计数」，去抖状态仍持续维护
（与开关无关），再重新开启后，那个仍被按住的键的下一次按下会被吞掉，漏计一次。

### 处理

新增 `RawInput::ResetKeyStates()`，清空 `keyDown_[]`。在 `IDC_CHK_KEYCOUNT`
开关切换与 `IDC_BTN_DEFAULT`（重置默认值，同样会改变 `enableKeyboardCount`）
两处调用，使开关变化后按键按下状态重新同步。

---

## 1.61 — 修复代码审查发现的数据丢失风险

### 改动

1. `KeyCountStore::Load()`：读失败时用 `std::filesystem::exists()` 区分"文件不存在"（正常，从 0 开始）与"存在但读不到"（置 `loadFailed_`，阻止写回），与 `CountStore::Load()` 的保护策略一致。
2. `CountStore::Save()` / `SettingsStore::Save()`：`SaveJson()` 成功后，若存在 `pending` 兜底标记则清除它，避免下次启动读回旧 INI 快照覆盖较新的 JSON；用 `exists()` 守卫，避免正常保存凭空创建 INI 文件。
3. `IDC_BTN_DEFAULT`：补上 `enableClickCircle` / `enableHoverDot` / `enableClickCount` 三个开关的重置，与 `Settings::Defaults()` 一致。
4. `tests/config_test.cpp`：新增 `TestPendingClearedOnHealthySave`、`TestKeyCountsUnreadableNotOverwritten`，覆盖上述两类数据丢失路径。

### 验证

`ConfigTest` 全绿；两套 exe 重建为 `1.61.0.0`。

---

## 1.60 — 鼠标造型整体下移

### 需求

用户反馈鼠标造型在弹窗右侧面板里位置偏上，要求整体向下一点。

### 处理

`DrawMouseShape()` 里不改各区块各自的坐标（避免破坏 1.59 的草图布局），
而是在统一的 `box()` 里给 y 增加 `kVerticalShift=16` 参考单位，主体和五个
按键区块一起下移、相对位置不变。参考面板高度相应从 154 增到 172，给下移
留出余量、避免底部圆角主体被裁切；弹窗宽度是缩放的限制因素，故图形不会缩小。

---

## 1.59 — 鼠标造型按参考草图重排按键布局

### 需求

用户给出一张草图，要求弹窗里的鼠标造型按该布局摆放：侧1、侧2 放到鼠标主体左侧
（原实现是侧1 在左、侧2 在右）。

### 处理

重写 `DrawMouseShape` 的坐标常量（`src/main.cpp`），在 168×154 参考面板内：

- 左键、中键、右键在顶部从左到右横排，骑在主体上边缘；
- 侧1、侧2 在左侧从上到下竖排，骑在主体左边缘；
- 圆角主体矩形移向右下（x=46, y=34, 118×116）。

各区块仍按 `HeatColor` 深浅着色并显示完整次数，缩放逻辑（`scale`）和计数来源未变。
配色、数字样式、左右分区（键盘在左、鼠标在右）均保持不变。

---

## 1.58 — 计数信息弹窗左右分区对调

### 需求

用户反馈：鼠标造型应放在键盘右边，符合真实桌面「键盘在左、鼠标在右」的直觉。

### 处理

`WM_PAINT` 里 `keyArea` / `mouseArea` 的 x 区间互换：键盘热力图占左侧约 81%，
鼠标造型占右侧约 19%。资源模板里累计标签顺序改为「累计按键」在左、「累计点击」
紧随其后，两个重置按钮位置保持不动，避免顶部控件重叠。仅布局调整，绘制逻辑、
计数存储与焦点行为不变。

---

## 1.57 — 鼠标与键盘计数统一到「计数信息」弹窗

### 需求与取舍

用户要求把鼠标计数也放进弹窗，并把主窗口的「重置计数」改成查看入口。
确认后的方案：

- 合并入口：删除「键盘统计」按钮，「重置计数」改名为「计数信息查看」，
  只保留一个打开弹窗的入口，避免两个按钮做同一件事。
- 弹窗左右分栏：左侧鼠标造型，右侧键盘热力图。窗口从 856 加宽到 1040
  DLU，键盘区实际绘制宽度基本不变，不需要重新压缩热力图。
- 鼠标造型按物理按键分区（左/右/中/侧1/侧2），复用 `HeatColor()` 与
  「标签在上、完整数字在下」的绘制方式，和键盘热力图风格一致。
- 两类计数各自独立重置。主窗口的静态计数文本框（IDC_TXT_LEFT 等）全部移除，
  `UpdateCountsUi()` 改为只 `InvalidateRect()` 弹窗，点击时弹窗保持实时刷新。
- 主窗口复选框重排：启用鼠标点击计数、启用键盘计数、开机自动启动依次向下；
  删掉「点击计数」分组后窗口高度从 374 缩到 328 DLU。

### 保持的约束

- 默认焦点仍为「关闭」：资源模板里 IDCLOSE 排在按钮顺序首位，
  `ShowCountInfo()` 在 `ShowWindow()` 之后执行 `DM_SETDEFID` + `SetFocus`。
- 存储格式不变：鼠标计数仍写原 `ClickShow.counts`，键盘计数仍写
  `ClickShow.keycounts`，`ConfigTest` 的字节断言不受影响。

---

## 1.56 — 统计弹窗默认焦点改为关闭

### 问题现象

打开“键盘统计”弹窗后，按 Enter 会直接触发“重置键盘计数”，容易误清空。

### 原因与处理

对话框模板中“重置键盘计数”排在“关闭”之前，是对话框管理器设置的第一个
Tab / 默认按钮。现在把“关闭”放到按钮顺序首位，并在 `ShowKeyboardStats()`
的 `ShowWindow()` **之后**调用 `DM_SETDEFID(IDCLOSE)` + `SetFocus(IDCLOSE)`；
放在 `WM_INITDIALOG` 里无效，因为那时窗口仍隐藏、无法持有焦点。实测焦点与默认
按钮均为 `IDCLOSE (8)`，从两处保证 Enter 只会关闭弹窗；用户仍可 Tab 到重置按钮
后主动执行清空。

---

## 1.55 — 数字键盘独立统计

### 问题现象

数字键盘按键会进入“累计按键”，但热力图里看不到对应格子，因此表现为
“数字键盘没有统计”。

### 排查结果

1. `src/input.cpp` 已通过 `RIDEV_INPUTSINK` 接收全部键盘 Raw Input，
   `src/counts.cpp` 也按虚拟键保存计数，输入链路本身没有丢失数字键盘。
2. `src/main.cpp` 的 `kHeatRows` 只描述了主键盘与导航区，没有
   `VK_NUMPAD0..9` / `VK_ADD` 等键位，所以数据存在但无法显示。
3. Num Lock 关闭时 Windows 会把数字键盘上报成 `VK_HOME` / `VK_END` /
   方向键等，与导航区虚拟键完全相同，单靠 VKey 无法区分物理来源。

### 实现取舍

- 数字键盘按 Raw Input 的 `MakeCode` 与 `RI_KEY_E0` 识别，映射到保留 VK
  区间内的独立存储 ID；Num Lock 开关不影响归属。
- 抬键去抖从原始 VKey 改为解析后的物理键码，避免主键盘 Enter 与数字键盘
  Enter（或左右 Shift）共享 VKey 时互相抑制按下边沿。
- 热力图保持原窗口尺寸，在主键盘右侧增加 4 列数字键盘区，+ 与 Enter
  按真实键盘跨两行。
- 1.54 已保存的 `VK_NUMPAD*` 计数在加载时迁移到新 ID；无法区分来源的
  `VK_RETURN` 保留在主键盘 Enter，不强行迁移。

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
