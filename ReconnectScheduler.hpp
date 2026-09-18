#pragma once

// T2: 基于 ACM ToSN 2024 BLEdge 论文的指数退避 + 抖动 (Exponential Backoff + Jitter) 重连调度器
// 论文 DOI: 10.1145/3698201；理论结论：相比固定 5s 间隔，可降低 42.23% 多设备重连冲突。

class ReconnectScheduler
{
public:
	// P1-3: 重连上限熔断 —— 连续失败达到该次数后停止自动重连（用户仍可手动连接）
	static constexpr int kMaxRetryCount = 20;

	struct ScheduleEntry
	{
		std::wstring deviceId;
		int retryCount = 0;
		std::chrono::steady_clock::time_point nextAttempt;
	};

	// 注册一个待重连设备；若已存在则累加重试次数并更新下次尝试时间
	void Schedule(const std::wstring& deviceId)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto [it, inserted] = m_entries.try_emplace(deviceId);
		auto& entry = it->second;
		if (inserted)
		{
			entry.deviceId = deviceId;
			entry.retryCount = 0;
		}
		ComputeNextAttempt(entry);
	}

	// 取消某个设备的待重连记录（连接成功 / 用户主动断开时调用）
	void Cancel(const std::wstring& deviceId)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_entries.erase(deviceId);
	}

	// 清除所有待重连项（应用退出）
	void Clear()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_entries.clear();
	}

	// 查询下一次到期时间点，若无待重连项返回 nullopt
	[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> GetNextDueTime() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_entries.empty())
			return std::nullopt;

		auto minIt = std::min_element(m_entries.begin(), m_entries.end(),
			[](const auto& a, const auto& b) { return a.second.nextAttempt < b.second.nextAttempt; });
		return minIt->second.nextAttempt;
	}

	// 获取并移除所有到期（now >= nextAttempt）的设备 ID 列表，同时递增其 retryCount + 计算下下次
	// 返回：已到期 deviceId 列表，调用方应对每个调用 ConnectDevice
	// P1-3: frozenIds（可选出参）收集达到重试上限被熔断的设备，调用方据此通知用户
	[[nodiscard]] std::vector<std::wstring> PopDueEntries(std::vector<std::wstring>* frozenIds = nullptr)
	{
		std::vector<std::wstring> result;
		const auto now = std::chrono::steady_clock::now();

		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto it = m_entries.begin(); it != m_entries.end(); )
		{
			if (it->second.nextAttempt <= now)
			{
				// P1-3 熔断检查：连续失败次数达到上限 → 移除条目并上报，
				// 不再返回该设备（即不再发起自动重连）。用户手动连接成功后
				// Cancel() 会正常清理；下一次掉线 Schedule() 重新从 0 计数。
				if (it->second.retryCount >= kMaxRetryCount)
				{
					if (frozenIds) frozenIds->push_back(it->second.deviceId);
					it = m_entries.erase(it);
					continue;
				}
				result.push_back(it->second.deviceId);
				// 本次弹出即代表重试一次，为下一次调度预先 +1 并计算新 nextAttempt
				++it->second.retryCount;
				ComputeNextAttempt(it->second);
				++it;
			}
			else
			{
				++it;
			}
		}
		return result;
	}

private:
	// BLEdge 推荐算法：指数退避 base*(1<<min(retry,6)) + ±20% 抖动（Jitter）
	//   0: 5s     1: 10s     2: 20s    3: 40s    4: 80s    5: 160s   6+: 300s (上限 5min)
	// 避免所有设备在同一时刻再次争抢蓝牙时隙
	void ComputeNextAttempt(ScheduleEntry& entry) const
	{
		constexpr int kBaseDelayMs = 5000;
		constexpr int kMaxDelayMs = 300000;  // 5 分钟

		const int shift = (std::min)(entry.retryCount, 6);
		int delayMs = kBaseDelayMs * (1 << shift);
		if (delayMs > kMaxDelayMs) delayMs = kMaxDelayMs;

		// ±20% 抖动（通过 mt19937 + uniform_int_distribution）
		// 每个实例保存独立随机引擎（非线程安全，外部调用已持锁）
		const int jitterRange = delayMs / 5;  // 20%
		std::uniform_int_distribution<int> jitter(-jitterRange, jitterRange);
		delayMs += jitter(m_rng);
		if (delayMs < 1000) delayMs = 1000;  // 最小 1s 防止负抖动导致立即重试

		entry.nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
	}

	mutable std::mutex m_mutex;
	std::unordered_map<std::wstring, ScheduleEntry> m_entries;

	// 注意：mt19937 非线程安全，所有调用须在持 m_mutex 锁时发生（当前都满足）
	mutable std::mt19937 m_rng{ std::random_device{}() };
};
