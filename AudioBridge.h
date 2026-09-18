#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_SHOWPICKER = WM_APP + 3;

// ===== Bug 修复：重连定时器重新武装 / last devices 运行期维护 =====
// WM_APP_REARM：调度器新增条目后通知主线程重新武装重连定时器。
//   背景：WM_TIMER(1) 每次触发先 KillTimer，仅在调度器非空时再武装；
//   一旦调度器清空定时器即死亡，后续 Schedule() 只加条目无人触发。
//   StateChanged 回调可能运行在非主线程，不能直接 SetTimer（要求窗口
//   属于调用线程），必须 PostMessage 转主线程。
constexpr UINT WM_APP_REARM = WM_APP + 0x101;
// WM_APP_ADDLAST：连接成功后通知主线程把设备加入 g_lastDevices
// （lParam = new std::wstring*，主线程处理并 delete；保持主线程独占无锁）
constexpr UINT WM_APP_ADDLAST = WM_APP + 0x102;
// WM_APP_REMOVELAST：设备已被系统移除（取消配对，CreateFromIdAsync 抛
// FILE_NOT_FOUND）时通知主线程从 g_lastDevices 删除（lParam = new std::wstring*，
// 主线程处理并 delete）。下次开机不再对已移除设备做无效自动重连。
constexpr UINT WM_APP_REMOVELAST = WM_APP + 0x104;
// WM_APP_QUITREQUEST：外部进程（安装包卸载器 / 覆盖升级）请求本程序优雅退出。
//   背景：程序常驻托盘并占用自身 exe 文件句柄，卸载或覆盖安装时 Inno 无法
//   删除/替换 Audio_Bridge.exe。安装包通过 PostMessageW 发送本消息，程序收到后
//   走与托盘菜单"退出"一致的清理流程（关闭连接、保存设置、移除托盘、释放 Mutex）。
//   注意：消息值必须与 setup.iss [Code] 段中的 WM_APP_QUITREQUEST 常量保持一致。
constexpr UINT WM_APP_QUITREQUEST = WM_APP + 0x103;

// Menu IDs
constexpr UINT IDM_SETTINGS = 2001;
constexpr UINT IDM_AUTOSTART = 2002;
constexpr UINT IDM_EXIT = 2003;
constexpr UINT IDM_STATS = 2004;  // P1-2: 连接统计

// 兼容层全局变量（供 SettingsUtil.hpp / I18n.hpp 等旧代码直接访问）
extern HINSTANCE g_hInst;
extern HWND g_hWnd;
extern HMENU g_hMenu;
extern DevicePicker g_devicePicker;
extern HICON g_hTrayIcon;
extern NOTIFYICONDATAW g_nid;
extern UINT WM_TASKBAR_CREATED;
extern bool g_reconnect;
extern bool g_isExiting;
extern std::vector<std::wstring> g_lastDevices;

void QueueAutoReconnect(std::wstring deviceId);
void ShowToastNotification(const std::wstring& title, const std::wstring& message);
void ShowDevicePickerAtCursor();

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device);
winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId);

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "ReconnectScheduler.hpp"
#include "AudioPerformanceBoost.hpp"
#include "ConnectionManager.hpp"
#include "AudioPlaybackApp.hpp"

// P1-1: 默认音频输出设备变更监听（全局实例，AudioBridge.cpp 定义）
#include "AudioDeviceMonitor.hpp"
extern AudioDeviceMonitor g_audioDeviceMonitor;
