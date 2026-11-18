# AudioPlaybackConnector-Win11 交接文档（给下一位接手者 / Gemini）

日期：2026-11-18  
适用对象：接手修复、重构或从零重写「把手机音频接到电脑播放」类工具的开发者/AI。  
本文只记录事实、证据和结论；不确定的地方会明确写「未验证」。

---

## 0. TL;DR

- 软件：把 Windows 变成蓝牙 A2DP 音箱，手机配对后声音从电脑出声；带独立音量条。
- 代码已从「抓流重放 + 2 倍数字增益」退回**单通道**（直接调手机在 Windows 里的播放会话音量），这是原版做法，也是目前最稳的方案。
- 已修复：首连无声、修复后无声、字体宋体、退出后遗留静音/音量被顶到 100%、音量周期性写入造成的振荡、代码结构。
- **剩余问题属于 Windows/蓝牙栈，不是应用代码**：切耳机再切回后音量变大、手机音量键失效。已用原版程序 A/B 复现。
- 如果目标是彻底绕开这些问题，唯一路线是「手机端 App 采集 + Wi-Fi 串流到电脑」，属于新产品，不是修补。

---

## 1. 项目与仓库

| 项 | 值 |
| --- | --- |
| 上游原版（窗口/系统选择器） | `ysc3839/AudioPlaybackConnector`（C++/WinRT） |
| Win11 兼容分支 | `summerwind6/AudioPlaybackConnector-Win11` |
| 用户 fork | `NaphRaph/AudioPlaybackConnector-Win11` |
| 本次工作副本 | `E:\A-WorkSpace\DSH_Common\AudioPlaybackConnector-Win11` |
| 工作分支 | `fix/audio-reliability-and-font`（基于 `fix/a2dp-first-connect-silence`） |
| 部署位置 | `E:\A-Tool\AudioPlaybackConnector64.exe`（旧版备份 `.bak-20261029`） |
| 原版对照 exe | `E:\A-WorkSpace\DSH_Common\apc-original-build\AudioPlaybackConnector-original.exe`（由 `win11-compatibility` 分支编译） |

技术栈：C++/WinRT + XAML Islands（DesktopWindowXamlSource）+ WASAPI COM + Win32 托盘。单文件 exe，无安装。

### 1.1 当前提交（从旧到新）

```
5c9d4fb fix: keep phone audio audible, restore its volume, and use the Windows UI font
099499f refactor: split the app into App, Ui and Connection translation units
3800a61 refactor: track one device operation per phone instead of two sets
1756407 fix: prime the first connection so the cold A2DP link carries audio
1b57d54 fix: keep the audio worker scheduled and event-driven under load   （随旁路一起已废弃）
ffb085d feat!: control the phone's Windows playback session instead of rendering a boosted copy
8bb2dcb fix: adopt the phone session volume on connect and prime after repair
0ae8cc6 fix: follow the phone session volume and re-apply it only when Windows recreates it
```

`1b57d54` 的 MMCSS/事件驱动属于已删除的抓流线程，当前正式版不再包含。

---

## 2. 当前代码结构

```
AudioPlaybackConnector.h       全局状态（inline 变量）、常量、跨单元声明、8 个辅助头
App.cpp                        入口 wWinMain、窗口过程 WndProc、托盘图标与通知区域
Ui.cpp                         三个弹窗（退出确认/右键菜单/设备选择器）、主题与字体、设备行
Connection.cpp                 连接生命周期、修复、蓝牙射频恢复、音量应用线程
AudioSessionVolume.hpp         手机端点 GUID 解析、会话音量读取/设置
SettingsUtil.hpp               设置读写与版本迁移
Logging.hpp / I18n.hpp / Util.hpp / Direct2DSvg.hpp / FnvHash.hpp
tests/PhoneAudioGainTests.cpp  目前只剩蓝牙恢复策略断言（增益测试已随旁路删除）
```

关键流程：

1. `ConnectDevice`：`AudioPlaybackConnection.StartAsync()` → `OpenAsync()` → 等状态到 `Opened`；
2. **首次连接预热**（每设备每进程一次）：成功后立即关闭 → 等 1.5 s → 重新建连；
3. 音量线程（`RunPhoneVolumeApplier`，500 ms 轮询）：只在你拖滑块或会话实例变化时写会话音量，其余时间跟随；
4. 失败处理：`0x8007001F` 自动重启蓝牙一次（每设备每进程一次），再失败显示手动按钮；
5. 修复按钮：清掉该设备的已预热标记后重连（走一次预热）。

