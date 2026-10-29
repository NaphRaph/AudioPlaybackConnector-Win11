#include "pch.h"
#include "AudioPlaybackConnector.h"

// Process entry point, window procedure and notification-area icon.

void QueueDeviceListRefresh()
{
	if (!g_shuttingDown && IsWindow(g_hWnd))
		PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;
	SetLastError(ERROR_SUCCESS);
	auto singleInstanceMutex = CreateMutexW(nullptr, FALSE,
		L"Local\\AudioPlaybackConnector.SingleInstance.2DAAABDD-2402-4023-BC3E-B6E93FAD567B");
	const auto singleInstanceError = GetLastError();
	g_singleInstanceMutex.reset(singleInstanceMutex);
	if (!g_singleInstanceMutex)
		return EXIT_FAILURE;
	if (singleInstanceError == ERROR_ALREADY_EXISTS)
		return EXIT_SUCCESS;

	winrt::init_apartment();
	LoadSettings();
	LoadTranslateData();
	AppendLog(L"Application started");

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	ApplyUiLanguage(g_xamlCanvas);
	desktopSource.Content(g_xamlCanvas);

	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();
	StartPhoneVolumeApplier();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CLOSE:
	{
		if (g_radioRecoveryInProgress)
		{
			g_exitPendingAfterRadioRecovery = true;
			AppendLog(L"Application exit deferred until Bluetooth radio recovery finishes");
			break;
		}
		if (g_shuttingDown.exchange(true))
			break;
		AppendLog(L"Application shutdown requested");

		if (g_deviceWatcher)
		{
			try
			{
				g_deviceWatcher.Stop();
			}
			catch (...)
			{
				LOG_CAUGHT_EXCEPTION();
			}
			g_deviceWatcher = nullptr;
		}

		// Preserve the connected device IDs before releasing the connections only
		// when automatic reconnect was requested. Otherwise save an empty list.
		if (g_reconnect)
			SaveSettings();

		StopPhoneVolumeApplier();
		bool releasedConnections = false;
		{
			std::lock_guard lock(g_connectionMutex);
			releasedConnections = !g_audioPlaybackConnections.empty();
			for (const auto& connection : g_audioPlaybackConnections)
				connection.second.Connection.Close();
			g_audioPlaybackConnections.clear();
			g_deviceErrorMessages.clear();
			g_radioRecoveryCandidates.clear();
		}

		if (!g_reconnect)
			SaveSettings();

		// AudioPlaybackConnection releases the underlying Bluetooth transport
		// asynchronously. Keep the process alive briefly after dropping the final
		// reference so Windows can finish deactivating the A2DP sink cleanly.
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		ShowWindow(hWnd, SW_HIDE);
		if (releasedConnections && SetTimer(hWnd, SHUTDOWN_TIMER_ID, SHUTDOWN_RELEASE_DELAY_MS, nullptr))
			break;

		DestroyWindow(hWnd);
	}
	break;
	case WM_TIMER:
		if (wParam == SHUTDOWN_TIMER_ID)
		{
			KillTimer(hWnd, SHUTDOWN_TIMER_ID);
			DestroyWindow(hWnd);
		}
		break;
	case WM_DESTROY:
		g_shuttingDown = true;
		AppendLog(L"Application stopped");
		StopPhoneVolumeApplier();
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& connection : g_audioPlaybackConnections)
				connection.second.Connection.Close();
			g_audioPlaybackConnections.clear();
			g_deviceErrorMessages.clear();
			g_radioRecoveryCandidates.clear();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
			SetupFlyout();
			SetupMenu();
			SetupDevicePicker();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);
			g_xamlCanvas.Width(rect.Width);
			g_xamlCanvas.Height(rect.Height);
			g_devicePickerVisible = true;
			RefreshDevicePicker();
			g_xamlDeviceFlyout.ShowAt(g_xamlCanvas);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTION_STATE_CHANGED:
		HandleConnectionStateChanged(reinterpret_cast<ConnectionStateChangedMessage*>(lParam));
		break;
	case WM_VOLUME_CHANGED:
		if (g_volumeSlider)
			g_volumeSlider.Value(g_playbackVolume.load() * 100.0);
		break;
	case WM_DEVICE_LIST_CHANGED:
		RefreshDevicePicker();
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				AutoConnectDevice(i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}


void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
