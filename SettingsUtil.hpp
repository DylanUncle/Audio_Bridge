#pragma once

#include <unordered_set>
#include <KnownFolders.h>
#include <shlobj_core.h>
#include <shobjidl.h>
#pragma comment(lib, "shell32.lib")

constexpr auto CONFIG_NAME = L"Audio_Bridge.json";
constexpr auto CONFIG_SUBDIR = L"Audio_Bridge";
constexpr auto BUFFER_SIZE = 4096;
constexpr auto AUTOSTART_VALUE = L"Audio_Bridge";
// 注意：必须与 setup.iss [Icons] 段的 {userstartup} 快捷方式名称保持一致
// setup.iss 创建的是 "Audio Bridge"（空格），这里也必须用空格
constexpr auto AUTOSTART_LINK_NAME = L"Audio Bridge.lnk";
// 旧版本代码使用下划线名称，禁用自启时一并清理，避免残留
constexpr auto AUTOSTART_LINK_NAME_LEGACY = L"Audio_Bridge.lnk";
constexpr auto RUN_KEY_PATH = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";

// 由 AudioBridge.cpp 提供的辅助查询（避免 SettingsUtil 直接依赖 ConnectionManager）
[[nodiscard]] extern std::vector<std::wstring> GetCurrentlyConnectedDeviceIds();

// 修复：全局变量加 inline 避免 ODR 违规
inline bool g_autoStart = false;

// ------------------------------
// T1: 合规配置存储路径
// ------------------------------

/// 获取配置文件的标准路径：%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json
/// 若目录不存在则自动创建，失败抛出 HRESULT 异常。
[[nodiscard]] inline fs::path GetConfigPath()
{
	wil::unique_cotaskmem_string localAppData;
	THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData));

	fs::path dir = fs::path(localAppData.get()) / CONFIG_SUBDIR;
	std::error_code ec;
	fs::create_directories(dir, ec);
	// create_directories 失败时（根只读/权限）不抛，允许下次在保存时通过 CreateFileW 再报错一次

	return dir / CONFIG_NAME;
}

/// 获取旧版本配置路径（exe 同目录下的 JSON），仅用于一次性迁移读取
[[nodiscard]] inline fs::path GetLegacyConfigPath()
{
	return GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
}

/// 将内存中的 JSON 字符串保存到指定路径（底层工具函数，供 Load/Save 复用）
inline void WriteJsonToFile(const fs::path& path, std::string_view utf8)
{
	// P1 优化：原子写入 —— 先写临时文件并刷盘，成功后再原子替换正式文件。
	// 直接 CREATE_ALWAYS 覆盖时若写入中途崩溃/断电，配置文件会截断损坏，
	// 下次启动全部设置丢失。临时文件后缀固定（*.tmp），残留时可被识别清理。
	const std::wstring tmpPath = path.wstring() + L".tmp";

	wil::unique_hfile hFile(CreateFileW(tmpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	THROW_LAST_ERROR_IF(!hFile);

	DWORD written = 0;
	THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
	THROW_HR_IF(E_FAIL, written != utf8.size());

	// 确保数据真正落盘后再替换（MoveFileEx 不等待文件系统缓存）
	THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(hFile.get()));
	hFile.reset();  // 替换前必须关闭句柄

	if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		(void)DeleteFileW(tmpPath.c_str());  // 替换失败清理残留临时文件
		THROW_LAST_ERROR();
	}
}

/// 从文件读取为 UTF-8 字符串，文件不存在返回空 optional
[[nodiscard]] inline std::optional<std::string> ReadFileUtf8(const fs::path& path)
{
	wil::unique_hfile hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!hFile)
	{
		const DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
			return std::nullopt;
		THROW_WIN32(err);
	}

	std::string string;
	while (true)
	{
		const size_t size = string.size();
		string.resize(size + BUFFER_SIZE);
		DWORD read = 0;
		THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, BUFFER_SIZE, &read, nullptr));
		string.resize(size + read);
		if (read == 0)
			break;
	}
	return string;
}

// 修复：以下函数加 inline 关键字避免 ODR 违规
inline void DefaultSettings()
{
	g_reconnect = true;
	g_autoStart = false;
	g_lastDevices.clear();
}

