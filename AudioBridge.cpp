#include "pch.h"
#include <unordered_set>
#include "AudioBridge.h"

// ==========================================================================
// 兼容层全局变量：保持旧代码的 extern 引用
// 注：连接状态统一在 ConnectionManager::m_connections 中管理
// ==========================================================================
HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
HMENU g_hMenu = nullptr;
DevicePicker g_devicePicker = nullptr;
HICON g_hTrayIcon = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
bool g_isExiting = false;
std::vector<std::wstring> g_lastDevices;

// ===== 退出安全修复：关键全局变量 =====
// 1) 单实例互斥锁：wWinMain 中持有的句柄，必须在退出前显式释放，
//    否则"进程虽然消息循环已挂死但 Mutex 仍被持有"会导致下一次启动完全无反应。
HANDLE g_hMutex = nullptr;
// 2) 自定义消息：ShutdownAsync 协程完成后，通过 PostMessage 切回主线程做 UI 销毁，
//    彻底避免"协程 continuation 可能在线程池恢复导致 PostQuitMessage 投错线程"的经典死锁。
constexpr UINT WM_APP_SHUTDOWNCOMPLETE = WM_APP + 0x100;

// ===== P0 优化：电源管理 + 设备热插拔 + 健康检测 =====
// 定时器 ID：1 已被重连调度器使用，2 用于连接健康检测，3 用于 picker flyout 居中
constexpr UINT_PTR IDT_HEALTHCHECK = 2;
constexpr UINT_PTR IDT_CENTERPICKER = 3;
constexpr UINT HEALTHCHECK_INTERVAL_MS = 30000;  // 30 秒一次
// 蓝牙适配器设备通知句柄（RegisterDeviceNotification 返回）
HDEVNOTIFY g_hDeviceNotify = nullptr;
// 【托盘修复】picker flyout 当前是否处于显示状态（Show 成功后置 true，
// DevicePickerDismissed 回调置 false）。用途：
//   a) 未显示过时跳过 Hide() —— 对 fresh picker 调 Hide 行为未文档化，
//      若抛异常会让后续 Show 永远不执行（"点了没反应"的一层保险）；
//   b) 连点防护的依据。
static bool s_pickerShown = false;
// 【居中修复】IDT_CENTERPICKER 定时器的重试计数（每次 Show 时清零，
// 见 TryCenterPickerFlyout 与 ShowDevicePickerAtTray）
static unsigned s_centerTicks = 0;
// 【居中修复】Show 前的可见窗口快照——用于系统级搜索 flyout：
// DevicePicker 的 flyout 窗口【不在本进程内】（由系统/XAML 运行时托管），
// 只能用"Show 后新出现的可见窗口"来定位。Show 前先快照所有可见顶层窗口，
// 定时器中找差集即为 flyout。
static std::unordered_set<HWND> s_preShowWindows;
// 前置声明：WndProc 的 WM_TIMER 分支先于函数定义处调用
void TryCenterPickerFlyout(HWND hWnd);
// 蓝牙 HCI 接口 GUID（文件作用域：注册通知与 WM_DEVICECHANGE 过滤共用）
// GUID_DEVINTERFACE_BLUETOOTH = {0850302A-B344-4fda-9BE9-90576B8D46F0}
constexpr GUID kGuidDevinterfaceBluetooth =
{ 0x0850302A, 0xB344, 0x4FDA, { 0x9B, 0xE9, 0x90, 0x57, 0x6B, 0x8D, 0x46, 0xF0 } };

// ===== P1-1: 默认音频输出设备变更监听（全局实例，进程生命周期存活） =====
AudioDeviceMonitor g_audioDeviceMonitor;

// ==========================================================================
// 前置声明
// ==========================================================================
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupMenu();
void ShowDevicePickerAtTray();
void SetupDevicePicker();
void SetupTrayIcons();
void UpdateNotifyIcon();
winrt::fire_and_forget FireShutdownAndDestroy();

// ==========================================================================
// AudioPlaybackApp — 类方法实现
// ==========================================================================

AudioPlaybackApp::AudioPlaybackApp() = default;

AudioPlaybackApp& AudioPlaybackApp::Instance()
{
	static AudioPlaybackApp s_instance;
	return s_instance;
}

void AudioPlaybackApp::Initialize(HINSTANCE hInstance)
{
	// 同步赋值到兼容层全局变量
	g_hInst = hInstance;
}

HINSTANCE AudioPlaybackApp::GetInstance() const noexcept { return g_hInst; }
HWND AudioPlaybackApp::GetMainWnd() const noexcept { return g_hWnd; }
void AudioPlaybackApp::SetMainWnd(HWND w) noexcept { g_hWnd = w; }
HMENU AudioPlaybackApp::GetMenu() const noexcept { return g_hMenu; }
void AudioPlaybackApp::SetMenu(HMENU m) noexcept { g_hMenu = m; }
DevicePicker& AudioPlaybackApp::GetDevicePicker() noexcept { return g_devicePicker; }
HICON AudioPlaybackApp::GetTrayIcon() const noexcept { return g_hTrayIcon; }
void AudioPlaybackApp::SetTrayIcon(HICON h) noexcept { g_hTrayIcon = h; }
NOTIFYICONDATAW& AudioPlaybackApp::GetNid() noexcept { return g_nid; }
bool AudioPlaybackApp::GetReconnectFlag() const noexcept { return g_reconnect; }
void AudioPlaybackApp::SetReconnectFlag(bool v) noexcept { g_reconnect = v; }
bool AudioPlaybackApp::IsExiting() const noexcept { return g_isExiting || m_isCancelling.load(); }
std::vector<std::wstring>& AudioPlaybackApp::LastDevices() noexcept { return g_lastDevices; }
ConnectionManager& AudioPlaybackApp::GetConnections() noexcept { return m_connections; }
ReconnectScheduler& AudioPlaybackApp::GetScheduler() noexcept { return m_connections.Scheduler(); }
UINT& AudioPlaybackApp::TaskbarCreatedMsg() noexcept { return WM_TASKBAR_CREATED; }

int AudioPlaybackApp::RunMessageLoop()
{
	// 【僵尸修复】GetMessageW 出错时返回 -1，旧代码把它当"真值"继续循环。
	// 只把 > 0（取到消息）当作继续条件；0（WM_QUIT）与 -1（错误）都退出。
	MSG msg{};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}

