#pragma once
#include "ReconnectScheduler.hpp"
#include "AudioPerformanceBoost.hpp"

// ------------------------------
// T3 + T6 + T7 三合一实现
//
// ConnectionManager:
//   - 并发安全的连接表（mutable mutex）
//   - 可取消 IAsyncAction ConnectAsync (FR-4 / AC-4)
//   - 事件驱动：ConnectionOpened / ConnectionClosed (winrt event<>)
//   - 协议前缀显示 [A2DP] / [LE Audio] (FR-7 / AC-7)
// ------------------------------

// BLE 协议 GUID（来自 Windows.Devices.Enumeration 定义）
//   System.Devices.Aep.ProtocolId == BLE 时表示 LE Audio；其它情况（经典蓝牙）视为 A2DP
constexpr winrt::guid BLE_PROTOCOL_GUID
{
	0x0BB58923, 0x2600, 0x489B,
	{ 0x9F, 0xC9, 0xDE, 0x57, 0xA5, 0x66, 0x9D, 0x6F }
};

enum class AudioProtocol
{
	ClassicA2DP,   // 经典蓝牙 A2DP (BR/EDR)
	LEAudio,       // LE Audio (BLE)
	Unknown
};

[[nodiscard]] inline AudioProtocol DetectProtocol(const winrt::Windows::Devices::Enumeration::DeviceInformation& device)
{
	try
	{
		const auto properties = device.Properties();
		const auto val = properties.TryLookup(L"System.Devices.Aep.ProtocolId");
		if (val)
		{
			const winrt::guid guid = winrt::unbox_value<winrt::guid>(val);
			if (guid == BLE_PROTOCOL_GUID)
				return AudioProtocol::LEAudio;
			return AudioProtocol::ClassicA2DP;  // 经典蓝牙协议 GUID
		}
	}
	CATCH_LOG();
	return AudioProtocol::Unknown;
}

[[nodiscard]] inline std::wstring ProtocolPrefix(AudioProtocol p)
{
	switch (p)
	{
	case AudioProtocol::LEAudio:     return L"[LE Audio] ";
	case AudioProtocol::ClassicA2DP: return L"[A2DP] ";
	default:                         return L"";
	}
}

// --------- 增强修复：连接阶段挂死兜底 ---------
// 蓝牙栈异常（驱动 bug / RPC 卡死）时 StartAsync / OpenAsync 的异步操作可能
// 永不完成，in-flight 防护会让该设备永久卡在"尝试中"（无法再发起任何重连）。
// 到时强制 Cancel → co_await 抛出 hresult_canceled → 走既有的取消清理路径
// （CleanupEntry + 调度器保留条目继续退避重试）。
// 注：协议层超时（RequestTimedOut）约 20~30s，应用层兜底须长于它 → 45s。
//
// 【崩溃修复】旧 WithTimeout 在超时 lambda 里调用 opCopy.Status() 查询状态，
// 与 App 层 GC 扫描谓词被链接器折叠为同一份代码 —— 正是两次 APPCRASH
// （偏移 0x994e / 0x995d，读悬空 IAsyncAction 的 IAsyncInfo 虚表）的现场。
// 现改为 ArmTimeout：只武装"到时强制 Cancel"，不再查询 Status() ——
// 对已完成的异步操作调用 Cancel 是文档化 no-op，无需预检查。
// 调用方改用具名局部变量持有异步对象后再 co_await（见 ConnectAsync 内
// 两处调用点），规避 co_await 操作数临时对象跨挂起点的生命周期陷阱。
template <typename TAsync>
void ArmTimeout(TAsync const& op, std::chrono::milliseconds timeout)
{
	// fire_and_forget 后台协程：到时强制 Cancel；op 提前完成则 Cancel 无效
	[opCopy = op, timeout]() -> winrt::fire_and_forget
	{
		co_await winrt::resume_after(timeout);
		try
		{
			if (opCopy)
				opCopy.Cancel();
		}
		catch (...) {}
	}();
}