/// Bug 修复：检测系统当前自启状态（注册表 Run 值或 Startup 快捷方式存在）。
/// 首次安装场景（无配置文件）用它初始化 g_autoStart：
/// 用户在安装向导勾选了 "Start on Boot"（setup.iss 写注册表 + 创建 lnk）后，
/// 旧逻辑 DefaultSettings() 强制 g_autoStart=false，wWinMain 随即 SetAutoStart(false)
/// 会把安装包刚创建的自启入口全部清除 —— 用户勾选的自启开机即失效。
[[nodiscard]] inline bool IsAutoStartActive()
{
	// 1) 注册表 Run 值
	{
		wil::unique_hkey hKey;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_QUERY_VALUE,
			hKey.addressof()) == ERROR_SUCCESS)
		{
			wchar_t buf[MAX_PATH];
			DWORD cb = sizeof(buf);
			if (RegQueryValueExW(hKey.get(), AUTOSTART_VALUE, nullptr, nullptr,
				reinterpret_cast<BYTE*>(buf), &cb) == ERROR_SUCCESS)
			{
				return true;
			}
		}
	}
	// 2) Startup 文件夹快捷方式（当前名 + 旧版下划线名）
	try
	{
		wil::unique_cotaskmem_string startup;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_DEFAULT, nullptr, &startup)))
		{
			const fs::path dir(startup.get());
			std::error_code ec;
			if (fs::exists(dir / AUTOSTART_LINK_NAME, ec) ||
				fs::exists(dir / AUTOSTART_LINK_NAME_LEGACY, ec))
			{
				return true;
			}
		}
	}
	CATCH_LOG();
	return false;
}

inline void LoadSettings()
{
	try
	{
		DefaultSettings();

		const fs::path newPath = GetConfigPath();
		std::optional<std::string> rawContent;

		// 1. 优先尝试新路径
		rawContent = ReadFileUtf8(newPath);

		// 2. 迁移逻辑：新路径不存在但旧 exe 同目录存在可读配置 → 导入后写入新路径，保留旧文件作备份
		if (!rawContent.has_value())
		{
			const fs::path legacyPath = GetLegacyConfigPath();
			auto legacy = ReadFileUtf8(legacyPath);
			if (legacy.has_value())
			{
				// 直接使用旧配置内容当作本次加载来源；同时写入新路径完成迁移
				rawContent = std::move(legacy);
				try
				{
					WriteJsonToFile(newPath, *rawContent);
					// 迁移完成后不删除旧文件（保留作用户可恢复的 fallback）
				}
				CATCH_LOG(); // 迁移保存失败只记日志，加载流程仍能继续
			}
		}

		if (!rawContent.has_value())
		{
			// 新老路径都不存在 → 首次安装：从系统现状推断自启状态
			// （安装向导勾选 = 用户想要自启），其余用默认值。
			g_autoStart = IsAutoStartActive();
			return;
		}

		const std::wstring utf16 = Utf8ToUtf16(*rawContent);
		const auto jsonObj = JsonObject::Parse(utf16);

		if (jsonObj.HasKey(L"reconnect"))
			g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();
		else
			g_reconnect = true;

		if (jsonObj.HasKey(L"autoStart"))
			g_autoStart = jsonObj.Lookup(L"autoStart").GetBoolean();

		if (jsonObj.HasKey(L"lastDevices"))
		{
			auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
			g_lastDevices.reserve(lastDevices.Size());
			for (const auto& i : lastDevices)
			{
				if (i.ValueType() == JsonValueType::String)
					g_lastDevices.push_back(std::wstring(i.GetString()));
			}
		}
	}
	CATCH_LOG();
}

// ------------------------------
// T4: 自启动双路径回退（注册表优先 + Startup 快捷方式备用）
// ------------------------------

/// 获取 Startup 已知文件夹路径
[[nodiscard]] inline fs::path GetStartupFolderPath()
{
	wil::unique_cotaskmem_string startup;
	THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &startup));
	return fs::path(startup.get()) / AUTOSTART_LINK_NAME;
}

/// 在 Startup 文件夹创建 ln 快捷方式；返回 true 表示成功
[[nodiscard]] inline bool CreateStartupShortcut()
{
	try
	{
		wchar_t exePath[MAX_PATH];
		THROW_LAST_ERROR_IF(!GetModuleFileNameW(g_hInst, exePath, ARRAYSIZE(exePath)));

		const fs::path linkPath = GetStartupFolderPath();

		wil::com_ptr<IShellLinkW> shellLink;
		THROW_IF_FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink)));

		const std::wstring args = L"/startup";
		THROW_IF_FAILED(shellLink->SetPath(exePath));
		THROW_IF_FAILED(shellLink->SetArguments(args.c_str()));
		THROW_IF_FAILED(shellLink->SetDescription(L"Audio Bridge - Bluetooth A2DP Sink Connector"));

		wil::com_ptr<IPersistFile> persistFile;
		THROW_IF_FAILED(shellLink->QueryInterface(IID_PPV_ARGS(&persistFile)));
		THROW_IF_FAILED(persistFile->Save(linkPath.c_str(), TRUE));
		return true;
	}
	CATCH_LOG();
	return false;
}

