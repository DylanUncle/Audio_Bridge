#pragma once

// ==========================================================================
// P1-1: 默认音频输出设备变更感知
//
// 背景：AudioPlaybackConnection 的音频由系统内部（audiodg.exe）路由到
// Windows 默认输出设备。用户切换默认设备（扬声器→耳机等）时系统自动迁移，
// 但用户毫无感知，容易误以为断连了。
//
// 方案：通过 IMMDeviceEnumerator::RegisterEndpointNotificationCallback 监听
// OnDefaultDeviceChanged(eRender, eMultimedia)，弹 Toast 告知新设备名。
//
// 线程模型：注册发生在主线程 STA，COM 回调经消息泵在主线程触发，
// 直接调用 ShowToastNotification（异步 WinRT）是安全的。
// ==========================================================================

class AudioDeviceMonitor : public IMMNotificationClient
{
public:
	AudioDeviceMonitor() = default;
	~AudioDeviceMonitor() { Shutdown(); }

	AudioDeviceMonitor(const AudioDeviceMonitor&) = delete;
	AudioDeviceMonitor& operator=(const AudioDeviceMonitor&) = delete;

	/// 注册默认设备变更回调（幂等）。返回是否成功。
	bool Initialize()
	{
		if (m_enumerator) return true;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(m_enumerator.put()));
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return false;
		}
		hr = m_enumerator->RegisterEndpointNotificationCallback(this);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			m_enumerator = nullptr;
			return false;
		}
		return true;
	}

	// 增强修复：渲染端点消失回调。A2DP 连接建立时 Windows 会创建对应的渲染
	// 端点；手机走远 / 关闭蓝牙时该端点转 NOT_PRESENT / 被移除 —— 这比蓝牙栈
	// StateChanged 事件和 State() 轮询都更真实（后两者可能长时间保持 Opened）。
	// 参数：消失端点的 friendly name（如 "Headphones (Pixel 7)"）。
	// 本类注册于主线程 STA，COM 回调经消息泵在主线程触发，
	// 回调内可直接操作连接表（ConnectionManager 自带锁）。
	std::function<void(const std::wstring& endpointName)> EndpointVanished;

	/// 注销回调（幂等；析构时自动调用）
	void Shutdown()
	{
		if (m_enumerator)
		{
			m_enumerator->UnregisterEndpointNotificationCallback(this);
			m_enumerator = nullptr;
		}
	}

	// ---------------- IMMNotificationClient ----------------
	STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) noexcept override
	{
		// 只关心渲染设备的"多媒体"默认设备 —— 蓝牙音频走该角色
		if (flow == eRender && role == eMultimedia && pwstrDefaultDeviceId && *pwstrDefaultDeviceId)
		{
			std::wstring message = _(L"Bluetooth audio will now play on the new default device.");
			const std::wstring name = GetDeviceFriendlyName(pwstrDefaultDeviceId);
			if (!name.empty())
				message = _(L"Bluetooth audio will now play on: ") + name;
			ShowToastNotification(_(L"Audio output switched"), message);
		}
		return S_OK;
	}

	STDMETHODIMP OnDeviceAdded(LPCWSTR) noexcept override { return S_OK; }

	// 增强修复：渲染端点被移除（对应 A2DP 音频流实际已死）
	STDMETHODIMP OnDeviceRemoved(LPCWSTR pwstrDeviceId) noexcept override
	{
		if (pwstrDeviceId)
			NotifyEndpointVanished(pwstrDeviceId);
		return S_OK;
	}

	// 增强修复：渲染端点状态变化。转 NOT_PRESENT / UNPLUGGED =
	// 音频流实际已死（手机走远 / 关闭蓝牙），据此触发真实断流检测
	// 注：NOT_PRESENT 常量在不同 SDK 中命名不一致（DEVICE_STATE_NOTPRESENT /
	// DEVICE_STATE_NOT_PRESENT），这些值属于稳定 API，直接用数值
	STDMETHODIMP OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) noexcept override
	{
		constexpr DWORD kDeviceStateNotPresent = 0x00000004;  // DEVICE_STATE_NOT(PRESENT)
		constexpr DWORD kDeviceStateUnplugged = 0x00000008;  // DEVICE_STATE_UNPLUGGED
		if (pwstrDeviceId
			&& (dwNewState == kDeviceStateNotPresent || dwNewState == kDeviceStateUnplugged))
		{
			NotifyEndpointVanished(pwstrDeviceId);
		}
		return S_OK;
	}
	STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) noexcept override { return S_OK; }

	// ---------------- IUnknown ----------------
	// 生命周期策略：本对象由全局/App 静态持有（存活至进程终止），
	// COM 的 AddRef/Release 不参与析构决策，返回固定值即可；
	// Shutdown() 先注销回调，之后 COM 不再持有引用。
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
	{
		if (!ppv) return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
		{
			*ppv = static_cast<IMMNotificationClient*>(this);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() noexcept override { return 2; }
	STDMETHODIMP_(ULONG) Release() noexcept override { return 1; }

private:
	/// 端点消失时查 friendly name 并触发回调（尽力而为：端点已消失时
	/// GetDevice / OpenPropertyStore 可能失败，失败则跳过 —— StateChanged
	/// 事件与 HealthCheck 轮询仍是兜底）
	void NotifyEndpointVanished(LPCWSTR deviceId) noexcept
	{
		if (!EndpointVanished) return;
		try
		{
			const std::wstring name = GetDeviceFriendlyName(deviceId);
			if (!name.empty())
				EndpointVanished(name);
		}
		CATCH_LOG();
	}

	/// 按设备 ID 查询友好名（如 "Speakers (Realtek Audio)"）；失败返回空串
	// P2 优化：复用 Initialize() 已创建的枚举器（COM 聚合对象，线程安全），
	// 不再每次调用 CoCreateInstance —— 低频路径，属顺手降开销。
	[[nodiscard]] std::wstring GetDeviceFriendlyName(LPCWSTR deviceId) const
	{
		if (!m_enumerator)
			return {};

		winrt::com_ptr<IMMDevice> device;
		if (FAILED(m_enumerator->GetDevice(deviceId, device.put())))
			return {};

		winrt::com_ptr<IPropertyStore> props;
		if (FAILED(device->OpenPropertyStore(STGM_READ, props.put())))
			return {};

		PROPVARIANT var{};
		std::wstring name;
		if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))
			&& var.vt == VT_LPWSTR && var.pwszVal)
		{
			name = var.pwszVal;
		}
		PropVariantClear(&var);
		return name;
	}

	winrt::com_ptr<IMMDeviceEnumerator> m_enumerator;
};