// ==========================================================================
// 【崩溃修复】pending ops 登记策略重构
//
// 旧设计（已删除）：RegisterPendingOp 把 IAsyncAction 存进向量，每次登记前
//   用 erase_if + op.Status() 全表扫描回收已完成项（GarbageCollectCompletedOps）。
//   对悬空元素调用 Status()（QI IAsyncInfo + get_Status 虚调用）就是两次
//   APPCRASH（偏移 0x994e / 0x995d，两个不同构建、同一路径）的崩溃现场。
//
// 新设计：Launch 协程启动时登记自身、任何出口自摘除 —— 自摘除只做指针
//   相等比较（零虚调用），永不对存储的元素调用任何 WinRT 方法。
//   ShutdownAsync 退出时快照清空并限时等待（快照持有自己的强引用，
//   不受 Launch 自摘除影响）。见 LaunchConnectAsyncByDeviceInfo 实现。
// ==========================================================================

// T6: 优雅退出异步流程
winrt::Windows::Foundation::IAsyncAction AudioPlaybackApp::ShutdownAsync()
{
	// 注：AudioPlaybackApp 是静态单例（Meyers Singleton），其生命周期持续到进程终止，
	// 无需 `get_strong()` 强引用保护。
	// 用 m_isCancelling (std::atomic<bool>) 做 exchange 实现"已退出则提前返回"原子语义；
	// g_isExiting 保持普通 bool 兼容旧代码读取（仅在主线程退出路径写入）。
	if (m_isCancelling.exchange(true)) co_return;
	g_isExiting = true;  // 同步旧代码读取 g_isExiting 处

	// 1. 触发取消标志（ConnectAsync 内所有检查点将被终止）
	m_isCancelling.store(true);
	m_connections.SetCancelling(true);

	// 2. 快照 pending ops，逐一等待最多 2s（总时长）
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	std::vector<winrt::Windows::Foundation::IAsyncAction> snapshot;
	{
		std::lock_guard lock(m_pendingMtx);
		snapshot = m_pendingOps;
		m_pendingOps.clear();
	}

	for (auto& op : snapshot)
	{
		if (!op) continue;
		// 【崩溃修复】不再调用 op.Status() 预检终态（两次 APPCRASH 均发生在
		// 该调用对悬空 IAsyncAction 的解引用上）。已完成的 op 在 co_await 内部
		// await_ready 快速路径立即返回，无需提前查询。

		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) break;
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		if (remaining.count() <= 0) break;

		// 【退出安全修复】单次 await 必须有独立的超时兜底：避免单个卡死的 op
		// 把整个 ShutdownAsync 永远挂住。策略：
		//   先用 fire_and_forget 启动"到时强制 Cancel"的后台协程，
		//   再正常 co_await op。要么 op 提前完成（Cancel 调用无效），
		//   要么超时后 Cancel 被触发 → op 抛出 hresult_canceled → catch 后立即前进。
		//   这样不依赖 when_any 的返回类型，兼容所有 C++/WinRT 版本。
		[op_copy = op, remaining]() -> winrt::fire_and_forget
		{
			co_await winrt::resume_after(remaining);
			try
			{
				// 【崩溃修复】不再查 Status()：对已完成的 op 调 Cancel 是文档化
				// no-op；对 Started 的 op 则强制取消（超时兜底）
				if (op_copy)
					op_copy.Cancel();
			}
			catch (...) {}
		}();
		try { co_await op; }
		catch (const winrt::hresult_canceled&) { /* expected (may come from timeout Cancel) */ }
		catch (...) { LOG_CAUGHT_EXCEPTION(); }
	}

	// 3. 关闭所有活动连接 + 卸载性能提升
	m_connections.CloseAll();

	// 4. 保存设置
	SaveSettings();

	co_return;
}

// 便捷 Launch 封装：启动连接并登记（保留 fire_and_forget 语义，便于 WndProc/回调 中使用）
// 【崩溃修复】登记策略重构：协程启动时把 op 登记进 m_pendingOps（向量持有
// 自己的强引用），任何出口（成功/失败/取消/异常）都自摘除 —— 自摘除只做
// 指针相等比较，零虚调用。取代旧的"RegisterPendingOp + Status() 全表扫描
// 回收"（扫描对悬空元素的 Status() 调用正是两次 APPCRASH 的崩溃现场）。
winrt::fire_and_forget AudioPlaybackApp::LaunchConnectAsyncByDeviceInfo(DevicePicker picker, DeviceInformation device)
{
	winrt::Windows::Foundation::IAsyncAction op{ nullptr };
	try
	{
		op = m_connections.ConnectAsync(std::move(picker), std::move(device));
		{
			std::lock_guard lock(m_pendingMtx);
			m_pendingOps.push_back(op);  // 拷贝登记：向量持有自己的强引用
		}
		co_await op;
	}
	catch (const winrt::hresult_canceled&) {}
	catch (...) { LOG_CAUGHT_EXCEPTION(); }

	// 自摘除（纯指针比较，不触碰对象）；ShutdownAsync 的快照持有自己的
	// 强引用，不受此处 erase 影响。
	if (op)
	{
		std::lock_guard lock(m_pendingMtx);
		std::erase_if(m_pendingOps, [&op](const auto& e) { return e == op; });
	}
}

winrt::fire_and_forget AudioPlaybackApp::LaunchConnectAsyncById(DevicePicker picker, std::wstring_view deviceId)
{
	winrt::Windows::Foundation::IAsyncAction op{ nullptr };
	try
	{
		op = m_connections.ConnectAsync(std::move(picker), deviceId);
		{
			std::lock_guard lock(m_pendingMtx);
			m_pendingOps.push_back(op);
		}
		co_await op;
	}
	catch (const winrt::hresult_canceled&) {}
	catch (...) { LOG_CAUGHT_EXCEPTION(); }

	if (op)
	{
		std::lock_guard lock(m_pendingMtx);
		std::erase_if(m_pendingOps, [&op](const auto& e) { return e == op; });
	}
}

// 为 SettingsUtil.hpp 提供：从 App 层查询当前活动连接 ID 列表（保持稳定顺序）
std::vector<std::wstring> GetCurrentlyConnectedDeviceIds()
{
	return AudioPlaybackApp::Instance().GetConnections().GetConnectedIds();
}