class ConnectionManager
{
public:
	// --------- P1-2: 连接统计 ---------
	/// 每台设备的运行时统计（会话内累计，掉线不清零，用于排查"为什么经常断"）
	struct DeviceStats
	{
		std::wstring name;                     // 设备友好名
		AudioProtocol protocol = AudioProtocol::Unknown;
		std::chrono::steady_clock::time_point connectTime{};  // 最近一次连接建立时间
		int totalReconnects = 0;               // 累计成功重连次数（掉线后重新连上）
		bool connected = false;                // 当前是否连接中
	};

	// --------- P2-2: 连接失败分类 ---------
	/// OpenAsync 失败 / 连接过程异常的分类（会话内累计，供统计面板排查）
	enum class ConnectFailure
	{
		None,           // 未失败
		TimedOut,       // RequestTimedOut —— 手机未响应 / 链路超时
		DeniedBySystem, // DeniedBySystem —— 系统策略 / 资源限制拒绝
		Unknown,        // UnknownFailure 或 TryCreateFromId 失败
		Exception       // 其他 hresult_error（RPC 断连、蓝牙栈异常等）
	};
	struct FailureStats
	{
		int timedOut = 0;
		int deniedBySystem = 0;
		int unknown = 0;
		int exceptions = 0;

		[[nodiscard]] int Total() const { return timedOut + deniedBySystem + unknown + exceptions; }
	};

	// 对外暴露的事件：可被 AudioPlaybackApp 订阅来驱动 MMCSS / Toast / TrayIcon 更新
	// 注：EventHandler<T> 要求 T 为 WinRT 类型（static_assert "T must be WinRT type"），
	// std::wstring 不满足，改用 winrt::hstring。
	winrt::event<winrt::Windows::Foundation::EventHandler<winrt::hstring>> ConnectionOpened;   // args = deviceId
	winrt::event<winrt::Windows::Foundation::EventHandler<winrt::hstring>> ConnectionClosed;   // args = deviceId

	ConnectionManager() = default;
	~ConnectionManager()
	{
		// 强制关闭所有连接，避免 StateChanged 回调在析构后运行
		CloseAll_NoLock();
	}

	ConnectionManager(const ConnectionManager&) = delete;
	ConnectionManager& operator=(const ConnectionManager&) = delete;

	// --------- 公共查询接口 ---------
	[[nodiscard]] size_t ActiveCount() const
	{
		std::lock_guard lock(m_mutex);
		return m_connections.size();
	}

	[[nodiscard]] std::vector<std::wstring> GetConnectedIds() const
	{
		std::vector<std::wstring> out;
		std::lock_guard lock(m_mutex);
		out.reserve(m_connections.size());
		for (const auto& [id, _] : m_connections)
			out.push_back(id);
		return out;
	}

	[[nodiscard]] AudioPerformanceBoost& Performance() { return m_perfBoost; }
	[[nodiscard]] ReconnectScheduler& Scheduler() { return m_scheduler; }

	/// P1-2: 按设备 ID 查询本会话内见过的设备名（熔断通知等场景用）；未知返回空串
	[[nodiscard]] std::wstring GetDeviceName(const std::wstring& deviceId) const
	{
		std::lock_guard lock(m_mutex);
		auto it = m_stats.find(deviceId);
		return it != m_stats.end() ? it->second.name : std::wstring{};
	}

