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
// 定时器 ID：1 已被重连调度器使用，2 用于连接健康检测
constexpr UINT_PTR IDT_HEALTHCHECK = 2;
constexpr UINT HEALTHCHECK_INTERVAL_MS = 30000;  // 30 秒一次
// 蓝牙适配器设备通知句柄（RegisterDeviceNotification 返回）
HDEVNOTIFY g_hDeviceNotify = nullptr;
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
void ShowDevicePickerAtCursor();
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
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}

void AudioPlaybackApp::RegisterPendingOp(winrt::Windows::Foundation::IAsyncAction op)
{
	if (!op) return;
	std::lock_guard lock(m_pendingMtx);
	GarbageCollectCompletedOps_NoLock();
	m_pendingOps.push_back(std::move(op));
}

void AudioPlaybackApp::GarbageCollectCompletedOps_NoLock()
{
	std::erase_if(m_pendingOps, [](const auto& op) {
		return !op || op.Status() != winrt::Windows::Foundation::AsyncStatus::Started;
	});
}

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
		const auto status = op.Status();
		if (status == winrt::Windows::Foundation::AsyncStatus::Completed ||
			status == winrt::Windows::Foundation::AsyncStatus::Error ||
			status == winrt::Windows::Foundation::AsyncStatus::Canceled)
			continue;

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
				if (op_copy && op_copy.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
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
winrt::fire_and_forget AudioPlaybackApp::LaunchConnectAsyncByDeviceInfo(DevicePicker picker, DeviceInformation device)
{
	try
	{
		auto op = m_connections.ConnectAsync(std::move(picker), std::move(device));
		RegisterPendingOp(op);
		co_await op;
	}
	catch (const winrt::hresult_canceled&) {}
	catch (...) { LOG_CAUGHT_EXCEPTION(); }
}

winrt::fire_and_forget AudioPlaybackApp::LaunchConnectAsyncById(DevicePicker picker, std::wstring_view deviceId)
{
	try
	{
		auto op = m_connections.ConnectAsync(std::move(picker), deviceId);
		RegisterPendingOp(op);
		co_await op;
	}
	catch (const winrt::hresult_canceled&) {}
	catch (...) { LOG_CAUGHT_EXCEPTION(); }
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
		ShowDevicePickerAtCursor();
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
			ShowDevicePickerAtCursor();
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
		ShowDevicePickerAtCursor();
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
void ShowDevicePickerAtCursor()
{
	// Bug 修复：正在退出（ShutdownAsync 协程运行中）时窗口即将销毁，
	// 不再弹出 picker；单实例二次启动（WM_SHOWPICKER）可能恰好撞上退出流程。
	if (AudioPlaybackApp::Instance().IsExiting()) return;

	POINT pt;
	GetCursorPos(&pt);
	// Bug 修复：光标在屏幕左上角时 pt-200 产生负坐标 → 钳制到 0
	// （(std::max) 加括号规避 windows.h 的 max 宏，与 ReconnectScheduler 一致）
	const float x = (std::max)(0.0f, static_cast<float>(pt.x) - 200.0f);
	const float y = (std::max)(0.0f, static_cast<float>(pt.y) - 200.0f);
	winrt::Windows::Foundation::Rect rect{ x, y, 400, 400 };
	AudioPlaybackApp::Instance().GetDevicePicker().Show(rect);
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

	picker.DevicePickerDismissed([](const auto&, const auto&) {});

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
