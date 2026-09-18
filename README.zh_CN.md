# Audio Bridge

[English](README.md) | **简体中文**

Audio Bridge 是一款轻量、开源的 Windows 蓝牙音频接收（A2DP Sink / LE Audio）连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持，但 Windows 本身并未提供管理连接的界面，仍需第三方软件来完成。Audio Bridge 在 Richard Yu 的 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 基础上继续演进：沿用该项目「在常驻通知区域的 Win32 应用中驱动 Windows 公开的 `AudioPlaybackConnection` API」的思路，并在此之上增加了自动重连、音频性能优化、多语言支持与单一安装包多架构打包。详见文末 [致谢](#致谢)。

---

## 概要

**它是什么。** Audio Bridge 是一款常驻系统托盘的 Windows 桌面应用，让你把 PC 连接到蓝牙音频源设备（手机、平板等）并通过 Windows 接收它们的音频。它提供的是**控制层**：可枚举的设备列表、连接生命周期管理、自动重连、通知，以及一个把三种 CPU 架构打包在一起的安装程序。

**它不是什么。** Audio Bridge **不**自己实现 A2DP / LE Audio 协议栈。真正的音频采集、解码与播放完全由 Windows 内置的蓝牙音频服务完成。Audio Bridge 只驱动 Windows 公开的 `AudioPlaybackConnection` API —— 这是刻意的设计取舍，让应用保持小巧、健壮，并能向前兼容未来的 Windows 版本。

**核心能力**

| 能力 | 说明 |
|---|---|
| 双协议支持 | 同时支持经典 A2DP 与 LE Audio 设备；在设备选择器中显示 `[A2DP]` / `[LE Audio]` 前缀 |
| 智能自动重连 | 指数退避 + 抖动的重连调度，降低多设备重连冲突 |
| 音频性能优化 | 连接期间应用 MMCSS "Pro Audio" 调度、系统执行状态锁与进程优先级提升 |
| 多设备记忆 | 记住多个已配对设备以便快速切换；旧版配置自动迁移 |
| 开机自启 | 可选在 Windows 启动时运行（优先注册表，回退到启动文件夹快捷方式） |
| Windows 通知 | 设备连接或断开时弹出 Toast 通知 |
| 系统托盘集成 | 常驻通知区域，并提供原生设备选择器 |
| 多语言界面 | 英文、简体中文、繁体中文（跟随系统界面语言） |
| 单一安装包、三种架构 | 一个安装包内含 x86、x64、ARM64 二进制，安装时自动匹配 |

**技术栈。** Win32 + C++20，使用 C++/WinRT 访问 Windows 运行时 API，使用 WIL 做错误处理与 RAII，构建基于 MSBuild/v143，打包使用 Inno Setup，本地化采用 gettext PO → 二进制 YMO 流水线。

---

## 操作手册

### 1. 安装

1. 下载并运行 `Audio_Bridge_Setup.exe`。
2. 按向导操作。在 **附加图标** 步骤可选择：
   - **创建桌面图标**
   - **开机启动**（登录时自动启动 Audio Bridge）
3. 安装程序会检测你的 CPU 架构（x86 / x64 / ARM64），并将匹配的二进制安装为 `%ProgramFiles%\Audio_Bridge` 下的 `Audio_Bridge.exe`（或对应的按用户目录，因为安装程序请求的是最低权限级别）。

> 安装包内包含三种架构的二进制，只会安装与你的系统匹配的那一个。

### 2. 首次运行

1. 从开始菜单启动 **Audio Bridge**。程序只以**托盘图标**的形式出现在通知区域 —— 没有主窗口。
2. 首次启动时，程序从 `%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json` 读取配置。若该文件尚不存在，则使用默认值，并根据系统状态（注册表 / 启动文件夹）推断开机自启状态。

### 3. 配对设备

在 Audio Bridge 能够连接之前，蓝牙设备必须已经与 Windows **配对**。Audio Bridge 不负责配对。

1. 右键点击托盘图标并选择 **蓝牙设置**，或打开 Windows 设置 → 蓝牙和其他设备。
2. 按常规方式添加 / 配对手机或平板。

### 4. 连接设备

1. **左键点击** 托盘图标（或右键 → 打开选择器）。一个原生设备选择器会列出支持 `AudioPlaybackConnection` 的蓝牙音频源设备。
2. 每个条目都带有检测到的协议前缀，例如 `[A2DP] Pixel 8`。
3. 选择设备进行连接。选择器的状态文本会报告进度（`连接中…` → `已连接`，失败时显示 `重试`）。
4. 来自该设备的音频现在通过 PC 的默认输出设备播放。

要断开连接，请使用选择器中的 **断开** 按钮，或在手机侧断开。通过其他（非 UWP）路径建立的连接只能由系统关闭；参见下文 *技术说明*。

### 5. 托盘菜单说明

右键点击托盘图标打开菜单：

| 菜单项 | 行为 |
|---|---|
| （设备选择器） | 左键在光标处打开选择器 |
| **蓝牙设置** | 打开 Windows 蓝牙设置页 |
| **重新连接** | 切换自动重连（勾选状态反映已保存的设置） |
| **开机启动** | 切换登录时启动（勾选状态反映已保存的设置） |
| **统计信息** | 显示连接统计面板 |
| **退出** | 退出程序（连接活跃时会请求确认） |

### 6. 配置

设置以 JSON 形式保存在：

```
%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json
```

| 键 | 类型 | 含义 |
|---|---|---|
| `reconnect` | boolean | 是否启用自动重连（默认 `true`） |
| `autoStart` | boolean | 是否在 Windows 启动时运行 |
| `lastDevices` | string[] | 记住的设备 ID，用于自动重连 |

说明：

- 文件以**原子方式**写入（临时文件 → flush → 原子替换），因此崩溃或断电都不会破坏你的设置。
- 首次运行时，会自动导入位于旧可执行文件同目录的旧版配置。旧文件会保留为备份。
- `开机启动` 通过 `HKCU\...\CurrentVersion\Run` 下的注册表值 `Audio_Bridge` 实现；若注册表写入失败，程序会回退为在启动文件夹中创建快捷方式。

### 7. 通知

- 设备连接时、设备断开时都会弹出 Toast 通知。
- 断开通知按设备做了节流，避免连接抖动时刷屏。
- 当自动重连在多次尝试后放弃时，会弹出"自动重连已停止"通知。
- Toast 头部使用应用的图标，这要求开始菜单快捷方式的 AppUserModelID（`AudioBridge`）与程序使用的保持一致。安装程序会为你创建该快捷方式。

### 8. 卸载

使用 **设置 → 应用 → 已安装的应用 → Audio Bridge → 卸载**，或开始菜单中的卸载入口。

卸载程序会先通过私有消息 `WM_APP_QUITREQUEST` 请求正在运行的托盘程序优雅退出，未及时退出则强制结束进程。如果进程始终结束不掉（罕见的蓝牙驱动挂起，进程被内核钉住），卸载**不会中止**：被锁定的 `Audio_Bridge.exe` 会被改名让位，使其余文件得以正常删除，同时在 `RunOnce` 中登记一条登录后清理任务（由 `wscript.exe` 执行一段自删除的 VBScript，全程静默、不会闪出控制台窗口），在下次登录后自动清除残留。你只会看到一条说明性提示，而不是"什么都没删掉"的失败结果。

### 9. 故障排查

| 现象 | 说明 / 解决方法 |
|---|---|
| 在音量合成器或 SteelSeries Sonar 中显示为 `svchost.exe` | 平台预期行为 —— 见 [关于 svchost.exe](#关于-svchostexe一个你需要知道的平台限制) |
| 设备未出现在选择器中 | 设备必须已配对且支持 A2DP Sink / LE Audio；请在 Windows 蓝牙设置中重新配对 |
| 第二次启动没有反应 | Audio Bridge 是单实例的；再次启动只会打开已运行实例的设备选择器 |
| 自动重连持续失败 | 重试预算耗尽后，程序会停止对该设备的尝试并通知你；请手动重连或重新配对 |
| 资源管理器重启后托盘没有图标 | 程序监听 `TaskbarCreated` 消息并会自动重新添加图标；若仍失败，请重启程序 |

---

# 功能特性
- **双音频协议支持**：同时支持 A2DP 和 LE Audio（低功耗音频），自动协商，并在设备选择器中显示 `[A2DP]` / `[LE Audio]` 前缀
- **智能自动重连**：采用指数退避 + 抖动（基于 BLEdge 算法）的重连调度，降低多设备重连冲突
- **音频性能优化**：连接期间自动应用 MMCSS "Pro Audio" 多媒体调度、系统执行状态锁及进程优先级提升，降低音频卡顿
- **开机自启**：可选择在 Windows 启动时自动运行程序（优先注册表，回退到启动文件夹快捷方式）
- **多设备记忆**：记住多个已配对设备，方便快速切换；旧版配置文件自动迁移
- **Windows 通知**：设备连接或断开时显示 Toast 通知
- **系统托盘集成**：最小化到系统托盘，托盘图标使用应用内置的图标资源
- **UWP 设备选择器**：现代化的设备选择界面，基于 Windows UWP API
- **多语言界面**：支持英文、简体中文、繁体中文（根据系统界面语言自动切换）

## 音频协议支持
本应用基于 Windows `AudioPlaybackConnection` API，协议协商由系统自动处理：

- **A2DP**：兼容大多数蓝牙设备，所有智能手机广泛支持
- **LE Audio**：更低延迟、更低功耗、更好的音质（需要蓝牙 5.2+ 硬件、设备支持，以及 Windows 11 22H2+）

协议选择由 Windows 蓝牙堆栈自动处理，应用本身不实现显式的协议选择逻辑。`[A2DP]` / `[LE Audio]` 前缀仅作信息提示，来自设备的 `System.Devices.Aep.ProtocolId` 属性。

## 关于 svchost.exe：一个你需要知道的平台限制

### 为什么在音量合成器 / SteelSeries Sonar 中显示为 svchost.exe？

这不是本应用的 Bug，而是 Windows **AudioPlaybackConnection** API 的架构决定的。

本应用只做"控制层"的事：管理设备列表、建立蓝牙连接、提供托盘 UI。真正的音频数据接收、解码和输出——手机通过 A2DP 传来的声音——全部由 Windows 内部的蓝牙音频服务处理，这些服务运行在 `svchost.exe` 进程里（具体是 `BTAGService` 蓝牙音频网关服务和 `Audiosrv` Windows 音频服务）。

所以：
- **Windows 音量合成器（sndvol）** 里你看到的出声进程是 svchost.exe，而不是 Audio Bridge
- **SteelSeries Sonar** 按进程分类音频轨道时，也把它归为 svchost.exe，并因为 svchost 属于 LocalService 系统身份而提示"不允许 Sonar 更改音频设置"
- Sonar 默认把 svchost 归入"游戏"或其他启发式分类，而不是你期望的"媒体"

这是所有使用 AudioPlaybackConnection API 的应用（包括微软官方示例、UWP 商店中的同类应用）的共同限制——目前在应用层面没有绕过的办法。

### Sonar 用户的解决办法

虽然不能在 Sonar 里直接把 svchost 改名或拖到指定轨道，但你可以通过下面的方式达到同样效果：

**方案一：在 Windows 系统层面切换默认输出设备（推荐）**

1. 打开 Windows 设置 → 系统 → 声音
2. 将"输出"的默认设备选为 **SteelSeries Sonar - 媒体**（或你想让蓝牙音频走的 Sonar 轨道对应的虚拟设备）
3. 让手机连接后播放音乐，音频就会自动从 Sonar 的"媒体"虚拟设备出来，你的 Sonar 调音、EQ、空间音频全部生效

**方案二：用 Windows 音量合成器调整**

打开 Windows 音量合成器（Win+R → 输入 `sndvol`），svchost.exe 的音频会话可以在这里正常调节音量和静音，不受 Sonar 限制。

**方案三：如果你使用的是 LE Audio（蓝牙 5.2+）**

部分 LE Audio 设备连接时走的是不同的系统路径，在 Sonar 中有时能被更准确地识别。如果你的设备同时支持 A2DP 和 LE Audio，可以试试切换。

### 能不能让它显示为 "Audio Bridge"？

理论上可以通过 `IAudioSessionManager2` 枚举 svchost 的音频会话并调用 `IAudioSession::SetDisplayName` 来"贴标签"。但这个 API 有两个硬限制：

1. **权限隔离**：svchost 里的 Audiosrv/BTAGService 以 `NT AUTHORITY\LocalService` 或 `NetworkService` 身份运行，普通用户进程调用 `SetDisplayName` 会返回 `E_ACCESSDENIED`
2. **Sonar 不认**：Sonar 的进程分类逻辑看的是**可执行文件名**（svchost.exe），不看会话的 Display Name

所以这条路目前走不通。这是一个已知的平台限制，等待微软未来在 AudioPlaybackConnection API 中暴露更完整的音频会话控制接口。

# 支持的架构
一个安装包内置三种架构的二进制，安装时自动检测并安装对应版本：

| 架构 | 构建产物 | 典型设备 |
|---|---|---|
| x86（32 位） | `Release\Audio_Bridge32.exe` | 旧版 32 位 Windows |
| x64（64 位） | `x64\Release\Audio_Bridge64.exe` | 标准 x64 电脑 |
| ARM64 | `ARM64\Release\Audio_BridgeARM64.exe` | 骁龙 / Surface Pro X |

安装后所有架构统一命名为 `Audio_Bridge.exe`；安装的实际架构记录在 `{app}\installed.arch.txt` 中，便于排错。

# 配置
设置保存在 JSON 文件中：
```
%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json
```
可配置项：`reconnect`（自动重连）、`autoStart`（开机自启）、`lastDevices`（记住的设备）。首次运行时，旧版位于 exe 同目录的配置会被自动导入迁移。

# 国际化
界面语言跟随系统界面语言。翻译以 gettext PO 文件维护在 `translate/source/` 下，构建时编译为紧凑的二进制资源（YMO 格式，使用 FNV-1a 32 位哈希查找，类似 gettext MO）：

1. `translate/gen_pot.sh` — 从源码提取字符串到 `messages.pot`
2. 翻译 `zh_CN.po` / `zh_TW.po`（例如使用 Poedit）
3. `translate/gen_rc.sh` — 将 PO 转换为 YMO 并生成 `translate/generated/translate.rc`

运行时，程序通过 `FindResourceExW(..., GetThreadUILanguage())` 加载与当前线程 UI 语言匹配的 YMO 资源。没有翻译的字符串回退为原始英文。

# 从源码构建

## 前置要求
- Visual Studio 2022，安装"使用 C++ 的桌面开发"工作负载（v143 工具集）
- Windows SDK 10.0.26100.0（或更高）
- [Inno Setup 6](https://jrsoftware.org/isinfo.php)（用于生成安装包）
- 依赖通过 NuGet 自动还原（`Microsoft.Windows.CppWinRT`、`Microsoft.Windows.ImplementationLibrary`）

## 构建步骤
1. 用 Visual Studio 打开 `AudioBridge.sln`。
2. 分别以 **Release** 配置构建三个平台：`Release|x86`（Win32）、`Release|x64`、`Release|ARM64`。
3. 将三个可执行文件复制到项目根目录的 `Release\` 文件夹：
   - `Release\Audio_Bridge32.exe`
   - `Release\Audio_Bridge64.exe`
   - `Release\Audio_BridgeARM64.exe`
4. 用 Inno Setup 打开 `setup.iss` 并编译，安装包生成在 `Output\Audio_Bridge_Setup.exe`。

也可以用命令行构建全部三种架构：

```powershell
$msb = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
foreach ($p in 'x86','x64','ARM64') {
    & $msb AudioBridge.sln /p:Configuration=Release /p:Platform=$p /m /v:minimal
}
```

# 实现与技术路径

本节描述应用实际上是如何构建的，从进程启动一直到音频控制 API。

## 架构总览

```
Audio_Bridge.exe
  ├── Win32 外壳层     AudioBridge.cpp / AudioBridge.h  (wWinMain、窗口过程、托盘、菜单、定时器)
  ├── 应用门面         AudioPlaybackApp.hpp             (Meyers 单例：状态 + 编排)
  ├── 连接层           ConnectionManager.hpp            (并发安全的连接表、ConnectAsync)
  │     ├── ReconnectScheduler.hpp                      (退避 + 抖动、熔断器)
  │     └── AudioPerformanceBoost.hpp                   (MMCSS "Pro Audio" 线程，引用计数)
  ├── 设备监控         AudioDeviceMonitor.hpp           (IMMNotificationClient：端点消失)
  ├── 持久化           SettingsUtil.hpp                 (JSON 配置、原子写入、开机自启)
  ├── 本地化           I18n.hpp / FnvHash.hpp           (YMO 查找 + FNV-1a 哈希)
  └── 工具             Util.hpp                         (UTF-8/16 转换、模块路径)
```

代码刻意做了拆分，尽可能让每个关注点都是自包含的头文件，并让 Win32 细节不会渗透到连接逻辑中。

## 1. 启动、窗口与消息循环

1. `wWinMain` 将进程 AppUserModelID 设置为 `AudioBridge`（Toast 通知所需），初始化**单线程套间**（`winrt::init_apartment`），并获取名为 `AudioBridge_SingleInstance` 的单实例互斥体。
   - 若已有实例在运行，新进程会按窗口类名 `AudioBridge` 找到已存在的隐藏窗口，投递一条私有消息以打开设备选择器，然后退出。
2. 注册窗口类 `AudioBridge`（图标来自编译进资源的 `.ico`），并创建一个**隐藏宿主窗口**（`WS_EX_TOOLWINDOW`）。所有外壳集成功能都挂在这个窗口上。
3. 初始化顺序：加载翻译资源 → 加载设置 → 应用开机自启 → 构建菜单 → 创建 WinRT 设备选择器 → 创建托盘图标。
4. 托盘图标通过 `Shell_NotifyIcon`（`NIM_ADD` / `NIM_MODIFY` / `NIM_SETVERSION`，`NOTIFYICON_VERSION_4`）注册。它还监听 `TaskbarCreated` 消息，以便资源管理器重启后图标能自动恢复。
5. 消息循环是标准的 `GetMessageW` / `TranslateMessage` / `DispatchMessageW` 泵。

**线程规则。** 所有与窗口绑定的操作（定时器布置、托盘更新、窗口销毁）都会被编排回主线程。后台工作（WinRT 连接协程、设备状态回调）从不直接触碰窗口，而是投递消息。这是设计的核心正确性不变量。

## 2. 设备枚举与连接

- 选择器是 WinRT 的 `Windows.Devices.Enumeration.DevicePicker`，通过 `IInitializeWithWindow` 绑定到隐藏窗口。其过滤器为 `AudioPlaybackConnection::GetDeviceSelector()`，因此只会列出 Windows API 真正可以连接的设备。
- 连接在 `ConnectionManager::ConnectAsync` 中完成，路径如下：
  1. **幂等 + 在途保护** —— 设备已连接则立即返回；对同一设备的连接已在进行中则忽略重复请求。
  2. 用 `AudioPlaybackConnection::TryCreateFromId(deviceId)` 创建连接对象。
  3. 订阅 `StateChanged` 以检测关闭（包括由手机侧触发的关闭）。
  4. `co_await connection.StartAsync()` 与 `co_await connection.OpenAsync()`，各自包裹在 **45 秒超时**（`WithTimeout`）中，刻意长于协议层自身 20–30 秒的超时。
  5. 成功时：记录统计、更新选择器状态、获取一次性能优化引用、取消该设备待处理的重连，并触发连接事件（驱动 Toast）。
  6. 失败时：对失败分类（`RequestTimedOut` / `DeniedBySystem` / `UnknownFailure`），清理条目并将选择器置为 `重试`。
- 取消是协作式的：map 条目在 `await` **之前**插入，因此关闭请求总能关掉正在建立的连接。

**为什么要冗余？** 通过桌面（非 UWP）路径建立的连接通常**无法**被程序关闭 —— 调用 `Close()` 会返回 Access Denied。因此断开是通过三种独立机制异步检测的：`StateChanged` 事件、30 秒健康检查定时器、以及默认端点通知（见下文）。

## 3. 协议检测（A2DP 与 LE Audio）

检测仅作信息展示。`DetectProtocol` 读取设备属性 `System.Devices.Aep.ProtocolId`，并与蓝牙 LE Audio 协议 GUID `{0BB58923-2600-489B-9FC9-DE57A5669D6F}` 比较：

- GUID 等于 BLE Audio GUID → `LEAudio`
- 属性存在但不同 → `ClassicA2DP`
- 属性缺失 / 无法读取 → `Unknown`

结果成为选择器与统计中显示的 `[LE Audio]` / `[A2DP]` 前缀。真正的协议协商完全由 Windows 蓝牙堆栈完成。

## 4. 自动重连调度器（指数退避 + 抖动）

`ReconnectScheduler` 实现了意外断开后使用的重试策略：

- 基础延迟 `5000 ms × 2^min(retryCount, 6)`，上限 `300000 ms` —— 即序列 5 / 10 / 20 / 40 / 80 / 160 / 300 秒。
- 每个延迟都施加 **±20% 随机抖动**，使多个设备同时重连时不会相互碰撞（该思路改编自 *BLEdge* 调度算法）。
- `1000 ms` 的下限防止出现接近零的延迟。
- 尝试 **20 次**后，调度器对该设备触发熔断，将其移出队列并弹出"自动重连已停止"通知。

计时由**单个 Win32 定时器**（ID 1）驱动。定时器根据调度计划的下一个到期时间重新布置；由于 `SetTimer` 必须由拥有窗口的线程调用，跨线程事件只是向主线程投递一条重新布置消息。

## 5. 音频性能优化（MMCSS）

只要有一个连接处于活跃状态，`AudioPerformanceBoost` 就会提升音频管线的调度优先级：

- 它将**进程**保持在 `NORMAL_PRIORITY_CLASS`，把所有提升限制在**一个专用线程**内，保证 `AvSetMmThreadCharacteristics` 与 `AvRevertMmThreadCharacteristics` 在同一线程上调用（这是文档要求）。
- 优化线程：以 **"Pro Audio"** 任务向 MMCSS 注册，设置 `AVRT_PRIORITY_HIGH`，并持有 `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED)`。
- 刻意**省略** `ES_DISPLAY_REQUIRED`，因此应用绝不会强制屏幕常亮。
- 优化是**引用计数**的：第一个连接打开时启动，最后一个连接关闭时停止。活跃期间线程以零 CPU 开销等待事件。

## 6. 端点监控与健康检查

- `AudioDeviceMonitor` 实现 `IMMNotificationClient`，监听默认音频端点变化与渲染端点移除。如果某个蓝牙连接正在渲染的端点消失（例如拔出 USB 耳机），程序会将连接视为已断开并清理。
- 每 30 秒（`IDT_HEALTHCHECK`）定时器会周期性地将内部连接表与实际状态对账，覆盖基于事件的路径无法观测到的情况。

## 7. 设置与配置存储

- 路径：`%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json`（目录按需创建）。
- 写入是**原子**的：先写 `*.tmp` 文件，再用 `FlushFileBuffers` 刷盘，最后用 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` 替换。部分写入绝不会留下被截断的配置。
- 旧版迁移：首次运行时，位于旧可执行文件旁的配置会被导入到新位置，并保留为备份。
- 开机自启通过两条通道应用：注册表 Run 值（优先）与启动文件夹快捷方式（回退）。两者互为排他，避免重复的启动项。

## 8. 本地化流水线

- 源码字符串使用 `_(L"...")` 宏。构建时 `gen_pot.sh` 用 `xgettext` 提取它们，`gen_rc.sh` 将翻译后的 `zh_CN.po` / `zh_TW.po` 编译为二进制 **YMO** 资源。
- YMO 格式是一个紧凑哈希表：16 位条目数，随后是 `(uint32 hash, uint16 offset)` 对，再是 UTF-16 翻译数据。哈希是对源字符串的 UTF-16LE 字节做 **FNV-1a 32 位**计算 —— 与 C++ 查找所使用的内存表示一致，因此运行时无需转换。
- 启动时 `LoadTranslateData` 选择与当前线程 UI 语言匹配的资源；随后 `Translate()` 做哈希查找，并对热点字符串使用指针缓存。未匹配的字符串返回原始英文。

## 9. 构建与打包

- 项目构建三个 Release 目标（`x86`/Win32、`x64`、`ARM64`），使用 v143 工具集、C++20（`/std:c++latest`）、`/utf-8`，并在第 4 级开启**警告即错误**。
- 输出二进制由 `TargetName` 属性命名：`Audio_Bridge32.exe`、`Audio_Bridge64.exe`、`Audio_BridgeARM64.exe`。
- `setup.iss`（Inno Setup）将三个二进制打包为单个 `Audio_Bridge_Setup.exe`，只安装匹配的架构为 `Audio_Bridge.exe`，并注册 AppUserModelID、开始菜单 / 桌面 / 启动快捷方式以及卸载项。
- 由于该应用是常驻托盘、会保持自身可执行文件被占用的进程，安装程序与卸载程序通过私有窗口消息 `WM_APP_QUITREQUEST` 停止它，等待优雅退出，失败则回退到 `taskkill /F`。若进程最终被内核钉住（罕见的蓝牙驱动挂起，连 `taskkill /F` 都杀不掉），流程不会中止：被锁定的 `Audio_Bridge.exe` 会被改名为 `.old` —— 已加载的映像无法删除，但仍可改名，改名后原路径立即空闲，卸载的其余部分得以正常完成 —— 同时登记一条 `RunOnce`，在下次登录后清除残留 —— 该任务由 `wscript.exe` 执行一段自删除的 VBScript：`wscript.exe` 属 GUI 子系统程序，配合 `//B` 完全静默，因此不会像 `cmd /C del` 那样在登录时闪出一个控制台窗口。（这里无法使用 `MOVEFILE_DELAY_UNTIL_REBOOT`：它需要写入 `HKLM\...\Session Manager\PendingFileRenameOperations`，而本安装程序以 `PrivilegesRequired=lowest` 运行。）同一套"改名让位"逻辑也保护覆盖安装与升级。

# 项目结构
| 文件 | 职责 |
|---|---|
| `AudioBridge.cpp` / `AudioBridge.h` | 入口点、隐藏宿主窗口、消息循环、托盘图标与菜单、定时器、通知消息 |
| `AudioPlaybackApp.hpp` | 应用根类（Meyers 单例门面）：共享状态、编排、优雅关闭、连接入口 |
| `ConnectionManager.hpp` | 并发安全的连接表、可取消的 `ConnectAsync`、协议检测（A2DP / LE Audio）、连接事件与统计 |
| `ReconnectScheduler.hpp` | 指数退避 + 抖动重连调度器，带熔断器 |
| `AudioPerformanceBoost.hpp` | MMCSS "Pro Audio" 调度 + 执行状态锁（RAII，引用计数，专用线程） |
| `AudioDeviceMonitor.hpp` | `IMMNotificationClient` 实现：默认端点变化 / 端点移除通知 |
| `SettingsUtil.hpp` | 配置存储（`%LOCALAPPDATA%`）、原子写入、旧配置迁移、开机自启（注册表 + 启动快捷方式回退） |
| `I18n.hpp` / `FnvHash.hpp` | 本地化（YMO 资源查找，FNV-1a 32 位哈希） |
| `Util.hpp` | UTF-8/UTF-16 转换、模块路径工具 |
| `AudioBridge.rc` / `resource.h` | Windows 资源：应用图标、版本信息、内嵌 SVG |
| `AudioBridge.manifest` | 应用清单（DPI 感知、支持的操作系统、通用控件） |
| `AudioBridge.ico` / `Audio_Bridge_Icon.png` | 编译进程序的应用图标及其 PNG 源图 |
| `translate/` | gettext PO 源文件及 YMO 构建工具（`po2ymo.py`、`gen_pot.sh`、`gen_rc.sh`） |
| `setup.iss` | Inno Setup 脚本，打包三种架构 |
| `clean.cmd` | 清理 MSBuild 中间/调试目录，不影响 Release 二进制 |

# 系统要求
- **Windows 10 版本 2004（内部版本 19041）或更高** —— A2DP Sink 音频接收的基础要求（`AudioPlaybackConnection` API 自该版本起提供）
- **LE Audio 为可选增强**，需额外的 Windows 11 版本 22H2（内部版本 22621）或更高、蓝牙 5.2+ 适配器，以及支持 LE Audio 的设备
- 支持 A2DP Sink 的蓝牙适配器
- 支持 x86、x64 或 ARM64 任一 CPU 架构

# 致谢
Audio Bridge 是 Richard Yu（[@ysc3839](https://github.com/ysc3839)）的 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的衍生作品，上游项目以 MIT 许可证发布。上游确立了本项目所沿用的核心思路 —— 在常驻通知区域的 Win32 应用中驱动 Windows 公开的 `AudioPlaybackConnection` API。Audio Bridge 在这一基础上继续演进，新增了指数退避 + 抖动的自动重连、音频性能优化（MMCSS）、多设备记忆、开机自启处理，以及本文档。

`translate/` 下的本地化工具链同样基于同作者的 [translate](https://github.com/ysc3839/translate)（translate-toolkit 的衍生版本），仅在构建期使用。

同时感谢本项目所依赖的以下开源组件：

| 组件 | 许可证 | 用途 |
|---|---|---|
| [Windows Implementation Library (WIL)](https://github.com/microsoft/wil) | MIT | 错误处理与 RAII 封装 |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | MIT | Windows 运行时 API 投影 |
| [Inno Setup](https://jrsoftware.org/isinfo.php) | 自有许可 | 安装包打包（仅构建期） |
| GNU gettext（`xgettext`） | GPL | 字符串提取（仅构建期） |

> Inno Setup 与 gettext 仅在构建阶段作为工具被调用，其代码不会链接进分发的二进制文件，因此不影响本项目自身产物采用 MIT 许可证。

# 许可证
MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。