// ==========================================================================
// 兼容层：旧 ConnectDevice 全局函数重定向到 App
// ==========================================================================
winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	AudioPlaybackApp::Instance().LaunchConnectAsyncByDeviceInfo(std::move(picker), std::move(device));
	co_return;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	AudioPlaybackApp::Instance().LaunchConnectAsyncById(std::move(picker), deviceId);
	co_return;
}

// 重连调度入口：Schedule + 武装定时器。
// Bug 修复：此函数原先无人调用（死代码），导致调度器清空一次后定时器死亡、
// 后续掉线永不重连。现在被 P0-1（睡眠唤醒）/ P0-3（适配器重插）路径使用；
// StateChanged / HealthCheck 回调因可能跨线程，走 PostMessage(WM_APP_REARM) 等效路径。
void QueueAutoReconnect(std::wstring deviceId)
{
	auto& app = AudioPlaybackApp::Instance();
	if (!app.GetReconnectFlag() || app.IsExiting()) return;
	app.GetScheduler().Schedule(deviceId);

	// 按需启动定时器（根据下一次到期时间）
	if (const auto due = app.GetScheduler().GetNextDueTime(); due.has_value())
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			*due - std::chrono::steady_clock::now()).count();
		const UINT msUINT = (ms <= 0) ? 1 : (ms > UINT_MAX ? UINT_MAX : static_cast<UINT>(ms));
		SetTimer(app.GetMainWnd(), 1, msUINT, nullptr);
	}
}

