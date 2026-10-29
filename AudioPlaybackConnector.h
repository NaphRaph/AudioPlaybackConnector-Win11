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

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
wil::unique_handle g_singleInstanceMutex;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
Flyout g_xamlDeviceFlyout = nullptr;
StackPanel g_deviceListPanel = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DeviceWatcher g_deviceWatcher = nullptr;
std::unordered_map<std::wstring, AudioPlaybackConnectionEntry> g_audioPlaybackConnections;
std::unordered_map<std::wstring, std::shared_ptr<PhoneAudioProcessor>> g_phoneAudioProcessors;
std::unordered_map<std::wstring, std::wstring> g_deviceErrorMessages;
std::unordered_set<std::wstring> g_radioRecoveryCandidates;
std::unordered_set<std::wstring> g_autoRadioRecoveryAttempted;
std::unordered_map<std::wstring, std::wstring> g_availableDeviceNames;
std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> g_lastConnectionCloseTimes;
std::unordered_set<std::wstring> g_repairingDevices;
std::unordered_set<std::wstring> g_disconnectingDevices;
std::recursive_mutex g_connectionMutex;
std::mutex g_deviceListMutex;
std::mutex g_logMutex;
std::mutex g_volumeMutex;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
std::atomic<double> g_playbackVolume = DEFAULT_PLAYBACK_VOLUME;
std::unordered_map<std::wstring, std::wstring> g_phoneEndpointGuids;
std::vector<std::wstring> g_lastDevices;
uint64_t g_nextConnectionGeneration = 0;
std::atomic_uint64_t g_deviceWatcherGeneration = 0;
std::atomic_bool g_deviceEnumerationCompleted = false;
std::atomic_bool g_devicePickerVisible = false;
std::atomic_bool g_shuttingDown = false;
std::atomic_bool g_radioRecoveryInProgress = false;
std::atomic_bool g_exitPendingAfterRadioRecovery = false;

#include "Util.hpp"
#include "Logging.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "AudioSessionVolume.hpp"
#include "PhoneAudioProcessor.hpp"
#include "Direct2DSvg.hpp"