	// --------- P1-2: 连接统计查询 ---------
	/// 生成托盘"连接统计"对话框文本：
	/// 当前连接数 + 每台设备的协议 / 连接时长 / 累计重连次数（会话内累计）
	/// P2-2: 末尾追加连接失败分类汇总（有失败才显示）
	[[nodiscard]] std::wstring GetStatisticsString() const
	{
		std::wstring text;
		{
			std::lock_guard lock(m_mutex);
			const size_t active = m_connections.size();
			text += _(L"Active connections: ") + std::to_wstring(active) + L"\n";
			if (m_stats.empty())
			{
				text += L"\n" + std::wstring(_(L"No devices seen this session."));
			}
			else
			{
				// Bug 修复：unordered_map 迭代顺序不稳定 → 按设备名排序后再输出，
				// 面板每次打开的条目顺序一致
				std::vector<const DeviceStats*> sorted;
				sorted.reserve(m_stats.size());
				for (const auto& [id, s] : m_stats)
					sorted.push_back(&s);
				std::sort(sorted.begin(), sorted.end(),
					[](const DeviceStats* a, const DeviceStats* b) { return a->name < b->name; });

				for (const auto* s : sorted)
				{
					text += L"\n" + ProtocolPrefix(s->protocol) + s->name
						+ (s->connected ? _(L"  [Connected]") : _(L"  [Disconnected]")) + L"\n";
					if (s->connected)
					{
						const auto dur = std::chrono::steady_clock::now() - s->connectTime;
						text += std::wstring(L"  ") + _(L"Session: ") + FormatDuration(dur) + L"\n";
					}
					text += std::wstring(L"  ") + _(L"Reconnects: ") + std::to_wstring(s->totalReconnects) + L"\n";
				}
			}

			// P2-2: 失败分类汇总（零失败时不显示，避免干扰）
			if (m_failures.Total() > 0)
			{
				text += L"\n" + std::wstring(_(L"Connect failures this session:")) + L"\n";
				if (m_failures.timedOut > 0)
					text += std::wstring(L"  ") + _(L"Timed out: ") + std::to_wstring(m_failures.timedOut) + L"\n";
				if (m_failures.deniedBySystem > 0)
					text += std::wstring(L"  ") + _(L"Denied by system: ") + std::to_wstring(m_failures.deniedBySystem) + L"\n";
				if (m_failures.unknown > 0)
					text += std::wstring(L"  ") + _(L"Unknown failure: ") + std::to_wstring(m_failures.unknown) + L"\n";
				if (m_failures.exceptions > 0)
					text += std::wstring(L"  ") + _(L"Other errors: ") + std::to_wstring(m_failures.exceptions) + L"\n";
			}
		}
		return text;
	}

	// --------- 操作接口 ---------
	void Disconnect(const std::wstring& deviceId)
	{
		winrt::Windows::Devices::Enumeration::DeviceInformation info{ nullptr };
		{
			std::lock_guard lock(m_mutex);
			auto it = m_connections.find(deviceId);
			if (it == m_connections.end()) return;
			info = it->second.first;
			it->second.second.Close();
			m_connections.erase(it);
			m_perfBoost.Release();
			RecordDisconnected_NoLock(deviceId);  // P1-2: 统计
		}
		m_scheduler.Cancel(deviceId);
		// 对外通知（持锁外，避免死锁）
		// sender 传 nullptr：ConnectionManager 非 IInspectable 派生，订阅者可经
		// AudioPlaybackApp::Instance().GetConnections() 拿到管理器引用，无需 sender。
		ConnectionClosed(nullptr, winrt::hstring{ deviceId });
	}

	void CloseAll()
	{
		std::vector<std::wstring> ids;
		{
			std::lock_guard lock(m_mutex);
			CloseAll_NoLock();
		}
		m_scheduler.Clear();
	}