// ==========================================================================
// Toast 通知（保留独立全局函数）
// ==========================================================================
void ShowToastNotification(const std::wstring& title, const std::wstring& message)
{
	try
	{
		auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(L"AudioBridge");
		auto toastXml = winrt::Windows::UI::Notifications::ToastNotificationManager::GetTemplateContent(
			winrt::Windows::UI::Notifications::ToastTemplateType::ToastText02);
		auto textNodes = toastXml.GetElementsByTagName(L"text");
		textNodes.Item(0).InnerText(title);
		textNodes.Item(1).InnerText(message);
		auto toast = winrt::Windows::UI::Notifications::ToastNotification(toastXml);
		notifier.Show(toast);
	}
	catch (const winrt::hresult_error&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

// ==========================================================================
// T6: ShutdownAsync 完成后触发主线程销毁流程
// 【退出安全修复】fire_and_forget 的 continuation 可能在线程池恢复，
// 因此：Shell_NotifyIcon / DestroyIcon / ReleaseMutex / DestroyWindow / PostQuitMessage
// 这些有严格线程亲和性要求的 API 绝不能直接在这里调用，
// 必须 PostMessage 切回主线程，在 WndProc 中执行。
// ==========================================================================
winrt::fire_and_forget FireShutdownAndDestroy()
{
	auto& app = AudioPlaybackApp::Instance();
	HWND hWnd = app.GetMainWnd();

	// 如果连窗口都没了（极端情况），直接尝试在当前线程 PostQuitMessage
	// 并指望进程退出；通常这是防御性代码。
	if (!IsWindow(hWnd))
	{
		PostQuitMessage(0);
		co_return;
	}

	// 即使协程卡死后用户再次点了退出（m_isCancelling.exchange 已经是 true），
	// ShutdownAsync 会立即 co_return，这里的 PostMessage 依然保证至少触发一次主线程销毁。
	co_await app.ShutdownAsync();

	// 切回主线程：发送自定义消息。
	// 注意：这里不使用 SendMessage（避免死锁），只使用 PostMessage。
	PostMessageW(hWnd, WM_APP_SHUTDOWNCOMPLETE, 0, 0);
}

// ==========================================================================
// wWinMain 入口
// ==========================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	auto& app = AudioPlaybackApp::Instance();
	app.Initialize(hInstance);

	// 显式设置当前进程的 AppUserModelID，与 setup.iss 开始菜单快捷方式的
	// AppUserModelID=AudioBridge 以及 ToastNotificationManager::CreateToastNotifier
	// 使用的 AUMID 保持一致。Windows 据此在开始菜单中找到匹配的快捷方式，
	// 并用其图标（exe 的 ICO 资源）作为 Toast 通知头部的应用图标；
	// 未设置时 Toast 头部不会显示图标。
	SetCurrentProcessExplicitAppUserModelID(L"AudioBridge");

	try
	{
		winrt::init_apartment(winrt::apartment_type::single_threaded);
	}
	catch (const winrt::hresult_error& e)
	{
		MessageBoxW(nullptr, e.message().c_str(), L"WinRT Init Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"AudioBridge_SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		// 单实例场景：用户再次点击桌面快捷方式 / 开始菜单图标。
		// 错误做法（旧）：ShowWindow(hExistingWnd, SW_SHOW) —— 把隐形主窗口暴露出来形成"空白窗口"。
		// 正确做法（新）：PostMessage 一条 WM_SHOWPICKER 给已运行实例，
		// 让它弹出 DevicePicker 对话框（与点击托盘图标行为完全一致）。
		HWND hExistingWnd = FindWindowW(L"AudioBridge", nullptr);
		if (hExistingWnd)
		{
			// 先确保目标窗口的消息循环正在处理（SetForegroundWindow 有助于 BringWindowToTop）
			SetForegroundWindow(hExistingWnd);
			PostMessageW(hExistingWnd, WM_SHOWPICKER, 0, 0);
		}
		CloseHandle(hMutex);
		return 0;
	}
	// 【退出安全修复】保存到全局，主线程最终销毁前会显式 Release + Close，
	// 确保即使消息循环已挂死也不会残留被持有的 Mutex 影响下次启动。
	g_hMutex = hMutex;

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioBridge",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	int windowWidth = 420;
	int windowHeight = 500;
	int posX = (screenWidth - windowWidth) / 2;
	int posY = (screenHeight - windowHeight) / 2;

	HWND hWnd = CreateWindowExW(
		WS_EX_TOOLWINDOW,  // 扩展样式：不在任务栏/Alt+Tab 中出现（纯消息宿主窗口）
		L"AudioBridge", _(L"Audio Bridge"),
		WS_OVERLAPPEDWINDOW,
		posX, posY, windowWidth, windowHeight, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(hWnd);
	app.SetMainWnd(hWnd);

	ShowWindow(hWnd, SW_HIDE);

	// ===== 【托盘修复：picker 弹不出来的根因】=====
	// DevicePicker 的 flyout 是 XAML UI，宿主线程必须先初始化 XAML Islands
	// 运行时（含 DispatcherQueue），否则 picker.Show() 抛异常（被
	// ShowDevicePickerAtTray 的 catch 静默吞掉 → "点了托盘没反应"）。
	// 上游 ysc3839/AudioPlaybackConnector 在此处创建 DesktopWindowXamlSource
	// 并 AttachToWindow —— 其副作用正是为宿主线程初始化 XAML 运行时；
	// 本程序不使用 XAML 菜单，无需 DesktopWindowXamlSource，
	// 用更轻量的 WindowsXamlManager::InitializeForCurrentThread() 达成同样效果。
	// 返回的管理器对象必须存活到进程结束（static 局部存储）。
	static winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager s_xamlManager{ nullptr };
	try
	{
		s_xamlManager =
			winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
	}
	CATCH_LOG();

	LoadTranslateData();
	LoadSettings();
	SetAutoStart(g_autoStart);
	SetupMenu();
	SetupDevicePicker();
	SetupTrayIcons();

	auto& nid = app.GetNid();
	nid.hWnd = hWnd;
	std::wstring tooltip = _(L"Audio Bridge");
	wcscpy_s(nid.szTip, tooltip.c_str());
	UpdateNotifyIcon();

	app.TaskbarCreatedMsg() = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(app.TaskbarCreatedMsg() == 0);

	// ===== P0-3: 注册蓝牙适配器设备通知（热插拔） =====
	DEV_BROADCAST_DEVICEINTERFACE dbcc = {
		.dbcc_size = sizeof(dbcc),
		.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE,
		.dbcc_classguid = kGuidDevinterfaceBluetooth
	};
	g_hDeviceNotify = RegisterDeviceNotificationW(hWnd, &dbcc, DEVICE_NOTIFY_WINDOW_HANDLE);
	LOG_LAST_ERROR_IF_NULL(g_hDeviceNotify);

	// ===== 增强修复：渲染端点消失检测（真实断流） =====
	// A2DP 连接的渲染端点转 NOT_PRESENT / 被移除时（手机走远/关蓝牙），
	// 蓝牙栈的 StateChanged / State() 可能仍显示 Opened（僵尸连接）。
	// 据此清理连接并立即调度重连（秒级响应，对比 HealthCheck 的 30s 轮询）。
	// 绑定须在 Initialize() 之前：注册回调后 COM 事件经消息泵到达，
	// 届时 lambda 必须已就位（赋值与回调同在主线程，无竞争）。
	g_audioDeviceMonitor.EndpointVanished = [](const std::wstring& endpointName)
	{
		AudioPlaybackApp::Instance().GetConnections().HandleEndpointVanished(endpointName);
	};

	// ===== P1-1: 监听默认音频输出设备变更（切换设备时弹 Toast 提示） =====
	(void)g_audioDeviceMonitor.Initialize();

	// ===== Bug 修复：g_lastDevices 运行期维护 =====
	// 连接成功（含自动重连成功）时把设备加入记忆列表，供睡眠唤醒 / 蓝牙适配器
	// 重插 / 下次开机时自动重连使用。ConnectionOpened 可能在非主线程触发
	// （WinRT 协程 resume 线程），堆分配后 PostMessage 转主线程处理（WM_APP_ADDLAST）。
	app.GetConnections().ConnectionOpened.add([](const auto&, const auto& args)
	{
		if (args.empty()) return;
		// P2 优化：PostMessage 失败（队列满/窗口销毁竞态）时 delete 防泄漏
		if (auto* const p = new std::wstring(args.c_str());
			!PostMessageW(g_hWnd, WM_APP_ADDLAST, 0, reinterpret_cast<LPARAM>(p)))
			delete p;
	});

	// ===== P0-2: 启动连接健康检测定时器（30s 轮询一次，检测僵尸连接） =====
	SetTimer(hWnd, IDT_HEALTHCHECK, HEALTHCHECK_INTERVAL_MS, nullptr);

	PostMessageW(hWnd, WM_CONNECTDEVICE, 0, 0);

	bool isStartupLaunch = (wcsstr(lpCmdLine, L"/startup") != nullptr);
	if (!isStartupLaunch)
	{
		ShowDevicePickerAtTray();
	}

	return app.RunMessageLoop();
}

// ==========================================================================
// WndProc 消息处理（统一通过 App::Instance 访问子系统）
// ==========================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto& app = AudioPlaybackApp::Instance();

	switch (message)
	{
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDM_SETTINGS:
		winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:bluetooth"));
		break;
	case IDM_STATS:
		// P1-2: 连接统计（当前连接数 / 每台设备协议 / 连接时长 / 累计重连）
		MessageBoxW(hWnd, app.GetConnections().GetStatisticsString().c_str(),
			_(L"Audio Bridge - Connection Statistics"), MB_OK | MB_ICONINFORMATION);
		break;
		case IDM_AUTOSTART:
		{
			const bool newVal = !g_autoStart;
			SetAutoStart(newVal);
			SaveSettings();
			break;
		}
		case IDM_EXIT:
		{
			const size_t active = app.GetConnections().ActiveCount();
			if (active == 0)
			{
				FireShutdownAndDestroy();
				break;
			}
			if (MessageBoxW(hWnd, _(L"All connections will be closed.\nExit anyway?"), _(L"Exit"), MB_YESNO | MB_ICONWARNING) == IDYES)
			{
				FireShutdownAndDestroy();
			}
		}
		break;
		}
		break;

	case WM_APP_REARM:
	{
		// Bug 修复：调度器新增条目（StateChanged 回调 / P0 唤醒 / 适配器重插）
		// 后重新武装重连定时器。定时器在调度器清空后已 KillTimer，必须在这里
		// 主线程重设。QueueAutoReconnect 里的 SetTimer 段就是做这件事，直接复用。
		if (!app.IsExiting() && app.GetReconnectFlag())
		{
			if (const auto due = app.GetScheduler().GetNextDueTime(); due.has_value())
			{
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					*due - std::chrono::steady_clock::now()).count();
				const UINT msUINT = (ms <= 0) ? 1 : (ms > UINT_MAX ? UINT_MAX : static_cast<UINT>(ms));
				SetTimer(hWnd, 1, msUINT, nullptr);
			}
		}
	}
	break;

	case WM_APP_ADDLAST:
	{
		// Bug 修复：连接成功后把设备加入 g_lastDevices（运行期维护）。
		// lParam = new std::wstring*（由 ConnectionOpened 订阅者堆分配，
		// 可能运行在非主线程，PostMessage 转主线程保证 g_lastDevices 主线程独占）
		auto* const id = reinterpret_cast<std::wstring*>(lParam);
		if (id)
		{
			std::erase(g_lastDevices, *id);   // 先移除保证去重 + 提到队尾（最近使用优先）
			g_lastDevices.push_back(*id);
			// P2 优化：MRU 截断 —— 换设备多的用户列表无限增长，
			// 开机全量重连 + 配置膨胀。保留最近 5 个。
			constexpr size_t kMaxLastDevices = 5;
			if (g_lastDevices.size() > kMaxLastDevices)
				g_lastDevices.erase(g_lastDevices.begin(), g_lastDevices.end() - kMaxLastDevices);
			delete id;
		}
	}
	break;

	case WM_APP_REMOVELAST:
	{
		// 增强修复：设备已被系统移除（取消配对 / 换手机，CreateFromIdAsync 抛
		// FILE_NOT_FOUND）—— 从记忆列表删除，下次开机不再对它做无效自动重连
		//（避免每次开机 20 次重试 + 熔断 toast）。
		// lParam = new std::wstring*（由 ConnectAsync(deviceId) 协程堆分配）
		auto* const id = reinterpret_cast<std::wstring*>(lParam);
		if (id)
		{
			std::erase(g_lastDevices, *id);
			delete id;
		}
	}
	break;

	case WM_APP_SHUTDOWNCOMPLETE:
	{
		// 【退出安全修复】由 FireShutdownAndDestroy 协程在完成 ShutdownAsync 后
		// 通过 PostMessage 切回主线程触发。所有与 UI / 消息循环 / Mutex 相关的销毁
		// 必须集中在这里执行，保证 100% 在主线程。
		// 1) 移除托盘图标 + 销毁图标资源
		Shell_NotifyIconW(NIM_DELETE, &app.GetNid());
		if (g_hTrayIcon) { DestroyIcon(g_hTrayIcon); g_hTrayIcon = nullptr; }
		// 2) 释放单实例互斥锁：先 ReleaseMutex（因为创建时 bInitialOwner=TRUE），
		//    再 CloseHandle，确保下次启动不会卡在"已存在但无响应"的状态。
		if (g_hMutex)
		{
			ReleaseMutex(g_hMutex);
			CloseHandle(g_hMutex);
			g_hMutex = nullptr;
		}
		// 3) 触发标准窗口销毁 → 走 WM_DESTROY → WM_DESTROY 中 PostQuitMessage
		//    （严格遵循 Win32 消息循环退出范式）
		DestroyWindow(hWnd);
		// 4) 【僵尸修复】DestroyWindow 内部（销毁 owned 的 picker flyout 窗口 /
		//    跨线程 SendMessage 派发）会运行嵌套消息泵，可能把 WM_DESTROY 里
		//    PostQuitMessage 投出的 WM_QUIT 消费掉 → 外层 GetMessageW 永远
		//    阻塞在 win32u 消息等待 → 进程变僵尸（窗口已亡但进程不死、Mutex
		//    残留，后续启动全部"点了没反应"）。DestroyWindow 返回后补发一次
		//    WM_QUIT，确保外层消息循环必定退出。多余的 WM_QUIT 无害（进程
		//    随即退出，不会有人再取）。
		PostQuitMessage(0);
	}
	break;

	case WM_DESTROY:
	{
		// 【退出安全修复】双重兜底：任何销毁路径都必须保证：
		//   托盘移除、Mutex 释放、PostQuitMessage。
		Shell_NotifyIconW(NIM_DELETE, &app.GetNid());
		if (g_hTrayIcon) { DestroyIcon(g_hTrayIcon); g_hTrayIcon = nullptr; }
		// P0: 停止健康检测定时器
		KillTimer(hWnd, IDT_HEALTHCHECK);
		// P0-3: 注销蓝牙设备通知
		if (g_hDeviceNotify)
		{
			UnregisterDeviceNotification(g_hDeviceNotify);
			g_hDeviceNotify = nullptr;
		}
		// P1-1: 注销默认音频设备变更监听
		g_audioDeviceMonitor.Shutdown();
		if (g_hMutex)
		{
			ReleaseMutex(g_hMutex);
			CloseHandle(g_hMutex);
			g_hMutex = nullptr;
		}
		// Win32 标准退出：在 WM_DESTROY（主线程）调用 PostQuitMessage，
		// 让 GetMessageW 返回 0，RunMessageLoop 正常结束返回 wWinMain，进程才会真正终止。
		PostQuitMessage(0);
	}
	break;

	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
			ShowDevicePickerAtTray();
			break;
		case WM_CONTEXTMENU:
		{
			HMENU hMenu = app.GetMenu();
			ModifyMenuW(hMenu, IDM_AUTOSTART, MF_STRING, IDM_AUTOSTART,
				g_autoStart ? _(L"[*] Start on Boot") : _(L"[ ] Start on Boot"));
			POINT pt;
			GetCursorPos(&pt);
			SetForegroundWindow(hWnd);
			TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
		}
		break;
		}
		break;

	case WM_CONNECTDEVICE:
		if (app.GetReconnectFlag())
		{
			// Bug 修复：原逻辑直接 ConnectDevice()，失败后无人重新调度 ——
			// 开机自启场景蓝牙栈往往未就绪，首次连接全部 TimedOut 后
			// 设备不在调度器中，定时器不武装，永不重试。
			// 改走 QueueAutoReconnect（Schedule + 武装定时器）：
			//   * 首次尝试延迟 5s，正好给蓝牙栈就绪时间
			//   * 失败后自动进入指数退避重试
			//   * P1-3 熔断（20 次）同样生效
			for (const auto& id : app.LastDevices())
			{
				if (app.IsExiting()) break;
				QueueAutoReconnect(id);
			}
			// 列表保留为"本会话已知设备"，供睡眠唤醒 / 适配器重插时重连
			//（运行期由 WM_APP_ADDLAST 维护）。
		}
		break;

	case WM_TIMER:
		if (wParam == 1)
		{
			KillTimer(hWnd, 1);
			if (app.IsExiting()) break;
			if (!app.GetReconnectFlag())
			{
				app.GetScheduler().Clear();
				break;
			}

			// 1) 取出所有到期项逐个连接（P1-3: 同时收集达到重试上限被熔断的设备）
			std::vector<std::wstring> frozenIds;
			auto dueIds = app.GetScheduler().PopDueEntries(&frozenIds);
			for (const auto& id : dueIds)
			{
				if (app.IsExiting()) break;
				ConnectDevice(app.GetDevicePicker(), id);
			}
			// P1-3: 熔断通知 —— 设备连续重连失败次数过多，停止自动重连并告知用户
			// （退出中不弹，避免退出瞬间冒通知）
			if (!app.IsExiting())
			{
				for (const auto& id : frozenIds)
				{
					const std::wstring name = app.GetConnections().GetDeviceName(id);
					const std::wstring failMsg = _(L"Reconnection failed too many times. Auto-reconnect stopped. Please reconnect manually.");
					ShowToastNotification(_(L"Auto-reconnect stopped"), name.empty() ? failMsg : name + L": " + failMsg);
				}
			}

			// 2) 还有未到期的 → 再按到期时间安排下一次定时器
			if (const auto next = app.GetScheduler().GetNextDueTime(); next.has_value())
			{
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					*next - std::chrono::steady_clock::now()).count();
				if (ms < 0) ms = 1;
				SetTimer(hWnd, 1, static_cast<UINT>(std::min<int64_t>(ms, UINT_MAX)), nullptr);
			}
		}
		else if (wParam == IDT_HEALTHCHECK)
		{
			// P0-2: 假连接健康检测 —— 蓝牙栈 StateChanged 不可靠，主动轮询所有连接状态
			app.GetConnections().HealthCheck();
		}
		else if (wParam == IDT_CENTERPICKER)
		{
			// 【居中修复】把已显示的 picker flyout 挪到工作区正中
			//（flyout 窗口由 XAML 异步创建，须轮询等待，见 TryCenterPickerFlyout）
			TryCenterPickerFlyout(hWnd);
		}
		break;

	// ======================================================================
	// P0-1: 休眠/睡眠唤醒处理
	// 系统睡眠后蓝牙连接大概率已断，但 StateChanged 可能不触发，导致僵尸连接。
	// 睡眠前主动 CloseAll，唤醒后重新调度重连。
	// ======================================================================
	case WM_POWERBROADCAST:
	{
		switch (wParam)
		{
		case PBT_APMSUSPEND:
			// 系统即将睡眠：主动关闭所有连接，避免唤醒后的僵尸状态
			app.GetConnections().CloseAll();
			break;
		case PBT_APMRESUMEAUTOMATIC:
		case PBT_APMRESUMESUSPEND:
			// 系统已唤醒：清理残留连接，重新调度重连
			// Bug 修复：改走 QueueAutoReconnect（Schedule + 武装定时器）——
			// 原先只调 Schedule()，定时器死亡后重连永远不会触发。
			// 另外：不再弹"正在重连"toast —— 现代待机(S0ix)会频繁短暂唤醒，
			// 每次都弹会轰炸；连接成功本身已有 Connected toast 反馈。
			app.GetConnections().CloseAll();
			if (app.GetReconnectFlag())
			{
				for (const auto& id : app.LastDevices())
					QueueAutoReconnect(id);
			}
			break;
		}
		return 0;
	}

	// ======================================================================
	// P0-3: 蓝牙适配器热插拔处理
	// 监听 WM_DEVICECHANGE，蓝牙适配器被移除时清理所有连接，重新插入时触发重连。
	// ======================================================================
	case WM_DEVICECHANGE:
	{
		// P3 优化：按 dbcc_classguid 精确过滤蓝牙 HCI 接口事件。
		// 仅检查 devicetype 时，其他设备接口类（USB 等）的到达/移除也会
		// 误触发"适配器检测到/移除"Toast 与全量重连。
		if (wParam == DBT_DEVICEREMOVECOMPLETE)
		{
			auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
			if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE
				&& reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE*>(hdr)->dbcc_classguid == kGuidDevinterfaceBluetooth)
			{
				// 蓝牙适配器被移除：清理所有连接
				app.GetConnections().CloseAll();
				ShowToastNotification(_(L"Bluetooth adapter removed"),
					_(L"All connections closed. Will retry when adapter returns."));
			}
		}
		else if (wParam == DBT_DEVICEARRIVAL)
		{
			auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
			if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE
				&& reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE*>(hdr)->dbcc_classguid == kGuidDevinterfaceBluetooth)
			{
				// 蓝牙适配器重新插入：立即触发一次重连
				// Bug 修复：改走 QueueAutoReconnect（Schedule + 武装定时器）
				if (app.GetReconnectFlag())
				{
					for (const auto& id : app.LastDevices())
						QueueAutoReconnect(id);
				}
				ShowToastNotification(_(L"Bluetooth adapter detected"),
					_(L"Attempting to reconnect..."));
			}
		}
		break;
	}

	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		return 0;

	case WM_SHOWPICKER:
		// 由单实例第二次启动（桌面快捷方式/开始菜单图标）触发，
		// 行为与点击托盘图标左键完全一致：弹出设备选择对话框。
		ShowDevicePickerAtTray();
		break;

	case WM_APP_QUITREQUEST:
		// 卸载 / 覆盖升级时由安装包（setup.iss）通过 PostMessageW 请求退出。
		// 走与托盘菜单"退出"相同的优雅清理流程（关闭音频连接、保存设置、
		// 移除托盘图标、释放单实例 Mutex），确保 exe 文件句柄被释放，
		// 安装包随后才能顺利删除 / 覆盖 Audio_Bridge.exe。
		// 注意：这里**不弹确认框** —— 卸载流程无人应答，弹框会阻塞安装包
		// 的等待逻辑，最终被其超时强杀（丢失清理）。
		FireShutdownAndDestroy();
		break;

	default:
		if (app.TaskbarCreatedMsg() && message == app.TaskbarCreatedMsg())
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

