#pragma once

#include "resource.h"
#include "PhoneAudioGain.hpp"
#include "BluetoothRecoveryPolicy.hpp"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_CONNECTION_STATE_CHANGED = WM_APP + 3;
constexpr UINT WM_DEVICE_LIST_CHANGED = WM_APP + 4;
constexpr UINT WM_PHONE_AUDIO_FAILED = WM_APP + 5;
constexpr UINT_PTR SHUTDOWN_TIMER_ID = 1;
constexpr UINT SHUTDOWN_RELEASE_DELAY_MS = 2000;
constexpr auto CONNECTION_RELEASE_COOLDOWN = std::chrono::milliseconds(1500);
constexpr auto CONNECTION_OPENED_TIMEOUT = std::chrono::seconds(5);
constexpr auto AUTO_CONNECT_STARTUP_DELAY = std::chrono::seconds(3);
constexpr int AUDIO_REPAIR_MAX_ATTEMPTS = 3;
// OpenAsync can take around 15 seconds on the affected Bluetooth stack.
// This is only a UI/repair observation timeout, never permission to close
// an OpenAsync that is still running.
constexpr auto AUDIO_REPAIR_CONNECTION_TIMEOUT = std::chrono::seconds(25);
constexpr auto AUDIO_REPAIR_POLL_INTERVAL = std::chrono::milliseconds(250);
constexpr double DEFAULT_PLAYBACK_VOLUME = 0.5;
constexpr int VOLUME_CONTROL_VERSION = 5;

struct ConnectionStateChangedMessage
{
	std::wstring deviceId;
	uint64_t generation{};
};

struct PhoneAudioFailureMessage
{
	std::wstring deviceId;
	uint64_t generation{};
	std::wstring reason;
};

class PhoneAudioProcessor;
void QueuePhoneAudioFailure(std::wstring deviceId, uint64_t generation, std::wstring reason);

struct AudioPlaybackConnectionEntry
{
	AudioPlaybackConnection Connection{ nullptr };
	uint64_t Generation{};
	bool Connecting{};
};

// Shared state. The application is a single process with a single UI thread;
// these objects are guarded by the mutexes below where noted.
inline HINSTANCE g_hInst;
inline HWND g_hWnd;
inline HWND g_hWndXaml;
inline wil::unique_handle g_singleInstanceMutex;
inline Canvas g_xamlCanvas = nullptr;
inline Flyout g_xamlFlyout = nullptr;
inline Flyout g_xamlDeviceFlyout = nullptr;
inline StackPanel g_deviceListPanel = nullptr;
inline MenuFlyout g_xamlMenu = nullptr;
inline FocusState g_menuFocusState = FocusState::Unfocused;
inline DeviceWatcher g_deviceWatcher = nullptr;
inline std::unordered_map<std::wstring, AudioPlaybackConnectionEntry> g_audioPlaybackConnections;
inline std::unordered_map<std::wstring, std::shared_ptr<PhoneAudioProcessor>> g_phoneAudioProcessors;
inline std::unordered_map<std::wstring, std::wstring> g_deviceErrorMessages;
inline std::unordered_set<std::wstring> g_radioRecoveryCandidates;
inline std::unordered_set<std::wstring> g_autoRadioRecoveryAttempted;
inline std::unordered_map<std::wstring, std::wstring> g_availableDeviceNames;
inline std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> g_lastConnectionCloseTimes;
// One long-running operation per device. Starting a second one while the first
// is still running would fight over the same Bluetooth link.
enum class DeviceOperation
{
	Repairing,
	Disconnecting
};

inline std::unordered_map<std::wstring, DeviceOperation> g_deviceOperations;
inline std::recursive_mutex g_connectionMutex;
inline std::mutex g_deviceListMutex;
inline std::mutex g_logMutex;
inline std::mutex g_volumeMutex;
inline HICON g_hIconLight = nullptr;
inline HICON g_hIconDark = nullptr;
inline NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
inline NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
inline UINT WM_TASKBAR_CREATED = 0;
inline bool g_reconnect = false;
inline std::atomic<double> g_playbackVolume = DEFAULT_PLAYBACK_VOLUME;
inline std::unordered_map<std::wstring, std::wstring> g_phoneEndpointGuids;
inline std::vector<std::wstring> g_lastDevices;
inline uint64_t g_nextConnectionGeneration = 0;
inline std::atomic_uint64_t g_deviceWatcherGeneration = 0;
inline std::atomic_bool g_deviceEnumerationCompleted = false;
inline std::atomic_bool g_devicePickerVisible = false;
inline std::atomic_bool g_shuttingDown = false;
inline std::atomic_bool g_radioRecoveryInProgress = false;
inline std::atomic_bool g_exitPendingAfterRadioRecovery = false;

// App.cpp
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void QueueDeviceListRefresh();
void SetupSvgIcon();
void UpdateNotifyIcon();

// Ui.cpp
void ApplyUiLanguage(winrt::Windows::UI::Xaml::FrameworkElement const& element);
void SetupFlyout();
void SetupMenu();
void SetupDevicePicker();
void RefreshDevicePicker();

// Connection.cpp
void HandleConnectionStateChanged(ConnectionStateChangedMessage* message);
void HandlePhoneAudioFailure(PhoneAudioFailureMessage* message);
winrt::fire_and_forget ConnectDevice(std::wstring deviceId);
winrt::fire_and_forget ConnectDevice(DeviceInformation device);
winrt::fire_and_forget AutoConnectDevice(std::wstring deviceId);
winrt::fire_and_forget DisconnectDevice(std::wstring deviceId);
winrt::fire_and_forget RepairDeviceAudio(std::wstring deviceId);
winrt::fire_and_forget RecoverBluetoothRadio(std::wstring deviceId);
void StopAllPhoneAudioBlocking();

#include "Util.hpp"
#include "Logging.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "AudioSessionVolume.hpp"
#include "PhoneAudioProcessor.hpp"
#include "Direct2DSvg.hpp"