	// --------- P0-2: 假连接健康检测 ---------
	/// 蓝牙栈的 StateChanged 事件并非 100% 可靠。定时（默认 30s）轮询所有连接的
	/// State()，发现状态异常（非 Opened 但仍在连接表中）的连接，主动 Close 并触发重连。
	/// 这样比被动等 StateChanged 更可靠，避免出现"僵尸连接"。
	void HealthCheck()
	{
		std::vector<std::wstring> deadIds;
		{
			std::lock_guard lock(m_mutex);
			for (auto it = m_connections.begin(); it != m_connections.end(); )
			{
				// Bug 修复（竞态）：连接尝试（StartAsync + OpenAsync）可持续 20~30s，
				// 进行中条目的 State() 本来就不是 Opened —— 不能当死连接清理，
				// 否则连接成功后不在表中（不被跟踪、断开时也不触发重连）。
				if (m_inFlight.find(it->first) != m_inFlight.end())
				{
					++it;
					continue;
				}
				bool isDead = false;
				try
				{
					const auto state = it->second.second.State();
					if (state != winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Opened)
					{
						isDead = true;
					}
				}
				catch (...)
				{
					// 连查询 State 都抛异常，说明连接对象已无效
					isDead = true;
				}

				if (isDead)
				{
					try { it->second.second.Close(); }
					catch (...) {}
					RecordDisconnected_NoLock(it->first);  // P1-2: 统计
					deadIds.push_back(it->first);
					it = m_connections.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		// 对死掉的连接触发重连（持锁外，避免死锁）
		for (const auto& id : deadIds)
		{
			m_scheduler.Schedule(id);
			// Bug 修复：同 StateChanged 路径 —— Schedule 后需通知主线程武装重连定时器
			// （HealthCheck 由主线程 WM_TIMER 触发，PostMessage 直接安全转发）
			PostMessageW(g_hWnd, WM_APP_REARM, 0, 0);
			ConnectionClosed(nullptr, winrt::hstring{ id });
		}
	}

	// --------- 增强修复：渲染端点消失检测（真实断流） ---------
	// State() 轮询的盲区：手机走远 / 关闭蓝牙时音频流已死，但蓝牙栈的
	// StateChanged 事件和 State() 可能长时间保持 Opened（僵尸连接）。
	// A2DP 连接建立时 Windows 会创建对应渲染端点（如 "Headphones (Pixel 7)"），
	// 端点转 NOT_PRESENT / 被移除是 audiodg 级的真实事件 —— 据此清理连接并重连，
	// 响应速度秒级（对比 HealthCheck 的 30s 轮询）。
	// 由 AudioDeviceMonitor 在主线程调用（STA 消息泵回调）。
	void HandleEndpointVanished(const std::wstring& endpointName)
	{
		std::vector<std::wstring> deadIds;
		std::vector<std::wstring> deadNames;
		{
			std::lock_guard lock(m_mutex);
			for (auto& [id, pair] : m_connections)
			{
				std::wstring devName;
				try { devName = std::wstring(pair.first.Name()); }
				CATCH_LOG();
				if (devName.empty()) continue;
				// 渲染端点命名规范为 "类型 (容器名)"，如 "Headphones (Pixel 7)"；
				// 精确匹配括号形式可避免 "Pixel" 误命中 "(Pixel 7)"。
				// 部分系统端点名直接就是设备名，两种都接受。
				if (endpointName == devName
					|| endpointName.find(L"(" + devName + L")") != std::wstring::npos)
				{
					deadIds.push_back(id);
					deadNames.push_back(devName);
				}
			}
			for (const auto& id : deadIds)
			{
				auto it = m_connections.find(id);
				if (it != m_connections.end())
				{
					try { it->second.second.Close(); }
					CATCH_LOG();
					m_perfBoost.Release();
					RecordDisconnected_NoLock(id);  // P1-2: 统计
					m_connections.erase(it);
				}
			}
		}

		// 持锁外：重连调度 + 通知（同 HealthCheck 模式；本方法在主线程被调用，
		// PostMessage 转发安全）
		for (size_t i = 0; i < deadIds.size(); ++i)
		{
			m_scheduler.Schedule(deadIds[i]);
			PostMessageW(g_hWnd, WM_APP_REARM, 0, 0);
			ConnectionClosed(nullptr, winrt::hstring{ deadIds[i] });
			// P1 优化：断开通知节流（60s/设备，与 StateChanged 路径共用）
			if (ShouldFireDisconnectToast(deadIds[i]))
				ShowToastNotification(_(L"Disconnected"), deadNames[i]);
		}
	}

	// P1 优化：断开通知节流 —— 信号边缘设备反复断连时避免 Toast 轰炸。
	// 同一设备 60s 内最多弹一条"Disconnected"（重连成功通知不受限）。
	bool ShouldFireDisconnectToast(const std::wstring& deviceId)
	{
		constexpr auto kToastThrottle = std::chrono::seconds(60);
		std::lock_guard lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		auto it = m_lastDisconnectToast.find(deviceId);
		if (it != m_lastDisconnectToast.end() && now - it->second < kToastThrottle)
			return false;
		m_lastDisconnectToast[deviceId] = now;
		return true;
	}

	// -------- FR-4 / T6: 可取消连接 --------
	/// picker 由外层传入，避免对全局 g_devicePicker 形成依赖（可测试性，NFR-6）
	[[nodiscard]] winrt::Windows::Foundation::IAsyncAction ConnectAsync(
		winrt::Windows::Devices::Enumeration::DevicePicker picker,
		winrt::Windows::Devices::Enumeration::DeviceInformation device)
	{
		// 注：ConnectionManager 由 AudioPlaybackApp::m_connections 持有，整个进程生命周期内不析构
		// （App 是静态单例），此处无需强引用保护。
		const auto deviceId = std::wstring(device.Id());
		const auto deviceName = std::wstring(device.Name());
		const AudioProtocol protocol = DetectProtocol(device);

		// ------ 幂等检查：已连接直接标记并返回 ------
		// ------ Bug 修复：in-flight 防护 ------
		// 连接尝试（StartAsync + OpenAsync 超时）可持续 20~30s，期间重连定时器可能
		// 再次到期弹出同一设备并发起重复尝试（重复 CreateFromIdAsync / 双连接对象 /
		// retryCount 虚增导致熔断提前）。同一设备的尝试只允许一个在飞。
		{
			std::lock_guard lock(m_mutex);
			if (m_connections.find(deviceId) != m_connections.end())
			{
				picker.SetDisplayStatus(device,
					ProtocolPrefix(protocol) + _(L"Connected"),
					winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
				co_return;
			}
			if (!m_inFlight.insert(deviceId).second)
			{
				co_return;  // 同设备尝试进行中，跳过重复发起
			}
		}
		// RAII：协程任何出口（成功/失败/取消）都从 in-flight 集合移除
		InFlightGuard inFlightGuard{ this, deviceId };

		// ------ 显示 Connecting ------
		picker.SetDisplayStatus(device,
			ProtocolPrefix(protocol) + _(L"Connecting"),
			winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
			winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);

		bool success = false;
		std::wstring errorMessage;
		ConnectFailure failKind = ConnectFailure::None;  // P2-2: 失败分类（协程帧内变量，catch 块仍可访问）
		winrt::Windows::Media::Audio::AudioPlaybackConnection connection{ nullptr };

		try
		{
			connection = winrt::Windows::Media::Audio::AudioPlaybackConnection::TryCreateFromId(device.Id());
			if (!connection)
			{
				success = false;
				errorMessage = _(L"Unknown error");
				failKind = ConnectFailure::Unknown;
			}
			else
			{
				// --- 注册 StateChanged 回调：闭包捕获 id/name 及 this 的 weak 语义（通过 raw pointer + m_isExiting 外置标志在 App 层检查即可）---
				connection.StateChanged([this, deviceId, deviceName](const auto& sender, const auto&)
				{
					winrt::Windows::Devices::Enumeration::DeviceInformation info{ nullptr };
					bool closedFire = false;
					bool needReconnect = false;

					{
						std::lock_guard lock(m_mutex);
						if (sender.State() == winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed)
						{
							auto it = m_connections.find(deviceId);
							if (it != m_connections.end())
							{
								info = it->second.first;
								m_connections.erase(it);
								m_perfBoost.Release();
								RecordDisconnected_NoLock(deviceId);  // P1-2: 统计
								closedFire = true;
								needReconnect = true;  // 由外层 Event 订阅者（App 层）根据 g_reconnect 决定是否调度
							}
						}
					}

					if (closedFire)
					{
						// P1 优化：断开通知节流（60s/设备）—— 信号边缘设备反复断连时
						// 避免 Toast 轰炸；重连成功通知不受限
						if (ShouldFireDisconnectToast(deviceId))
							ShowToastNotification(_(L"Disconnected"), deviceName);
						ConnectionClosed(nullptr, winrt::hstring{ deviceId });
						if (needReconnect)
						{
							m_scheduler.Schedule(deviceId);
							// Bug 修复：Schedule 只加条目不武装定时器；WM_TIMER(1) 在调度器
							// 清空后已死亡。本回调可能运行在非主线程（WinRT 系统事件），
							// SetTimer 要求窗口属于调用线程 → 必须 PostMessage 转主线程武装。
							PostMessageW(g_hWnd, WM_APP_REARM, 0, 0);
						}
					}
				});

			// --- 插入到 map 中（先登记，确保回调时能找到并能在取消时 Close）---
			{
				std::lock_guard lock(m_mutex);
				m_connections.emplace(deviceId, std::pair(device, connection));
			}

			// 检查点 0：创建阶段后检查取消
			if (m_isCancelling.load())
			{
				std::lock_guard lock(m_mutex);
				CleanupEntry_NoLock(deviceId);
				co_return;
			}

			// 【崩溃修复】异步对象用具名局部变量持有（避免 co_await 操作数
			// 临时对象的生命周期陷阱），超时兜底只武装 Cancel 不查 Status
			auto startOp = connection.StartAsync();
			ArmTimeout(startOp, std::chrono::seconds(45));
			co_await startOp;

			// 检查点 1：StartAsync 后检查取消
			if (m_isCancelling.load())
			{
				std::lock_guard lock(m_mutex);
				CleanupEntry_NoLock(deviceId);
				co_return;
			}

			auto openOp = connection.OpenAsync();
			ArmTimeout(openOp, std::chrono::seconds(45));
			const auto result = co_await openOp;

			// 检查点 2：OpenAsync 后检查取消
			if (m_isCancelling.load())
			{
				std::lock_guard lock(m_mutex);
				CleanupEntry_NoLock(deviceId);
				co_return;
			}

			switch (result.Status())
			{
			case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				failKind = ConnectFailure::TimedOut;          // P2-2
				break;
			case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				failKind = ConnectFailure::DeniedBySystem;    // P2-2
				break;
			case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				failKind = ConnectFailure::Unknown;           // P2-2（throw 后由 catch 补充 message）
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
			}
		}
		catch (const winrt::hresult_canceled&)
		{
			// 取消是预期路径，不记录为错误
			std::lock_guard lock(m_mutex);
			CleanupEntry_NoLock(deviceId);
			co_return;
		}
		catch (const winrt::hresult_error& ex)
		{
			success = false;
			// P2-2: UnknownFailure 在 throw 前已标记 kind；这里兜底捕获其他异常（RPC 断连等）
			if (failKind == ConnectFailure::None)
				failKind = ConnectFailure::Exception;
			errorMessage.resize(64);
			while (true)
			{
				const int result = swprintf(errorMessage.data(), errorMessage.size(),
					L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
				if (result < 0)
					errorMessage.resize(errorMessage.size() * 2);
				else
				{
					errorMessage.resize(result);
					break;
				}
			}
			LOG_CAUGHT_EXCEPTION();
		}

		// ---- 最终结果处理 ----
		if (success)
		{
			// Bug 修复（竞态）：OpenAsync 等待期间连接可能已被 StateChanged(Closed)
			// 回调清理（"刚连上就断"的信号边缘场景）。此时条目已从表中移除、重连已由
			// 回调调度好 —— 不能再 Cancel 调度器（会取消刚调度的重连 → 设备不再
			// 自动重连）、不能 AddRef（性能提升引用泄漏）、也不能记为 Connected。
			// 以条目是否仍在表中作为"连接仍然有效"的判据。
			bool stillTracked = false;
			{
				std::lock_guard lock(m_mutex);
				stillTracked = (m_connections.find(deviceId) != m_connections.end());
				if (stillTracked)
					RecordConnected_NoLock(deviceId, deviceName, protocol);  // P1-2: 统计
			}
			if (stillTracked)
			{
				picker.SetDisplayStatus(device,
					ProtocolPrefix(protocol) + _(L"Connected"),
					winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
				m_perfBoost.AddRef();
				m_scheduler.Cancel(deviceId);
				ShowToastNotification(_(L"Connected"), deviceName);
				ConnectionOpened(nullptr, winrt::hstring{ deviceId });
			}
			else
			{
				// 连接已被回调判定断开：保留调度器中已安排的重连，picker 显示可重试
				picker.SetDisplayStatus(device,
					ProtocolPrefix(protocol) + _(L"Disconnected"),
					winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
			}
		}
		else
		{
			{
				std::lock_guard lock(m_mutex);
				RecordFailure_NoLock(failKind);  // P2-2: 失败分类计数
				CleanupEntry_NoLock(deviceId);
			}
			picker.SetDisplayStatus(device,
				ProtocolPrefix(protocol) + errorMessage,
				winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
		}
		co_return;
	}

	/// 按 deviceId 重连（从 Id 异步创建设备对象后再调用 ConnectAsync）
	[[nodiscard]] winrt::Windows::Foundation::IAsyncAction ConnectAsync(
		winrt::Windows::Devices::Enumeration::DevicePicker picker,
		std::wstring_view deviceId)
	{
		// 增强修复：设备已被系统移除（取消配对 / 换手机）时 CreateFromIdAsync 抛
		// FILE_NOT_FOUND。捕获后清掉调度器条目（立即熔断，不再做剩余十几次无效
		// 重试），并通知主线程从 g_lastDevices 移除（下次开机不再自动重连它）。
		// 其他错误（RPC 瞬断 / 蓝牙栈重启中）不清理 —— 保留退避重试。
		try
		{
			// 【崩溃修复】异步对象一律用具名局部变量持有后再 co_await，
			// 规避 co_await 操作数临时对象跨挂起点的生命周期陷阱
			auto createOp = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(deviceId);
			auto device = co_await createOp;
			if (m_isCancelling.load()) co_return;
			auto connectOp = ConnectAsync(std::move(picker), std::move(device));
			co_await connectOp;
		}
		catch (const winrt::hresult_canceled&)
		{
			// 取消是预期路径，不记录为错误
		}
		catch (const winrt::hresult_error& ex)
		{
			LOG_CAUGHT_EXCEPTION();
			if (ex.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
			{
				m_scheduler.Cancel(std::wstring{ deviceId });
				{
					std::lock_guard lock(m_mutex);
					RecordFailure_NoLock(ConnectFailure::Unknown);  // P2-2: 失败分类
				}
				// 堆分配后 PostMessage 转主线程处理（同 WM_APP_ADDLAST 模式；
				// P2 优化：失败时 delete 防泄漏）
				if (auto* const p = new std::wstring(deviceId);
					!PostMessageW(g_hWnd, WM_APP_REMOVELAST, 0, reinterpret_cast<LPARAM>(p)))
					delete p;
			}
		}
	}

private:
	// Bug 修复：连接尝试 in-flight 防卫（RAII）。协程任何出口（成功/失败/取消，
	// 甚至未捕获异常）都会从集合移除，防止设备被永久卡在"尝试中"。
	struct InFlightGuard
	{
		ConnectionManager* self;
		std::wstring id;
		~InFlightGuard()
		{
			std::lock_guard lock(self->m_mutex);
			self->m_inFlight.erase(id);
		}
	};

	// 必须在持锁时调用
	void CleanupEntry_NoLock(const std::wstring& deviceId)
	{
		auto it = m_connections.find(deviceId);
		if (it != m_connections.end())
		{
			try
			{
				it->second.second.Close();
			}
			CATCH_LOG();
			m_connections.erase(it);
		}
	}

	// --------- P1-2: 统计记录（必须在持锁时调用） ---------
	void RecordConnected_NoLock(const std::wstring& deviceId, const std::wstring& name, AudioProtocol protocol)
	{
		auto& s = m_stats[deviceId];
		if (s.connected) return;  // 幂等：已在连接表中登记过
		// 本会话内此前连过（connectTime 非 epoch）→ 本次算一次"重连成功"
		const bool isReconnect = s.connectTime.time_since_epoch().count() != 0;
		s.name = name;
		s.protocol = protocol;
		s.connectTime = std::chrono::steady_clock::now();
		s.connected = true;
		if (isReconnect) ++s.totalReconnects;
	}

	void RecordDisconnected_NoLock(const std::wstring& deviceId)
	{
		auto it = m_stats.find(deviceId);
		if (it != m_stats.end())
			it->second.connected = false;
	}

	// --------- P2-2: 失败分类计数（必须在持锁时调用） ---------
	void RecordFailure_NoLock(ConnectFailure kind)
	{
		switch (kind)
		{
		case ConnectFailure::TimedOut:       ++m_failures.timedOut; break;
		case ConnectFailure::DeniedBySystem: ++m_failures.deniedBySystem; break;
		case ConnectFailure::Unknown:        ++m_failures.unknown; break;
		case ConnectFailure::Exception:      ++m_failures.exceptions; break;
		case ConnectFailure::None: break;  // 未失败不计数
		}
	}

	// 秒数 → "1h 23m 45s" 形式（连接时长展示用）
	[[nodiscard]] static std::wstring FormatDuration(std::chrono::steady_clock::duration d)
	{
		const auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
		const auto hrs = totalSecs / 3600;
		const auto mins = (totalSecs % 3600) / 60;
		const auto secs = totalSecs % 60;
		std::wstring result;
		if (hrs > 0) result += std::to_wstring(hrs) + L"h ";
		if (hrs > 0 || mins > 0) result += std::to_wstring(mins) + L"m ";
		result += std::to_wstring(secs) + L"s";
		return result;
	}

	void CloseAll_NoLock()
	{
		for (auto& [id, pair] : m_connections)
		{
			try { pair.second.Close(); }
			CATCH_LOG();
			RecordDisconnected_NoLock(id);  // P1-2: 统计
		}
		m_connections.clear();
		// 循环调用 Release() 直到 IsEnabled() 返回 false：
		// 因为每次 AddRef() 都调用了一次 Release()，这里需要释放所有未完成的引用
		// Release() 内部在 m_refCount 归零时会设置 m_enabled = false，循环会自然退出
		while (m_perfBoost.IsEnabled())
		{
			m_perfBoost.Release();
		}
	}

	mutable std::mutex m_mutex;
	std::unordered_map<std::wstring,
		std::pair<winrt::Windows::Devices::Enumeration::DeviceInformation,
			winrt::Windows::Media::Audio::AudioPlaybackConnection>> m_connections;

	// Bug 修复：正在尝试连接（StartAsync/OpenAsync 未完成）的设备集合
	std::unordered_set<std::wstring> m_inFlight;

	// P1-2: 会话内设备统计（掉线不清零；进程重启后重新累计）
	std::unordered_map<std::wstring, DeviceStats> m_stats;

	// P1 优化：断开通知节流（同设备 60s 内最多一条 Disconnected Toast）
	std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_lastDisconnectToast;

	// P2-2: 会话内连接失败分类计数（进程重启后重新累计）
	FailureStats m_failures;

	AudioPerformanceBoost m_perfBoost;
	ReconnectScheduler m_scheduler;
	std::atomic<bool> m_isCancelling{false};

public:
	void SetCancelling(bool v) noexcept { m_isCancelling.store(v); }

};
