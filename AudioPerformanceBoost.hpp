#pragma once
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

// ==========================================================================
// P2-1: MMCSS 优先级细化
//
// 旧实现问题：
//   1) SetPriorityClass(HIGH) 提升整个进程（含 UI/定时器/托盘线程），
//      对"自身几乎不占 CPU 的控制层程序"毫无收益，反而挤占其他程序；
//   2) AvSetMmThreadCharacteristics 在协程调用线程上登记，但 C++/WinRT
//      协程 co_await 后会切换线程 —— AvRevertMmThreadCharacteristics
//      要求必须在登记线程上调用，跨线程撤销是未定义行为；
//   3) SetThreadExecutionState(ES_CONTINUOUS) 同样是 per-thread 状态，
//      协程切线程后设置/清除落在了不同线程上，执行状态锁不可靠。
//
// 新实现：进程保持 NORMAL_PRIORITY_CLASS，所有提升收敛到一个
// "专用 MMCSS 线程"（登记/撤销 100% 同线程，线程空闲等待零 CPU）：
//   活跃连接 0→1：创建线程 → 登记Pro Audio + AVRT_PRIORITY_HIGH
//                 + 执行状态锁（防睡眠断流）→ 等待退出事件
//   活跃连接 1→0：触发退出事件 → 线程内清除执行状态 + 撤销 MMCSS → join
// 对外接口（AddRef/Release/IsEnabled）保持不变，ConnectionManager 零改动。
// ==========================================================================
class AudioPerformanceBoost
{
public:
	AudioPerformanceBoost() = default;
	~AudioPerformanceBoost()
	{
		// 防止用户忘记 Disable：析构时若仍启用则强制还原
		if (m_enabled)
		{
			// Don't throw from destructor
			try { DisableImpl(); }
			CATCH_LOG();
		}
	}

	AudioPerformanceBoost(const AudioPerformanceBoost&) = delete;
	AudioPerformanceBoost& operator=(const AudioPerformanceBoost&) = delete;

	// 活跃连接数变为 1（第一个建立）时调用；多次调用等价于引用计数累加
	void AddRef()
	{
		std::lock_guard lock(m_mutex);
		++m_refCount;
		if (m_refCount == 1 && !m_enabled)
		{
			EnableImpl();
		}
	}

	// 活跃连接数减至 0 时调用
	void Release()
	{
		std::lock_guard lock(m_mutex);
		if (m_refCount > 0)
		{
			--m_refCount;
			if (m_refCount == 0 && m_enabled)
			{
				DisableImpl();
			}
		}
	}

	[[nodiscard]] bool IsEnabled() const
	{
		std::lock_guard lock(m_mutex);
		return m_enabled;
	}

private:
	// 在持锁时调用：创建专用 MMCSS 线程。任一步失败即回滚并清零计数。
	void EnableImpl()
	{
		// manual-reset 事件：活跃期间保持无信号，退出时一次性唤醒线程
		m_exitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!m_exitEvent)
		{
			LOG_LAST_ERROR();
			m_refCount = 0;
			return;
		}

		m_thread = CreateThread(nullptr, 0, ThreadProcStatic, this, 0, nullptr);
		if (!m_thread)
		{
			LOG_LAST_ERROR();
			CloseHandle(m_exitEvent);
			m_exitEvent = nullptr;
			m_refCount = 0;
			return;
		}

		m_enabled = true;
	}

	// 在持锁时调用：唤醒线程（线程内自行撤销 MMCSS/执行状态）后 join
	void DisableImpl()
	{
		if (m_exitEvent)
		{
			SetEvent(m_exitEvent);
		}
		if (m_thread)
		{
			// 线程只是在等事件，正常立即返回；超时兜底防挂死
			if (WaitForSingleObject(m_thread, 5000) != WAIT_OBJECT_0)
			{
				LOG_LAST_ERROR();
			}
			CloseHandle(m_thread);
			m_thread = nullptr;
		}
		if (m_exitEvent)
		{
			CloseHandle(m_exitEvent);
			m_exitEvent = nullptr;
		}
		m_refCount = 0;
		m_enabled = false;
	}

	static DWORD WINAPI ThreadProcStatic(LPVOID param)
	{
		static_cast<AudioPerformanceBoost*>(param)->ThreadProc();
		return 0;
	}

	// 专用 MMCSS 线程主体：登记与撤销严格同线程执行
	void ThreadProc()
	{
		// MMCSS "Pro Audio" 登记本线程 + 线程内优先级 HIGH
		DWORD taskIdx = 0;
		HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
		if (mmcss)
		{
			LOG_IF_WIN32_BOOL_FALSE(AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH));
		}
		else
		{
			LOG_LAST_ERROR();
		}

		// 执行状态锁 —— 防止活跃连接期间系统睡眠断流。
		// 注意：不使用 ES_DISPLAY_REQUIRED。蓝牙音频接收场景用户通常不需要屏幕
		// （如笔记本放一晚上歌），强制屏幕常亮是负体验；ES_SYSTEM_REQUIRED +
		// ES_AWAYMODE_REQUIRED 已足够保证系统不睡、后台媒体继续。
		const EXECUTION_STATE prevState = SetThreadExecutionState(
			ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
		if (prevState == 0)
		{
			LOG_LAST_ERROR();
		}

		// 活跃期间空闲等待（零 CPU），直到 DisableImpl 触发退出事件
		WaitForSingleObject(m_exitEvent, INFINITE);

		// 反序清理：执行状态 → MMCSS（与登记同一线程，保证有效）
		if (SetThreadExecutionState(ES_CONTINUOUS) == 0)
		{
			LOG_LAST_ERROR();
		}
		if (mmcss)
		{
			LOG_IF_WIN32_BOOL_FALSE(AvRevertMmThreadCharacteristics(mmcss));
		}
	}

	mutable std::mutex m_mutex;
	int m_refCount = 0;
	bool m_enabled = false;
	HANDLE m_thread = nullptr;
	HANDLE m_exitEvent = nullptr;
};