---

## 3. 已修复的问题（含根因与证据）

### 3.1 首连无声（开机/开程序后第一次连接）

- 现象：连上、显示已连接，但没声音；断开重连一次恢复。
- 根因：Windows A2DP Sink 冷启动。`OpenAsync()` 返回 `Success`、状态为 `Opened`，但链路不传样本。日志证据：`19:22:57.786 processor started` 之后 10 秒没有任何音频包；用户点修复后 `19:23:13.361` 立即恢复。
- 修复：**首次连接预热**——成功后关闭 → 等 1.5 s（`CONNECTION_RELEASE_COOLDOWN`）→ 重开。日志：`19:28:09.395 Priming first connection` → `19:28:11.074 Connection opened`。
- 注意：原版也有此问题。这不是新增旁路造成的，换语言/重写也躲不开，必须保留预热或等价机制。

### 3.2 修复后无声

- 根因：修复只做「关闭 → 等 3 s → 重连」，没有走预热，重连可能落进空链路。
- 修复：修复前 `g_primedDevices.erase(deviceId)`，让重连执行一次完整预热。

### 3.3 退出/断开后遗留静音、音量被顶到 100%

- 根因（旧双路版）：把会话静音到 0% 后，恢复逻辑用「参考音量」映射，实际把会话设成 100%；进程被杀时静音留在 0%，下次开机系统原生通道就是静音。
- 修复（双路版阶段）：接管前记录原始音量，停止时按原值恢复；启动时发现会话为 0% 就恢复 1.0。
- 现状（单通道版）：不再静音会话，问题从根上消失；音量只由滑块控制。

### 3.4 字体是宋体

- 根因：XAML 树默认 `en-US`，中文回退到 SimSun；只设 `FrameworkElement.Language` 不够。
- 修复：`ApplyUiLanguage()` 设语言 + `UiFontFamily()` 按 UI 语言显式返回 `Microsoft YaHei UI` / `Microsoft JhengHei UI` / `Segoe UI Variable Text`，逐个文字控件设置。
- 背景色/菜单配色：Codex 版本已修，本次未再改。

### 3.5 音量周期性写入造成振荡

- 现象（日志）：`23:13:47 set to 25% (was 56%)` → `10%` → `82%` → `100%`，1.5 秒内反复。
- 根因：每 500 ms 强制写会话音量，与 Windows/AVRCP 的绝对音量互相打架。
- 修复：只在「用户拖滑块」或「会话实例变化」时写一次；其余时间跟随会话值。用 `IAudioSessionControl2::GetSessionInstanceIdentifier` 识别会话重建。

### 3.6 满载断续（已随旁路删除）