/// 删除 Startup 文件夹快捷方式；返回 true 表示删除成功或本就不存在
/// 同时清理当前名称和旧版下划线名称，确保安装包创建的快捷方式也能被删除
[[nodiscard]] inline bool DeleteStartupShortcut()
{
	try
	{
		wil::unique_cotaskmem_string startup;
		THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &startup));
		const fs::path startupDir(startup.get());

		bool allSuccess = true;
		std::error_code ec;

		// 删除当前名称（空格，与 setup.iss 一致）
		const fs::path linkPath = startupDir / AUTOSTART_LINK_NAME;
		if (fs::exists(linkPath, ec))
		{
			fs::remove(linkPath, ec);
			if (ec) allSuccess = false;
		}

		// 删除旧版名称（下划线），兼容旧安装
		const fs::path legacyPath = startupDir / AUTOSTART_LINK_NAME_LEGACY;
		if (fs::exists(legacyPath, ec))
		{
			fs::remove(legacyPath, ec);
			if (ec) allSuccess = false;
		}

		return allSuccess;
	}
	CATCH_LOG();
	return false;
}

inline bool SetAutoStart(bool enable)
{
	g_autoStart = enable;
	bool anySuccess = false;

	// 第一步：操作注册表（优先）
	{
		wil::unique_hkey hKey;
		const LSTATUS statusCreate = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, hKey.addressof(), nullptr);
		if (statusCreate == ERROR_SUCCESS)
		{
			LSTATUS status = ERROR_SUCCESS;
			if (enable)
			{
				wchar_t path[MAX_PATH];
				THROW_LAST_ERROR_IF(!GetModuleFileNameW(g_hInst, path, ARRAYSIZE(path)));
				const std::wstring commandLine = std::wstring(L"\"") + path + L"\" /startup";
				status = RegSetValueExW(hKey.get(), AUTOSTART_VALUE, 0, REG_SZ,
					reinterpret_cast<const BYTE*>(commandLine.c_str()),
					static_cast<DWORD>((commandLine.size() + 1) * sizeof(WCHAR)));
			}
			else
			{
				status = RegDeleteValueW(hKey.get(), AUTOSTART_VALUE);
				if (status == ERROR_FILE_NOT_FOUND)
					status = ERROR_SUCCESS;
			}
			if (status == ERROR_SUCCESS)
			{
				anySuccess = true;
				if (enable)
				{
					// 注册表成功时，顺手清理快捷方式（保持单一机制，避免双重入口困惑）
					(void)DeleteStartupShortcut();
				}
			}
			else
			{
				LOG_LAST_ERROR();
			}
		}
		else
		{
			SetLastError(statusCreate);
			LOG_LAST_ERROR();
		}
	}

	// 第二步：注册表失败 → 回退 Startup 快捷方式；禁用时两边都清理
	if (!anySuccess && enable)
	{
		anySuccess = CreateStartupShortcut();
	}
	else if (!enable)
	{
		// 禁用时清理快捷方式入口；失败不影响主结果（注册表已清理即为成功）
		(void)DeleteStartupShortcut();
	}

	return anySuccess;
}

inline void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		jsonObj.Insert(L"autoStart", JsonValue::CreateBooleanValue(g_autoStart));

		JsonArray lastDevices;
		std::unordered_set<std::wstring> lastDevicesSet(g_lastDevices.begin(), g_lastDevices.end());
		for (const auto& deviceId : g_lastDevices)
		{
			lastDevices.Append(JsonValue::CreateStringValue(deviceId));
		}
		// 从 App 层获取当前活动连接 ID（避免直接使用被接管的全局 map）
		for (const auto& deviceId : GetCurrentlyConnectedDeviceIds())
		{
			if (lastDevicesSet.insert(deviceId).second)
			{
				lastDevices.Append(JsonValue::CreateStringValue(deviceId));
			}
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		const fs::path configPath = GetConfigPath();
		const std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		WriteJsonToFile(configPath, utf8);
	}
	CATCH_LOG();
}