// ==========================================================================
// 辅助：DevicePicker 定位、菜单、托盘图标
// ==========================================================================
// 【Bug 修复：点托盘图标弹不出设备列表，连点几次后进程直接消失】
// 旧实现只调 picker.Show(光标位置-200, 400x400)，缺了四个关键步骤（对照上游
// ysc3839/AudioPlaybackConnector 验证过的成熟序列）：
//   1) Show 的 Rect 参数是 DIP（96 DPI 基准），本程序是 PerMonitorV2 DPI 感知
//      （见 AudioBridge.manifest），直接传物理像素在 125%/150% 缩放屏上
//      坐标偏差 1.25/1.5 倍，flyout 弹到屏幕外 → "点了没弹出界面"。
//   2) 任务栏本身是 topmost 窗口，宿主窗口不置顶时 flyout 会被任务栏盖住。
//   3) 宿主窗口未取得前台激活权，flyout 拿不到焦点，随即被系统自动关闭。
//   4) picker 处于"显示中"时再次调用 Show() 会抛 hresult_error，异常逃出
//      WndProc → std::terminate → 进程无声退出（"多点几次程序就没了"）。
// 修复：隐藏宿主窗口置顶铺屏 → SetForegroundWindow → Hide 后
// Show(工作区中心锚点, Placement::Above) → 定时器系统级搜索 flyout 并精确居中；
// （flyout 不在本进程内，用 Show 前快照 + 差集定位，详见 TryCenterPickerFlyout）