- 双路抓流版在高负载（游戏编译着色器）下断续。
- 当时的修复：MMCSS（`AvSetMmThreadCharacteristics(L"Audio")` + `AVRT_PRIORITY_HIGH`）+ 事件驱动（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetWaitable`/`WaitForMultipleObjects` 代替 `sleep(10ms)`）。
- 单通道版无自建音频线程，此问题不再存在。

### 3.7 代码结构

- 原 `AudioPlaybackConnector.cpp` 1839 行，拆成 `App.cpp`(327) / `Ui.cpp`(693) / `Connection.cpp`(826)。
- 全局对象改 `inline` 变量，辅助头函数加 `inline`，否则多编译单元重复定义。
- 两个窗口消息处理搬进 `Connection.cpp`（`HandleConnectionStateChanged` / 旧 `HandlePhoneAudioFailure` 已随旁路删除）。
- 设备忙碌状态从两个 `unordered_set` 合并为 `unordered_map<wstring, DeviceOperation>`，用 `emplace().second` 兼作互斥。

---

## 4. 平台级问题（应用层无法修复）

### 4.1 `0x8007001F`（ERROR_GEN_FAILURE）

- 现象：开机/唤醒后第一次连接失败，重试/重启程序都无效，只有重启蓝牙射频恢复。
- 日志：`12:39:48 OpenAsync` → `12:40:03 returned status 3`；射频循环后 `12:41:44 status 0`。
- 处理：自动重启蓝牙一次（会短暂断开所有蓝牙设备，含键鼠），失败后给出手动按钮。
- 定性：Windows 蓝牙栈/驱动状态问题，应用层无 API 可修。

### 4.2 切耳机再切回后音量变大、手机音量键失效

- 现象：手机中途连蓝牙耳机再切回电脑，声音明显变大；此后手机音量键完全不影响电脑。
- 证据：
  - 单通道版日志：切换期间 App **没有写过任何音量**（`23:16:41` 之后无 `set to`/`changed outside`），仍复现；
  - **A/B 对照**：用 `win11-compatibility` 分支原版 exe（单通道、无抓流、无预热）在同一台机器复现同样现象。
- 定性：手机切换输出设备时，Windows 与手机之间的 **AVRCP 绝对音量链路没有恢复**，Windows 用一个固定电平播放；手机端音量不再同步。
- 影响：任何走「Windows 接 A2DP、Windows 出声」的程序都会继承，换语言/重写无效。
- 缓解：切换后用 App 滑块把音量压回去；或断开/重连一次手机重新协商。
- 未验证的猜测：手机开发者选项里的「蓝牙绝对音量」开关可能有影响；蓝牙驱动更新可能改善（用户此前拒绝更新驱动）。

### 4.3 抓流端点（旧双路方案）在切换后失效

- 旧版抓 A2DP 采集端点自己重放；手机切耳机再切回后，端点经常不再送数据，表现为「已连接但没声，修复一次也可能没声，要修两次」。
- 日志：`22:33:49 修复 → 22:33:55 processor started` 之后再无 takeover；`22:37:17` 再修复仍无。
- 结论：这条旁路不可靠，已整体删除。

---

## 5. 走过的弯路（不要重复）

1. **抓流重放 + 2 倍数字增益**（fork 原有）
   - 优点：能超过 Windows 会话 100% 上限。
   - 代价：静音接管失败=无声；切设备后端点失效；与系统会话双路叠加；满载断续；退出时恢复音量容易出错。
   - 结论：删除。若将来非要做 >100%，必须做成默认关闭的可选开关，并保证失败时回退到原生通道。
2. **50% 分界混合方案**（0–50% 调会话、50–100% 才抓流）
   - 讨论过，用户最终选择纯单通道，未实现。
3. **响度守护 / AGC**
   - 用抓流做测量、自动调会话音量抵消跳变。能压住切设备跳变，但压缩动态、依赖抓流端点。用户选择接受现状，未实现。
4. **换语言重写（C#/Rust/Go）**
   - 调用的还是 `AudioPlaybackConnection` + Windows A2DP Sink，三个平台问题原样继承，无收益。
5. **原版对照**：确认了「原版没这个问题」的印象不成立（见 4.2），这一步是本项目最重要的定性依据。

---

## 6. 构建、部署与调试环境

### 6.1 本机工具链（已装好）

- `C:\BuildTools`：VS Build Tools + MSVC 14.44（v143）+ Windows SDK 10.0.26100。
- 项目文件写的是 v142，构建时传 `PlatformToolset=v143`。
- `E:\A-WorkSpace\DSH_Common\nuget.exe`；NuGet 缓存与临时目录已重定向到工作区（`NUGET_PACKAGES`、`NUGET_SCRATCH`、`TMP`、`TEMP`），因为沙箱下用户目录会拒绝写入。
- 一键构建脚本：`E:\A-WorkSpace\DSH_Common\build-apc.ps1`（x64 Release + 单测）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File E:\A-WorkSpace\DSH_Common\build-apc.ps1
```

### 6.2 必知坑：Low Integrity 标签

在本工作区编译出的 exe 会带「低完整性级别」标签。Windows 会以低权限启动它，结果：

- 托盘图标注册失败（程序「点了没出现」）；
- 无法写 `%LOCALAPPDATA%\AudioPlaybackConnector`，日志/设置全丢。

构建脚本在链接后执行 `icacls <exe> /setintegritylevel medium` 修正。**任何在此工作区产出的 exe 都要做这一步**。

### 6.3 运行/测试注意

- 托盘类程序必须由用户正常启动（资源管理器/计划任务），脚本 `Start-Process` 的进程可能拿不到托盘/写不了 AppData。
- 验证音量前必须先让手机实际播放——手机暂停时 Windows 不创建播放会话，日志不会有任何音量记录。
- 设置：`%LOCALAPPDATA%\AudioPlaybackConnector\AudioPlaybackConnector.json`，当前 `volumeControlVersion = 6`。
- 日志：`%LOCALAPPDATA%\AudioPlaybackConnector\Logs\AudioPlaybackConnector.log`（每次连接/音量变更都有记录）。
- 单实例互斥体：`Local\AudioPlaybackConnector.SingleInstance.2DAAABDD-2402-4023-BC3E-B6E93FAD567B`。

### 6.4 CI

