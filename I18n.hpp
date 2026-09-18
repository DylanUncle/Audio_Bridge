#pragma once
#include "FnvHash.hpp"

// 修复：使用 inline 关键字避免 ODR（One Definition Rule）违规
// 之前每个包含此头文件的编译单元都会生成独立副本，导致内存浪费
inline std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

inline void LoadTranslateData()
{
	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), GetThreadUILanguage());
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				hashToStrMap.reserve(ymo->len);

				for (int i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(hResData) + offset);
					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

inline const wchar_t* Translate(const wchar_t* str)
{
	// Bug 修复：ptrToStrMap 缓存非线程安全。_() 被多线程调用：
	// 主线程（菜单/Toast）+ StateChanged 回调线程 + WinRT 协程 resume 线程。
	// 两个线程同时 find miss → 并发 emplace → unordered_map 数据竞争（UB）。
	// 翻译非热路径，加锁的开销可忽略。
	static std::mutex translateMtx;
	std::lock_guard lock(translateMtx);

	static std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

#define _(str) Translate(str)