// 主窗口所在显示器的工作区（不含任务栏；多显示器下取主窗口所在屏）。
// 取不到显示器信息时退回主屏工作区。
RECT WorkAreaOf(HWND hWnd)
{
	HMONITOR const hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(mi) };
	if (hMon && GetMonitorInfoW(hMon, &mi))
		return mi.rcWork;
	RECT wa{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	return wa;
}

// 【居中修复】把 DevicePicker 的 flyout 窗口持续校正到工作区正中。
// DevicePicker.Show 只支持"相邻于锚点矩形"放置（Above/Below/Left/Right），
// 无法直接指定屏幕中心 —— Placement::Above 把 flyout 放在锚点上方，
// 锚点在屏幕中心时 flyout 落在上半区，垂直方向不居中。
// 【关键发现】flyout 窗口【不在本进程内】——它由系统/XAML 运行时托管
// （可能是 ApplicationFrameWindow 的子窗或独立进程的 CoreWindow），
// 所以不能用 PID 过滤 EnumWindows。改用 Show 前快照 + Show 后差集定位：
// s_preShowWindows 在 Show 前记录所有可见顶层窗口，定时器中找新出现的
// 可见窗口（尺寸 > 100px）即为 flyout。
// 【为什么必须"持续校正"而非移动一次】flyout 窗口的创建是异步的：
//   窗口刚可见时 XAML 布局/入场定位仍在进行，移动会被覆盖。定时器
//   （60ms）持续监视并反复校正到工作区正中，直至 picker 关闭或超时（3s）。
void TryCenterPickerFlyout(HWND hWnd)
{
	if (!s_pickerShown)
	{
		KillTimer(hWnd, IDT_CENTERPICKER);
		s_preShowWindows.clear();
		return;
	}
	// 安全上限（50 × 60ms ≈ 3s）：flyout 迟迟未创建就放弃
	if (++s_centerTicks > 50)
	{
		KillTimer(hWnd, IDT_CENTERPICKER);
		s_preShowWindows.clear();
		return;
	}

	// 系统级搜索：找 Show 后新出现的可见窗口（不在快照中）
	struct FindCtx { HWND found; } ctx{ nullptr };
	EnumWindows([](HWND h, LPARAM lp) -> BOOL
		{
			auto* const c = reinterpret_cast<FindCtx*>(lp);
			if (!IsWindowVisible(h)) return TRUE;
			// 排除 Show 前已存在的窗口
			if (s_preShowWindows.count(h) > 0) return TRUE;
			// 排除本进程的隐藏宿主窗口（虽然 IsWindowVisible 已过滤，双保险）
			DWORD pid = 0;
			GetWindowThreadProcessId(h, &pid);
			if (pid == GetCurrentProcessId()) return TRUE;
			// 尺寸合理（flyout 通常 200×300 ~ 600×800）
			RECT r{};
			if (!GetWindowRect(h, &r)) return TRUE;
			if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
			if (r.right - r.left > 1200 || r.bottom - r.top > 1200) return TRUE;
			c->found = h;
			return FALSE;
		}, reinterpret_cast<LPARAM>(&ctx));

	if (!ctx.found)
		return;  // flyout 尚未创建，下一拍再试

	RECT wr{};
	if (!GetWindowRect(ctx.found, &wr)
		|| wr.right - wr.left < 50 || wr.bottom - wr.top < 50)
		return;  // 尺寸未定（创建早期），下一拍再试

	const RECT wa = WorkAreaOf(hWnd);
	const int tx = wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2;
	const int ty = wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2;
	// 已在目标位（±1px）：不再 SetWindowPos，空转等下一拍（或停表）
	if (std::abs(wr.left - tx) <= 1 && std::abs(wr.top - ty) <= 1)
		return;
	SetWindowPos(ctx.found, nullptr, tx, ty, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 整段 try/catch 兜底，任何 picker 故障只记日志，绝不终止进程。
void ShowDevicePickerAtTray()
{
	auto& app = AudioPlaybackApp::Instance();
	// Bug 修复：正在退出（ShutdownAsync 协程运行中）时窗口即将销毁，
	// 不再弹出 picker；单实例二次启动（WM_SHOWPICKER）可能恰好撞上退出流程。
	if (app.IsExiting()) return;

	// ===== 【连点防护】=====
	//   1) 重入闸（同调用栈）：picker.Show() 的 flyout 内部若运行嵌套消息泵，
	//      二次托盘点击会被嵌套泵派发成重入调用 —— Hide/Show 重入可把 flyout
	//      内部状态机打入永久失效（此后 Show 每次失败 → "点了没反应"）。
	//   2) 节流闸（400ms）：Show 返回后 flyout 仍处于显示中，快速连续点击会
	//      反复 Hide/Show —— Hide 是异步生效的，紧随其后的 Show 撞上
	//      "仍显示中"会抛异常。
	static bool s_inCall = false;
	static std::chrono::steady_clock::time_point s_lastShow{};
	if (s_inCall) return;
	const auto now = std::chrono::steady_clock::now();
	if (now - s_lastShow < std::chrono::milliseconds(400)) return;
	struct CallGuard { ~CallGuard() { s_inCall = false; } } callGuard;
	s_inCall = true;
	s_lastShow = now;

	try
	{
		HWND const hWnd = app.GetMainWnd();

		// 1) 隐藏宿主窗口置顶并铺满屏幕：flyout 从属于宿主，宿主不置顶就画不过
		//    topmost 的任务栏。SWP_HIDEWINDOW 保证窗口保持隐藏，不会闪现。
		SetWindowPos(hWnd, HWND_TOPMOST, 0, 0,
			GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
		// 2) 抢占前台激活权，flyout 才不会被系统瞬间自动关闭
		SetForegroundWindow(hWnd);

		auto& picker = app.GetDevicePicker();
		// 3) 只在确曾显示过时 Hide：picker 仍"显示中"时直接 Show 会抛异常
		//    （连点崩溃的元凶）；而对从未显示过的 fresh picker 调 Hide 行为
		//    未文档化，若抛异常会让后面的 Show 永远不执行。s_pickerShown 由
		//    Show 成功置 true、DevicePickerDismissed 回调置 false，见全局定义。
		if (s_pickerShown)
			picker.Hide();

		// 4) 【居中修复】锚点矩形 = 工作区中心（不再依赖托盘图标位置）。
		//    Placement::Above 把 flyout 放在锚点上方——锚点在中心时 flyout
		//    落在上半区。为补偿：把锚点 Y 下移约半个 flyout 高度（≈工作区
		//    高度的 15%），使 flyout 底部在中心下方、顶部在中心上方，近似居中。
		//    定时器随后用 flyout 真实尺寸精确校正（见 TryCenterPickerFlyout）。
		const RECT wa = WorkAreaOf(hWnd);
		const UINT dpi = GetDpiForWindow(hWnd);
		const float scale = static_cast<float>(USER_DEFAULT_SCREEN_DPI) / static_cast<float>(dpi);
		// 锚点 Y 下移：补偿 Placement::Above 的偏移（flyout 底=锚点 Y）
		const float waH = static_cast<float>(wa.bottom - wa.top);
		const float offsetY = waH * 0.15f;  // ≈半个 flyout 高度
		const winrt::Windows::Foundation::Rect anchor{
			((wa.left + wa.right) * 0.5f) * scale - 1.0f,
			((wa.top + wa.bottom) * 0.5f + offsetY) * scale - 1.0f,
			2.0f, 2.0f };

		// 4b) Show 前快照所有可见顶层窗口——定时器用差集定位 flyout
		//    （flyout 不在本进程内，只能用"新出现的可见窗口"来识别）
		s_preShowWindows.clear();
		EnumWindows([](HWND h, LPARAM lp) -> BOOL
			{
				if (IsWindowVisible(h))
					reinterpret_cast<std::unordered_set<HWND>*>(lp)->insert(h);
				return TRUE;
			}, reinterpret_cast<LPARAM>(&s_preShowWindows));

		picker.Show(anchor, winrt::Windows::UI::Popups::Placement::Above);
		s_pickerShown = true;

		// 5) 启动居中定时器：flyout 窗口由 XAML 在 Show 之后异步创建，
		//    其布局/入场定位可能覆盖我们的移动 —— 定时器持续监视并反复
		//    校正到工作区正中，直至 picker 关闭或超时（见 TryCenterPickerFlyout）。
		s_centerTicks = 0;
		SetTimer(hWnd, IDT_CENTERPICKER, 60, nullptr);
	}
	catch (...)
	{
		// 兜底：托盘常驻工具绝不能因 UI 故障而退出
		LOG_CAUGHT_EXCEPTION();
	}
}

void SetupMenu()
{
	HMENU hMenu = CreatePopupMenu();
	AppendMenuW(hMenu, MF_STRING, IDM_AUTOSTART,
		g_autoStart ? _(L"[*] Start on Boot") : _(L"[ ] Start on Boot"));
	AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, _(L"Bluetooth Settings"));
	// P1-2: 连接统计（当前连接数 / 协议 / 连接时长 / 累计重连）
	AppendMenuW(hMenu, MF_STRING, IDM_STATS, _(L"Connection Statistics"));
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(hMenu, MF_STRING, IDM_EXIT, _(L"Exit"));
	AudioPlaybackApp::Instance().SetMenu(hMenu);
}

void SetupDevicePicker()
{
	auto& app = AudioPlaybackApp::Instance();
	DevicePicker picker;
	winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(app.GetMainWnd()));

	picker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());

	// flyout 关闭（用户点击别处 / 选择设备 / 断开）时清除显示状态标志，
	// 供 ShowDevicePickerAtTray 的条件 Hide 与连点防护使用
	picker.DevicePickerDismissed([](const auto&, const auto&) {
		s_pickerShown = false;
		s_preShowWindows.clear();
	});

	picker.DeviceSelected([](const auto& sender, const auto& args)
	{
		ConnectDevice(sender, args.SelectedDevice());
	});

	picker.DisconnectButtonClicked([](const auto& sender, const auto& args)
	{
		const auto device = args.Device();
		// device.Id() 返回 winrt::hstring，需显式转 std::wstring（hstring 无隐式转换）
		const std::wstring deviceId{ device.Id().c_str() };
		auto& app = AudioPlaybackApp::Instance();
		app.GetConnections().Disconnect(deviceId);
		app.GetScheduler().Cancel(deviceId);
		// Bug 修复：用户主动断开 → 从记忆列表移除（下次开机 / 睡眠唤醒不再自动重连它）
		std::erase(g_lastDevices, deviceId);
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});

	app.GetDevicePicker() = std::move(picker);
}

void SetupTrayIcons()
{
	auto& app = AudioPlaybackApp::Instance();
	const HINSTANCE hInst = app.GetInstance();

	// 托盘图标直接从 ICO 资源（IDI_AUDIOPLAYBACKCONNECTOR）加载——
	// 彩色圆角矩形 LOGO 在浅色/深色任务栏下都有足够对比度，单图标即可。
	const int cx = GetSystemMetrics(SM_CXSMICON);
	const int cy = GetSystemMetrics(SM_CYSMICON);
	HICON hTray = static_cast<HICON>(LoadImageW(
		hInst, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR), IMAGE_ICON,
		cx, cy, LR_DEFAULTCOLOR));
	FAIL_FAST_LAST_ERROR_IF_NULL(hTray);

	app.SetTrayIcon(hTray);
}

void UpdateNotifyIcon()
{
	auto& app = AudioPlaybackApp::Instance();
	auto& nid = app.GetNid();
	nid.hIcon = app.GetTrayIcon();

	if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