`.github/workflows/build.yaml`：push 到 `fix/**` 或 tag `v*` 时构建 x86/x64/ARM64，跑 `tests/PhoneAudioGainTests`。本次所有提交 CI 均通过。

---

## 7. 音量语义（当前正式版）

| 滑块 | 含义 |
| --- | --- |
| 0% | 会话静音 |
| 100% | 手机送来的原样（会话音量 100%） |
| 0–100% | 线性衰减，**不能超过原声**（Windows 会话系数上限 1.0） |

- 用户没动过滑块：App 只「采用」会话当前值，不写；
- 用户拖过滑块：记住该值；Windows 重建会话时重新应用一次；
- 不再周期性写音量。

设置迁移：`volumeControlVersion < 6` 的旧值（4/5，50% = 中性点）不直接搬运，App 改为采用会话当前音量，保证升级后响度不变。

---

## 8. 如果要从零重写：建议与验收清单

### 8.1 方案选择

| 目标 | 推荐方案 |
| --- | --- |
| 保留系统配对、单文件、够用即可 | 继续用 `AudioPlaybackConnection` + 会话音量（本仓库现状） |
| 一定要 >100% 音量 | 抓流重放（默认关闭的开关）或系统级 APO；后者需签名/安装 |
| 彻底绕开 Windows 蓝牙栈 | 手机端 App 采集 + Wi-Fi 串流到 PC |

### 8.2 必须保留的机制

1. 首连预热（open → close → 1.5 s → reopen），每设备每进程一次；
2. 修复按钮也走预热；
3. `0x8007001F` → 自动重启蓝牙一次（提示会断开所有蓝牙设备）；
4. 音量只写一次，不轮询写；
5. 单文件、托盘、i18n（en/zh-CN/zh-TW）、设置迁移。

### 8.3 验收清单（建议按此实测）

- 开机后第一次连接：有声（预热日志）；
- 断开→重连：有声；
- 修复声音：一次成功；
- 切耳机再切回：**已知会变响**（环境问题），确认 App 未写音量、可用滑块压回；
- 退出→重开：音量设置保留、无遗留静音；
- 输出设备切换：不崩、不静音；
- 高负载（游戏/编译）：不断续；
- 多手机同时连接：互不影响。

### 8.4 手机端串流方案（如果要做新产品）

- 采集：Android `MediaProjection` + `AudioPlaybackCaptureConfiguration`（Android 10+），需用户授权；**DRM 保护的应用会拒绝采集**。
- 编码：Opus（低延迟、48 kHz 立体声，64–128 kbps）。
- 传输：局域网 UDP + 抖动缓冲（建议 100–200 ms），mDNS/UDP 广播发现，失败回退手输 IP。
- PC 端：接收、抖动缓冲、WASAPI 共享模式播放（可任意增益）。
- 已知限制：延迟 100–300 ms（视频会音画不同步）；只能 Wi-Fi；需要两个组件；部分音乐 App 禁止采集。
- 可先验证现成方案：AudioRelay、sndcpy（开源）、SoundWire。

---

## 9. 关键日志证据（摘录，便于复核）

```
# 首连空链路
19:22:57.786 Phone-only audio processor started
（10 秒无音频包）
19:23:07.668 Audio repair requested
19:23:13.361 Phone-only audio takeover started

# 预热生效
19:28:09.395 Priming first connection
19:28:10.919 Connection requested
19:28:11.074 Connection opened

# 周期性写音量造成振荡
23:13:47.182 set to 25% (was 56%)
23:13:47.696 set to 10% (was 25%)
23:13:48.211 set to 82% (was 10%)
23:13:48.727 set to 100% (was 82%)

# 单通道版切耳机期间 App 无任何音量写入
23:16:41.581 Matched phone A2DP endpoint {b8159fb3-...}
（之后无 set to / changed outside，仍复现变响）

# 修复后重连落进空链路（旧双路版）
22:33:49 repair → 22:33:55 processor started → 无 takeover
22:37:17 repair → 22:37:23 processor started → 无 takeover
```

---

## 10. 给接手者的检查表

- [ ] 先跑原版 A/B，再判定「是代码问题还是平台问题」；
- [ ] 任何音频改动都验证：第一次连接、断开重连、修复、切设备、退出重开；
- [ ] 不引入周期性音量写入；
- [ ] 不在没有实际播放的情况下验证音量（会话不存在）；
- [ ] 构建产物先修 Low IL 再测试；
- [ ] 改动推 `fix/**` 分支，走 CI 三架构；
- [ ] 平台问题不要试图用「重写/换语言」解决。
