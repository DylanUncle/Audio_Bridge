// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

#include "targetver.h"

// Windows Header Files
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
// P0-3: 蓝牙适配器热插拔需要 DEV_BROADCAST_* 结构体和 DBT_* 常量
// WIN32_LEAN_AND_MEAN 排除了 dbt.h，这里显式引入
#include <dbt.h>
// P1-1: 默认音频设备变更监听需要（在 winrt 之前展开，遵循 IUnknown 冲突规避模式）
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
// 注：在 pch 阶段（using namespace winrt 之前）预先展开所有 SettingsUtil.hpp 用到的
// Win32 shell 头文件，让后续 SettingsUtil.hpp 的 #include 因 include guard 跳过，
// 从而避免 winrt::Windows::Foundation::IUnknown 与 Win32 ::IUnknown 在系统头文件
// 展开时的 C2872 歧义。
#include <shlobj_core.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <KnownFolders.h>
#include <shlwapi.h>

// C++ RunTime Header Files
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <optional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <memory>
#include <functional>

// wil
#ifndef _DEBUG
#define RESULT_DIAGNOSTICS_LEVEL 1
#endif

#include <wil/common.h>
#include <wil/result.h>
#include <wil/com.h>
#include <wil/cppwinrt.h>

// C++/WinRT
// Fixes warning C4002: too many arguments for function-like macro invocation 'GetCurrentTime'
#undef GetCurrentTime
// 修复 IUnknown 冲突：Win32 头文件定义了 ::IUnknown，C++/WinRT 定义了 winrt::Windows::Foundation::IUnknown
#ifdef IUnknown
#undef IUnknown
#endif

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Data.Xml.Dom.h>

#endif //PCH_H
