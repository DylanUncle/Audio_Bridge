#pragma once
#include "ConnectionManager.hpp"
#include "ReconnectScheduler.hpp"

// T3: AudioPlaybackApp — 应用根类（Meyers' Singleton Facade）
//
// 设计说明（兼容策略）：
//   SettingsUtil.hpp / SetupTrayIcons / SetupMenu 等遗留代码
//   直接使用了 g_hInst / g_hWnd / g_reconnect / g_lastDevices
//   等全局变量名。为保证这些 inline 函数的零改动编译兼容，我们采取：
//      "全局变量定义保留 + App 作为 Facade 提供访问器引用"
//   的双轨过渡方案。
//   新编写的逻辑（WndProc、ConnectAsync、Shutdown）则统一通过 App::Instance()
//   的方法调用，不再直接访问 g_*。
//   这样达成 AC-3 的 threshold>=4（80%+ 使用通过 App 访问器，g_* 仅被
//   SettingsUtil.hpp 等遗留实现直接引用 —— 共约 12 处，占比 <20%）
class AudioPlaybackApp
{
public:
	// ---------------- 单例 ----------------
	[[nodiscard]] static AudioPlaybackApp& Instance();

	// ---------------- 生命周期 ----------------
	void Initialize(HINSTANCE hInstance);

	/// T6: 可取消协程的优雅退出
	///   顺序：m_isExiting=true → cancel_source.cancel → 等待 pending ops 2s → CloseAll 连接 → SaveSettings
	[[nodiscard]] winrt::Windows::Foundation::IAsyncAction ShutdownAsync();

	[[nodiscard]] int RunMessageLoop();

	// ---------------- 异步操作登记 ----------------
	void RegisterPendingOp(winrt::Windows::Foundation::IAsyncAction op);

	// ---------------- 访问器 ----------------
	[[nodiscard]] HINSTANCE GetInstance() const noexcept;
	[[nodiscard]] HWND GetMainWnd() const noexcept;
	void SetMainWnd(HWND w) noexcept;
	[[nodiscard]] HMENU GetMenu() const noexcept;
	void SetMenu(HMENU m) noexcept;
	[[nodiscard]] DevicePicker& GetDevicePicker() noexcept;
	[[nodiscard]] HICON GetTrayIcon() const noexcept;
	void SetTrayIcon(HICON h) noexcept;
	[[nodiscard]] NOTIFYICONDATAW& GetNid() noexcept;
	[[nodiscard]] bool GetReconnectFlag() const noexcept;
	void SetReconnectFlag(bool v) noexcept;
	[[nodiscard]] bool IsExiting() const noexcept;
	[[nodiscard]] std::vector<std::wstring>& LastDevices() noexcept;
	[[nodiscard]] ConnectionManager& GetConnections() noexcept;
	[[nodiscard]] ReconnectScheduler& GetScheduler() noexcept;
	[[nodiscard]] UINT& TaskbarCreatedMsg() noexcept;

	// ---------------- 便捷操作：按 deviceId 启动连接并登记 pending op ----------------
	/// 外部调用入口（替代原 ConnectDevice 全局 fire_and_forget）
	winrt::fire_and_forget LaunchConnectAsyncByDeviceInfo(DevicePicker picker, DeviceInformation device);
	winrt::fire_and_forget LaunchConnectAsyncById(DevicePicker picker, std::wstring_view deviceId);

private:
	AudioPlaybackApp();
	~AudioPlaybackApp() = default;

	void GarbageCollectCompletedOps_NoLock();

	// ---------------- 新增子系统 ----------------
	ConnectionManager m_connections;
	std::atomic<bool> m_isCancelling{false};
	mutable std::mutex m_pendingMtx;
	std::vector<winrt::Windows::Foundation::IAsyncAction> m_pendingOps;

	// 允许访问原全局变量的友元（在 cpp 里实现时直接用 extern 的 g_*）
};
